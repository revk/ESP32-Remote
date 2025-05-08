# Faikin Remote

<img src=https://github.com/user-attachments/assets/fe21073f-2d91-4ceb-891b-1b56910d66a1 width=25% align=right>

A remote control for Daikin air conditioners which use the [Faikin](https://faikin.revk.uk/) controller.

This provides hardware and software for a wall mounted controller with 2" full colour display and 5 way *joystick* control button.

USB-C powered (or DC 5V-36V), linked via Bluetooth BLE to the Faikin. This provides a display of current mode and simple controls of basic operations. This is ideal for installations that do not have Home Assistant or MQTT.

It can also work fan and radiator controls via MQTT and so operate in cases without an airconditioner / Faikin or where these supplement the air conditioner.

A number of sensors are included, which can be reported to Home Assistant. The hardware can also run my [EPD project](https://epd.revk.uk/) code.

A key feature is that this can work as the temperature reference for *Faikin auto* mode.

## Basic operation

The display shows current temperature, target temperature, operation mode and fan speed, as well as other details (e.g. CO₂ if fitted). There is a button which can be pushed up, down, left, or right.

### Display off

If the display is off, or displaying some override message, pushing the button any direction will simply cause the display to turn on for a whiole, in normal (idle) mode. The display can be turned off by MQTT or based on light sensor.

### Idle mode

In an idle mode the buttons work as follows :-

|Button|Action|
|------|------|
|Up/Down|Change target temperature, hold to step quickly|
|Left|Turn off|
|Right|Turn on|
|Hold left|Holding left will go to *away* mode (and turn off)|
|Hold right|Holding right will go to settings mode, selectign first adjustable setting (normallt a/c mode)|

### Settings

One in settings mode the buttons operate differently.

|Button|Action|
|------|------|
|Up/Down|Change selected setting|
|Left/Right|Move through possible settings|

Waiting several seconds will end settings mode.

## BLE working

- A BLE announcement advises current settings, and a Faikin can be set to use this as a control input.
- The BLE can announce in a number of common temperature sensor modes instead (BTHome v1 and v2).

To link to Faikin.

- Ensure you have a sensible hostnmame for Faikin and the remote.
- On Faikin, enable BLE and select `Remote:` and the name for the remote.
- Once that is done you should be able to select the Faikin by name as the A/C on the Remote settings

## Fan/radiator

In addition to working with the Faikin this can send MQTT messages to turn on or off a fan or radiator.

- The radiator mode is typically where heating is cheaper than using an air conditioning unit.
- The fan control applies when (configurable) high CO₂ or high humidity.

## Hardware and options

<img src=PCB/Remote/Remote.png width=50% align=right>

- ESP32-S3-MINI-1-N4-R2 dual processor, 4M flash, 2M SPIRAM.
- USB-C power, or DC 5V-36V (WAGO).
- Connection for DS18B20 external temperature sensors (WAGO).
- 4 way *joystick* control button.
- On board temperatrure sensor (TMP1075)
- On board pressure sensor (GZP6816D)
- On board ambient light sensor with color (VEML6040)
- The PCB is designed to work with a Waveshare 2" colour LCD. Four M2x3mm screws and 8 way 0.1" header pins are required if you get the display separately.
- The board can work without a display if it is simply to be used as a sensor or temperature reference.
- The board can work with or without the SCD41 CO₂ sensor - as this is an expensive part, and not always required.

Available on [Tindie](https://www.tindie.com/products/revk/faikin-remote-aircon-control-display-dev-board/) now.

## MQTT

Some useful MQTT commands.

|Command|Meaning|
|-------|-------|
|`on`|Power on (also `power 1` or `power true`)|
|`off`|Power off (also `power 0` or `power false`)|
|`away`|Set away mode|
|`home`|Set normal mode (also `away 0` or `away false`)|
|`light`|Set display on|
|`dark`|Set display off (also `light 0` or `light false`)|
|`message`|Display a message|

## Temperature calibration

Temperature is tricky stuff. The actual sensors on the board are very accurate and calibrated, but a key challenge is internal heating and heat from PCB from other components - this means the temperature typically needs some offset applied, and you can only really tell after installing the module, in a case if needed, and running for several minutes to be up to temperature.

For this purpose the on board sensors can have a simple offset added/subtracted to the temperature read, after it is read. For convenience this is in degrees (°C or °F as set).

The most reliable connected sensor is a DS18B20 on a lead as this does not pick up heat from the PCB - do no position above the module to avoid heating by convention. We do not apply an offset to this (let us know if you find a case where that is actually useful). The other reliable sensor is an external BLE sensor - these have such low power usage they do not have internal heating and are also very accurate - again no offset is configurable for these.

The internal sensors (TMP1075 or MCP9808) pick up heat from the PCB and often need several degrees of adjustment. As I say, do this after installing, in case, in position, and running for some time. The pressure sensor also does temperature, but this is right next to the processor, so typically needs way more adjustment.

The SCD41 provides CO₂, temperature, and humidity. Whilst the CO₂ is not affected by temperature, it does have atmospheric pressure adjustment applied automatically from the on board pressure sensor. However the humidity accuracy is impacted by the temperature. As such the temperature offset is initially set in the SCD41 at boot. Any changes to `scd41dt` will apply in real time, but it is recommended that you reboot once you are happy with it to ensure humidity is calculated correctly.

Note that `autocal` adjusts temp offsets to get closer selected/best temp every hour (once on for at least half an hour).
