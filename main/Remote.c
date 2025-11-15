// Faikout Remote

const char TAG[] = "Remote";

#include "revk.h"
#include <driver/i2c.h>
#include <hal/spi_types.h>
#include <math.h>
#include <esp_sntp.h>
#include "esp_http_server.h"
#include <driver/mcpwm_prelude.h>
#include <driver/i2s_pdm.h>
#include <onewire_bus.h>
#include <ds18b20.h>
#include "gfx.h"
#include "led_strip.h"
#include "bleenv.h"
#include "halib.h"
#include <lwpng.h>
#include <ir.h>
#include "icons.c"

struct
{
   uint8_t die:1;               // Shutting down
   uint8_t ha:1;                // HA update
   uint8_t display:1;           // Display update
   uint8_t night:1;             // Display dark
   uint8_t nightdark:1;         // Night set by light
   uint8_t nighttime:1;         // Night set by timer
   uint8_t fan:1;               // Fan on
   uint8_t rad:1;               // Rad on
   uint8_t connect:1;           // MQTT connect
   uint8_t faikoutheat:1;        // Faikout auto is heating
   uint8_t faikoutcool:1;        // Faikout auto is cooling
   uint8_t faikoutbad:1;         // Faikout antifreeze ot slave
   uint8_t cal:1;               // Auto cal manual
   // Power
   uint8_t manual:1;            // Manual override (cleared when matches non override)
   uint8_t away:1;              // Away mode (disabled timer functions)
   uint8_t manualon:1;          // Manual is on
   uint8_t timeron:1;           // Within time range (and not away)
   uint8_t earlyon:1;           // Latching, pre timer range outside temp for early, clear on timer or away
   uint8_t poweron:1;           // Derived state from above
} b = { 0 };

#define BL_TIMEBASE_RESOLUTION_HZ 1000000       // 1MHz, 1us per tick
#define BL_TIMEBASE_PERIOD        1000
uint8_t bl = 0;

struct
{                               //  Snapshot for HA
   uint16_t co2;
   uint8_t tempfrom;
   uint8_t rhfrom;
   float temp;
   float target;
   float tmin;
   float tmax;
   float lux;
   float rh;
   float pressure;
   uint8_t poweron:1;
   uint8_t mode:3;
   uint8_t fan:3;
} data = { 0 };

led_strip_handle_t strip = NULL;
httpd_handle_t webserver = NULL;
SemaphoreHandle_t lcd_mutex = NULL;
SemaphoreHandle_t data_mutex = NULL;
int8_t i2cport = 0;
uint8_t ds18b20_num = 0;
const char *message = NULL;
char *override = NULL;
enum
{
   EDIT_NONE,
   EDIT_TARGET,
   EDIT_MODE,
   EDIT_FAN,
   EDIT_START,
   EDIT_STOP,
   EDIT_REVERT,
   EDIT_NUM,
};
uint8_t edit = EDIT_NONE;       // Edit mode
uint8_t wake = 0;               // Wake (uptime) timeout
uint8_t hold = 0;               // Display hold
bleenv_t *bleidtemp = NULL;
bleenv_t *bleidfaikout = NULL;

gfx_colour_t temp_colour (float t);
gfx_colour_t co2_colour (uint16_t co2);
gfx_colour_t rh_colour (float rh);

const uint8_t icon_mode[] = { icon_unknown, icon_modeauto, icon_modefan, icon_modedry, icon_modecool, icon_modeheat, icon_unknown, icon_modefaikout };   // order same as acmode
const char *const icon_mode_message[] =
   { NULL, "Mode: Auto", "Mode: Fan", "Mode: Dry", "Mode: Cool", "Mode: Heat", NULL, "Mode: Faikout" };

const char led_mode[] = "KyrbBRKY";
const uint8_t icon_fans5[] = { icon_unknown, icon_fanauto, icon_fan1, icon_fan2, icon_fan3, icon_fan4, icon_fan5, icon_fanquiet };      // order same as acfan
const char *const icon_fan5_message[] = { NULL, "Fan: Auto", "Fan: 1", "Fan: 2", "Fan: 3", "Fan: 4", "Fan: 5", "Fan: Quiet" };

const uint8_t icon_fans3[] = { icon_unknown, icon_fanauto, icon_fanlow, 0xFF, icon_fanmid, 0xFF, icon_fanhigh, icon_fanquiet }; // order same as acfan
const char *const icon_fan3_message[] = { NULL, NULL, "Fan: Low", NULL, "Fan: Mid", NULL, "Fan: High", NULL };

static inline float
T (float C)
{                               // Celsius to temp
   if (fahrenheit && isfinite (C))
      return (C + 40) * 1.8 - 40;
   return C;
}

static inline float
C (float T)
{                               // Temp to Celsuis
   if (fahrenheit && isfinite (T))
      return (T + 40) / 1.8 - 40;
   return T;
}

static inline float
DT (float C)
{                               // Celsius offset to temp
   if (fahrenheit && isfinite (C))
      return 1.8;
   return C;
}

static inline float
DC (float T)
{                               // Temp offset to Celsuis
   if (fahrenheit && isfinite (T))
      return 1.8;
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
   uint32_t serial;
   float t;
   float rh;
} sht40 = { 0 };

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
   uint16_t to;
   float t;
   float rh;
} scd41 = { 0 };

struct
{
   uint8_t found:1;
   uint8_t ok:1;
   float peak60;
   float mean60;
} i2s = { 0 };

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

void
led_task (void *x)
{
   while (!b.die)
   {
      usleep (100000);
      uint32_t status = revk_blinker ();
      for (int i = 0; i < sizeof (lightmode) / sizeof (*lightmode); i++)
         switch (lightmode[i])
         {
         case REVK_SETTINGS_LIGHTMODE_STATUS:
            revk_led (strip, i, 255, status);
            break;
         case REVK_SETTINGS_LIGHTMODE_MODE:
            revk_led (strip, i, 255, revk_rgb (b.away ? 'O' : !b.poweron ? 'K' : led_mode[data.mode]));
            break;
         case REVK_SETTINGS_LIGHTMODE_CO2:
            {
               uint32_t c = co2_colour (data.co2);
               if (dark && c == 0x00FF00)
                  c = 0;
               revk_led (strip, i, 255, c);
            }
            break;
         case REVK_SETTINGS_LIGHTMODE_RH:
            {
               uint32_t c = rh_colour (data.rh);
               if (dark && c == 0x00FF00)
                  c = 0;
               revk_led (strip, i, 255, c);
               break;
            }
         case REVK_SETTINGS_LIGHTMODE_TEMP:
            {
               uint32_t c = temp_colour (data.temp);
               if (dark && c == 0x00FF00)
                  c = 0;
               revk_led (strip, i, 255, c);
            }
            break;
         }
      led_strip_refresh (strip);
   }
   vTaskDelete (NULL);
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
   if (!strcmp (suffix, "light"))
   {
      if (*value == '0' || *value == 'f')
         suffix = "dark";
      else
      {
         b.night = 0;
         bl = gfxhigh;
      }
   }
   if (!strcmp (suffix, "dark"))
   {
      b.night = 1;
      bl = gfxlow;
   }
   if (!strcmp (suffix, "message"))
   {
      free (override);
      override = strdup (value);
   }
   if (!strcmp (suffix, "autocal"))
      b.cal = 1;
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
   if (isfinite (data.rh))
   {
      jo_litf (j, "rh", "%.2f", data.rh);
      if (data.rhfrom)
         add_enum ("rh_source", data.rhfrom, REVK_SETTINGS_RHREF_ENUMS);
   }
   if (isfinite (data.lux))
      jo_litf (j, "lux", "%.4f", data.lux);
   if (isfinite (data.pressure))
      jo_litf (j, "pressure", "%.2f", data.pressure);
   if (isfinite (i2s.peak60))
      jo_litf (j, "noise_peak60", "%.2f", i2s.peak60);
   if (isfinite (i2s.mean60))
      jo_litf (j, "noise_mean60", "%.2f", i2s.mean60);
   if (isfinite (data.temp))
   {
      jo_litf (j, "temp", "%.2f", data.temp);
      if (data.tempfrom)
         add_enum ("temp_source", data.tempfrom, REVK_SETTINGS_TEMPREF_ENUMS);
   }
   if (!notarget)
   {
      if (isfinite (data.tmin) && isfinite (data.tmax) && data.tmin == data.tmax)
         jo_litf (j, "temp_target", "%.2f", data.tmin);
      else
      {
         jo_array (j, "temp_target");
         if (isfinite (data.tmin))
            jo_litf (j, NULL, "%.2f", data.tmin);
         if (isfinite (data.tmax))
            jo_litf (j, NULL, "%.2f", data.tmax);
         jo_close (j);
      }
   }
   if (data.mode && !nomode && !nofaikout)
      add_enum ("mode", data.mode, REVK_SETTINGS_ACMODE_ENUMS);
   if (data.fan && !nofan && !nofaikout)
      add_enum ("fan", data.fan, REVK_SETTINGS_ACFAN_ENUMS);
   jo_string (j, "state",
              b.faikoutbad ? "bad" : b.manual ? "manual" : b.away ? "away" : b.earlyon ? "early" : b.timeron ? "timer" : "off");
   jo_bool (j, "power", data.poweron);
   if (fancontrol)
      jo_bool (j, "extfan", b.fan);
   if (radcontrol)
      jo_bool (j, "extrad", b.rad);
   xSemaphoreGive (data_mutex);
}

static void
settings_blefaikout (httpd_req_t * req)
{
   revk_web_send (req, "<tr><td>Faikout</td><td>"        //
                  "<select name=blefaikout>");
   revk_web_send (req, "<option value=\"\">-- None --");
   char found = 0;
   for (bleenv_t * e = bleenv; e; e = e->next)
      if (e->faikoutset)
      {
         revk_web_send (req, "<option value=\"%s\"", e->mac);
         if (*blefaikout && (!strcmp (blefaikout, e->name) || !strcmp (blefaikout, e->mac)))
         {
            revk_web_send (req, " selected");
            found = 1;
         }
         revk_web_send (req, ">%s", e->name);
         if (!e->missing && e->rssi)
            revk_web_send (req, " %ddB", e->rssi);
      }
   if (!found && *blefaikout)
      revk_web_send (req, "<option selected value=\"%s\">%s", blefaikout, blefaikout);
   revk_web_send (req, "</select>");
   revk_web_send (req, "</td><td>Air conditioner Faikout</td></tr>");
}

static void
settings_bletemp (httpd_req_t * req)
{
   revk_web_send (req, "<tr><td>BLE</td><td>"   //
                  "<select name=bletemp>");
   revk_web_send (req, "<option value=\"\">-- None --");
   char found = 0;
   for (bleenv_t * e = bleenv; e; e = e->next)
      if (!e->faikoutset)
      {
         revk_web_send (req, "<option value=\"%s\"", e->mac);
         if (*bletemp && (!strcmp (bletemp, e->name) || !strcmp (bletemp, e->mac)))
         {
            revk_web_send (req, " selected");
            found = 1;
         }
         revk_web_send (req, ">%s", e->name);
         if (!e->missing && e->rssi)
            revk_web_send (req, " %ddB", e->rssi);
         if (!e->missing && e->tempset)
            revk_web_send (req, " %.2f°", (float) e->temp / 100);
      }
   if (!found && *bletemp)
      revk_web_send (req, "<option selected value=\"%s\">%s", bletemp, bletemp);
   revk_web_send (req, "</select>");
   revk_web_send (req, "</td><td>External BLE temperature reference</td></tr>");
}

void
revk_web_extra (httpd_req_t * req, int page)
{
   revk_web_setting_title (req, "Controls");
   if (!nofaikout)
      settings_blefaikout (req);
   if (!notarget)
   {
      revk_web_setting (req, "Target now", "actarget");
      if (!norevert)
         revk_web_setting (req, "Target fixed", "acrevert");
   }
   if (!nofaikout)
   {
      revk_web_setting (req, "±", "acmargin");
      if (!nofaikout && !nomode)
         revk_web_setting (req, "Mode", "acmode");
      if (!nofaikout && !nofan)
         revk_web_setting (req, "Fan", "acfan");
   }
   revk_web_setting (req, "Start", "acstart");
   revk_web_setting (req, "Stop", "acstop");
   if (veml6040.found)
   {
      revk_web_setting_title (req, "Backlight");
      revk_web_setting_info (req, "The display can go dark below specified light level, 0 to disable. Current light level %.2f",
                             veml6040.w);
      revk_web_setting (req, "Auto dark", "veml6040dark");
      revk_web_setting (req, "Start", "veml6040start");
      revk_web_setting (req, "Stop", "veml6040stop");
   }
   revk_web_setting_title (req, "Temperature");
   revk_web_setting_info (req, "Configured temperature sources (in priority order)...");
   if (*bletemp)
      revk_web_send (req, "<tr><td>%s</td><td align=right>%.2f°</td><td>External BLE sensor.</td></tr>", bletemp,
                     bleidtemp ? T ((float) bleidtemp->temp / 100.0) : NAN);
   for (int i = 0; i < ds18b20_num; i++)
      revk_web_send (req, "<tr><td>DS18B20</td><td align=right>%.2f°</td><td>External wired sensor.</td></tr>", ds18b20s[i].t);
   if (scd41.found)
      revk_web_send (req, "<tr><td>SCD41</td><td align=right>%.2f°</td><td>Internal CO₂ sensor%s.</td></tr>", scd41.t,
                     isnan (scd41.t) ? ", shows after startup delay" : "");
   if (sht40.found)
      revk_web_send (req, "<tr><td>SHT40</td><td align=right>%.2f°</td><td>Internal temperature sensor.</td></tr>", sht40.t);
   if (tmp1075.found)
      revk_web_send (req, "<tr><td>TMP1075</td><td align=right>%.2f°</td><td>Internal temperature sensor.</td></tr>", tmp1075.t);
   if (mcp9808.found)
      revk_web_send (req, "<tr><td>MCP9808</td><td align=right>%.2f°</td><td>Internal temperature sensor.</td></tr>", mcp9808.t);
   if (*blefaikout)
      revk_web_send (req, "<tr><td>%s</td><td align=right>%.2f°</td><td>Aircon temperature via Faikout.</td></tr>", blefaikout,
                     bleidfaikout ? T ((float) bleidfaikout->temp / 100.0) : NAN);
   if (gzp6816d.found)
      revk_web_send (req,
                     "<tr><td>GZP6816D</td><td align=right>%.2f°</td><td>Internal pressure sensor, not recommended.</td></tr>",
                     gzp6816d.t);
   revk_web_setting_info (req, "Note that internal sensors may need an offset, depending on orientation and if in a case, etc.");
   revk_web_setting (req, "Temp", "tempref");
   settings_bletemp (req);
   if (data.tempfrom == REVK_SETTINGS_TEMPREF_SCD41 || tempref == REVK_SETTINGS_TEMPREF_SCD41)
      revk_web_setting (req, "Temp offset", "scd41dt");
   if (data.tempfrom == REVK_SETTINGS_TEMPREF_SHT40 || tempref == REVK_SETTINGS_TEMPREF_SHT40)
      revk_web_setting (req, "Temp offset", "sht40dt");
   if (data.tempfrom == REVK_SETTINGS_TEMPREF_TMP1075 || tempref == REVK_SETTINGS_TEMPREF_TMP1075)
      revk_web_setting (req, "Temp offset", "tmp1075dt");
   if (data.tempfrom == REVK_SETTINGS_TEMPREF_MCP9808 || tempref == REVK_SETTINGS_TEMPREF_MCP9808)
      revk_web_setting (req, "Temp offset", "mcp9808dt");
   if (data.tempfrom == REVK_SETTINGS_TEMPREF_AC || tempref == REVK_SETTINGS_TEMPREF_AC)
      revk_web_setting (req, "Temp offset", "acdt");
   if (data.tempfrom == REVK_SETTINGS_TEMPREF_GZP6816D || tempref == REVK_SETTINGS_TEMPREF_GZP6816D)
      revk_web_setting (req, "Temp offset", "gzp6816ddt");
   revk_web_setting (req, "Auto cal", "autocal");
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

static int32_t
i2c_read_16lh2 (uint8_t addr, uint8_t cmd)
{                               // try twice
   int32_t v = i2c_read_16lh (addr, cmd);
   if (v < 0)
      v = i2c_read_16lh (addr, cmd);
   return v;
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
crc8 (uint8_t b1, uint8_t b2)
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
sht40_write (uint8_t cmd)
{
   i2c_cmd_handle_t t = i2c_cmd_link_create ();
   i2c_master_start (t);
   i2c_master_write_byte (t, (sht40i2c << 1) | I2C_MASTER_WRITE, true);
   i2c_master_write_byte (t, cmd, true);
   i2c_master_stop (t);
   esp_err_t err = i2c_master_cmd_begin (i2cport, t, 100 / portTICK_PERIOD_MS);
   i2c_cmd_link_delete (t);
   if (err)
      ESP_LOGE (TAG, "I2C write %02X %02X fail %s", sht40i2c & 0x7F, cmd, esp_err_to_name (err));
   usleep (cmd >= 0xE0 ? 10000 : 1000);
   return err;
}

static esp_err_t
sht40_read (uint16_t * ap, uint16_t * bp)
{
   if (ap)
      *ap = 0;
   if (bp)
      *bp = 0;
   uint8_t buf[6];
   i2c_cmd_handle_t t = i2c_cmd_link_create ();
   i2c_master_start (t);
   i2c_master_write_byte (t, (sht40i2c << 1) + I2C_MASTER_READ, true);
   i2c_master_read (t, buf, sizeof (buf), I2C_MASTER_LAST_NACK);
   i2c_master_stop (t);
   esp_err_t err = i2c_master_cmd_begin (i2cport, t, 100 / portTICK_PERIOD_MS);
   i2c_cmd_link_delete (t);
   if (err)
      ESP_LOGE (TAG, "I2C read %02X fail %s", sht40i2c & 0x7F, esp_err_to_name (err));
   if (!err)
   {                            // CRC checks
      int p = 0;
      while (p < sizeof (buf) && !err)
      {
         if (crc8 (buf[p], buf[p + 1]) != buf[p + 2])
         {
            ESP_LOGE (TAG, "SCD41 CRC fail on read");
            err = ESP_FAIL;
         }
         p += 3;
      }
   }
   if (!err)
   {
      if (ap)
         *ap = ((buf[0] << 8) | buf[1]);
      if (bp)
         *bp = ((buf[3] << 8) | buf[4]);
   }
   return err;
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
      ESP_LOGE (TAG, "I2C read %02X %04X %d fail %s", scd41i2c & 0x7F, c, len, esp_err_to_name (err));
   if (!err)
   {                            // CRC checks
      int p = 0;
      while (p < len && !err)
      {
         if (crc8 (buf[p], buf[p + 1]) != buf[p + 2])
         {
            ESP_LOGE (TAG, "SCD41 CRC fail on read %02x", c);
            err = ESP_FAIL;
         }
         p += 3;
      }
   }
   return err;
}

static esp_err_t
scd41_write (uint16_t c, uint16_t v)
{
   i2c_cmd_handle_t t = i2c_cmd_link_create ();
   i2c_master_start (t);
   i2c_master_write_byte (t, (scd41i2c << 1) | I2C_MASTER_WRITE, true);
   i2c_master_write_byte (t, c >> 8, true);
   i2c_master_write_byte (t, c, true);
   i2c_master_write_byte (t, v >> 8, true);
   i2c_master_write_byte (t, v, true);
   i2c_master_write_byte (t, crc8 (v >> 8, v), true);
   i2c_master_stop (t);
   esp_err_t err = i2c_master_cmd_begin (i2cport, t, 100 / portTICK_PERIOD_MS);
   i2c_cmd_link_delete (t);
   if (err)
      ESP_LOGE (TAG, "SCD41 write %02X %04X %04X fail %s", scd41i2c & 0x7F, c, v, esp_err_to_name (err));
   return err;
}

void
bl_task (void *x)
{
   mcpwm_cmpr_handle_t comparator = NULL;
   mcpwm_timer_handle_t bltimer = NULL;
   mcpwm_timer_config_t timer_config = {
      .group_id = 0,
      .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
      .resolution_hz = BL_TIMEBASE_RESOLUTION_HZ,
      .period_ticks = BL_TIMEBASE_PERIOD,
      .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
   };
   mcpwm_new_timer (&timer_config, &bltimer);
   mcpwm_oper_handle_t oper = NULL;
   mcpwm_operator_config_t operator_config = {
      .group_id = 0,            // operator must be in the same group to the timer
   };
   mcpwm_new_operator (&operator_config, &oper);
   mcpwm_operator_connect_timer (oper, bltimer);
   mcpwm_comparator_config_t comparator_config = {
      .flags.update_cmp_on_tez = true,
   };
   mcpwm_new_comparator (oper, &comparator_config, &comparator);
   mcpwm_gen_handle_t generator = NULL;
   mcpwm_generator_config_t generator_config = {
      .gen_gpio_num = gfxbl.num,
      .flags.invert_pwm = gfxbl.invert,
   };
   mcpwm_new_generator (oper, &generator_config, &generator);
   mcpwm_comparator_set_compare_value (comparator, 0);
   mcpwm_generator_set_action_on_timer_event (generator,
                                              MCPWM_GEN_TIMER_EVENT_ACTION (MCPWM_TIMER_DIRECTION_UP,
                                                                            MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
   mcpwm_generator_set_action_on_compare_event
      (generator, MCPWM_GEN_COMPARE_EVENT_ACTION (MCPWM_TIMER_DIRECTION_UP, comparator, MCPWM_GEN_ACTION_LOW));
   mcpwm_timer_enable (bltimer);
   mcpwm_timer_start_stop (bltimer, MCPWM_TIMER_START_NO_STOP);
   int b = 0;
   while (1)
   {
      if (b == bl)
      {
         usleep (100000);
         continue;
      }
      if (bl > b)
         b++;
      else
         b--;
      mcpwm_comparator_set_compare_value (comparator, b * BL_TIMEBASE_PERIOD / 100);
      usleep (10000);
   }
}

void
i2s_task (void *x)
{
   const int rate = 25;
   const int samples = 320;
   i2s_chan_handle_t mic_handle = { 0 };
   esp_err_t err;
   i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG (I2S_NUM_AUTO, I2S_ROLE_MASTER);
   err = i2s_new_channel (&chan_cfg, NULL, &mic_handle);
   i2s_pdm_rx_config_t cfg = {
      .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG ((samples * rate)),
      .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG (I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
                   .clk = i2sclock.num,
                   .din = i2sdata.num,
                   .invert_flags = {
                                    .clk_inv = i2sclock.invert,
                                    }
                   }
   };
   cfg.slot_cfg.slot_mask = (i2sright ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT);
   if (!err)
      err = i2s_channel_init_pdm_rx_mode (mic_handle, &cfg);
   gpio_pulldown_en (i2sdata.num);
   if (!err)
      err = i2s_channel_enable (mic_handle);
   if (err)
   {
      ESP_LOGE (TAG, "Mic I2S failed");
      jo_t j = jo_object_alloc ();
      jo_int (j, "code", err);
      jo_string (j, "error", esp_err_to_name (err));
      jo_int (j, "data", i2sdata.num);
      jo_int (j, "clock", i2sclock.num);
      revk_error ("i2s", &j);
      vTaskDelete (NULL);
      return;
   }
   ESP_LOGE (TAG, "Mic init PDM CLK %d DAT %d %s", i2sclock.num, i2sdata.num, i2sright ? "R" : "L");
   double means[60] = { 0 };
   double peaks[60] = { 0 };
   uint8_t sec = 0;
   int16_t sample[samples];
   uint8_t tick = 0;
   double peak = -INFINITY;
   double sum = 0;
   while (!b.die)
   {
      size_t n = 0;
      err = i2s_channel_read (mic_handle, sample, sizeof (sample), &n, 100);
      if (err || n != sizeof (sample))
      {
         ESP_LOGE (TAG, "Bad read %d %s", n, esp_err_to_name (err));
         sleep (1);
         continue;
      }
      uint64_t t = 0;
      {
         int32_t bias = 0;
         for (int i = 0; i < samples; i++)
            bias += sample[i];  // DC
         bias /= samples;
         for (int i = 0; i < samples; i++)
         {
            int32_t v = (int32_t) sample[i] - bias;
            t += v * v;
         }
      }
      if (t && !i2s.found)
      {
         i2s.found = 1;
         b.ha = 1;
      }
      double db = log10 ((double) t / samples) * 10 + (double) i2sdb / i2sdb_scale;     // RMS
      if (db > peak)
         peak = db;
      sum += db;
      if (++tick == rate)
      {
         sum /= rate;
         means[sec] = sum;
         peaks[sec] = peak;
         if (++sec == 60)
            sec = 0;
         //ESP_LOGE (TAG, "Peak %.2lf Mean %.2lf", peak, sum);
         double p = -INFINITY,
            m = 0;
         for (int s = 0; s < 60; s++)
         {
            if (peaks[(s + sec) % 60] > p)
               p = peaks[(s + sec) % 60];
            m += means[(s + sec) % 60];
         }
         if (isfinite (p) && isfinite (m))
         {
            i2s.peak60 = p;
            i2s.mean60 = m / 60;
            i2s.ok = 1;
         } else
         {
            i2s.ok = 0;
            i2s.peak60 = NAN;
            i2s.mean60 = NAN;
         }
         tick = 0;
         peak = -INFINITY;
         sum = 0;
      }
   }
   i2s_channel_disable (mic_handle);
   i2s_del_channel (mic_handle);
   vTaskDelete (NULL);
}

void
i2c_task (void *x)
{
   scd41.t = NAN;
   scd41.rh = NAN;
   tmp1075.t = NAN;
   sht40.t = NAN;
   sht40.rh = NAN;
   mcp9808.t = NAN;
   gzp6816d.t = NAN;
   i2s.peak60 = NAN;
   i2s.mean60 = NAN;
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
   sleep (1);
   // Init
   if (veml6040i2c)
   {
      if (i2c_read_16lh2 (veml6040i2c, 0) < 0)
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
      uint8_t try = 5;
      while (try--)
      {
         err = scd41_command (0x3F86);  // Stop periodic
         if (!err)
         {
            usleep (500000);
            break;
         }
         sleep (1);
      }
      uint8_t buf[9];
      scd41.to = ((uint32_t) DC (scd41dt < 0 ? -scd41dt : 0)) * 65536 / scd41dt_scale / 175;    // Temp offset
      if (!err)
         err = scd41_read (0x2318, 3, buf);     // get offset
      if (!err && scd41.to != (buf[0] << 8) + buf[1])
      {
         err = scd41_write (0x241D, scd41.to);  // set offset
         if (!err)
            err = scd41_command (0x3615);       // persist
         if (!err)
            usleep (800000);
      }
      if (!err)
         err = scd41_read (0x3682, 9, buf);     // Get serial
      if (err)
         fail (scd41i2c, "SCD41");
      else
      {
         scd41.serial =
            ((unsigned long long) buf[0] << 40) + ((unsigned long long) buf[1] << 32) +
            ((unsigned long long) buf[3] << 24) + ((unsigned long long) buf[4] << 16) +
            ((unsigned long long) buf[6] << 8) + ((unsigned long long) buf[7]);
         if (!scd41_command (0x21B1))   // Start periodic
            scd41.found = 1;
      }
   }
   if (tmp1075i2c)
   {
      if (i2c_read_16hl (tmp1075i2c, 0x0F) != 0x7500 || i2c_write_16hl (tmp1075i2c, 1, 0x60FF))
         fail (tmp1075i2c, "TMP1075");
      else
         tmp1075.found = 1;
   }
   if (sht40i2c)
   {
      uint16_t a,
        b;
      usleep (1000);
      if (sht40_write (0x94) || sht40_write (0x89) || sht40_read (&a, &b))
         fail (sht40i2c, "SHT40");
      else
      {
         sht40.serial = (a << 16) + b;
         sht40.found = 1;
      }
   }
   b.ha = 1;
   sleep (3);
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
         veml6040.r = (v = i2c_read_16lh2 (veml6040i2c, 0x08)) >= 0 ? (float) v *1031 / 65535 : NAN;
         veml6040.g = v >= 0 && (v = i2c_read_16lh2 (veml6040i2c, 0x09)) >= 0 ? (float) v *1031 / 65535 : NAN;
         veml6040.b = v >= 0 && (v = i2c_read_16lh2 (veml6040i2c, 0x0A)) >= 0 ? (float) v *1031 / 65535 : NAN;
         veml6040.w = v >= 0 && (v = i2c_read_16lh2 (veml6040i2c, 0x0B)) >= 0 ? (float) v *1031 / 65535 : NAN;
         veml6040.ok = (v < 0 ? 0 : 1);
      }
      if (mcp9808.found)
      {
         int32_t v = i2c_read_16hl (mcp9808i2c, 5);
         if (v < 0)
         {
            mcp9808.ok = 0;
            mcp9808.t = NAN;
         } else
         {
            int16_t t = (v << 3);
            mcp9808.t = T ((float) t / 128) + (float) mcp9808dt / mcp9808dt_scale;
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
         if (!(err = scd41_read (0xE4B8, 3, buf)) && ((buf[0] & 0x7) || buf[1]) && !(err = scd41_read (0xEC05, sizeof (buf), buf)))
         {
            scd41.ppm = (buf[0] << 8) + buf[1];
            if (uptime () >= scd41startup)
            {
               scd41.t =
                  T (-45.0 + 175.0 * (float) (((uint32_t) ((buf[3] << 8) + buf[4])) + scd41.to) / 65536.0) +
                  (float) scd41dt / scd41dt_scale;
               scd41.rh = 100.0 * (float) ((buf[6] << 8) + buf[7]) / 65536.0;
            }
            scd41.ok = 1;
            if (gzp6816d.ok)
               scd41_write (0x0E000, gzp6816d.hpa);
         } else if (err)
         {
            scd41.ok = 0;
            scd41.t = NAN;
            scd41.rh = NAN;
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
      if (sht40.found)
      {
         uint16_t a,
           b;
         if (!sht40_write (0xFD) && !sht40_read (&a, &b))
         {
            sht40.t = T (-45.0 + 175.0 * a / 65535.0) + (float) sht40dt / sht40dt_scale;
            sht40.rh = -6.0 + 125.0 * b / 65535.0;
            sht40.ok = 1;
         } else
         {
            sht40.ok = 0;
            sht40.t = NAN;
            sht40.rh = NAN;
         }
      }
      {                         // Next second
         struct timeval tv;
         gettimeofday (&tv, NULL);
         usleep (1000000 - tv.tv_usec);
      }
   }
   if (scd41.found)
      scd41_command (0x3F86);   // Stop periodic
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
      for (int i = 0; i < ds18b20_num; ++i)
      {
         float c;
         REVK_ERR_CHECK (ds18b20_trigger_temperature_conversion (ds18b20s[i].handle));
         REVK_ERR_CHECK (ds18b20_get_temperature (ds18b20s[i].handle, &c));
         if (isfinite (c) && c > -100 && c < 200)
            ds18b20s[i].t = T (c);
      }
      {                         // Next second
         struct timeval tv;
         gettimeofday (&tv, NULL);
         usleep (1000000 - tv.tv_usec);
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
   revk_web_head (req, hostname);
   char *qs = NULL;
   revk_web_send (req, "<h1>%s</h1>", revk_web_safe (&qs, hostname));
   free (qs);
#ifndef CONFIG_GFX_BUILD_SUFFIX_GFXNONE
#ifdef	CONFIG_LWPNG_ENCODE
   revk_web_send (req, "<p><img src=frame.png style='border:10px solid black;'></p>");
#endif
#endif
   if (btnu.set || btnd.set || btnl.set || btnr.set)
      revk_web_send (req, "<table border=1>"    //
                     "<tr><td colspan=2></td><td><a href='btn?u'>U</a></td><td colspan=2></td></tr>"    //
                     "<tr><td><a href='btn?L'><b>L</b></a></td><td><a href='btn?l'>L</a></td><td>◆</td><td><a href='btn?r'>R</a></td><td><a href='btn?R'><b>R</b></a></td></tr>"      //
                     "<tr><td colspan=2></td><td><a href='btn?d'>D</a></td><td colspan=2></td></tr>"    //
                     "</table>");
#ifndef CONFIG_GFX_BUILD_SUFFIX_GFXNONE
#ifdef	CONFIG_LWPNG_ENCODE
   revk_web_send (req, "<p><a href=/>Reload</a></p>");
#endif
#endif
   return revk_web_foot (req, 0, 1, NULL);
}

static esp_err_t
web_status (httpd_req_t * req)
{
   jo_t j = jo_object_alloc ();
   revk_state_extra (j);
   char *js = jo_finisha (&j);
   httpd_resp_set_type (req, "application/json");
   httpd_resp_send (req, js, strlen (js));
   return ESP_OK;
}

void
btnud (int8_t d)
{
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
   case EDIT_REVERT:
      if (tempstep)
      {
         int16_t t = acrevert;
         t = t / tempstep * tempstep;
         t += d * tempstep;
         if (t < tempmin)
            t = tempmin;
         if (t > tempmax)
            t = tempmax;
         jo_litf (j, "acrevert", "%.1f", (float) t / acrevert_scale);
      }
      break;
   case EDIT_MODE:
      {
         uint8_t m = acmode;
         do
         {
            m += d;
            if (m < REVK_SETTINGS_ACMODE_AUTO)
               m = REVK_SETTINGS_ACMODE_FAIKOUT;
            else if (m > REVK_SETTINGS_ACMODE_FAIKOUT)
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
btnlr (int8_t d)
{
   if (edit >= EDIT_MODE)
   {
      edit += d;
      while (edit <= EDIT_TARGET || edit >= EDIT_NUM || ((faikoutonly || nomode || nofaikout) && edit == EDIT_MODE)
             || ((nofaikout || nofan) && edit == EDIT_FAN) || (norevert && edit == EDIT_REVERT))
      {
         edit += d;
         if (edit <= EDIT_TARGET)
            edit = EDIT_NUM;
         else if (edit >= EDIT_NUM)
            edit = EDIT_MODE;
      }
   } else if (b.away)
   {
      edit = 0;
      b.away = 0;
      message = "Home mode";
   } else if (d < 0)
   {
      edit = 0;
      b.manualon = 0;
      b.manual = 1;
      message = "Power off";
   } else
   {
      edit = 0;
      b.manualon = 1;
      b.manual = 1;
      message = "Power on";
   }
}

void
btnL (void)
{
   edit = 0;
   b.away = 1;
   b.manual = 0;
   message = "Away mode";
}

void
btnp (void)
{
   edit = 0;
}

void
btnR (void)
{
   if (edit < EDIT_MODE)
      edit = EDIT_MODE;
   while (((faikoutonly || nomode || nofaikout) && edit == EDIT_MODE) || ((nofaikout || nofan) && edit == EDIT_FAN))
      edit++;
}

void
btn (char c)
{
   message = NULL;
   ESP_LOGE (TAG, "Btn %c", c);
   if (b.night && !wake)
   {                            // Light up
      wake = 10;
      edit = 0;
      b.display = 1;
      return;
   }
   wake = 10;
   if (hold)
   {                            // Remove hold message
      hold = 0;
      b.display = 1;
      return;
   }
   if (!edit && !notarget)
      edit = EDIT_TARGET;
   switch (c)
   {
   case 'u':
      btnud (1);
      break;
   case 'd':
      btnud (-1);
      break;
   case 'l':
      btnlr (-1);
      break;
   case 'r':
      btnlr (1);
      break;
   case 'p':
      btnp ();
      break;
   case 'L':
      btnL ();
      break;
   case 'R':
      btnR ();
      break;
   }
   b.display = 1;
}

void
btn_task (void *x)
{
   // We accept one button at a time
   revk_gpio_t btng[] = { btnu, btnd, btnl, btnr };
   const char btns[] = "udlr";
   uint8_t b;
   for (b = 0; b < 4; b++)
      revk_gpio_input (btng[b]);
   while (1)
   {
      // Wait for a button press
      for (b = 0; b < 4 && !revk_gpio_get (btng[b]); b++);
      if (b == 4)
      {
         usleep (10000);
         continue;
      }
      uint8_t c = 0;
      while (revk_gpio_get (btng[b]))
      {
         if (c < 255)
            c++;
         if (c == 5)
         {                      // Initial press
            btn (btns[b]);
         } else if (c == 100 && b < 2)
         {                      // Simple repeat
            btn (btns[b]);
            c = 50;             // Repeat
         } else if (c == 200)
         {                      // Hold
            btn (toupper ((int) (uint8_t) btns[b]));
         }
         usleep (10000);
      }
      // Wait all clear
      c = 0;
      while (1)
      {
         for (b = 0; b < 4 && !revk_gpio_get (btng[b]); b++);
         if (b < 4)
            c = 0;
         if (++c == 5)
            break;
         usleep (10000);
      }
   }
}

static void
ir_callback (uint8_t coding, uint16_t lead0, uint16_t lead1, uint8_t len, uint8_t * data)
{                               // Handle generic IR https://www.amazon.co.uk/dp/B07DJ58XGC
   static char key = 0;
   static uint8_t count = 0;
   //ESP_LOGE (TAG, "IR CB %d %d %d %d", coding, lead0, lead1, len);
   if (coding == IR_PDC && len == 32 && lead0 > 8500 && lead0 < 9500 && lead1 > 4000 && lead1 < 5000 && (data[0] ^ data[1]) == 0xFF
       && (data[2] ^ data[3]) == 0xFF)
   {                            // Key (generic or TV remote)
      key = 0;
      uint16_t code = ((data[0] << 8) | data[2]);
      if ((irtv && (code >> 8) == 4) || (irgeneric && (!(code >> 8))))
      {
         if (code == 0x0407 || code == 0x0008)
            key = 'l';
         else if (code == 0x0406 || code == 0x005A)
            key = 'r';
         else if (code == 0x0440 || code == 0x0018)
            key = 'u';
         else if (code == 0x0441 || code == 0x0052)
            key = 'd';
         else if (code == 0x0444 || code == 0x001C)
            key = 'p';
         if (key)
            count = 1;
      }
      //ESP_LOGE (TAG, "Code %04X", code);
   }
   if (count && coding == IR_ZERO && len == 1 && lead0 > 8500 && lead0 < 9500 && lead1 > 1500 && lead1 < 2500 && key)
   {                            // Continue
      if (count < 255)
         count++;
      if (key == 'u' || key == 'd')
      {                         // non hold
         if (count == 10 || count == 20)
         {
            btn (key);
            count = 15;
         }
      } else if (count == 10)
         btn (toupper ((int) (uint8_t) key));   // Hold
   }
   if (count && coding == IR_IDLE)
   {
      if (count < 10)
         btn (key);
      key = 0;
      count = 0;
   }
   if (coding == IR_PDC && lead0 > 3000 && lead0 < 4000 && lead1 > 1000 && lead1 < 2000 && (len == 64 || len == 152)
       && data[0] == 0x11 && data[1] == 0xDA && data[2] == 0x27)
   {                            // Looks like Daikin
      // Checksum - unknown
      uint8_t c = 0;
      for (int i = 0; i < len / 8 - 1; i++)
         c += data[i];
      if (c != data[len / 8 - 1])
         ESP_LOGE (TAG, "IT Daikin bad checksum");
      else
      {
         jo_t j = jo_object_alloc ();
         // Decode
         if (len == 64 && data[3] == 0 && data[4] == 0xC5)
         {
            jo_int (j, "gfxhigh", (data[5] >> 4) * 33 + 1);     // brightness (%)
         }
         if (len == 64 && data[3] == 0 && data[4] == 0x42)
         {                      // Time related
         }
         if (len == 152 && data[3] == 0 && data[4] == 0)
         {
            if (!nomode && (data[5] >> 4) < 7)
               jo_int (j, "acmode", "1034502"[(data[5] >> 4)] - '0');   // mode
            // TODO if we are not using AC temp reference, for Auto set Faikout Auto...
            if (!nofan)
               jo_int (j, "acfan", "0002345600170000"[(data[8] >> 4)] - '0');   // fan
            if (!notarget && data[6] > 20 && data[6] < 100)
               jo_litf (j, "actarget", "%.1f", (float) data[6] / 2);    // target (not relevant for dry more, for example)
            b.manualon = (data[5] & 1); // power
            b.manual = 1;
            // Not yet doing other settings
            // 13 01 is powerful
            // 13 20 is quiet outside
            // 16 04 is econo
            //  8 0F is swing-v
            //  9 0F is swing-h
            // 16 02 sensor
            //  8 top bits comfort (A0 on 70 off)
         }
         revk_setting (j);
         jo_free (&j);
      }
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
icon_plot (uint8_t i, uint8_t m)
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
   gfx_draw (w, h, m, m, &ox, &oy);
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

gfx_colour_t
temp_colour (float t)
{
   gfx_colour_t c = 0x888888;
   if (isfinite (t))
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
   return c;
}

gfx_colour_t
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
   return c;
}

gfx_colour_t
rh_colour (float rh)
{
   gfx_colour_t c = 0x888888;
   if (isfinite (rh))
   {
      if (rh < rhblue)
         c = 0x0000FF;
      else if (rh < rhgreen)
         c = 0x00FF00;
      else if (rh < rhred)
         c = 0xFFFF00;
      else
         c = 0xFF0000;
   }
   return c;
}

void
send_fan (uint8_t fan)
{
   b.fan = fan;
   char *m = fan ? fanon : fanoff;
   if (*m)
      revk_mqtt_send_str (m);
}

void
send_rad (uint8_t rad)
{
   b.rad = rad;
   char *m = rad ? radon : radoff;
   if (*m)
      revk_mqtt_send_str (m);
}

void
show_temp (float t)
{                               // Show current temp
   gfx_colour_t c = temp_colour (t);
   gfx_background (c);
   gfx_foreground (c);
   if (isnan (t) || t <= -100 || t >= 1000)
      gfx_7seg (GFX_7SEG_SMALL_DOT, 11, "---.-%c", fahrenheit ? 'F' : 'C');
   else
      gfx_7seg (GFX_7SEG_SMALL_DOT, 11, "%5.1f%c", t, fahrenheit ? 'F' : 'C');
}

void
show_target (float t)
{                               // Show target temp
   if (edit == EDIT_TARGET || edit == EDIT_REVERT)
   {
      select_icon_plot (icon_select2, -2, -2);
      message = (edit == EDIT_TARGET ? "Target temp" : "Revert temp");
   }
   if (notarget)
      return;
   gfx_colour_t c = temp_colour (t);
   gfx_background (c);
   gfx_foreground (c);
   if (isnan (t) || t <= -10 || t >= 100)
      gfx_7seg (GFX_7SEG_SMALL_DOT, 6, "--.-%c", fahrenheit ? 'F' : 'C');
   else
      gfx_7seg (GFX_7SEG_SMALL_DOT, 6, "%4.1f%c", t, fahrenheit ? 'F' : 'C');
}

void
show_mode (void)
{
   if (edit == EDIT_MODE)
   {
      select_icon_plot (icon_select, 0, 0);
      message = icon_mode_message[acmode];
   }
   if (edit != EDIT_MODE && b.away && !b.poweron)
      icon_plot (icon_modeaway, 2);
   else if (edit != EDIT_MODE && radcontrol && b.rad && (!b.poweron || nomode || nofaikout))
      icon_plot (icon_moderad, 2);
   else if (edit != EDIT_MODE && !b.poweron)
      icon_plot (icon_modeoff, 2);
   else if (nomode || nofaikout)
      return;
   else if (b.faikoutbad)        // Antifreeze or slave
      icon_plot (icon_modebad, 2);
   else if (acmode == REVK_SETTINGS_ACMODE_FAIKOUT)
      icon_plot (b.faikoutheat ? icon_modefaikoutheat : b.faikoutcool ? icon_modefaikoutcool : icon_modefaikout, 2);
   else
      icon_plot (icon_mode[acmode], 2);
}

void
show_fan (void)
{
   if (edit == EDIT_FAN)
   {
      select_icon_plot (icon_select, 0, 0);
      message = (fan3 ? icon_fan3_message : icon_fan5_message)[acfan];
   }
   if (nofan || nofaikout)
      return;
   icon_plot ((fan3 ? icon_fans3 : icon_fans5)[acfan], 2);
}

void
show_co2 (uint16_t co2)
{
   if (noco2 || !scd41.found)
      return;
   gfx_colour_t c = co2_colour (co2);
   gfx_background (c);
   gfx_foreground (c);
   if (gfx_a () & GFX_R)
      icon_plot (icon_co2, 5);
   if (co2 < 400 || co2 > 10000)
      gfx_7seg (0, 5, "----");
   else
      gfx_7seg (0, 5, "%4u", co2);
   if (!(gfx_a () & GFX_R))
      icon_plot (icon_co2, 5);
   if (!message && co2 >= co2red)
      message = "*High CO₂";
}

void
show_rh (float rh)
{
   if (norh)
      return;
   gfx_colour_t c = rh_colour (rh);
   gfx_background (c);
   gfx_foreground (c);
   if (gfx_a () & GFX_R)
      icon_plot (icon_humidity, 5);
   if (isnan (rh) || rh >= 100)
      gfx_7seg (0, 5, "--");
   else
      gfx_7seg (0, 5, "%2.0f", rh);
   if (!(gfx_a () & GFX_R))
      icon_plot (icon_humidity, 5);
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
   ha_config_sensor ("ram",.name = "RAM",.field = "mem",.unit = "B");
   ha_config_sensor ("spi",.name = "PSRAM",.field = "spi",.unit = "B");
   ha_config_sensor ("co2",.name = "CO₂",.type = "carbon_dioxide",.unit = "ppm",.field = "co2",.delete = !scd41.found
                     && !t6793.found);
   ha_config_sensor ("temp",.name = "Temp",.type = "temperature",.unit = "°C",.field = "temp");
   ha_config_sensor ("hum",.name = "Humidity",.type = "humidity",.unit = "%",.field = "rh",.delete = !scd41.found);
   ha_config_sensor ("lux",.name = "Lux",.type = "illuminance",.unit = "lx",.field = "lux",.delete = !veml6040.found);
   ha_config_sensor ("pressure",.name = "Pressure",.type = "pressure",.unit = "mbar",.field = "pressure",.delete = !gzp6816d.found);
   ha_config_sensor ("noiseM60",.name = "NOISE-MEAN60",.type = "sound_pressure",.unit = "dB",.field =
                     "noise_mean60",.delete = !i2s.found);
   ha_config_sensor ("noiseP60",.name = "NOISE-PEAK60",.type = "sound_pressure",.unit = "dB",.field =
                     "noise_peak60",.delete = !i2s.found);
}

void
app_main ()
{
   data.temp = NAN;
   data.target = NAN;
   data.tmin = NAN;
   data.tmax = NAN;
   data.lux = NAN;
   data.pressure = NAN;
   data.rh = NAN;
   lcd_mutex = xSemaphoreCreateMutex ();
   xSemaphoreGive (lcd_mutex);
   data_mutex = xSemaphoreCreateMutex ();
   xSemaphoreGive (data_mutex);
   revk_boot (&app_callback);
   revk_start ();
   for (int i = 0; i < sizeof (fixedgpio) / sizeof (*fixedgpio); i++)
      if (fixedgpio[i].set)
         revk_gpio_output (fixedgpio[i], 1);
   if (lightgpio.set)
   {
      led_strip_config_t strip_config = {
         .strip_gpio_num = (lightgpio.num),
         .max_leds = 3,
         .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
         .led_model = LED_MODEL_WS2812, // LED strip model
         .flags.invert_out = lightgpio.invert,  // whether to invert the output signal(useful when your hardware has a level inverter)
      };
      led_strip_rmt_config_t rmt_config = {
         .clk_src = RMT_CLK_SRC_DEFAULT,        // different clock source can lead to different power consumption
         .resolution_hz = 10 * 1000 * 1000,     // 10 MHz
         .flags.with_dma = true,
      };
      REVK_ERR_CHECK (led_strip_new_rmt_device (&strip_config, &rmt_config, &strip));
      revk_task ("blink", led_task, NULL, 4);
   }
   // Web interface
   httpd_config_t config = HTTPD_DEFAULT_CONFIG ();
   config.stack_size += 1024 * 4;
   config.lru_purge_enable = true;
   config.max_uri_handlers = 7 + revk_num_web_handlers ();
   if (!httpd_start (&webserver, &config))
   {
      register_get_uri ("/", web_root);
      register_get_uri ("/status", web_status);
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
      revk_task ("i2c", i2c_task, NULL, 4);
   if (btnu.set || btnd.set || btnl.set || btnr.set)
      revk_task ("btn", btn_task, NULL, 4);
   if (ds18b20.set)
      revk_task ("18b20", ds18b20_task, NULL, 4);
   if (irgpio.set)
      ir_start (irgpio, ir_callback);
   if (i2sdata.set && i2sclock.set)
      revk_task ("i2s", i2s_task, NULL, 10);
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
      } else if (gfxbl.set)
         revk_task ("BL", bl_task, NULL, 4);
   }
   bl = gfxhigh;
   xSemaphoreTake (lcd_mutex, portMAX_DELAY);
   revk_gfx_init (5);
   xSemaphoreGive (lcd_mutex);
#endif
   int8_t lastsec = -1;
   int8_t lastmin = -1;
   int8_t lasthour = -1;
   int8_t lastday = -1;
   int8_t lastreport = -1;
   float blet = NAN;
   float blerh = NAN;
   uint8_t blebat = 0;
   uint8_t change = 0;
   uint8_t tempfrom = tempref;
   uint8_t rhfrom = rhref;
   float t = NAN;
   uint16_t co2 = 0;
   float rh = NAN;
   while (!revk_shutting_down (NULL))
   {
      if (!wake)
         message = NULL;        // set by Show functions
      struct tm tm;
      time_t now = time (0);
      localtime_r (&now, &tm);
      if (!b.display && tm.tm_sec == lastsec)
         continue;
      uint32_t up = uptime ();
      b.display = 0;
      if (tm.tm_sec != lastsec)
      {                         // Once per second
         t = NAN;
         rh = NAN;
         co2 = 0;
         if (faikoutonly)
            acmode = REVK_SETTINGS_ACMODE_FAIKOUT;
         if (wake && !--wake)
            edit = 0;
         if (b.ha)
         {
            b.ha = 0;
            ha_config ();
         }
         if (b.connect)
         {
            b.connect = 0;
            if (radcontrol)
               send_rad (b.rad);
            if (fancontrol)
               send_fan (b.fan);
         }
         if (veml6040.ok && veml6040dark)
         {
            uint8_t darkness = ((veml6040.w < (float) veml6040dark / veml6040dark_scale) ? 1 : 0);
            if (darkness != b.nightdark)
               b.nightdark = b.night = darkness;
         }
         if (tm.tm_min != lastmin && veml6040start != veml6040stop)
         {
            uint8_t darkness = 0;
            uint16_t hhmm = tm.tm_hour * 100 + tm.tm_min;
            if (veml6040start < veml6040stop)
               darkness = ((veml6040start <= hhmm && hhmm < veml6040stop) ? 1 : 0);
            else
               darkness = ((veml6040start <= hhmm || hhmm < veml6040stop) ? 1 : 0);
            if (darkness != b.nighttime)
               b.nighttime = b.night = darkness;
         }
         bleenv_expire (120);
         if ((*bletemp && (!bleidtemp || (strcmp (bleidtemp->name, bletemp) && strcmp (bleidtemp->mac, bletemp)))) || (!*bletemp && bleidtemp)  //
             || (*blefaikout && (!bleidfaikout || (strcmp (bleidfaikout->name, blefaikout) && strcmp (bleidfaikout->mac, blefaikout))))
             || (!*blefaikout && bleidfaikout))
         {                      // Update BLE pointers
            bleidtemp = NULL;
            bleidfaikout = NULL;
            bleenv_clean ();
            for (bleenv_t * e = bleenv; e; e = e->next)
               if (!strcmp (e->name, bletemp) || !strcmp (e->mac, bletemp))
               {
                  bleidtemp = e;
                  break;
               }
            for (bleenv_t * e = bleenv; e; e = e->next)
               if (!strcmp (e->name, blefaikout) || !strcmp (e->mac, blefaikout))
               {
                  bleidfaikout = e;
                  break;
               }
         }
         if (bleidtemp && !bleidtemp->missing)
         {                      // Use temp
            if (bleidtemp->tempset)
               blet = T ((float) bleidtemp->temp / 100.0);
            if (bleidtemp->humset)
               blerh = (float) bleidtemp->hum / 100;
            if (bleidtemp->batset)
               blebat = bleidtemp->bat;
         } else
         {
            blet = NAN;
            blerh = NAN;
         }
         if (*blefaikout && (!bleidfaikout || !bleidfaikout->faikoutset || bleidfaikout->missing) && !message)
            message = "*Faikout gone";
         b.faikoutheat =
            ((bleidfaikout && bleidfaikout->faikoutset && !bleidfaikout->missing && bleidfaikout->power &&
              bleidfaikout->mode == REVK_SETTINGS_ACMODE_HEAT) ? 1 : 0);
         b.faikoutcool =
            ((bleidfaikout && bleidfaikout->faikoutset && !bleidfaikout->missing && bleidfaikout->power &&
              bleidfaikout->mode == REVK_SETTINGS_ACMODE_COOL) ? 1 : 0);
         // Work out current values to show / test
         switch (tempref)
         {
         case REVK_SETTINGS_TEMPREF_MCP9808:
            t = mcp9808.t;
            break;
         case REVK_SETTINGS_TEMPREF_TMP1075:
            t = tmp1075.t;
            break;
         case REVK_SETTINGS_TEMPREF_SHT40:
            t = sht40.t;
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
            if (bleidfaikout && !bleidfaikout->missing && bleidfaikout->faikoutset)
               t = T ((float) bleidfaikout->temp / 100);
            break;
         case REVK_SETTINGS_TEMPREF_DS18B20_0_:
            if (ds18b20_num >= 1)
               t = ds18b20s[0].t;
            break;
         case REVK_SETTINGS_TEMPREF_DS18B20_1_:
            if (ds18b20_num >= 2)
               t = ds18b20s[1].t;
            break;
         case REVK_SETTINGS_TEMPREF_DS18B20_2_:
            if (ds18b20_num >= 3)
               t = ds18b20s[2].t;
            break;
         }
         if (isnan (t) && isfinite (t = blet))
            tempfrom = REVK_SETTINGS_TEMPREF_BLE;
         if (isnan (t) && ds18b20_num >= 1 && isfinite (t = ds18b20s[0].t))
            tempfrom = REVK_SETTINGS_TEMPREF_DS18B20_0_;
         if (isnan (t) && ds18b20_num >= 2 && isfinite (t = ds18b20s[1].t))
            tempfrom = REVK_SETTINGS_TEMPREF_DS18B20_1_;
         if (isnan (t) && ds18b20_num >= 3 && isfinite (t = ds18b20s[2].t))
            tempfrom = REVK_SETTINGS_TEMPREF_DS18B20_2_;
         if (isnan (t) && scd41.ok && isfinite (t = scd41.t))
            tempfrom = REVK_SETTINGS_TEMPREF_SCD41;
         if (isnan (t) && tmp1075.ok && isfinite (t = tmp1075.t))
            tempfrom = REVK_SETTINGS_TEMPREF_TMP1075;
         if (isnan (t) && sht40.ok && isfinite (t = sht40.t))
            tempfrom = REVK_SETTINGS_TEMPREF_SHT40;
         if (isnan (t) && mcp9808.ok && isfinite (t = mcp9808.t))
            tempfrom = REVK_SETTINGS_TEMPREF_MCP9808;
         if (isnan (t) && bleidfaikout && !bleidfaikout->missing && bleidfaikout->faikoutset)
         {
            t = T ((float) bleidfaikout->temp / 100);
            tempfrom = REVK_SETTINGS_TEMPREF_AC;
         }
         if (isnan (t) && gzp6816d.ok && isfinite (t = gzp6816d.t))
            tempfrom = REVK_SETTINGS_TEMPREF_GZP6816D;
         if (isfinite (t))
         {                      // Smoother
            static float lastt = NAN;
            float n = t;
            if (isfinite (lastt))
               n = (t + lastt) / 2;
            lastt = t;
            t = n;
         }
         // Pick RH
         switch (rhref)
         {
         case REVK_SETTINGS_RHREF_BLE:
            rh = blerh;
            break;
         case REVK_SETTINGS_RHREF_SCD41:
            rh = scd41.rh;
            break;
         case REVK_SETTINGS_RHREF_SHT40:
            rh = sht40.rh;
            break;
         }
         if (isnan (rh) && scd41.ok && isnan (rh = scd41.rh))
            rhfrom = REVK_SETTINGS_RHREF_SCD41;
         rh = scd41.rh;
         if (isnan (rh) && sht40.ok && isnan (rh = sht40.rh))
            rhfrom = REVK_SETTINGS_RHREF_SHT40;
         if (isnan (rh) && sht40.ok && isnan (rh = blerh))
            rhfrom = REVK_SETTINGS_RHREF_BLE;
         // Pick CO2
         if (scd41.ok)
            co2 = scd41.ppm;
         else if (t6793.ok)
            co2 = t6793.ppm;
         if (!message && blebat && blebat < 10)
            message = "*Low BLE bat";
      }
      // power set based on time and manual
      int16_t early = 0;
      {
         uint8_t on = 0;
         if (acstart == acstop || b.away)
         {                      // Timer disabled
            on = 0;
            b.earlyon = 0;
         } else
         {                      // Timer state
            uint16_t start = acstart / 100 * 60 + acstart % 100;
            uint16_t stop = acstop / 100 * 60 + acstop % 100;
            uint16_t min = tm.tm_hour * 60 + tm.tm_min;
            if (start < stop)
               on = ((min >= start && min < stop) ? 1 : 0);
            else
               on = ((min >= start || min < stop) ? 1 : 0);
            if (b.manual ? b.manualon : on)
               b.earlyon = 0;
            else
            {
               early = start - min;
               if (early < 0)
                  early += 24 * 60;
            }
         }
         if (on != b.timeron || (acstart == acstop && tm.tm_mday != lastday))
         {                      // Change
            b.timeron = on;
            if (!norevert && !on && actarget != acrevert)
            {                   // Revert temp
               jo_t j = jo_object_alloc ();
               float t = (float) acrevert / acrevert_scale;
               if (temprevstep)
               {
                  if (actarget > acrevert)
                     t += (float) temprevstep / temprevstep_scale;
                  else if (actarget < acrevert)
                     t -= (float) temprevstep / temprevstep_scale;
                  jo_litf (j, "acrevert", "%.1f", t);
               }
               jo_litf (j, "actarget", "%.1f", t);
               revk_setting (j);
               jo_free (&j);
            }
         }
      }
      float targetmin = (float) actarget / actarget_scale - (nofaikout ? 0 : (float) acmargin / acmargin_scale);
      float targetmax = (float) actarget / actarget_scale + (nofaikout ? 0 : (float) acmargin / acmargin_scale);
      if (early)
      {                         // Target adjust for early
         if (earlyheat)
            targetmin -= (float) earlyheat / earlyheat_scale * early / 60;
         else
            targetmin = (float) tempmin / tempmin_scale;
         if (earlycool)
            targetmax += (float) earlycool / earlycool_scale * early / 60;
         else
            targetmax = (float) tempmax / tempmax_scale;
      }
      // Target range clip
      if (targetmin < (float) tempmin / tempmin_scale)
         targetmin = (float) tempmin / tempmin_scale;
      else if (targetmin > (float) tempmax / tempmax_scale)
         targetmin = (float) tempmax / tempmax_scale;
      if (targetmax < (float) tempmin / tempmin_scale)
         targetmax = (float) tempmin / tempmin_scale;
      else if (targetmax > (float) tempmax / tempmax_scale)
         targetmax = (float) tempmax / tempmax_scale;
      //ESP_LOGE(TAG,"early=%d min=%.2f max=%.2f earlyon %d manual %d manualon %d timeron %d",early,targetmin,targetmax,b.earlyon,b.manual,b.manualon,b.timeron);
      // Turn on early (latch)
      if (!b.earlyon && early && (t < targetmin || t > targetmax))
         b.earlyon = 1;
      // Unlatch manual
      if (b.manual && b.manualon == (b.earlyon | b.timeron))
         b.manual = 0;
      // Decide on power state
      b.poweron = (b.manual ? b.manualon : b.earlyon | b.timeron);
      // We don't power control a/c if Faikout mode, so use temp control
      if ((nomode || nofaikout || acmode == REVK_SETTINGS_ACMODE_FAIKOUT) && !b.poweron && !early)
      {                         // Full range as not power on - allows faikout to turn off itself even - we leave if early so could decide to turn on itself
         targetmin = (float) tempmin / tempmin_scale;
         targetmax = (float) tempmax / tempmax_scale;
      }
      // No point in range if no a/c
      if (nofaikout)
         targetmax = targetmin; //  not a range just a setting for rad
      // Fan control
      if (!fancontrol || b.away || ((!co2green || co2 < co2green) && (rhgreen || rh <= rhgreen)))
      {                         // Fan off
         if (b.fan)
            send_fan (0);
      } else if (tm.tm_min != lastmin && ((co2red && co2 >= co2red) || (rhred && rh >= rhred)))
      {                         // Fan on
         if (!b.fan)
            send_fan (1);
      }
      // Radiator control
      if (tm.tm_min != lastmin)
      {                         // Rad control
         static float last1 = NAN,
            last2 = NAN;
         float predict = t,
            fade = (float) radfade / radfade_scale;;
         if (radahead && isfinite (last2) && ((last2 <= last1 && last1 <= t) || // going up - turn off early if predict above target
                                              ((last2 >= last1 && last1 >= t) &&        // going down - turn on early in 10 (heatfadem) min stages if predict is below target
                                               (!radfade || !radfadem
                                                || (lastmin % radfadem) < (targetmin + fade - t) * radfadem / fade))))
            predict += radahead * (t - last2) / 2;      // Use predicted value, i.e. turn on/off early
         if (!radcontrol || predict > targetmin || b.faikoutcool)
         {                      /* Heat off */
            if (b.rad)
               send_rad (0);
         } else
         {                      /* Heat on, change */
            if (!b.rad)
               send_rad (1);
         }
         last2 = last1;
         last1 = t;
      }
      // Info from a/c
      if (tm.tm_sec != lastsec && bleidfaikout && bleidfaikout->faikoutset && !bleidfaikout->missing)
      {                         // From Faikout
         if (change)
            change--;
         if (!change)
         {                      // Update - this only updates if there is no change from us pending
            jo_t j = jo_object_alloc ();
            if (acmode != REVK_SETTINGS_ACMODE_FAIKOUT && bleidfaikout->power != b.poweron)
            {
               b.poweron = b.manualon = bleidfaikout->power;
               b.manual = 1;
            }
            if (bleidfaikout->fan != acfan)
            {
               jo_int (j, "acfan", bleidfaikout->fan);
               change = 1;
            }
            if (bleidfaikout->mode != acmode && acmode != REVK_SETTINGS_ACMODE_FAIKOUT)
            {
               jo_int (j, "acmode", bleidfaikout->mode);
               change = 1;
            }
            b.faikoutbad = bleidfaikout->rad;
            float target = T ((float) (bleidfaikout->targetmin + bleidfaikout->targetmax) / 200);
            if (acmode != REVK_SETTINGS_ACMODE_FAIKOUT && actarget != target)
            {
               jo_litf (j, "actarget", "%.1f", target);
               change = 1;
            }
            if (change)
               revk_setting (j);
            jo_free (&j);
         }
      }
      // Record data for logging
      xSemaphoreTake (data_mutex, portMAX_DELAY);
      if (data.poweron != b.poweron || data.mode != acmode || data.fan != acfan
          || (acmode != REVK_SETTINGS_ACMODE_FAIKOUT && data.target != (float) actarget / actarget_scale))
         change = 10;           // Delay incoming updates
      data.poweron = b.poweron;
      data.mode = acmode;
      data.fan = acfan;
      data.co2 = co2;
      data.rh = rh;
      data.tempfrom = tempfrom;
      data.rhfrom = rhfrom;
      data.temp = t;
      data.tmin = targetmin;
      data.tmax = targetmax;
      data.target = (float) actarget / actarget_scale;
      data.lux = (veml6040.ok ? veml6040.w : NAN);
      data.pressure = (gzp6816d.ok ? gzp6816d.hpa : NAN);
      xSemaphoreGive (data_mutex);
      if (isfinite (t) && (b.cal || (tm.tm_hour != lasthour && up > 15 * 60 && autocal)))
      {                         // Autocal
         uint8_t found = 0;
         jo_t j = jo_object_alloc ();
         void cal (const char *tag, float dt, float temp)
         {
            if (isnan (temp))
               return;
            if (temp - t < 0.1 && t - temp < 0.1)
               return;
            float a = (t - temp);
            if (!b.cal)
               a /= 10;         // Slowly
            jo_litf (j, tag, "%.2f", dt + a);
            found++;
            ESP_LOGE (TAG, "Adjust %s %.2f to %.2f", tag, dt, dt + a);
         }
         cal ("scd41dt", (float) scd41dt / scd41dt_scale, scd41.t);
         cal ("tmp1075dt", (float) tmp1075dt / tmp1075dt_scale, tmp1075.t);
         cal ("sht40dt", (float) sht40dt / sht40dt_scale, sht40.t);
         cal ("mcp9808dt", (float) mcp9808dt / mcp9808dt_scale, mcp9808.t);
         cal ("gzp6816ddt", (float) gzp6816ddt / gzp6816ddt_scale, gzp6816d.t);
         if (found)
            revk_setting (j);
         jo_free (&j);
         b.cal = 0;
      }
      if (reporting && (int8_t) (now / reporting) != lastreport)
      {
         lastreport = now / reporting;
         revk_command ("status", NULL);
         if (radcontrol)
            send_rad (b.rad);   // Periodic rather than retained as could be separate commands or even just one command one way
         if (fancontrol)
            send_fan (b.fan);
      }
      // BLE
      switch (bleadvert)
      {
      case REVK_SETTINGS_BLEADVERT_FAIKOUT:
         bleenv_faikout (hostname, C (t), C (targetmin), C (targetmax), b.manual ? b.manualon : b.poweron, b.rad, acmode, acfan);
         break;
      case REVK_SETTINGS_BLEADVERT_BTHOME1:
         bleenv_bthome1 (hostname, C (t), rh, co2, veml6040.w);
         break;
      case REVK_SETTINGS_BLEADVERT_BTHOME2:
         bleenv_bthome2 (hostname, C (t), rh, co2, veml6040.w);
         break;
      }
#ifndef CONFIG_GFX_BUILD_SUFFIX_GFXNONE
      if (override && gfxmessage)
      {
         char *m = override;
         override = NULL;
         hold = 1 + gfxmessage;
         gfx_message (m);
         free (m);
      }
      if (hold && tm.tm_sec != lastsec)
         hold--;
      if (!hold)
      {
         epd_lock ();
         gfx_clear (0);
         // Main temp display
         gfx_pos (gfx_width () - 1, 0, GFX_R);
         if (tempfrom == REVK_SETTINGS_TEMPREF_BLE)
            select_icon_plot (icon_bt, -15, 0);
         show_temp (t);
         if (gfx_width () < gfx_height ())
         {                      // Portrait
            gfx_pos (2, 125, GFX_L | GFX_T | GFX_H);
            if (edit == EDIT_REVERT)
               show_target ((float) acrevert / acrevert_scale);
            else
               show_target ((float) actarget / actarget_scale);
            gfx_pos (gfx_width () - 3, gfx_y (), GFX_R | GFX_T | GFX_H);
            show_fan ();
            gfx_pos (gfx_x () - 10, gfx_y (), GFX_R | GFX_T | GFX_H);
            show_mode ();
            gfx_pos (0, 205, GFX_L | GFX_T | GFX_H);
            if (edit == EDIT_START || edit == EDIT_STOP)
            {
               show_start ();
               gfx_pos (gfx_width () - 3, gfx_y (), GFX_R | GFX_T | GFX_H);
               show_stop ();
            } else
            {
               if (rhfrom == REVK_SETTINGS_RHREF_BLE)
                  icon_plot (icon_bt, 0);
               show_rh (rh);
               gfx_pos (gfx_width () - 1, gfx_y (), GFX_R | GFX_T | GFX_H);
               show_co2 (co2);
            }
         } else
         {                      // Landscape
            gfx_pos (2, 2, GFX_T | GFX_L);
            if (edit == EDIT_REVERT)
               show_target ((float) acrevert / acrevert_scale);
            else
               show_target ((float) actarget / actarget_scale);
            gfx_pos (0, 66, GFX_T | GFX_L | GFX_H);
            show_mode ();
            gfx_pos (gfx_x () + 10, gfx_y (), GFX_T | GFX_L | GFX_H);
            show_fan ();
            if (edit == EDIT_START || edit == EDIT_STOP)
            {
               gfx_pos (2, 135, GFX_T | GFX_L);
               show_start ();
               gfx_pos (gfx_width () - 3, gfx_y (), GFX_T | GFX_R);
               show_stop ();
            } else
            {
               gfx_pos (gfx_width () - 3, 135, GFX_T | GFX_R | GFX_H);
               show_co2 (co2);
               gfx_pos (2, gfx_y (), GFX_T | GFX_L | GFX_H);
               if (isfinite (blerh))
                  icon_plot (icon_bt, 0);
               show_rh (rh);
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
      }
#endif
      bl = (wake || hold > 1 || !b.night ? gfxhigh : gfxlow);
      usleep (10000);
      lastsec = tm.tm_sec;
      lastmin = tm.tm_min;
      lasthour = tm.tm_hour;
      lastday = tm.tm_mday;
   }

   b.die = 1;
   bl = gfxhigh;
   while (1)
   {
      epd_lock ();
      gfx_clear (0);
      const char *reason;
      int t = revk_shutting_down (&reason);
      if (t < 3)
         bl = 0;
      if (t > 1)
      {
         gfx_text (0, 5, "Reboot");
         gfx_pos (gfx_width () / 2, gfx_height () / 2, GFX_C | GFX_M);
         gfx_text (1, 2, "%s", reason);
         int i = revk_ota_progress ();
         if (i >= 0 && i <= 100)
         {
            gfx_pos (gfx_width () / 2 - 50, gfx_height () - 1, GFX_L | GFX_B | GFX_H);
            gfx_7seg (0, 5, "%3d", i);
            gfx_text (0, 5, "%%");
         }
      }
      epd_unlock ();
      usleep (100000);
   }
}
