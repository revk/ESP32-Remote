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
#include "icons.c"

struct
{
   uint8_t die:1;               // Shutting down
   uint8_t ha:1;                // HA update
   uint8_t display:1;           // Display update
   uint8_t night:1;             // Display dark
   uint8_t away:1;              // Away mode
   uint8_t poweron:1;           // Logical power on
   uint8_t manual:1;            // Manual set power
   uint8_t manualon:1;          // Manual is on
} b = { 0 };

httpd_handle_t webserver = NULL;
SemaphoreHandle_t epd_mutex = NULL;
int8_t i2cport = 0;
uint8_t ds18b20_num = 0;
const char *message = NULL;
enum
{
   EDIT_NONE,
   EDIT_TARGET,
   EDIT_MODE,
   EDIT_FAN,
   EDIT_START,
   EDIT_STOP,
   EDIT_NUM,
};
uint8_t edit = EDIT_NONE;       // Edit mode
uint32_t wake = 0;              // Wake (uptime) timeout

struct
{
   uint8_t found:1;
   uint8_t ok:1;
   float r,
     g,
     b,
     w;
} veml6040 = { 0 };

struct
{
   uint8_t found:1;
   uint8_t ok:1;
   float c;
} tmp1075 = { 0 };

struct
{
   uint8_t found:1;
   uint8_t ok:1;
   float c;
} mcp9808 = { 0 };

struct
{
   uint8_t found:1;
   uint8_t ok:1;
   float hpa;
   float c;
} gzp6816d = { 0 };

struct
{
   uint8_t found:1;
   uint8_t ok:1;
   uint16_t ppm;
} t6793 = { 0 };

struct
{
   uint8_t found:1;
   uint8_t ok:1;
   uint32_t serial;
   uint16_t ppm;
   float c;
   float rh;
} scd41 = { 0 };

struct
{
   uint8_t ok:1;
   ds18b20_device_handle_t handle;
   uint64_t serial;
   float c;
} *ds18b20s = NULL;

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

void
btn_task (void *x)
{
   while (1)
   {
      sleep (1);                // TODO
   }
}

static int32_t
i2c_read_16lh (uint8_t addr, uint8_t cmd)
{
   uint8_t h = 0,
      l = 0;
   i2c_cmd_handle_t t = i2c_cmd_link_create ();
   i2c_master_start (t);
   i2c_master_write_byte (t, (addr << 1) | I2C_MASTER_WRITE, true);
   i2c_master_write_byte (t, cmd, true);
   i2c_master_start (t);
   i2c_master_write_byte (t, (addr << 1) | I2C_MASTER_READ, true);
   i2c_master_read_byte (t, &l, I2C_MASTER_ACK);
   i2c_master_read_byte (t, &h, I2C_MASTER_LAST_NACK);
   i2c_master_stop (t);
   esp_err_t err = i2c_master_cmd_begin (i2cport, t, 10 / portTICK_PERIOD_MS);
   i2c_cmd_link_delete (t);
   if (err)
   {
      ESP_LOGE (TAG, "I2C %02X %02X fail %s", addr & 0x7F, cmd, esp_err_to_name (err));
      return -1;
   }
   ESP_LOGD (TAG, "I2C %02X %02X %02X%02X OK", addr & 0x7F, cmd, h, l);
   return (h << 8) + l;
}

static esp_err_t
i2c_write_16lh (uint8_t addr, uint8_t cmd, uint16_t val)
{
   i2c_cmd_handle_t t = i2c_cmd_link_create ();
   i2c_master_start (t);
   i2c_master_write_byte (t, (addr << 1) | I2C_MASTER_WRITE, true);
   i2c_master_write_byte (t, cmd, true);
   i2c_master_write_byte (t, val & 0xFF, true);
   i2c_master_write_byte (t, val >> 8, true);
   i2c_master_stop (t);
   esp_err_t err = i2c_master_cmd_begin (i2cport, t, 10 / portTICK_PERIOD_MS);
   i2c_cmd_link_delete (t);
   return err;
}

static int32_t
i2c_read_16hl (uint8_t addr, uint8_t cmd)
{
   uint8_t h = 0,
      l = 0;
   i2c_cmd_handle_t t = i2c_cmd_link_create ();
   i2c_master_start (t);
   i2c_master_write_byte (t, (addr << 1) | I2C_MASTER_WRITE, true);
   i2c_master_write_byte (t, cmd, true);
   i2c_master_start (t);
   i2c_master_write_byte (t, (addr << 1) | I2C_MASTER_READ, true);
   i2c_master_read_byte (t, &h, I2C_MASTER_ACK);
   i2c_master_read_byte (t, &l, I2C_MASTER_LAST_NACK);
   i2c_master_stop (t);
   esp_err_t err = i2c_master_cmd_begin (i2cport, t, 10 / portTICK_PERIOD_MS);
   i2c_cmd_link_delete (t);
   if (err)
   {
      ESP_LOGE (TAG, "I2C %02X %02X fail %s", addr & 0x7F, cmd, esp_err_to_name (err));
      return -1;
   }
   ESP_LOGD (TAG, "I2C %02X %02X %02X%02X OK", addr & 0x7F, cmd, h, l);
   return (h << 8) + l;
}

static int32_t
i2c_modbus_read (uint8_t addr, uint16_t a)
{
   uint8_t s = 0,
      b = 0,
      h = 0,
      l = 0;
   i2c_cmd_handle_t t = i2c_cmd_link_create ();
   i2c_master_start (t);
   i2c_master_write_byte (t, (addr << 1) | I2C_MASTER_WRITE, true);
   i2c_master_write_byte (t, 0x04, true);
   i2c_master_write_byte (t, a >> 8, true);
   i2c_master_write_byte (t, a, true);
   i2c_master_write_byte (t, 0x00, true);
   i2c_master_write_byte (t, 0x01, true);
   i2c_master_start (t);
   i2c_master_write_byte (t, (addr << 1) | I2C_MASTER_READ, true);
   i2c_master_read_byte (t, &s, I2C_MASTER_ACK);
   i2c_master_read_byte (t, &b, I2C_MASTER_ACK);
   i2c_master_read_byte (t, &h, I2C_MASTER_ACK);
   i2c_master_read_byte (t, &l, I2C_MASTER_LAST_NACK);
   i2c_master_stop (t);
   esp_err_t err = i2c_master_cmd_begin (i2cport, t, 10 / portTICK_PERIOD_MS);
   i2c_cmd_link_delete (t);
   if (err)
   {
      ESP_LOGE (TAG, "I2C %02X %04X fail %s", addr & 0x7F, a, esp_err_to_name (err));
      return -1;
   }
   if (s != 4 || b != 2)
   {
      ESP_LOGE (TAG, "I2C %02X %04X %02X %02X %02X%02X Bad", addr & 0x7F, a, s, b, h, l);
      return -1;
   }
   ESP_LOGD (TAG, "I2C %02X %04X %02X %02X %02X%02X OK", addr & 0x7F, a, s, b, h, l);
   return (h << 8) + l;
}

static uint8_t
scd41_crc (uint8_t b1, uint8_t b2)
{
   uint8_t crc = 0xFF;
   void b (uint8_t v)
   {
      crc ^= v;
      uint8_t n = 8;
      while (n--)
      {
         if (crc & 0x80)
            crc = (crc << 1) ^ 0x31;
         else
            crc <<= 1;
      }
   }
   b (b1);
   b (b2);
   return crc;
}

static esp_err_t
scd41_command (uint16_t c)
{
   i2c_cmd_handle_t t = i2c_cmd_link_create ();
   i2c_master_start (t);
   i2c_master_write_byte (t, (scd41i2c << 1) | I2C_MASTER_WRITE, true);
   i2c_master_write_byte (t, c >> 8, true);
   i2c_master_write_byte (t, c, false);
   i2c_master_stop (t);
   esp_err_t err = i2c_master_cmd_begin (i2cport, t, 100 / portTICK_PERIOD_MS);
   i2c_cmd_link_delete (t);
   if (err)
      ESP_LOGE (TAG, "I2C %02X %04X fail %s", scd41i2c & 0x7F, c, esp_err_to_name (err));
   return err;
}

static esp_err_t
scd41_read (uint16_t c, int8_t len, uint8_t * buf)
{
   i2c_cmd_handle_t t = i2c_cmd_link_create ();
   i2c_master_start (t);
   i2c_master_write_byte (t, (scd41i2c << 1) | I2C_MASTER_WRITE, true);
   i2c_master_write_byte (t, c >> 8, true);
   i2c_master_write_byte (t, c, true);
   i2c_master_start (t);
   i2c_master_write_byte (t, (scd41i2c << 1) + I2C_MASTER_READ, true);
   i2c_master_read (t, buf, len, I2C_MASTER_LAST_NACK);
   i2c_master_stop (t);
   esp_err_t err = i2c_master_cmd_begin (i2cport, t, 100 / portTICK_PERIOD_MS);
   i2c_cmd_link_delete (t);
   if (err)
      ESP_LOGE (TAG, "I2C %02X %d fail %s", scd41i2c & 0x7F, len, esp_err_to_name (err));
   return err;
}

void
i2c_task (void *x)
{
   void fail (uint8_t addr, const char *e)
   {
      ESP_LOGE (TAG, "I2C fail %02X: %s", addr & 0x7F, e);
      jo_t j = jo_object_alloc ();
      jo_string (j, "error", e);
      jo_int (j, "sda", sda.num);
      jo_int (j, "scl", scl.num);
      if (addr)
         jo_stringf (j, "addr", "%02X", addr & 0x7F);
      revk_error ("I2C", &j);
   }
   if (i2c_driver_install (i2cport, I2C_MODE_MASTER, 0, 0, 0))
   {
      fail (0, "Driver install");
      i2cport = -1;
   } else
   {
      i2c_config_t config = {
         .mode = I2C_MODE_MASTER,
         .sda_io_num = sda.num,
         .scl_io_num = scl.num,
         .sda_pullup_en = true,
         .scl_pullup_en = true,
         .master.clk_speed = 100000,
      };
      if (i2c_param_config (i2cport, &config))
      {
         i2c_driver_delete (i2cport);
         fail (0, "Config fail");
         i2cport = -1;
      } else
         i2c_set_timeout (i2cport, 31);
   }
   if (i2cport < 0)
      vTaskDelete (NULL);
   // Init
   if (veml6040i2c)
   {
      if (i2c_read_16lh (veml6040i2c, 0) < 0 && i2c_read_16lh (veml6040i2c, 0) < 0)
         fail (veml6040i2c, "VEML6040");
      else
      {
         veml6040.found = 1;
         i2c_write_16lh (veml6040i2c, 0x00, 0x0040);    // IT=4 TRIG=0 AF=0 SD=0
      }
   }
   if (mcp9808i2c)
   {
      if (i2c_read_16hl (mcp9808i2c, 6) != 0x54 || i2c_read_16hl (mcp9808i2c, 7) != 0x0400)
         fail (mcp9808i2c, "MCP9808");
      else
         mcp9808.found = 1;
   }
   if (gzp6816di2c)
   {
      uint8_t v = 0;
      i2c_cmd_handle_t t = i2c_cmd_link_create ();
      i2c_master_start (t);
      i2c_master_write_byte (t, (gzp6816di2c << 1) | I2C_MASTER_READ, true);
      i2c_master_read_byte (t, &v, I2C_MASTER_LAST_NACK);
      i2c_master_stop (t);
      esp_err_t err = i2c_master_cmd_begin (i2cport, t, 10 / portTICK_PERIOD_MS);
      i2c_cmd_link_delete (t);
      if (!err)
         gzp6816d.found = 1;
   }
   if (t6793i2c)
   {
      if (i2c_modbus_read (t6793i2c, 0x1389) < 0)
         fail (t6793i2c, "T6793");
      else
         t6793.found = 1;
   }
   if (scd41i2c)
   {
      esp_err_t err = 0;
      uint8_t try = 10;
      while (try--)
      {
         err = scd41_command (0x3F86);  /* Stop measurement(SCD41) */
         if (!err)
         {
            usleep (500000);
            err = scd41_command (0x3646);       /* Reinit */
         }
         if (!err)
         {
            usleep (20000);
            break;
         }
         sleep (1);
      }
      uint8_t buf[9];
      if (err || scd41_read (0x3682, 9, buf))
         fail (scd41i2c, "SCD41");
      else
      {
         if (scd41_crc (buf[0], buf[1]) == buf[2] && scd41_crc (buf[3], buf[4]) == buf[5] && scd41_crc (buf[6], buf[7]) == buf[8])
         {
            scd41.serial =
               ((unsigned long long) buf[0] << 40) + ((unsigned long long) buf[1] << 32) +
               ((unsigned long long) buf[3] << 24) + ((unsigned long long) buf[4] << 16) +
               ((unsigned long long) buf[6] << 8) + ((unsigned long long) buf[7]);
            if (!scd41_command (0x21B1))
               scd41.found = 1;
         } else
            ESP_LOGE (TAG, "SCD41 CRC bad %02X %02X %02X %02X %02X %02X %02X %02X %02X", buf[0], buf[1], buf[2], buf[3], buf[4],
                      buf[5], buf[6], buf[7], buf[8]);
      }
   }
   if (tmp1075i2c)
   {
      // TODO
   }
   b.ha = 1;
   // Poll
   while (!b.die)
   {
      if (gzp6816d.found)
      {
         i2c_cmd_handle_t t = i2c_cmd_link_create ();
         i2c_master_start (t);
         i2c_master_write_byte (t, (gzp6816di2c << 1) | I2C_MASTER_WRITE, true);
         i2c_master_write_byte (t, 0xAC, false);
         i2c_master_stop (t);
         i2c_master_cmd_begin (i2cport, t, 10 / portTICK_PERIOD_MS);
         i2c_cmd_link_delete (t);
      }
      usleep (500000);
      if (veml6040.found)
      {                         // Scale to lux
         int32_t v;
         veml6040.r = (v = i2c_read_16lh (veml6040i2c, 0x08)) >= 0 ? (float) v *1031 / 65535 : NAN;
         veml6040.g = v >= 0 && (v = i2c_read_16lh (veml6040i2c, 0x09)) >= 0 ? (float) v *1031 / 65535 : NAN;
         veml6040.b = v >= 0 && (v = i2c_read_16lh (veml6040i2c, 0x0A)) >= 0 ? (float) v *1031 / 65535 : NAN;
         veml6040.w = v >= 0 && (v = i2c_read_16lh (veml6040i2c, 0x0B)) >= 0 ? (float) v *1031 / 65535 : NAN;
         veml6040.ok = (v < 0 ? 0 : 1);
         if (veml6040dark && gfxbl.set)
            revk_gpio_set (gfxbl, veml6040.w < (float) veml6040dark / veml6040dark_scale ? 0 : 1);
      }
      if (mcp9808.found)
      {
         static int16_t last1 = 20 * 128,
            last2 = 20 * 128,
            last3 = 20 * 128;
         int32_t v = i2c_read_16hl (mcp9808i2c, 5);
         if (v < 0)
         {
            mcp9808.ok = 0;
            mcp9808.c = NAN;
         } else
         {
            int16_t t = (v << 3),
               a = (last1 + last2 + last3 + t) / 4;
            last3 = last2;
            last2 = last1;
            last1 = t;
            mcp9808.c = (float) a / 128 + (float) mcp9808dt / mcp9808dt_scale;
            mcp9808.ok = 1;
         }
      }
      if (gzp6816d.found)
      {
         uint8_t s,
           p1,
           p2,
           p3,
           t1,
           t2;
         i2c_cmd_handle_t t = i2c_cmd_link_create ();
         i2c_master_start (t);
         i2c_master_write_byte (t, (gzp6816di2c << 1) | I2C_MASTER_READ, true);
         i2c_master_read_byte (t, &s, I2C_MASTER_ACK);
         i2c_master_read_byte (t, &p1, I2C_MASTER_ACK);
         i2c_master_read_byte (t, &p2, I2C_MASTER_ACK);
         i2c_master_read_byte (t, &p3, I2C_MASTER_ACK);
         i2c_master_read_byte (t, &t1, I2C_MASTER_ACK);
         i2c_master_read_byte (t, &t2, I2C_MASTER_LAST_NACK);
         i2c_master_stop (t);
         esp_err_t err = i2c_master_cmd_begin (i2cport, t, 10 / portTICK_PERIOD_MS);
         i2c_cmd_link_delete (t);
         if (!err && !(s & 0x20))
         {
            gzp6816d.c = (float) ((t1 << 8) | t2) * 190 / 65536 - 40 + (float) gzp6816ddt / gzp6816ddt_scale;
            gzp6816d.hpa = (float) 800 *(((p1 << 16) | (p2 << 8) | p3) - 1677722) / 13421772 + 300;
            gzp6816d.ok = 1;
         } else
         {
            gzp6816d.ok = 0;
            gzp6816d.c = NAN;
            gzp6816d.hpa = NAN;
         }
      }
      if (t6793.found)
      {
         int32_t v = i2c_modbus_read (t6793i2c, 0x138B);
         if (v > 0)
         {
            t6793.ppm = v;
            t6793.ok = 1;
         } else
         {
            t6793.ok = 0;
            t6793.ppm = 0;
         }
      }
      if (scd41.found)
      {
         uint8_t buf[9];
         esp_err_t err = 0;
         if (!(err = scd41_read (0xE4B8, 3, buf)) && scd41_crc (buf[0], buf[1]) == buf[2] && ((buf[0] & 0x7) || buf[1]) &&
             !(err = scd41_read (0xEC05, sizeof (buf), buf)) &&
             scd41_crc (buf[0], buf[1]) == buf[2] && scd41_crc (buf[3], buf[4]) == buf[5] && scd41_crc (buf[6], buf[7]) == buf[8])
         {
            scd41.ppm = (buf[0] << 8) + buf[1];
            if (uptime () > 30)
               scd41.c = -45.0 + 175.0 * (float) ((buf[3] << 8) + buf[4]) / 65536.0 + (float) scd41dt / scd41dt_scale;
            scd41.rh = 100.0 * (float) ((buf[6] << 8) + buf[7]) / 65536.0;
            scd41.ok = 1;
         } else if (err)
         {
            scd41.ok = 0;
            scd41.c = NAN;
            scd41.rh = 0;
         }
      }
      if (tmp1075.found)
      {
         // TODO
      }
      usleep (500000);
   }
   vTaskDelete (NULL);
}

void
ds18b20_task (void *x)
{
   onewire_bus_config_t bus_config = { ds18b20.num };
   onewire_bus_rmt_config_t rmt_config = { 20 };
   onewire_bus_handle_t bus_handle = { 0 };
   REVK_ERR_CHECK (onewire_new_bus_rmt (&bus_config, &rmt_config, &bus_handle));
   void init (void)
   {
      onewire_device_iter_handle_t iter = { 0 };
      REVK_ERR_CHECK (onewire_new_device_iter (bus_handle, &iter));
      onewire_device_t dev = { };
      while (!onewire_device_iter_get_next (iter, &dev))
      {
         ds18b20s = realloc (ds18b20s, (ds18b20_num + 1) * sizeof (*ds18b20s));
         ds18b20s[ds18b20_num].c = NAN;
         ds18b20s[ds18b20_num].serial = dev.address;
         ds18b20_config_t config = { };
         REVK_ERR_CHECK (ds18b20_new_device (&dev, &config, &ds18b20s[ds18b20_num].handle));
         REVK_ERR_CHECK (ds18b20_set_resolution (ds18b20s[ds18b20_num].handle, DS18B20_RESOLUTION_12B));
         ds18b20_num++;
      }
   }
   init ();
   if (!ds18b20_num)
   {
      usleep (100000);
      init ();
   }
   if (!ds18b20_num)
   {
      jo_t j = jo_object_alloc ();
      jo_string (j, "error", "No DS18B20 devices");
      jo_int (j, "port", ds18b20.num);
      revk_error ("temp", &j);
      ESP_LOGE (TAG, "No DS18B20 port %d", ds18b20.num);
      vTaskDelete (NULL);
      return;
   }
   b.ha = 1;
   while (!b.die)
   {
      usleep (250000);
      for (int i = 0; i < ds18b20_num; ++i)
      {
         REVK_ERR_CHECK (ds18b20_trigger_temperature_conversion (ds18b20s[i].handle));
         REVK_ERR_CHECK (ds18b20_get_temperature (ds18b20s[i].handle, &ds18b20s[i].c));
      }
   }
   vTaskDelete (NULL);
}

static esp_err_t
web_favicon (httpd_req_t * req)
{                               // serve image -  maybe make more generic file serve
   httpd_resp_set_type (req, "image/x-icon");
   extern const char fistart[] asm ("_binary_favicon_ico_start");
   extern const char fiend[] asm ("_binary_favicon_ico_end");
   httpd_resp_send (req, fistart, fiend - fistart);
   return ESP_OK;
}

static esp_err_t
web_apple (httpd_req_t * req)
{                               // serve image -  maybe make more generic file serve
   httpd_resp_set_type (req, "image/png");
   extern const char istart[] asm ("_binary_apple_touch_icon_png_start");
   extern const char iend[] asm ("_binary_apple_touch_icon_png_end");
   httpd_resp_send (req, istart, iend - istart);
   return ESP_OK;
}

static esp_err_t
web_icon (httpd_req_t * req)
{                               // serve image -  maybe make more generic file serve
   char *name = strrchr (req->uri, '?');
   if (name)
   {
      name++;
      for (int i = 0; i < sizeof (icons) / sizeof (*icons); i++)
         if (!strcasecmp (icons[i].name, name))
         {
            httpd_resp_set_type (req, "image/png");
            httpd_resp_send (req, (const char *) icons[i].start, icons[i].end - icons[i].start);
            return ESP_OK;
         }
   }
   httpd_resp_send_404 (req);
   return ESP_OK;
}

static esp_err_t
web_root (httpd_req_t * req)
{
   if (revk_link_down ())
      return revk_web_settings (req);   // Direct to web set up
   revk_web_head (req, *hostname ? hostname : appname);
#ifdef	CONFIG_LWPNG_ENCODE
   revk_web_send (req, "<p><img src=frame.png style='border:10px solid black;'></p>"    //
                  "<table border=1>"    //
                  "<tr><td></td><td><a href='btn?N'>N</a></td><td></td></tr>"   //
                  "<tr><td><a href='btn?W'>W</a></td><td><a href='btn?P'>P</a></td><td><a href='btn?E'>E</a></td></tr>" //
                  "<tr><td></td><td><a href='btn?S'>S</a></td><td></td><td><a href='btn?H'>Hold</a></td></tr>"  //
                  "</table>"    //
                  "<p><a href=/>Reload</a></p>");
#endif
   return revk_web_foot (req, 0, 1, NULL);
}

uint8_t
btnwake (void)
{
   b.display = 1;
   b.away = 0;
   if (!wake)
   {
      if (b.night)
      {
         edit = 0;
         wake = uptime () + 10;
         return 1;              // Woken up
      }
   }
   if (!edit)
      edit = EDIT_TARGET;
   wake = uptime () + 10;
   return 0;
}

void
btnNS (int8_t d)
{
   if (btnwake ())
      return;
   jo_t j = jo_object_alloc ();
   switch (edit)
   {
   case EDIT_TARGET:
      if (tempstep)
      {
         int16_t t = actarget;
         t = t / tempstep * tempstep;
         t += d * tempstep;
         if (t < tempmin)
            t = tempmin;
         if (t > tempmax)
            t = tempmax;
         jo_litf (j, "actarget", "%.1f", (float) t / actarget_scale);
      }
      break;
   case EDIT_MODE:
      {
      }
      break;
   case EDIT_FAN:
      {
      }
      break;
   case EDIT_START:
      {
      }
      break;
   case EDIT_STOP:
      {
      }
      break;
   }
   revk_setting (j);
   jo_free (&j);
}

void
btnEW (int8_t d)
{
   if (btnwake ())
      return;
   edit += d;
   if (!edit)
      edit = EDIT_NUM - 1;
   while ((nomode && edit == EDIT_MODE) || (nofan && edit == EDIT_FAN))
      edit += d;
   if (edit == EDIT_NUM)
      edit = 1;
}

void
btnP (void)
{
   if (btnwake ())
      return;
   if (!b.manual)
      b.manualon = b.poweron;
   b.manualon ^= 1;
   b.manual = 1;
   edit = 0;
}

void
btnH (void)
{
   if (btnwake ())
      return;
   edit = 0;
   b.away = 1;
}

static esp_err_t
web_btn (httpd_req_t * req)
{
   char *name = strrchr (req->uri, '?');
   if (name)
      switch (name[1])
      {
      case 'N':
         btnNS (1);
         break;
      case 'S':
         btnNS (-1);
         break;
      case 'E':
         btnEW (1);
         break;
      case 'W':
         btnEW (-1);
         break;
      case 'P':
         btnP ();
         break;
      case 'H':
         btnH ();
         break;
      }
   usleep (500000);
   return web_root (req);
}

#ifndef CONFIG_GFX_BUILD_SUFFIX_GFXNONE
typedef struct plot_s
{
   gfx_pos_t ox,
     oy;
} plot_t;

static const char *
pixel (void *opaque, uint32_t x, uint32_t y, uint16_t r, uint16_t g, uint16_t b, uint16_t a)
{
   plot_t *p = opaque;
   gfx_pixel_argb (p->ox + x, p->oy + y, ((a >> 8) << 24) | ((r >> 8) << 16) | ((g >> 8) << 8) | (b >> 8));
   return NULL;
}
#endif

void
icon_plot (uint8_t i)
{
#ifndef CONFIG_GFX_BUILD_SUFFIX_GFXNONE
   if (i >= sizeof (icons) / sizeof (*icons))
      return;
   uint32_t w,
     h;
   const char *e = lwpng_get_info (icons[i].end - icons[i].start, icons[i].start, &w, &h);
   if (e)
      return;
   gfx_pos_t ox = 0,
      oy = 0;
   gfx_draw (w, h, 0, 0, &ox, &oy);
   plot_t settings = { ox, oy };
   lwpng_decode_t *p = lwpng_decode (&settings, NULL, &pixel, &my_alloc, &my_free, NULL);
   lwpng_data (p, icons[i].end - icons[i].start, icons[i].start);
   e = lwpng_decoded (&p);
#endif
}

void
select_icon_plot (uint8_t i)
{
#ifndef CONFIG_GFX_BUILD_SUFFIX_GFXNONE
   if (i >= sizeof (icons) / sizeof (*icons))
      return;
   uint32_t w,
     h;
   const char *e = lwpng_get_info (icons[i].end - icons[i].start, icons[i].start, &w, &h);
   if (e)
      return;
   gfx_pos_t ox = 0,
      oy = 0,
      wx = gfx_x (),
      wy = gfx_y ();
   gfx_align_t wa = gfx_a ();
   gfx_draw (w, h, 0, 0, &ox, &oy);
   plot_t settings = { ox, oy };
   lwpng_decode_t *p = lwpng_decode (&settings, NULL, &pixel, &my_alloc, &my_free, NULL);
   lwpng_data (p, icons[i].end - icons[i].start, icons[i].start);
   e = lwpng_decoded (&p);
   gfx_pos (wx, wy, wa);
#endif
}

const uint8_t icon_mode[] = { icon_modeauto, icon_modefan, icon_modedry, icon_modecool, icon_modeheat, icon_modefaikin };       // order same as acmode
const uint8_t icon_fans5[] = { icon_fanauto, icon_fan1, icon_fan2, icon_fan3, icon_fan4, icon_fan5, icon_fanquiet };    // order same as acfan
const uint8_t icon_fans3[] = { icon_fanauto, icon_fanlow, 0xFF, icon_fanmedium, 0xFF, icon_fanhigh, icon_fanquiet };    // order same as acfan

void
temp_colour (float t)
{
   gfx_colour_t c = 0x888888;
   if (!isnan (t))
   {
      if (t < tempblue - 0.5)
         c = 0x0000FF;
      else if (t < tempblue + 0.5)
         c = gfx_blend (0x0000FF, 0x00FF00, 255 * (t - tempblue + 0.5));
      else if (t < tempred - 0.5)
         c = 0x00FF00;
      else if (t < tempred + 0.5)
         c = gfx_blend (0x00FF00, 0xFF0000, 255 * (t - tempred + 0.5));
      else
         c = 0xFF0000;
   }
   gfx_foreground (c);
}

void
co2_colour (uint16_t co2)
{
   gfx_colour_t c = 0x888888;
   if (co2)
   {
      if (co2 < co2green)
         c = 0x00FF00;
      else if (co2 < co2red)
         c = 0xFFFF00;
      else
         c = 0xFF0000;
   }
   gfx_foreground (c);
}

void
rh_colour (uint8_t rh)
{
   gfx_colour_t c = 0x888888;
   if (rh)
   {
      if (rh < rhgreen)
         c = 0x00FF00;
      else if (rh < rhred)
         c = 0xFFFF00;
      else
         c = 0xFF0000;
   }
   gfx_foreground (c);
}

void
show_temp (float t)
{                               // Show current temp
   if (fahrenheit && !isnan (t))
      t = (t + 40) * 1.8 - 40;
   temp_colour (t);
   if (isnan (t) || t <= -100 || t >= 1000)
      gfx_7seg (GFX_7SEG_SMALL_DOT, 11, "---.-%c", fahrenheit ? 'F' : 'C');
   else
      gfx_7seg (GFX_7SEG_SMALL_DOT, 11, "%5.1f%c", t, fahrenheit ? 'F' : 'C');
}

void
show_target (float t)
{                               // Show target temp
   if (fahrenheit && !isnan (t))
      t = (t + 40) * 1.8 - 40;
   if (edit == EDIT_TARGET)
   {
      select_icon_plot (icon_select2);
      message = "Target temp";
   }
   temp_colour (t);
   if (isnan (t) || t <= -10 || t >= 100)
      gfx_7seg (GFX_7SEG_SMALL_DOT, 6, "--.-%c", fahrenheit ? 'F' : 'C');
   else
      gfx_7seg (GFX_7SEG_SMALL_DOT, 6, "%4.1f%c", t, fahrenheit ? 'F' : 'C');
}

void
show_mode (void)
{
   if (nomode)
      return;
   if (edit == EDIT_MODE)
   {
      select_icon_plot (icon_select);
      message = "Mode:";        // TODO
   }
   if (b.away && !b.manual && !b.manualon)
      icon_plot (icon_modeaway);
   else if (edit != EDIT_MODE && !(b.manual ? b.manualon : b.poweron))
      icon_plot (icon_modeoff);
   else
      icon_plot (icon_mode[acmode]);
}

void
show_fan (void)
{
   if (nofan)
      return;
   if (edit == EDIT_FAN)
   {
      select_icon_plot (icon_select);
      message = "Fan:";         // TODO
   }
   icon_plot (icon_fans5[acfan]);       // TODO 3 or 5 levels
}

void
show_co2 (uint16_t co2)
{
   if (noco2)
      return;
   co2_colour (co2);
   if (co2 < 400 || co2 > 10000)
      gfx_7seg (GFX_7SEG_SMALL_DOT, 5, "----");
   else
      gfx_7seg (GFX_7SEG_SMALL_DOT, 5, "%4u", co2);
   if (!message && co2 >= co2red)
      message = "*High CO₂";
}

void
show_rh (uint8_t rh)
{
   if (norh)
      return;
   rh_colour (rh);
   // Assumes right align
   gfx_text (0, 4, "%%");
   if (!rh || rh >= 100)
      gfx_7seg (GFX_7SEG_SMALL_DOT, 5, "--");
   else
      gfx_7seg (GFX_7SEG_SMALL_DOT, 5, "%2u", rh);
   if (!message && rh >= rhred)
      message = "*High humidity";
}

void
show_start (void)
{
   if (edit == EDIT_START)
   {
      select_icon_plot (icon_select3);
      message = "Start time";
   }
   gfx_foreground (0xFFFFFF);
   gfx_7seg (GFX_7SEG_SMALL_DOT, 4, "%02u:%02u", acstart / 100, acstart % 100);
}

void
show_stop (void)
{
   if (edit == EDIT_STOP)
   {
      select_icon_plot (icon_select3);
      message = "Stop time";
   }
   gfx_foreground (0xFFFFFF);
   gfx_7seg (GFX_7SEG_SMALL_DOT, 4, "%02u:%02u", acstop / 100, acstop % 100);
}

void
show_clock (struct tm *t)
{
   gfx_foreground (0xFFFFFF);
   gfx_7seg (0, 5, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
}

void
app_main ()
{
   epd_mutex = xSemaphoreCreateMutex ();
   xSemaphoreGive (epd_mutex);
   revk_boot (&app_callback);
   revk_start ();

   // Web interface
   httpd_config_t config = HTTPD_DEFAULT_CONFIG ();
   config.stack_size += 1024 * 4;
   config.lru_purge_enable = true;
   config.max_uri_handlers = 6 + revk_num_web_handlers ();
   if (!httpd_start (&webserver, &config))
   {
      register_get_uri ("/", web_root);
#ifdef	CONFIG_LWPNG_ENCODE
      register_get_uri ("/frame.png", web_frame);
#endif
      register_get_uri ("/apple-touch-icon.png", web_apple);
      register_get_uri ("/favicon.ico", web_favicon);
      register_get_uri ("/icon.png", web_icon); // expects ?name
      register_get_uri ("/btn", web_btn);       // expects ?D
      revk_web_settings_add (webserver);
   }
   if (sda.set && scl.set)
      revk_task ("i2c", i2c_task, NULL, 10);
   if (btnn.set || btns.set || btne.set || btnw.set || btnp.set)
      revk_task ("btn", btn_task, NULL, 10);
   if (ds18b20.set)
      revk_task ("18b20", ds18b20_task, NULL, 10);
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
   xSemaphoreTake (epd_mutex, portMAX_DELAY);
   revk_gfx_init (5);
   xSemaphoreGive (epd_mutex);
#endif
   int8_t lastsec = -1;
   while (!revk_shutting_down (NULL))
   {
      message = NULL;           // set by Show functions
      struct tm t;
      uint32_t up = uptime ();
      time_t now = time (0);
      localtime_r (&now, &t);
      if (!b.display && (now & 0x7F) == lastsec)
         continue;
      if (wake && wake < up)
      {
         wake = 0;
         edit = 0;
      }
      b.display = 0;
      // TODO do we mutex this
      // TODO override
      // Work out current values to show / test
      float c = NAN;
      switch (tempref)
      {
      case REVK_SETTINGS_TEMPREF_MCP9808:
         c = mcp9808.c;
         break;
      case REVK_SETTINGS_TEMPREF_TMP1075:
         c = tmp1075.c;
         break;
      case REVK_SETTINGS_TEMPREF_SCD41:
         c = scd41.c;
         break;
      case REVK_SETTINGS_TEMPREF_GZP6816D:
         c = gzp6816d.c;
         break;
      case REVK_SETTINGS_TEMPREF_BLE:
         break;                 // TODO
      case REVK_SETTINGS_TEMPREF_DS18B200:
         if (ds18b20_num >= 1)
            c = ds18b20s[0].c;
         break;
      case REVK_SETTINGS_TEMPREF_DS18B201:
         if (ds18b20_num >= 2)
            c = ds18b20s[1].c;
         break;
      }
      if (isnan (c))
      {                         // Auto
         // TODO BLE first
         if (isnan (c) && ds18b20_num)
            c = ds18b20s[0].c;
         if (isnan (c) && scd41.ok && !isnan (scd41.c))
            c = scd41.c;
         if (isnan (c) && tmp1075.ok)
            c = tmp1075.c;
         if (isnan (c) && mcp9808.ok)
            c = mcp9808.c;
         if (isnan (c) && gzp6816d.ok)
            c = gzp6816d.c;
      }
      uint16_t co2 = 0;
      uint8_t rh = 0;
      // TODO BLE RH
      if (scd41.ok)
      {
         co2 = scd41.ppm;
         rh = scd41.rh;
      } else if (t6793.ok)
         co2 = t6793.ppm;
      if (lastsec != (now & 0x7F))
      {                         // Once per second
         if (veml6040.ok && veml6040dark)
            b.night = ((veml6040.w < (float) veml6040dark / veml6040dark_scale) ? 1 : 0);
         revk_gpio_set (gfxbl, wake || !b.night ? 1 : 0);
         // TODO rad control
         // TODO fan control
      }
      lastsec = (now & 0x7F);
      // TODO override
#ifndef CONFIG_GFX_BUILD_SUFFIX_GFXNONE
      epd_lock ();
      gfx_clear (0);
      // Main temp display
      gfx_pos (gfx_width () - 1, 0, GFX_R);
      show_temp (c);
      if (gfx_width () < gfx_height ())
      {
         gfx_pos (2, 130, GFX_L | GFX_T | GFX_H);
         if (edit == EDIT_START || edit == EDIT_STOP)
         {
            show_start ();
            gfx_pos (gfx_width () - 3, gfx_y (), GFX_R | GFX_T | GFX_H);
            show_stop ();
         } else
         {
            show_target ((float) actarget / actarget_scale);
            gfx_pos (gfx_width () - 3, gfx_y (), GFX_R | GFX_T | GFX_H);
            show_fan ();
            gfx_pos (gfx_x () - 10, gfx_y (), GFX_R | GFX_T | GFX_H);
            show_mode ();
         }
         gfx_pos (0, 210, GFX_L | GFX_T | GFX_H);
         show_co2 (co2);
         gfx_pos (gfx_width () - 1, gfx_y (), GFX_R | GFX_T | GFX_H);
         show_rh (rh);
         gfx_pos (gfx_width () / 2, gfx_height () - 1, GFX_C | GFX_B);
         if (message)
         {
            const char *m = message;
            if (*m == '*')
            {
               m++;
               gfx_foreground (0xFF0000);
            } else
               gfx_foreground (0xFFFFFF);
            gfx_text (1, 3, "%s", m);
         } else
            show_clock (&t);
      } else
      {                         // Landscape
         // TODO
      }
      epd_unlock ();
#endif
      usleep (10000);
   }
   b.die = 1;
   epd_lock ();
   gfx_clear (0);
   gfx_text (0, 3, "Reboot");
   epd_unlock ();
}
