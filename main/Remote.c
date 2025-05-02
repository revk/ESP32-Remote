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
   uint8_t fan:1;               // Fan on
   uint8_t rad:1;               // Rad on
   uint8_t connect:1;           // MQTT connect
   uint8_t faikinheat:1;        // Faikin auto is heating
} b = { 0 };

struct
{                               // For HA, etc
   uint16_t co2;
   uint8_t rh;
   uint8_t tempfrom;
   float temp;
   float target;
   float tmin;
   float tmax;
   float lux;
   float pressure;
   uint8_t extfan:1;
   uint8_t extrad:1;
   uint8_t away:1;
   uint8_t poweron:1;
   uint8_t mode:3;
   uint8_t fan:3;
} data = { 0 };

httpd_handle_t webserver = NULL;
SemaphoreHandle_t lcd_mutex = NULL;
SemaphoreHandle_t data_mutex = NULL;
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
bleenv_t *bleidtemp = NULL;
bleenv_t *bleidfaikin = NULL;

const uint8_t icon_mode[] = { icon_unknown, icon_modeauto, icon_modefan, icon_modedry, icon_modecool, icon_modeheat, icon_unknown, icon_modefaikin };   // order same as acmode
const char *const icon_mode_message[] =
   { NULL, "Mode: Auto", "Mode: Fan", "Mode: Dry", "Mode: Cool", "Mode: Heat", NULL, "Mode: Faikin" };

const uint8_t icon_fans5[] = { icon_unknown, icon_fanauto, icon_fan1, icon_fan2, icon_fan3, icon_fan4, icon_fan5, icon_fanquiet };      // order same as acfan
const char *const icon_fan5_message[] = { NULL, "Fan: Auto", "Fan: 1", "Fan: 2", "Fan: 3", "Fan: 4", "Fan: 5", "Fan: Quiet" };

const uint8_t icon_fans3[] = { icon_unknown, icon_fanauto, icon_fanlow, 0xFF, icon_fanmid, 0xFF, icon_fanhigh, icon_fanquiet }; // order same as acfan
const char *const icon_fan3_message[] = { NULL, NULL, "Fan: Low", NULL, "Fan: Mid", NULL, "Fan: High", NULL };

static inline float
T (float C)
{                               // Celsius to temp
   if (fahrenheit && !isnan (C))
      return (C + 40) * 1.8 - 40;
   return C;
}

static inline float
C (float T)
{                               // Temp to Celsuis
   if (fahrenheit && !isnan (T))
      return (T + 40) / 1.8 - 40;
   return T;
}

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
   float t;
} tmp1075 = { 0 };

struct
{
   uint8_t found:1;
   uint8_t ok:1;
   float t;
} mcp9808 = { 0 };

struct
{
   uint8_t found:1;
   uint8_t ok:1;
   float hpa;
   float t;
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
   float t;
   float rh;
} scd41 = { 0 };

struct
{
   uint8_t ok:1;
   ds18b20_device_handle_t handle;
   uint64_t serial;
   float t;
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
   if (!strcmp (suffix, "connect"))
      b.connect = 1;
   if (!strcmp (suffix, "away"))
   {
      if (*value == '0' || *value == 'f')
         suffix = "home";
      else
      {
         b.away = 1;
         b.manual = 0;
      }
   }
   if (!strcmp (suffix, "home"))
   {
      b.away = 0;
   }
   if (!strcmp (suffix, "power"))
      suffix = ((*value == '1' || *value == 't') ? "on" : "off");
   if (!strcmp (suffix, "on"))
   {
      b.away = 0;
      b.manualon = 1;
      b.manual = 1;
   }
   if (!strcmp (suffix, "off"))
   {
      b.manualon = 0;
      b.manual = 1;
   }
   if (!strcmp (suffix, "dark"))
   {
      b.night = 1;
      revk_gpio_set (gfxbl, 0);
   }
   if (!strcmp (suffix, "light"))
   {
      b.night = 0;
      revk_gpio_set (gfxbl, 1);
   }
   return NULL;
}

void
revk_state_extra (jo_t j)
{
   xSemaphoreTake (data_mutex, portMAX_DELAY);
   void add_enum (const char *tag, int i, const char *e)
   {
      while (i--)
      {
         while (*e && *e != ',')
            e++;
         if (*e)
            e++;
      }
      int l = 0;
      while (e[l] && e[l] != ',')
         l++;
      jo_stringf (j, tag, "%.*s", l, e);
   }
   if (data.co2)
      jo_int (j, "co2", data.co2);
   if (data.rh)
      jo_int (j, "rh", data.rh);
   if (!isnan (data.lux))
      jo_litf (j, "lux", "%.4f", data.lux);
   if (!isnan (data.pressure))
      jo_litf (j, "pressure", "%.2f", data.pressure);
   if (!isnan (data.temp))
   {
      jo_litf (j, "temp", "%.2f", data.temp);
      if (data.tempfrom)
         add_enum ("source", data.tempfrom, REVK_SETTINGS_TEMPREF_ENUMS);
   }
   if (!isnan (data.tmin) && !isnan (data.tmax) && data.tmin == data.tmax)
      jo_litf (j, "target", "%.2f", data.tmin);
   else
   {
      if (!isnan (data.tmin))
         jo_litf (j, "min", "%.2f", data.tmin);
      if (!isnan (data.tmax))
         jo_litf (j, "max", "%.2f", data.tmax);
   }
   if (data.mode)
      add_enum ("mode", data.mode, REVK_SETTINGS_ACMODE_ENUMS);
   if (data.fan)
      add_enum ("fan", data.fan, REVK_SETTINGS_ACFAN_ENUMS);
   jo_bool (j, "power", data.poweron);
   jo_bool (j, "away", data.away);
   jo_bool (j, "extfan", data.extfan);
   jo_bool (j, "extrad", data.extrad);
   xSemaphoreGive (data_mutex);
}

static void
settings_blefaikin (httpd_req_t * req)
{
   revk_web_send (req, "<tr><td>Faikin</td><td>"        //
                  "<select name=blefaikin>");
   revk_web_send (req, "<option value=\"\">-- None --");
   char found = 0;
   for (bleenv_t * e = bleenv; e; e = e->next)
      if (e->faikinset)
      {
         revk_web_send (req, "<option value=\"%s\"", e->name);
         if (*blefaikin && !strcmp (blefaikin, e->name))
         {
            revk_web_send (req, " selected");
            found = 1;
         }
         revk_web_send (req, ">%s", e->name);
         if (!e->missing && e->rssi)
            revk_web_send (req, " %ddB", e->rssi);
      }
   if (!found && *blefaikin)
      revk_web_send (req, "<option selected value=\"%s\">%s", blefaikin, blefaikin);
   revk_web_send (req, "</select>");
   revk_web_send (req, "</td><td>Air conditioner Faikin</td></tr>");
}

static void
settings_bletemp (httpd_req_t * req)
{
   revk_web_send (req, "<tr><td>BLE</td><td>"   //
                  "<select name=bletemp>");
   revk_web_send (req, "<option value=\"\">-- None --");
   char found = 0;
   for (bleenv_t * e = bleenv; e; e = e->next)
      if (!e->faikinset)
      {
         revk_web_send (req, "<option value=\"%s\"", e->name);
         if (*bletemp && !strcmp (bletemp, e->name))
         {
            revk_web_send (req, " selected");
            found = 1;
         }
         revk_web_send (req, ">%s", e->name);
         if (!e->missing && e->rssi)
            revk_web_send (req, " %ddB", e->rssi);
      }
   if (!found && !*bletemp)
      revk_web_send (req, "<option selected value=\"%s\">%s", bletemp, bletemp);
   revk_web_send (req, "</select>");
   revk_web_send (req, "</td><td>External BLE temperature reference</td></tr>");
}

void
revk_web_extra (httpd_req_t * req, int page)
{
   revk_web_setting_title (req, "Controls");
   settings_blefaikin (req);
   revk_web_setting (req, "Target", "actarget");
   revk_web_setting (req, "±", "tempmargin");
   revk_web_setting (req, "Mode", "acmode");
   revk_web_setting (req, "Fan", "acfan");
   revk_web_setting (req, "Start", "acstart");
   revk_web_setting (req, "Stop", "acstop");
   if (veml6040.found)
   {
      revk_web_setting_title (req, "Backlight");
      revk_web_setting_info (req, "The display can go dark below specified light level, 0 to disable. Current light level %.2f",
                             veml6040.w);
      revk_web_setting (req, "Auto dark", "veml6404dark");
   }
   revk_web_setting_title (req, "Temperature");
   revk_web_setting_info (req, "Temperature can be read from (in priority order)...<ul>"        //
                          "<li>External Bluetooth sensor (BLE)</li>"    //
                          "<li>Connected temperature probe (DS18B20)</li>"      //
                          "<li>Internal CO₂ sensor (SCD41)</li>"      //
                          "<li>Internal temperature sensor (%s)</li>"   //
                          "<li>Aircon temperature via Faikin (AC)</li>" //
                          "<li>Internal pressure sensor (GZP6816D), not recommended</li>"       //
                          "</ul>"       //
                          "Note that internal sensors may need adjustment, depending on orientation and if in a case, etc.",    //
                          tmp1075.found ? "TMP1075" : mcp9808.found ? "MCP9808" : "TMP1075/MCP98708"    //
      );
   revk_web_setting (req, "Temp", "tempref");
   settings_bletemp (req);
   if (tempref == REVK_SETTINGS_TEMPREF_GZP6816D && gzp6816d.found)
      revk_web_setting (req, "Temp offset", "gzp6816ddt");
   else if (tempref == REVK_SETTINGS_TEMPREF_SCD41 && scd41.found)
      revk_web_setting (req, "Temp offset", "scd41dt");
   else if (tempref == REVK_SETTINGS_TEMPREF_AC && bleidfaikin && !bleidfaikin->missing)
      revk_web_setting (req, "Temp offset", "acdt");
   else if (tmp1075.found)
      revk_web_setting (req, "Temp offset", "tmp1075dt");
   else if (mcp9808.found)
      revk_web_setting (req, "Temp offset", "mcp9808dt");
   revk_web_setting (req, "Fahrenheit", "fahrenheit");
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
   xSemaphoreTake (lcd_mutex, portMAX_DELAY);
   gfx_lock ();
}

void
epd_unlock (void)
{
   gfx_unlock ();
   xSemaphoreGive (lcd_mutex);
}

#ifdef	CONFIG_LWPNG_ENCODE
static esp_err_t
web_frame (httpd_req_t * req)
{
   xSemaphoreTake (lcd_mutex, portMAX_DELAY);
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
   xSemaphoreGive (lcd_mutex);
   return ESP_OK;
}
#endif

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


static esp_err_t
i2c_write_16hl (uint8_t addr, uint8_t cmd, uint16_t val)
{
   i2c_cmd_handle_t t = i2c_cmd_link_create ();
   i2c_master_start (t);
   i2c_master_write_byte (t, (addr << 1) | I2C_MASTER_WRITE, true);
   i2c_master_write_byte (t, cmd, true);
   i2c_master_write_byte (t, val >> 8, true);
   i2c_master_write_byte (t, val & 0xFF, true);
   i2c_master_stop (t);
   esp_err_t err = i2c_master_cmd_begin (i2cport, t, 10 / portTICK_PERIOD_MS);
   i2c_cmd_link_delete (t);
   return err;
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
      scd41.t = NAN;
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
      if (i2c_read_16hl (tmp1075i2c, 0x0F) != 0x7500 || i2c_write_16hl (tmp1075i2c, 1, 0x60FF))
         fail (tmp1075i2c, "TMP1075");
      else
         tmp1075.found = 1;
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
            mcp9808.t = NAN;
         } else
         {
            int16_t t = (v << 3),
               a = (last1 + last2 + last3 + t) / 4;
            last3 = last2;
            last2 = last1;
            last1 = t;
            mcp9808.t = T ((float) a / 128) + (float) mcp9808dt / mcp9808dt_scale;
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
            gzp6816d.t = T ((float) ((t1 << 8) | t2) * 190 / 65536 - 40) + (float) gzp6816ddt / gzp6816ddt_scale;
            gzp6816d.hpa = (float) 800 *(((p1 << 16) | (p2 << 8) | p3) - 1677722) / 13421772 + 300;
            gzp6816d.ok = 1;
         } else
         {
            gzp6816d.ok = 0;
            gzp6816d.t = NAN;
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
               scd41.t = T (-45.0 + 175.0 * (float) ((buf[3] << 8) + buf[4]) / 65536.0) + (float) scd41dt / scd41dt_scale;
            scd41.rh = 100.0 * (float) ((buf[6] << 8) + buf[7]) / 65536.0;
            scd41.ok = 1;
         } else if (err)
         {
            scd41.ok = 0;
            scd41.t = NAN;
            scd41.rh = 0;
         }
      }
      if (tmp1075.found)
      {
         int32_t v = i2c_read_16hl (tmp1075i2c, 0);
         if (v < 0)
         {
            tmp1075.ok = 0;
            tmp1075.t = NAN;
         } else
         {
            tmp1075.t = T (((float) (int16_t) v) / 256) + (float) tmp1075dt / tmp1075dt_scale;;
            tmp1075.ok = 1;
         }
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
         ds18b20s[ds18b20_num].t = NAN;
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
         float c;
         REVK_ERR_CHECK (ds18b20_trigger_temperature_conversion (ds18b20s[i].handle));
         REVK_ERR_CHECK (ds18b20_get_temperature (ds18b20s[i].handle, &c));
         ds18b20s[i].t = T (c);
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
         uint8_t m = acmode;
         do
         {
            m += d;
            if (m < REVK_SETTINGS_ACMODE_AUTO)
               m = REVK_SETTINGS_ACMODE_FAIKIN;
            else if (m > REVK_SETTINGS_ACMODE_FAIKIN)
               m = REVK_SETTINGS_ACMODE_AUTO;
         }
         while (!icon_mode_message[m]);
         jo_litf (j, "acmode", "%d", m);
      }
      break;
   case EDIT_FAN:
      {
         uint8_t f = acfan;
         do
         {
            f += d;
            if (f < REVK_SETTINGS_ACFAN_AUTO)
               f = REVK_SETTINGS_ACFAN_QUIET;
            else if (f > REVK_SETTINGS_ACFAN_QUIET)
               f = REVK_SETTINGS_ACFAN_AUTO;
         }
         while (!(fan3 ? icon_fan3_message : icon_fan5_message)[f]);
         jo_litf (j, "acfan", "%d", f);
      }
      break;
   case EDIT_START:
      {
         int16_t t = acstart / 100 * 60 + acstart % 100;
         t = t / timestep * timestep;
         t += d * timestep;
         if (t < 0)
            t = 24 * 60 - timestep;
         else if (t >= 24 * 60)
            t = 0;
         jo_stringf (j, "acstart", "%04d", t / 60 * 100 + t % 60);
      }
      break;
   case EDIT_STOP:
      {
         int16_t t = acstop / 100 * 60 + acstop % 100;
         t = t / timestep * timestep;
         t += d * timestep;
         if (t < 0)
            t = 24 * 60 - timestep;
         else if (t >= 24 * 60)
            t = 0;
         jo_stringf (j, "acstop", "%04d", t / 60 * 100 + t % 60);
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
   b.manualon = b.poweron = 0;
   b.manual = 1;
}

void
btn (char c)
{
   switch (c)
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
}

void
btn_task (void *x)
{
   for (int i = 0; i < 5; i++)
      revk_gpio_input (btns[i]);
   uint8_t t[5] = { 0 };
   const char b[] = "NSEWP";
   while (1)
   {
      for (int i = 0; i < 5; i++)
      {
         if (revk_gpio_get (btns[i]))
         {
            if (t[i] < 255)
               t[i]++;
            if (t[i] == 2)
               btn (b[i]);
            else if (i == 4 && t[i] == 200)
               btn ('H');
            else if (i < 4 && t[i] == 100)
            {
               btn (b[i]);
               t[i] = 50;
            }
         } else
            t[i] = 0;
      }
      usleep (10000);
   }
}


static esp_err_t
web_btn (httpd_req_t * req)
{
   char *name = strrchr (req->uri, '?');
   if (name)
      btn (name[1]);
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
select_icon_plot (uint8_t i, int8_t dx, int8_t dy)
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
   plot_t settings = { ox + dx, oy + dy };
   lwpng_decode_t *p = lwpng_decode (&settings, NULL, &pixel, &my_alloc, &my_free, NULL);
   lwpng_data (p, icons[i].end - icons[i].start, icons[i].start);
   e = lwpng_decoded (&p);
   gfx_pos (wx, wy, wa);
#endif
}

void
temp_colour (float t)
{
   gfx_colour_t c = 0x888888;
   if (!isnan (t))
   {
      if (t < tempblue - 0.5)
         c = 0x0000FF;
      else if (t < tempblue)
         c = 0x0000FF + ((uint8_t) (255 * 2 * (t - tempblue + 0.5)) << 8);
      else if (t < tempblue + 0.5)
         c = 0x00FF00 + (uint8_t) (255 - 255 * 2 * (t - tempblue));
      else if (t < tempred - 0.5)
         c = 0x00FF00;
      else if (t < tempred)
         c = 0x00FF00 + ((uint8_t) (255 * 2 * (t - tempred + 0.5)) << 16);
      else if (t < tempred + 0.5)
         c = 0xFF0000 + ((uint8_t) (255 - 255 * 2 * (t - tempred)) << 8);
      else
         c = 0xFF0000;
   }
   gfx_background (c);
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
   gfx_background (c);
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
   gfx_background (c);
   gfx_foreground (c);
}

void
send_fan (uint8_t fan)
{
   b.fan = fan;
   if (*mqttfan)
      revk_mqtt_send_raw (mqttfan, 1, fan ? "1" : "0", 1);
}

void
send_rad (uint8_t rad)
{
   b.rad = rad;
   if (*mqttrad)
      revk_mqtt_send_raw (mqttrad, 1, rad ? "1" : "0", 1);
}

void
show_temp (float t)
{                               // Show current temp
   temp_colour (t);
   if (isnan (t) || t <= -100 || t >= 1000)
      gfx_7seg (GFX_7SEG_SMALL_DOT, 11, "---.-%c", fahrenheit ? 'F' : 'C');
   else
      gfx_7seg (GFX_7SEG_SMALL_DOT, 11, "%5.1f%c", t, fahrenheit ? 'F' : 'C');
}

void
show_target (float t)
{                               // Show target temp
   if (edit == EDIT_TARGET)
   {
      select_icon_plot (icon_select2, -2, -2);
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
      select_icon_plot (icon_select, 0, 0);
      message = icon_mode_message[acmode];
   }
   if (edit != EDIT_MODE && b.away && !b.manual && !b.manualon)
      icon_plot (icon_modeaway);
   else if (edit != EDIT_MODE && !(b.manual ? b.manualon : b.poweron))
      icon_plot (icon_modeoff);
   else if (acmode == REVK_SETTINGS_ACMODE_FAIKIN)
      icon_plot (b.faikinheat ? icon_modefaikinheat : icon_modefaikincool);
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
      select_icon_plot (icon_select, 0, 0);
      message = (fan3 ? icon_fan3_message : icon_fan5_message)[acfan];
   }
   icon_plot ((fan3 ? icon_fans3 : icon_fans5)[acfan]);
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
   if (gfx_a () | GFX_R)
      gfx_text (0, 4, "%%");
   if (!rh || rh >= 100)
      gfx_7seg (GFX_7SEG_SMALL_DOT, 5, "--");
   else
      gfx_7seg (GFX_7SEG_SMALL_DOT, 5, "%2u", rh);
   if (!(gfx_a () | GFX_R))
      gfx_text (0, 4, "%%");
   if (!message && rh >= rhred)
      message = "*High humidity";
}

void
show_start (void)
{
   if (edit == EDIT_START)
   {
      select_icon_plot (icon_select3, -2, -2);
      message = "Start time";
   }
   gfx_background (0xFFFFFF);
   gfx_foreground (0xFFFFFF);
   gfx_7seg (GFX_7SEG_SMALL_DOT, 4, "%02u:%02u", acstart / 100, acstart % 100);
}

void
show_stop (void)
{
   if (edit == EDIT_STOP)
   {
      select_icon_plot (icon_select3, 2, -2);
      message = "Stop time";
   }
   gfx_background (0xFFFFFF);
   gfx_foreground (0xFFFFFF);
   gfx_7seg (GFX_7SEG_SMALL_DOT, 4, "%02u:%02u", acstop / 100, acstop % 100);
}

void
show_clock (struct tm *t)
{
   gfx_background (0xFFFFFF);
   gfx_foreground (0xFFFFFF);
   gfx_7seg (0, 5, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
}

void
ha_config (void)
{
 ha_config_sensor ("co2", name: "CO₂", type: "carbon_dioxide", unit: "ppm", field: "co2", delete:!scd41.found && !t6793.
                     found);
 ha_config_sensor ("temp", name: "Temp", type: "temperature", unit: "C", field:"temp");
 ha_config_sensor ("hum", name: "Humidity", type: "humidity", unit: "%", field: "rh", delete:!scd41.found);
 ha_config_sensor ("lux", name: "Lux", type: "illuminance", unit: "lx", field: "lux", delete:!veml6040.found);
 ha_config_sensor ("pressure", name: "Pressure", type: "pressure", unit: "mbar", field: "pressure", delete:!gzp6816d.found);
}

void
app_main ()
{
   lcd_mutex = xSemaphoreCreateMutex ();
   xSemaphoreGive (lcd_mutex);
   data_mutex = xSemaphoreCreateMutex ();
   xSemaphoreGive (data_mutex);
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
   if (btns[0].set || btns[1].set || btns[2].set || btns[3].set || btns[4].set)
      revk_task ("btn", btn_task, NULL, 10);
   if (ds18b20.set)
      revk_task ("18b20", ds18b20_task, NULL, 10);
   bleenv_run ();
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
   xSemaphoreTake (lcd_mutex, portMAX_DELAY);
   revk_gfx_init (5);
   xSemaphoreGive (lcd_mutex);
#endif
   int8_t lastsec = -1;
   int8_t lastmin = -1;
   int8_t lastreport = -1;
   float blet = NAN;
   uint8_t blerh = 0;
   uint8_t blebat = 0;
   uint8_t change = 0;
   while (!revk_shutting_down (NULL))
   {
      message = NULL;           // set by Show functions
      struct tm tm;
      uint32_t up = uptime ();
      time_t now = time (0);
      localtime_r (&now, &tm);
      if (!b.display && tm.tm_sec == lastsec)
         continue;
      b.display = 0;
      if (tm.tm_sec != lastsec)
      {                         // Once per second
         if (b.ha)
         {
            b.ha = 0;
            ha_config ();
         }
         if (b.connect)
         {
            b.connect = 0;
            send_rad (b.rad);
            send_fan (b.fan);
         }
         if (veml6040.ok && veml6040dark)
            b.night = ((veml6040.w < (float) veml6040dark / veml6040dark_scale) ? 1 : 0);
         revk_gpio_set (gfxbl, wake || !b.night ? 1 : 0);
         if (wake && wake < up)
         {
            wake = 0;
            edit = 0;
         }
         bleenv_expire (120);
         if (!bleidtemp || strcmp (bleidtemp->name, bletemp))
         {
            bleidtemp = NULL;
            bleenv_clean ();
            for (bleenv_t * e = bleenv; e; e = e->next)
               if (!strcmp (e->name, bletemp))
               {
                  bleidtemp = e;
                  break;
               }
         }
         if (!bleidfaikin || strcmp (bleidfaikin->name, blefaikin))
         {
            bleidfaikin = NULL;
            bleenv_clean ();
            for (bleenv_t * e = bleenv; e; e = e->next)
               if (!strcmp (e->name, blefaikin))
               {
                  bleidfaikin = e;
                  break;
               }
         }
         if (bleidtemp && !bleidtemp->missing)
         {                      // Use temp
            if (bleidtemp->tempset)
               blet = T ((float) bleidtemp->temp / 100.0);
            if (bleidtemp->humset)
               blerh = bleidtemp->hum / 100;
            if (bleidtemp->batset)
               blebat = bleidtemp->bat;
         } else
         {
            blet = NAN;
            blerh = 0;
         }
         if (bleidfaikin && (!bleidfaikin->faikinset || bleidfaikin->missing) && !message)
            message = "*Faikin missing";
      }
      // Manual
      if (b.manual && b.manualon == b.poweron)
         b.manual = 0;
      // Work out current values to show / test
      uint8_t tempfrom = tempref;
      float t = NAN;
      switch (tempref)
      {
      case REVK_SETTINGS_TEMPREF_MCP9808:
         t = mcp9808.t;
         break;
      case REVK_SETTINGS_TEMPREF_TMP1075:
         t = tmp1075.t;
         break;
      case REVK_SETTINGS_TEMPREF_SCD41:
         t = scd41.t;
         break;
      case REVK_SETTINGS_TEMPREF_GZP6816D:
         t = gzp6816d.t;
         break;
      case REVK_SETTINGS_TEMPREF_BLE:
         t = blet;
         break;
      case REVK_SETTINGS_TEMPREF_AC:
         if (bleidfaikin && !bleidfaikin->missing && bleidfaikin->faikinset)
            t = T ((float) bleidfaikin->temp / 100);
         break;
      case REVK_SETTINGS_TEMPREF_DS18B200:
         if (ds18b20_num >= 1)
            t = ds18b20s[0].t;
         break;
      case REVK_SETTINGS_TEMPREF_DS18B201:
         if (ds18b20_num >= 2)
            t = ds18b20s[1].t;
         break;
      }
      if (isnan (t) && !isnan (t = blet))
         tempfrom = REVK_SETTINGS_TEMPREF_BLE;
      if (isnan (t) && ds18b20_num && !isnan (t = ds18b20s[0].t))
         tempfrom = REVK_SETTINGS_TEMPREF_DS18B200;
      if (isnan (t) && scd41.ok && !isnan (t = scd41.t))
         tempfrom = REVK_SETTINGS_TEMPREF_SCD41;
      if (isnan (t) && tmp1075.ok && !isnan (t = tmp1075.t))
         tempfrom = REVK_SETTINGS_TEMPREF_TMP1075;
      if (isnan (t) && mcp9808.ok && !isnan (t = mcp9808.t))
         tempfrom = REVK_SETTINGS_TEMPREF_MCP9808;
      if (isnan (t) && bleidfaikin && !bleidfaikin->missing && bleidfaikin->faikinset)
      {
         t = T ((float) bleidfaikin->temp / 100);
         tempfrom = REVK_SETTINGS_TEMPREF_AC;
      }
      if (isnan (t) && gzp6816d.ok && !isnan (t = gzp6816d.t))
         tempfrom = REVK_SETTINGS_TEMPREF_GZP6816D;
      uint16_t co2 = 0;
      uint8_t rh = 0;
      if (blerh)
         rh = blerh;
      if (scd41.ok)
      {
         co2 = scd41.ppm;
         if (!rh)
            rh = scd41.rh;
      } else if (t6793.ok)
         co2 = t6793.ppm;
      if (!message && blebat && blebat < 10)
         message = "*Low BLE bat";
      if (!b.fan && ((co2red && co2 >= co2red) || (rhred && rh >= rhred)))
      {
         if (!b.fan)
            send_fan (1);
      } else if (b.fan && (!co2green || co2 < co2green) && (rhgreen || rh <= rhgreen))
      {
         if (b.fan)
            send_fan (0);
      }
      // power set based on time and manual
      int16_t early = 0;
      if (acstart != acstop)
      {
         uint16_t start = acstart / 100 * 60 + acstart % 100;
         uint16_t stop = acstart / 100 * 60 + acstart % 100;
         uint16_t min = tm.tm_hour * 60 + tm.tm_min;
         if (b.away)
            b.poweron = 0;
         else if (start < stop)
         {
            if (min >= start && now < stop)
               b.poweron = 1;
            else
               b.poweron = 0;
         } else
         {
            if (min >= start || now < stop)
               b.poweron = 1;
            else
               b.poweron = 0;
         }
         if (!b.poweron)
         {
            early = start - min;
            if (early < 0)
               early += 24 * 60;
         }
      }
      float targetlow = (float) actarget / actarget_scale - (float) tempmargin / tempmargin_scale;
      float targethigh = (float) actarget / actarget_scale + (float) tempmargin / tempmargin_scale;
      if (early)
      {
         if (earlyheat)
            targetlow -= (float) earlyheat / earlyheat_scale * early / 60;
         if (earlycool)
            targethigh += (float) earlycool / earlycool_scale * early / 60;
      } else if (b.away)
      {
         targetlow = (float) tempmin / tempmin_scale;
         targethigh = (float) tempmax / tempmin_scale;
      }
      if (targetlow < (float) tempmin / tempmin_scale)
         targetlow = (float) tempmin / tempmin_scale;
      if (targetlow > (float) tempmax / tempmax_scale)
         targetlow = (float) tempmax / tempmax_scale;
      if (targethigh < (float) tempmin / tempmin_scale)
         targethigh = (float) tempmin / tempmin_scale;
      if (targethigh > (float) tempmax / tempmax_scale)
         targethigh = (float) tempmax / tempmax_scale;
      if (acmode != REVK_SETTINGS_ACMODE_FAIKIN)
      {                         // Direct control not Faikin auto
         if (!b.poweron && early && (t < targetlow || t > targethigh))
         {                      // Early on
            ESP_LOGE (TAG, "Early on");
            b.poweron = 1;
         }
         targetlow = targethigh = (float) actarget / actarget_scale;
      }
      if (b.poweron == b.manualon)
         b.manual = 0;
      // Limits
      if (tm.tm_min != lastmin)
      {
         if (t < targetlow)
         {
            if (!b.rad)
               send_rad (1);
         } else
         {
            if (b.rad)
               send_rad (0);
         }
      }
      xSemaphoreTake (data_mutex, portMAX_DELAY);
      if (data.poweron != (b.manual ? b.manualon : b.poweron) ||
          data.mode != acmode ||
          data.fan != acfan || (acmode != REVK_SETTINGS_ACMODE_FAIKIN && data.target != (float) actarget / actarget_scale))
         change = 10;           // Delay incoming updates
      data.away = b.away;
      data.poweron = (b.manual ? b.manualon : b.poweron);
      data.extrad = b.rad;
      data.extfan = b.fan;
      data.mode = acmode;
      data.fan = acfan;
      data.co2 = co2;
      data.rh = rh;
      data.tempfrom = tempfrom;
      data.temp = t;
      data.tmin = targetlow;
      data.tmax = targethigh;
      data.target = (float) actarget / actarget_scale;
      data.lux = (veml6040.ok ? veml6040.w : NAN);
      data.pressure = (gzp6816d.ok ? gzp6816d.hpa : NAN);
      if (tm.tm_sec != lastsec && bleidfaikin && bleidfaikin->faikinset && !bleidfaikin->missing)
      {                         // From Faikin
         if (change)
            change--;
         if (!change)
         {                      // Update
            jo_t j = jo_object_alloc ();
            if (data.mode != REVK_SETTINGS_ACMODE_FAIKIN && bleidfaikin->power != data.poweron)
            {
               data.poweron = b.manualon = bleidfaikin->power;
               b.manual = 1;
            }
            if (bleidfaikin->fan != data.fan)
               jo_int (j, "acfan", data.fan = bleidfaikin->fan);
            if (bleidfaikin->mode != data.mode)
            {
               if (data.mode != REVK_SETTINGS_ACMODE_FAIKIN)
                  jo_int (j, "acmode", data.mode = bleidfaikin->mode);
               else
                  b.faikinheat = ((bleidfaikin->mode == REVK_SETTINGS_ACMODE_HEAT) ? 1 : 0);
            }
            float target = T ((float) (bleidfaikin->targetlow + bleidfaikin->targethigh) / 200);
            if (data.mode != REVK_SETTINGS_ACMODE_FAIKIN && data.target != target)
               jo_litf (j, "actarget", "%.1f", data.target = target);
            revk_setting (j);
            jo_free (&j);
         }
      }
      xSemaphoreGive (data_mutex);
      if (reporting && (int8_t) (now / reporting) != lastreport)
      {
         lastreport = now / reporting;
         revk_command ("status", NULL);
      }
      // BLE
      switch (bleadvert)
      {
      case REVK_SETTINGS_BLEADVERT_FAIKIN:
         bleenv_faikin (hostname, C (t), C (targetlow), C (targethigh), b.manual ? b.manualon : b.poweron, b.rad, acmode, acfan);
         break;
      case REVK_SETTINGS_BLEADVERT_BTHOME1:
         bleenv_bthome1 (hostname, C (t), rh, co2, veml6040.w);
         break;
      case REVK_SETTINGS_BLEADVERT_BTHOME2:
         bleenv_bthome2 (hostname, C (t), rh, co2, veml6040.w);
         break;
      }

      // TODO override message
#ifndef CONFIG_GFX_BUILD_SUFFIX_GFXNONE
      epd_lock ();
      gfx_clear (0);
      // Main temp display
      gfx_pos (gfx_width () - 1, 0, GFX_R);
      if (tempfrom == REVK_SETTINGS_TEMPREF_BLE)
         select_icon_plot (icon_bt, -15, 0);
      show_temp (t);
      if (gfx_width () < gfx_height ())
      {
         gfx_pos (2, 125, GFX_L | GFX_T | GFX_H);
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
         gfx_pos (0, 205, GFX_L | GFX_T | GFX_H);
         show_co2 (co2);
         gfx_pos (gfx_width () - 1, gfx_y (), GFX_R | GFX_T | GFX_H);
         show_rh (rh);
         if (blerh)
            icon_plot (icon_bt);
      } else
      {                         // Landscape
         gfx_pos (2, 2, GFX_T | GFX_L);
         show_target ((float) actarget / actarget_scale);
         gfx_pos (0, 64, GFX_T | GFX_L);
         show_mode ();
         gfx_pos (120, gfx_y (), GFX_T | GFX_R);
         show_fan ();
         if (edit == EDIT_START || edit == EDIT_STOP)
         {
            gfx_pos (2, 130, GFX_T | GFX_L);
            show_start ();
            gfx_pos (gfx_width () - 3, gfx_y (), GFX_T | GFX_R);
            show_stop ();
         } else
         {
            gfx_pos (gfx_width () - 3, 130, GFX_T | GFX_R);
            show_co2 (co2);
            gfx_pos (2, gfx_y (), GFX_T | GFX_L | GFX_H);
            show_rh (rh);
            if (blerh)
               icon_plot (icon_bt);
         }
      }
      gfx_pos (gfx_width () / 2, gfx_height () - 4, GFX_C | GFX_B);
      if (message)
      {
         const char *m = message;
         gfx_foreground (0xFFFFFF);
         if (*m == '*')
         {
            m++;
            gfx_background (0xFF0000);
         } else
            gfx_background (0);
         gfx_text (1, 3, "%s", m);
      } else
         show_clock (&tm);
      epd_unlock ();
#endif
      usleep (10000);
      lastsec = tm.tm_sec;
      lastmin = tm.tm_min;
   }

   b.die = 1;
   epd_lock ();
   gfx_clear (0);
   gfx_text (0, 5, "Reboot");
   epd_unlock ();
}
