# dew point ventilator

**notice**: this is still a work in prorgess. I try to keep the main branch in a stable state and will try to fix issues if any come up / are reported but I do this in my spare time so...

## abstract

This was adpated to work on an ESP 8266 (I used the Huzzah feather) and to add some connectivity. I added:

- MQTT to 
  - post the sensor data and ventilation status
  - be able to change settings remotely
- possibility to save config values across reboots

sources:

- original project + code from [Taupunktlüfter bei heise make](https://github.com/MakeMagazinDE/Taupunktluefter)
- article in [heise make](https://www.heise.de/select/make/2022/1/2135511212557842576)

## project setup

I used Visual Studio Code with the `PlatformIO IDE` extension.

Arduino IDE should also work - hasn't been tested (and the folder structure would have to be adapted). The required libraries can be found in the `platformio.ini` file.

### configuration

configuration is mainly done in separate header files:

- see `src/secrets.h ` for configuring your WiFi and MQTT server.
- see `src/settings.h` for configuring the service (offsets, pins etc.)

## features

Publishes sensor data via MQTT and listens to commands. (`BASETOPIC` can be set in `settings.h` - if not set it will be omitted.)

### telemetry topics

- `BASETOPIC/ventilation/sensor-outside/temperature`: Outside sensor - current temperature
- `BASETOPIC/ventilation/sensor-outside/humidity`: Outside sensor - current humidity
- `BASETOPIC/ventilation/sensor-outside/dewpoint`: Outside sensor - current dewpoint temperature
- `BASETOPIC/ventilation/sensor-inside/temperature`: Inside sensor - current temperature
- `BASETOPIC/ventilation/sensor-inside/humidity`: Inside sensor - current humidity
- `BASETOPIC/ventilation/sensor-inside/dewpoint`: Inside sensor - current dewpoint temperature

### status topics

- `BASETOPIC/config/mode`: Mode of the dewpoint ventilator. Can be ON, OFF, AUTO.
- `BASETOPIC/config/deltaDPmin`: minimum difference between dewpoints before ventilator is turned on
- `BASETOPIC/config/hysteresis`: distance between switch-on and switch-off point
- `BASETOPIC/config/tempInside_min`: minimum inside temperature at which ventilation is activated
- `BASETOPIC/config/tempOutside_min`: minimum outdoor temperature at which ventilation is activated
- `BASETOPIC/config/tempOutside_max`: maximum outdoor temperature at which ventilation is activated
- `BASETOPIC/log/startup`: startup time in UTC
- `BASETOPIC/log/heartbeat`: heartbeat timestamp in UTC - sent every 10 seconds
- `BASETOPIC/ventilation/reason`: Reason as string for the current state of the ventilator.
- `BASETOPIC/ventilation/state`: State of the ventilator. Can be `ON`, `OFF`.
- `BASETOPIC/ventilation/stateNum`: State of the ventilator. Can be `1`, `0` (with 1=ON and 0=OFF).

### command (config) topics

- `BASETOPIC/config/mode/set`: allows to set the mode of the dewpoint ventilator. This can be any of ON, OFF, AUTO. This setting is not persisted across reboots and defaults to AUTO.
- `BASETOPIC/config/deltaDPmin/set`: minimum difference between dewpoints before ventilator is turned on
- `BASETOPIC/config/hysteresis/set`: distance between switch-on and switch-off point
- `BASETOPIC/config/tempInside_min/set`: minimum inside temperature at which ventilation is activated
- `BASETOPIC/config/tempOutside_min/set`: minimum outdoor temperature at which ventilation is activated
- `BASETOPIC/config/tempOutside_max/set`: maximum outdoor temperature at which ventilation is activated
- `BASETOPIC/config/reset`: set this to `1` or `true` to reset the config values to the default config (aka what was in `settings.h`)

## learnings

- don't put the outside sensor close to where the ventilator is. In my case I had it about 50cm from the ventilator and as soon as the ventilator ran, the outside temperature was strongly affected by the air coming from the ventilator.
- think about how to easily update your software during testing (I ran into the basement with my laptop from the 2nd floor about 100 times) -> will look at OTA in the future
- ESP8266 seems to be pretty resistant to interferences. It worked without the preventing measures mentioned in the original article (I only put the 1000 µF capacitor between + and - and left out all other capacitors & resistors that were meant to help with interferences)
- It's really hard to debug issues that come up after longer operation (for some reason I see restarts every few days and have no idea what it is. My solution for now: design everything in a way that restarts are not a problem)