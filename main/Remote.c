// Faikin Remote

const char TAG[] = "Remote";

#include "revk.h"
#include <driver/i2c.h>
#include <hal/spi_types.h>
#include <math.h>
#include <esp_sntp.h>
#include "esp_http_server.h"
#include <onewire_bus.h>
#include <ds18b20.h>
#include "gfx.h"
#include "led_strip.h"
#include "bleenv.h"
#include "halib.h"
#include <lwpng.h>

struct
{
   uint8_t die:1;               // Shutting down
} b = { 0 };

httpd_handle_t webserver = NULL;
SemaphoreHandle_t epd_mutex = NULL;

static void *
my_alloc (void *opaque, uInt items, uInt size)
{
   return mallocspi (items * size);
}

static void
my_free (void *opaque, void *address)
{
   free (address);
}

const char *
app_callback (int client, const char *prefix, const char *target, const char *suffix, jo_t j)
{
   char value[1000];
   int len = 0;
   *value = 0;
   if (j)
   {
      len = jo_strncpy (j, value, sizeof (value));
      if (len < 0)
         return "Expecting JSON string";
      if (len > sizeof (value))
         return "Too long";
   }
   if (client || !prefix || target || strcmp (prefix, topiccommand) || !suffix)
      return NULL;

   return NULL;
}

void
revk_state_extra (jo_t j)
{
}

void
revk_web_extra (httpd_req_t * req, int page)
{
}

static void
register_uri (const httpd_uri_t * uri_struct)
{
   esp_err_t res = httpd_register_uri_handler (webserver, uri_struct);
   if (res != ESP_OK)
   {
      ESP_LOGE (TAG, "Failed to register %s, error code %d", uri_struct->uri, res);
   }
}

static void
register_get_uri (const char *uri, esp_err_t (*handler) (httpd_req_t * r))
{
   httpd_uri_t uri_struct = {
      .uri = uri,
      .method = HTTP_GET,
      .handler = handler,
   };
   register_uri (&uri_struct);
}

void
epd_lock (void)
{
   xSemaphoreTake (epd_mutex, portMAX_DELAY);
   gfx_lock ();
}

void
epd_unlock (void)
{
   gfx_unlock ();
   xSemaphoreGive (epd_mutex);
}

#ifdef	CONFIG_LWPNG_ENCODE
static esp_err_t
web_frame (httpd_req_t * req)
{
   xSemaphoreTake (epd_mutex, portMAX_DELAY);
   uint8_t *png = NULL;
   size_t len = 0;
   uint32_t w = gfx_raw_w ();
   uint32_t h = gfx_raw_h ();
   uint8_t *b = gfx_raw_b ();
   const char *e = NULL;
   if (gfx_bpp () == 16)
   {                            // RGB packed
      uint8_t *buf = mallocspi (w * 3);
      if (!buf)
         e = "malloc";
      else
      {
         lwpng_encode_t *p = lwpng_encode_rgb (w, h, &my_alloc, &my_free, NULL);
         if (b)
            while (h--)
            {
               for (int x = 0; x < w; x++)
               {
                  uint16_t v = (b[x * 2 + 0] << 8) | b[x * 2 + 1];
                  uint8_t r = (v >> 11) << 3;
                  r += (r >> 5);
                  uint8_t g = ((v >> 5) & 0x3F) << 2;
                  g += (g >> 6);
                  uint8_t b = (v & 0x1F) << 3;
                  b += (b >> 5);
                  buf[x * 3 + 0] = r;
                  buf[x * 3 + 1] = g;
                  buf[x * 3 + 2] = b;
               }
               lwpng_encode_scanline (p, buf);
               b += w * 2;
            }
         e = lwpng_encoded (&p, &len, &png);
         free (buf);
      }
   }
   ESP_LOGD (TAG, "Encoded %u bytes %s", len, e ? : "");
   if (e)
   {
      revk_web_head (req, *hostname ? hostname : appname);
      revk_web_send (req, e);
      revk_web_foot (req, 0, 1, NULL);
   } else
   {
      httpd_resp_set_type (req, "image/png");
      httpd_resp_send (req, (char *) png, len);
   }
   free (png);
   xSemaphoreGive (epd_mutex);
   return ESP_OK;
}
#endif

static esp_err_t
web_root (httpd_req_t * req)
{
   if (revk_link_down ())
      return revk_web_settings (req);   // Direct to web set up
   revk_web_head (req, *hostname ? hostname : appname);
#ifdef	CONFIG_LWPNG_ENCODE
   revk_web_send (req, "<p><img src=frame.png></p>");
   revk_web_send (req, "<p><a href=/>Reload</a></p>");
#endif
   return revk_web_foot (req, 0, 1, NULL);
}

void
app_main ()
{
   epd_mutex = xSemaphoreCreateMutex ();
   xSemaphoreGive (epd_mutex);
   revk_boot (&app_callback);
   revk_start ();

#ifndef	CONFIG_GFX_BUILD_SUFFIX_GFXNONE
   if (gfxmosi.set)
   {
    const char *e = gfx_init (cs: gfxcs.num, sck: gfxsck.num, mosi: gfxmosi.num, dc: gfxdc.num, rst: gfxrst.num, flip:gfxflip);
      if (e)
      {
         jo_t j = jo_object_alloc ();
         jo_string (j, "error", "Failed to start");
         jo_string (j, "description", e);
         revk_error ("GFX", &j);
      }
   }
   gfx_message ("Startup");
#endif
   // Web interface
   httpd_config_t config = HTTPD_DEFAULT_CONFIG ();
   config.stack_size += 1024 * 4;
   config.lru_purge_enable = true;
   config.max_uri_handlers = 2 + revk_num_web_handlers ();
   if (!httpd_start (&webserver, &config))
   {
      register_get_uri ("/", web_root);
#ifdef	CONFIG_LWPNG_ENCODE
      register_get_uri ("/frame.png", web_frame);
#endif
      revk_web_settings_add (webserver);
   }

   while (!revk_shutting_down (NULL))
   {
      sleep (1);
   }
   b.die = 1;
}
