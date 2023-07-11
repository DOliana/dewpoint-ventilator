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

Arduino IDE should also work - hasn't been tested. The required libraries can be found in the `platformio.ini` file.

### configuration

see `secrets.h ` for configuring your WiFi and MQTT server.
see `settings.h` for configuring the service (offsets, pins etc.)
see `main.ino` for logic - there shouldn't be any changes required.

## features

Publishes sensor data via MQTT and listens to commands.

### telemetry topics

- `BASETOPIC/ventilation/sensor-outside/temperature`: Outside sensor - current temperature
- `BASETOPIC/ventilation/sensor-outside/humidity`: Outside sensor - current humidity
- `BASETOPIC/ventilation/sensor-outside/dewpoint`: Outside sensor - current dewpoint temperature
- `BASETOPIC/ventilation/sensor-inside/temperature`: Inside sensor - current temperature
- `BASETOPIC/ventilation/sensor-inside/humidity`: Inside sensor - current humidity
- `BASETOPIC/ventilation/sensor-inside/dewpoint`: Inside sensor - current dewpoint temperature
- `BASETOPIC/ventilation/reason`: Reason as string for the current state of the ventilator.
- `BASETOPIC/ventilation/state`: State of the ventilator. Can be `ON`, `OFF`.
- `BASETOPIC/ventilation/stateNum`: State of the ventilator. Can be `1`, `0` (with 1=ON and 0=OFF).
- `BASETOPIC/mode`: Mode of the dewpoint ventilator. Can be ON, OFF, AUTO.

### command topics

- `BASETOPIC/mode/set`: allows to set the mode of the dewpoint ventilator. This can be any of ON, OFF, AUTO. This setting is not persisted across reboots and defaults to AUTO.