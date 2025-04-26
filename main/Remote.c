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


httpd_handle_t webserver = NULL;

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

static esp_err_t
web_root (httpd_req_t * req)
{
   if (revk_link_down ())
      return revk_web_settings (req);   // Direct to web set up
   revk_web_head (req, *hostname ? hostname : appname);

   return revk_web_foot (req, 0, 1, NULL);
}

void
app_main ()
{
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

   while (1)
   {
      sleep (1);
   }
}
