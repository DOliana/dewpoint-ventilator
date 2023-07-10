# dew point ventilator

## abstract

This was adpated to work on an ESP 8266 (I used the Huzzah feather) and to add some connectivity. I added:

- MQTT to post the sensor data and ventilation status
- Translated everything to english to make it more available

sources:

- original project + code from [Taupunktlüfter bei heuse make](https://github.com/MakeMagazinDE/Taupunktluefter)
- article in [heise make](https://www.heise.de/select/make/2022/1/2135511212557842576)

## project setup

- VSCode
  - isntall `PlatformIO IDE` extension

### configuration

see `secrets.h ` for configuring your WiFi and MQTT server.
see `main.ino` for configuring basic parameters for your sensors & calculation

## features

Publishes sensor data via MQTT and listens to commands for setting the mode (AUTO/ON/OFF).

### telemetry

- `BASETOPIC/ventilation/state`: State of the ventilator. Can be `ON`, `OFF`.
- `BASETOPIC/ventilation/stateNum`: State of the ventilator. Can be `1`, `0` (with 1=ON and 0=OFF).
- `BASETOPIC/mode`: Mode of the dewpoint ventilator. Can be ON, OFF, AUTO.

### commands

- `BASETOPIC/mode/set`: allows to set the mode of the dewpoint ventilator. This can be any of ON, OFF, AUTO. This setting is not persisted across reboots and defaults to AUTO.