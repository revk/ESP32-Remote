<img src=https://github.com/user-attachments/assets/3a38b1b3-448a-4cf5-b6a8-5e15899f7698 width=25% align=right>

# Faikin Remote

A remote control for Daikin air conditioners which use the [Faikin](https://faikin.revk.uk/) controller.

This provides hardware and software for a wall mounted controller with 2" full colour display and 5 way *joystick* control button.

USB-C powered (or DC 5V-36V), linked via Bluetooth BLE to the Faikin. This provides a display of current mode and simple controls of basic operations. This is ideal for installations that do not have Home Assistant or MQTT.

It can also work fan and radiator controls via MQTT and so operate in cases without an airconditioner / Faikin or where these supplement the air conditioner.

A number of sensors are included, which can be reported to Home Assistant. The hardware can also run my [EPD project](https://epd.revk.uk/) code.

A key feature is that this can work as the temperature reference for *Faikin auto* mode.

## Basic operation

The display shows current temperature, target temperature, operation mode and fan speed, as well as other details (e.g. CO₂ if fitted)

- Push in to turn on/off
- Push and hold to set *away* mode
- Up/down on control changes target temp.
- Left/right moves to another feature which can be controller with up/down, such as mode, fan speed, power on and off times.
- Light sensor can turn off display when dark, so press control (any way) to light up.
- Other settings can be set via web interface over WiFi.
- The display can be set to go off when it is dark.

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
- 5 way *joystick* control button.
- On board temperatrure sensor (TMP1075)
- On board pressure sensor (GZP6816D)
- On board ambient light sensor with color (VEML6040)
- The PCB is designed to work with a Waveshare 2" colour LCD. Four M2x3mm screws and 8 way 0.1" header pins are required.
- The board can without a display if it is simply to be used as a sensor or temperature reference.
- The board can work with or without the SCD41 CO₂ sensor - as this is an expensive part, and not always required.

The plan is to sell on Tindie with LCD being optional and SCD41 being optional.

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
