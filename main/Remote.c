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
#include "icons.h"
#include "bleenv.h"
#include "halib.h"

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

   while (1)
   {
      sleep (1);
   }
}
