#include <Arduino.h>     // for Arduino functions
#include "DHT.h"         // for DHT sensors
#include "SHT85.h"       // for SHT30 sensor
#include <ESP8266WiFi.h> // for WiFi
#include <MQTT.h>        // for MQTT
#include <WiFiUdp.h>     // for time sync
#include <secrets.h>     // for WiFi and MQTT secrets
#include <settings.h>    // for settings
#include <ArduinoJson.h> // for JSON parsing (config)
#include "FS.h"          // for file system (config)
#include <LittleFS.h>    // for file system (config)
#include <iterator>      // for map
#include <time.h>        // for time functions

#define RELAIS_ON HIGH // define the relais on value
#define RELAIS_OFF LOW // define the relais off value

// ********* WiFi + MQTT setup (values defined in secret.h) ******
const char *mqtt_server = SECRET_MQTT_SERVER;
const char *mqtt_user = SECRET_MQTT_USER;
const char *mqtt_password = SECRET_MQTT_PASSWORD;

#ifdef SECRET_MQTT_CLIENT_ID
const char *mqtt_clientID = SECRET_MQTT_CLIENT_ID;
#else
const char *mqtt_clientID = "dewpoint-vent-client";
#endif
#ifndef SECRET_MQTT_PORT
const int mqtt_port = 1883;
#else
const int mqtt_port = SECRET_MQTT_PORT;
#endif

const char *ssid = SECRET_WIFI_SSID;
const char *password = SECRET_WIFI_PASSWORD;
const char *wifi_hostname = mqtt_clientID;

long unsigned maxMilliSecondsWithoutWiFi = 1800000UL;              // maximum time without wifi after which we perform a full reboot
long unsigned lastTimeWiFiOK = ULONG_MAX;                          // used to keep track of the last time the WiFi was connected
String startupTime;                                                // startup time - if set its been sent. Used to prevent sending the startup message multiple times
bool ventilatorStatus = false;                                     // needs to be a global variable, so the state is saved across loops
long unsigned lastTimeVentilatorStatusChange = 0;                  // used to keep track of the last time the ventilator status changed
int min_humidity_for_override = MIN_HUMIDITY_FOR_OVERRIDE;         // if the humidity inside is above this value, the ventilator will be turned on regardless of the dew point difference
int max_hours_without_ventilation = MAX_HOURS_WITHOUT_VENTILATION; // after this time, the ventilator will be turned on for at least VENTILATION_OVERRIDE_TIME minutes
int ventilation_override_minutes = VENTILATION_OVERRIDE_MINUTES;   // amount of minutes to override the ventilation status
bool ventilationOverride = false;                                  // used to override the ventilation status
String baseTopic = "dewppoint-ventilator";                         // used to store the MQTT base topic (can be an empty string if no base topic is desired)
String requestedMode = "AUTO";                                     // default mode after reboot is AUTO
int min_delta = MIN_Delta;                                         // minimum difference between the dew points inside and outside to turn on the ventilator
int hysteresis = HYSTERESIS;                                       // hysteresis for turning off the ventilator
int tempInside_min = TEMPINSIDE_MIN;                               // minimum temperature inside to turn on the ventilator
int tempOutside_min = TEMPOUTSIDE_MIN;                             // minimum temperature outside to turn on the ventilator
int tempOutside_max = TEMPOUTSIDE_MAX;                             // maximum temperature outside to turn on the ventilator
float correction_temp_inside = CORRECTION_TEMP_INSIDE;             // correction value for indoor sensor temperature
float correction_temp_outside = CORRECTION_TEMP_OUTSIDE;           // correction value for outdoor sensor temperature
float correction_humidity_inside = CORRECTION_HUMIDITY_INSIDE;     // correction value for indoor sensor humidity
float correction_humidity_outside = CORRECTION_HUMIDITY_OUTSIDE;   // correction value for outdoor sensor humidity
bool stopSleeping = false;                                         // a simple flag to prevent the microcontroller from going to sleep - set from a different thread on wifi-connected
long unsigned lastTimeHeaterOn = 0;                                // last time the heater was turned on
bool isOutsideSensorHeaterOn = false;                              // flag to keep track of the heater status of the outdoor sensor
float lastHumidityOutside = 0;                                     // last humidity value of the outdoor sensor - used to average the values
float outsideSensorReferenceTemperature = 0;                       // reference temperature for the outdoor sensor
long unsigned lastTimeSensorOutsideReferenceTemperature = 0;       // last time the reference temperature was set
float referenceTempDifferenceThreshold = -2;                       // difference between the reference temperature and the outside temperature at which the reference temperature is used

// config map to track changes
bool configChangedMap[14] = {true, true, true, true, true, true, true, true, true, true, true, true, true, true}; // used to keep track of whether the configuration has changed since the last loop - initialize to true to send the config on first loop
#define CONFIG_IDX_MODE 0
#define CONFIG_IDX_MIN_DELTA 1
#define CONFIG_IDX_HYSTERESIS 2
#define CONFIG_IDX_TEMPINSIDE_MIN 3
#define CONFIG_IDX_TEMPOUTSIDE_MIN 4
#define CONFIG_IDX_TEMPOUTSIDE_MAX 5
#define CONFIG_IDX_MIN_HUMIDITY_FOR_OVERRIDE 6
#define CONFIG_IDX_MAX_HOURS_WITHOUT_VENTILATION 7
#define CONFIG_IDX_VENTILATION_OVERRIDE_MINUTES 8
#define CONFIG_IDX_CORRECTION_TEMP_INSIDE 9
#define CONFIG_IDX_CORRECTION_TEMP_OUTSIDE 10
#define CONFIG_IDX_CORRECTION_HUMIDITY_INSIDE 11
#define CONFIG_IDX_CORRECTION_HUMIDITY_OUTSIDE 12
#define CONFIG_IDX_REFERENCE_TEMP_DIFFERENCE_THRESHOLD 13

const float HEATER_HUMIDITY_THRESHOLD = 75;
const float HEATER_TEMPERATURE_THRESHOLD = 3;
const unsigned long HEATER_COOLDOWN_DELAY = 1000L;
const unsigned long MAX_HEATER_RUNTIME = 300000UL;

const unsigned long MAX_AGE_OUTSIDE_REFERENCE_TEMPERATURE = 3600000UL; // 1 hour

WiFiEventHandler wifiConnectHandler;

WiFiClient wifiClient;
MQTTClient mqttClient;

WiFiUDP ntpUDP;

DHT dhtInside(DHTPIN_INSIDE, DHTTYPE_INSIDE); // The indoor sensor is now addressed with dhtInside
SHT31 sensorOutside(0x44);                    // The outdoor sensor is now addressed with sensorOutside

/**
 * This struct that represents the result of checking the DHT sensors.
 * It contains a boolean field "sensorsOK" indicating whether the sensors are working correctly,
 * and a string field "errorReason" containing any error messages if applicable.
 */
struct SensorValues
{
    float humidityInside;
    float tempInside;
    float humidityOutside;
    float tempOutside;

    bool sensorsOK;
    String errorReason;
};

/**
 * The setup function that runs once when the microcontroller starts up.
 * It performs the following tasks:
 * - Enables the watchdog timer to prevent the microcontroller from freezing
 * - Initializes the built-in LED pins and the relay pin
 * - Starts the serial communication at 115200 baud rate
 * - Turns off the ventilator
 * - Initializes the DHT sensors
 * - Sets the MQTT callback function
 * - Sets the MQTT base topic (if defined in secrets.h)
 *
 * @return void
 */
void setup()
{
    ESP.wdtEnable(WDTO_8S); // Enable watchdog timer to prevent the microcontroller from freezing with a timeout of 8 seconds

    pinMode(LED_BUILTIN_RED, OUTPUT);  // Define LED pin as output
    pinMode(LED_BUILTIN_BLUE, OUTPUT); // Define LED pin as output
    pinMode(RELAIPIN, OUTPUT);         // Define relay pin as output

    digitalWrite(LED_BUILTIN_RED, LOW); // Turn on LED to show we have power

    Serial.begin(115200);               // Start serial communication
    Wire.begin();                       // Start I2C communication
    Wire.setClock(100000);              // Set I2C clock to 100kHz
    configTime(TIME_ZONE, TIME_SERVER); // Set time zone and NTP server

    initializeWiFi(); // Initialize WiFi connection

    Serial.println(F("Testing sensors..."));

    dhtInside.begin();     // Start indoor sensor
    sensorOutside.begin(); // Start outdoor sensor

    // set baseTopic to use for MQTT messages
#ifdef SECRET_MQTT_BASETOPIC
    baseTopic = SECRET_MQTT_BASETOPIC; // set baseTopic to use for MQTT messages
    if (baseTopic.endsWith("/") == false)
    {
        baseTopic.concat("/");
    }
#endif
    Serial.print("MQTT base topic set: ");
    Serial.println(baseTopic);

    // mount filesystem for config file
    Serial.println("Mounting FS...");
    if (!LittleFS.begin())
    {
        Serial.println("Failed to mount file system");
    }

    // load config from file if it exists
    if (loadConfig() == false)
    {
        // default values will be used instead
        Serial.println("Failed to load config from file - saving default values in config");
        resetConfig();
    }

    ESP.wdtFeed(); // feed the watchdog
    Serial.println("Setup complete");
    Serial.println();
}

/**
 * The main loop function that runs continuously after the setup function has completed.
 * It performs the following tasks:
 * - Connects to WiFi and MQTT if necessary
 * - Publishes the current configuration to MQTT
 * - Calculates and sets the ventilator status based on the difference between the dew points inside and outside
 * - Posts the startup time to MQTT on the first run
 * - Checks for MQTT messages and processes them if received
 * - Sleeps for 60 seconds before running again
 *
 * @return void
 */
void loop()
{
    // if we do not have a WiFi connection, reboot the microcontroller after a certain time
    if (WiFi.status() == WL_CONNECTED)
    {
        lastTimeWiFiOK = millis();
    }
    else if (millis() > maxMilliSecondsWithoutWiFi && millis() - lastTimeWiFiOK > maxMilliSecondsWithoutWiFi)
    {
        Serial.println("Rebooting after " + String(maxMilliSecondsWithoutWiFi) + "ms without WiFi connection");
        ESP.restart();
    }
    else
    {
        Serial.println("WiFi not connected for " + String(millis() - lastTimeWiFiOK) + "ms");
    }

    digitalWrite(LED_BUILTIN_BLUE, LOW); // Turn on LED when loop is active
    connectMQTTIfDisconnected();         // Connect to MQTT if not connected do this at the beginning so it can run in the background
    publishConfigIfChanged();
    stopSleeping = false; // wifi connection resets this during startup. if we are here we do not need it.
    calculateAndSetVentilatorStatus();

    Serial.println();
    digitalWrite(LED_BUILTIN_BLUE, HIGH); // Turn off LED while sleeping

    // if we do not call mqttclient.loop for to long, the connection will be lost
    for (short i = 0; i < 60; i++) // sleep for 60 seconds
    {
        if (mqttClient.connected())
        {
            // publish heartbeat every 10 seconds
            if (i % 10 == 0)
                mqttClient.publish(baseTopic + "log/heartbeat", getTimeString(), true, 0);
            mqttClient.loop(); // Check for MQTT messages
            delay(10);         // <- fixes some issues with WiFi stability
        }
        else if (i % 10 == 0)
        {
            Serial.print("MQTT connected: false, Wifi connected: ");
            Serial.print(WiFi.status() == WL_CONNECTED ? "true" : "false");
            Serial.println(" - no heartbeat sent");
        }

        // if an MQTT command was received, stop sleeping and process the command
        if (stopSleeping)
        {
            stopSleeping = false;
            break;
        }
        ESP.wdtFeed(); // feed the watchdog
        delay(1000);
    }
}

/**
 * Calculates the difference between the dew points inside and outside, and sets the ventilator status accordingly.
 * If the difference is greater than the minimum delta plus the hysteresis, the ventilator is turned on.
 * Otherwise, the ventilator is turned off.
 */
void calculateAndSetVentilatorStatus()
{
    SensorValues sensorValues = getSensorValues();
    bool newVentilatorStatus = ventilatorStatus;

    if (sensorValues.sensorsOK == false)
    {
        if (mqttClient.connected())
        {
            mqttClient.publish(baseTopic + "log/message", "sensors show errors: " + sensorValues.errorReason, false, 1);
            mqttClient.publish(baseTopic + "log/status", "error", false, 1);
            Serial.println(F("Error message sent"));
            delay(500); // wait for message to be sent
        }

        Serial.println(F("Restarting in 10 seconds..."));
        delay(10000);
        ESP.restart();
    }
    else
    {
        if (mqttClient.connected())
        {
            mqttClient.publish(baseTopic + "log/message", "sensors OK", false, 1);
            mqttClient.publish(baseTopic + "log/status", "OK", false, 1);
        }
    }

    ESP.wdtFeed(); // feed the watchdog

    //**** Print sensor values********
    Serial.println(getTimeString());
    Serial.print(F("sensor-inside: "));
    Serial.print(F("humidity: "));
    Serial.print(sensorValues.humidityInside);
    Serial.print(F("%  temperature: "));
    Serial.print(sensorValues.tempInside);
    Serial.println(F("°C"));

    Serial.print("sensor-outside: ");
    Serial.print(F("humidity: "));
    Serial.print(sensorValues.humidityOutside);
    Serial.print(F("%  temperature: "));
    Serial.print(sensorValues.tempOutside);
    Serial.println(F("°C"));

    //**** Calculate dew points********
    Serial.println(F("calculating dew point..."));
    float dewPoint_inside = calculateDewpoint(sensorValues.tempInside, sensorValues.humidityInside);
    float dewPoint_outside = calculateDewpoint(sensorValues.tempOutside, sensorValues.humidityOutside);

    //**** Print dew points********
    Serial.print(F("sensor-inside dew point: "));
    Serial.print(dewPoint_inside);
    Serial.println(F("°C  "));

    Serial.print(F("sensor-outside dew point: "));
    Serial.print(dewPoint_outside);
    Serial.println(F("°C  "));

    //**** Calculate difference between dew points********
    float deltaDP = dewPoint_inside - dewPoint_outside;

    //**** decide if ventilator should run or not ********
    String ventilatorStatusReason = "Hysteresis phase";
    if (deltaDP > (min_delta + hysteresis))
    {
        newVentilatorStatus = true;
        ventilatorStatusReason = "DeltaDP > (MIN_Delta + HYSTERESIS): " + String(deltaDP) + " > " + String(min_delta) + " + " + String(hysteresis);
    }
    else if (deltaDP <= (min_delta))
    {
        newVentilatorStatus = false;
        ventilatorStatusReason = "DeltaDP < (MIN_Delta): " + String(deltaDP) + " < " + String(min_delta);
    }

    // check overrides
    if (newVentilatorStatus && sensorValues.tempInside < tempInside_min)
    {
        newVentilatorStatus = false;
        ventilatorStatusReason = "tempInside < TEMPINSIDE_MIN: " + String(sensorValues.tempInside) + " < " + String(tempInside_min);
    }
    else if (newVentilatorStatus && sensorValues.tempOutside < tempOutside_min)
    {
        newVentilatorStatus = false;
        ventilatorStatusReason = "tempOutside < TEMPOUTSIDE_MIN: " + String(sensorValues.tempOutside) + " < " + String(tempOutside_min);
    }
    else if (newVentilatorStatus && sensorValues.tempOutside > tempOutside_max)
    {
        newVentilatorStatus = false;
        ventilatorStatusReason = "tempOutside > TEMPOUTSIDE_MAX: " + String(sensorValues.tempOutside) + " > " + String(tempOutside_max);
    }

    // check can only run after first x hours passed since millis starts at 0
    if (millis() > (unsigned long)(max_hours_without_ventilation) * 60UL * 60UL * 1000UL)
    {
        // every x hours, turn on the ventilator if it has been off for x hours
        if (sensorValues.humidityInside >= min_humidity_for_override && ventilatorStatus == false && lastTimeVentilatorStatusChange < (millis() - (unsigned long)(max_hours_without_ventilation) * 60UL * 60UL * 1000UL))
        {
            ventilationOverride = true;
            ventilatorStatusReason = "ventilator off for " + String(max_hours_without_ventilation) + " hours - turning on";
        }
        // reset override after specified time or when we would ventilate anyway
        else if (ventilationOverride && lastTimeVentilatorStatusChange < (millis() - (unsigned long)(ventilation_override_minutes) * 60UL * 1000UL))
        {
            Serial.println("-> Resetting ventilation override");
            ventilationOverride = false;
        }
    }

    if (ventilationOverride)
    {
        ventilatorStatusReason = "ventilationOverride == true";
        newVentilatorStatus = true;
    }
    else if (requestedMode != "AUTO")
    {
        ventilatorStatusReason = "requestedMode == " + requestedMode;
        newVentilatorStatus = (requestedMode == "ON");
    }

    setVentilatorOn(newVentilatorStatus);
    Serial.println("-> Reason: " + ventilatorStatusReason);

    // **** publish vlaues via MQTT ********
    if (mqttClient.connected())
    {
        mqttClient.publish(baseTopic + "sensor-inside/temperature", String(sensorValues.tempInside, 2), false, 1);
        mqttClient.publish(baseTopic + "sensor-inside/humidity", String(sensorValues.humidityInside, 2), false, 1);
        mqttClient.publish(baseTopic + "sensor-inside/dewpoint", String(dewPoint_inside, 2), false, 1);
        mqttClient.publish(baseTopic + "sensor-outside/temperature", String(sensorValues.tempOutside, 2), false, 1);
        mqttClient.publish(baseTopic + "sensor-outside/humidity", String(sensorValues.humidityOutside, 2), false, 1);
        mqttClient.publish(baseTopic + "sensor-outside/dewpoint", String(dewPoint_outside, 2), false, 1);
        mqttClient.publish(baseTopic + "log/ventilatorStatusReason", ventilatorStatusReason, false, 1);
        setSensorOutsideHeaterMode(isOutsideSensorHeaterOn); // publish heater status

        Serial.println(F("published sensor values"));
    }
}

/**
 * Checks the readings from the DHT sensors and returns a SensorCheckResult struct indicating whether the sensors are working correctly.
 * If any of the readings are NaN or outside of the expected range, the function sets the "sensorsOK" field of the result to false and adds an error message to the "errorReason" field.
 *
 * @return A SensorCheckResult struct indicating whether the sensors are working correctly and any error messages if applicable.
 */
SensorValues getSensorValues()
{
    // if the heater is on, turn it off to be able to read the sensor values
    // this loop is reached at least once a minute, so since the heater should not run more than 3 minutes, we can turn it off here
    if (isOutsideSensorHeaterOn)
    {
        Serial.println("-> Heater is on - turning off for reading sensor values");
        setSensorOutsideHeaterMode(false);
        delay(HEATER_COOLDOWN_DELAY); // wait for the sensor to cool down
    }

    SensorValues result = {0, 0, 0, 0, true, ""};

    result.humidityInside = dhtInside.readHumidity() + correction_humidity_inside;
    result.tempInside = dhtInside.readTemperature() + correction_temp_inside;
    sensorOutside.read(); // Read outdoor sensor
    result.humidityOutside = sensorOutside.getHumidity() + correction_humidity_outside;
    result.tempOutside = sensorOutside.getTemperature() + correction_temp_outside;
    float outsideReferenceTemperature = getSensorOutsideReferenceTemperature();
    // if the reference temperature is higher than the outside temperature, use the reference temperature (only check for higher temperatures, since only that is caused by the sun shining on the sensor)
    if (outsideReferenceTemperature != 0 && outsideReferenceTemperature - result.tempOutside < referenceTempDifferenceThreshold)
    {
        outsideReferenceTemperature = outsideReferenceTemperature - referenceTempDifferenceThreshold; // subtract the threshold to prevent base sensor differences from being added
        mqttClient.publish(baseTopic + "log/message", "using reference temp " + String(outsideReferenceTemperature) + " instead of " + String(result.tempOutside), false, 1);
        Serial.println("-> using reference temp " + String(outsideReferenceTemperature) + " instead of " + String(result.tempOutside));
        result.tempOutside = outsideReferenceTemperature;
    }

    // average the humidity values of the outdoor sensor to smooth the readings
    if (lastHumidityOutside == 0)
    {
        lastHumidityOutside = result.humidityOutside;
    }
    else
    {
        float averagedHumidity = (lastHumidityOutside + result.humidityOutside) / 2;
        lastHumidityOutside = result.humidityOutside;
        result.humidityOutside = averagedHumidity;
    }

    if (isnan(result.humidityInside) || isnan(result.tempInside) || result.humidityInside > 100 || result.humidityInside < 1 || result.tempInside < -40 || result.tempInside > 80)
    {
        Serial.println(F("### Error reading from sensor inside: ###"));
        Serial.print(F("-> humidity: "));
        Serial.print(result.humidityInside);
        Serial.print(F("%  temperature: "));
        Serial.print(result.tempInside);
        Serial.println(F("°C"));
        result.sensorsOK = false;
        result.errorReason = "Error reading from sensor inside. ";
    }
    else
    {
        Serial.println(F("sensor inside OK"));
    }

    if (sensorOutside.getError() != 0x00)
    {
        Serial.println(F("Error reading from sensor outside."));
        Serial.print(F("error code: "));
        Serial.println(sensorOutside.getError());
        result.sensorsOK = false;
        result.errorReason.concat("Error reading from sensor outside. ");
    }
    else
    {
        Serial.println(F("sensor outside OK"));
    }

    // if the humidity outside is above 75% and the heater has not been on for the last 5 minutes, turn on the heater.
    // The heater should not run more than 3 minutes and cool down for at least 3 minutes between runs
    if (result.sensorsOK && (result.humidityOutside > HEATER_HUMIDITY_THRESHOLD || result.tempOutside < HEATER_TEMPERATURE_THRESHOLD) && lastTimeHeaterOn < (unsigned long)(millis() - min(millis(), MAX_HEATER_RUNTIME)))
    {
        Serial.println("Current millis: " + String(millis()) + ", Last time heater on: " + String(lastTimeHeaterOn));
        setSensorOutsideHeaterMode(true);
    }
    else if (result.sensorsOK == false) // if the sensor is not working, turn off the heater (we will restart)
    {
        setSensorOutsideHeaterMode(false);
    }

    return result;
}

float getSensorOutsideReferenceTemperature()
{
    if (outsideSensorReferenceTemperature != 0 && millis() - lastTimeSensorOutsideReferenceTemperature > MAX_AGE_OUTSIDE_REFERENCE_TEMPERATURE)
    {
        Serial.println("-> Reference temperature too old - resetting");
        setOutsideReferenceTemperature(0);
        mqttClient.publish(baseTopic + "log/message", "Reference temperature too old - resetting", false, 1);
        return 0;
    }

    return outsideSensorReferenceTemperature;
}

void setOutsideReferenceTemperature(float temp)
{
    outsideSensorReferenceTemperature = temp;
    lastTimeSensorOutsideReferenceTemperature = millis();
    Serial.println("-> Reference temperature set to " + String(temp));
    if (mqttClient.connected())
    {
        mqttClient.publish(baseTopic + "sensor-outside/reference_temperature", String(temp), false, 1);
    }
}

void setSensorOutsideHeaterMode(bool on)
{
    if (on && millis() < 180000UL)
    {
        // do not turn on the heater in the first 3 minutes
        Serial.println("-> Heater not turned on in the first 3 minutes");
    }
    else if (on)
    {
        lastTimeHeaterOn = millis();
        isOutsideSensorHeaterOn = true;
        sensorOutside.heatOn();
    }
    else
    {
        isOutsideSensorHeaterOn = false;
        sensorOutside.heatOff();
    }

    Serial.println("-> Sensor outside heater " + String(isOutsideSensorHeaterOn ? "ON" : "OFF"));

    if (mqttClient.connected())
    {
        mqttClient.publish(baseTopic + "sensor-outside/heater", on ? "1" : "0", true, 1);
    }
}

/**
 * Sets the state of the ventilator to either on or off.
 *
 * @param running A boolean value indicating whether the ventilator should be turned on (true) or off (false).
 *
 * @return void
 */
void setVentilatorOn(bool running)
{
    if (running == true)
    {
        digitalWrite(RELAIPIN, RELAIS_ON); // Turn on relay
        Serial.println(F("-> ventilation ON"));
    }
    else
    {
        digitalWrite(RELAIPIN, RELAIS_OFF); // Turn off relay
        Serial.println(F("-> ventilation OFF"));
    }

    // only send MQTT message if the status has changed
    if (ventilatorStatus != running)
    {
        // save time when ventilator status has changed
        lastTimeVentilatorStatusChange = millis();
        ventilatorStatus = running;

        if (mqttClient.connected())
        {
            mqttClient.publish(baseTopic + "ventilation/state", running ? "ON" : "OFF", true, 1);
            mqttClient.publish(baseTopic + "ventilation/stateNum", running ? "1" : "0", true, 1);
        }
    }
}

/**
 * Calculates the dew point temperature (in Celsius) based on the given temperature (in Celsius) and relative humidity (in percentage).
 * The formula used for the calculation is based on the Magnus-Tetens approximation.
 *
 * @param t The temperature in Celsius.
 * @param r The relative humidity in percentage.
 * @return The dew point temperature in Celsius.
 */
float calculateDewpoint(float t, float r)
{
    float a = 0, b = 0;

    if (t >= 0)
    {
        a = 7.5;
        b = 237.3;
    }
    else if (t < 0)
    {
        a = 7.6;
        b = 240.7;
    }

    // saturation vapor pressure in hPa
    float sdd = 6.1078 * pow(10, (a * t) / (b + t));

    // vapor pressure in hPa
    float dd = sdd * (r / 100);

    // v-parameter
    float v = log10(dd / 6.1078);

    // dew point temperature (°C)
    float tt = (b * v) / (a - v);
    return {tt};
}

/**
 * Initializes the WiFi connection by setting the hostname, enabling auto-reconnect, and storing the configuration in flash memory.
 * If the WiFi is already connected, it will disconnect and forget the settings before attempting to reconnect.
 *
 * @return void
 */
void initializeWiFi()
{
    Serial.println(F("connecting to WiFi..."));

    wifiConnectHandler = WiFi.onStationModeGotIP(onWiFiConnect);
    WiFi.setHostname(wifi_hostname); // Set WiFi hostname.
    WiFi.setAutoReconnect(true);     // Auto reconnect WiFi when connection lost.
    WiFi.persistent(true);           // Store WiFi configuration in flash memory.

#if defined(SECRET_WIFI_SSID) && defined(SECRET_WIFI_PASSWORD)
    WiFi.begin(ssid, password);
    short waitCounter = 6; // wait max 5 seconds for connection
    while (WiFi.status() != WL_CONNECTED && waitCounter >= 0)
    {
        sleepAndBlink(500);
        Serial.print(".");
        waitCounter--;
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println(F("WiFi connection failed - retrying in the background"));
    }
    else
    {
        Serial.println(F("WiFi connected"));
    }
#else
    Serial.println("WiFi not configured");
#endif
    ESP.wdtFeed(); // feed the watchdog
}

/**
 * Callback function that is called when the WiFi connection is established successfully.
 * It sets stopSleeping to true to inform the main loop that the device should not go to sleep.
 *
 * @param event The WiFiEventStationModeGotIP event that triggered the callback.
 *
 * @return void
 */
void onWiFiConnect(const WiFiEventStationModeGotIP &event)
{
    Serial.println("Connected to Wi-Fi sucessfully.");
    ESP.wdtFeed();       // feed the watchdog
    stopSleeping = true; // stop sleeping if WiFi is connected to process MQTT commands immediately.
}

/**
 * This function checks if the MQTT client is connected to the server and connects if necessary.
 * It also subscribes to the necessary MQTT topics for receiving commands.
 * If the MQTT client is already connected, this function does nothing.
 * If the MQTT connection attempt fails, an error message is printed to the serial monitor.
 */
void connectMQTTIfDisconnected()
{
    // Check if WiFi is connected
    if (WiFi.status() == WL_CONNECTED)
    {
// Check if necessary MQTT credentials are defined
#ifndef SECRET_MQTT_USER
#error "SECRET_MQTT_USER not defined"
#endif
#ifndef SECRET_MQTT_PASSWORD
#error "SECRET_MQTT_PASSWORD not defined"
#endif
#ifndef SECRET_MQTT_SERVER
#error "SECRET_MQTT_SERVER not defined"
#endif
#ifndef SECRET_MQTT_CLIENT_ID
#error "SECRET_MQTT_CLIENT_ID not defined"
#endif

        // Check if client is connected to MQTT server, if not, connect
        if (mqttClient.connected() == false)
        {
            Serial.println(F("Connecting to MQTT..."));
            mqttClient.begin(mqtt_server, mqtt_port, wifiClient);
            mqttClient.setKeepAlive(20);
            mqttClient.setTimeout(2000);
            mqttClient.onMessage(mqttCallback);
            mqttClient.setCleanSession(false); // ensure that the device receives all messages after reconnecting

            if (mqttClient.connect(mqtt_clientID, mqtt_user, mqtt_password))
            {
                Serial.println(F("MQTT connected"));
                mqttClient.subscribe(baseTopic + "config/mode/set", 1);
                mqttClient.subscribe(baseTopic + "config/deltaDPmin/set", 1);
                mqttClient.subscribe(baseTopic + "config/hysteresis/set", 1);
                mqttClient.subscribe(baseTopic + "config/tempInside_min/set", 1);
                mqttClient.subscribe(baseTopic + "config/tempOutside_min/set", 1);
                mqttClient.subscribe(baseTopic + "config/tempOutside_max/set", 1);
                mqttClient.subscribe(baseTopic + "config/correction_temp_inside/set", 1);
                mqttClient.subscribe(baseTopic + "config/correction_temp_outside/set", 1);
                mqttClient.subscribe(baseTopic + "config/correction_humidity_inside/set", 1);
                mqttClient.subscribe(baseTopic + "config/correction_humidity_outside/set", 1);
                mqttClient.subscribe(baseTopic + "config/minHumidityForcedVentilation/set", 1);
                mqttClient.subscribe(baseTopic + "config/maxHoursWithoutForcedVentilation/set", 1);
                mqttClient.subscribe(baseTopic + "config/forcedVentilationMinutes/set", 1);
                mqttClient.subscribe(baseTopic + "config/reset", 1);

                mqttClient.subscribe(baseTopic + "sensor-outside/reference_temperature/set", 0);
                Serial.println("command topics subscribed");

                if (startupTime == NULL)
                {
                    startupTime = getTimeString();
                    // if startuptime starts with 1970, we have not set the time yet
                    if (startupTime.startsWith("1970"))
                    {
                        startupTime = NULL;
                    } else {
                        Serial.print(F("Startup time: "));
                        Serial.println(startupTime);
                        mqttClient.publish(baseTopic + "log/startup", startupTime, true, 1);
                    }
                }
            }
            else
            {
                Serial.println(F("MQTT connection failed"));
            }
        }
    }
}

/**
 * @brief Callback function for MQTT messages.
 *
 * This function is called whenever a message is received on a subscribed topic.
 * It parses the topic and payload of the message and updates the relevant variables
 * or configuration values accordingly.
 *
 * @param topic The topic of the received message.
 * @param payload The payload of the received message.
 * @param length The length of the payload.
 */
void mqttCallback(String &topic, String &payload)
{
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    Serial.println(payload);

    if (topic.equals(baseTopic + "config/mode/set"))
    {
        if (payload == "AUTO")
        {
            requestedMode = "AUTO";
            Serial.println("Mode set to AUTO");
        }
        else if (payload == "ON")
        {
            requestedMode = "ON";
            Serial.println("Mode set to ON");
        }
        else if (payload == "OFF")
        {
            requestedMode = "OFF";
            Serial.println("Mode set to OFF");
        }
        else
        {
            requestedMode = "AUTO";
            Serial.println("Unknown mode");
        }
        configChangedMap[CONFIG_IDX_MODE] = true;
        saveConfig();
    }
    else if (topic.equals(baseTopic + "config/deltaDPmin/set"))
    {
        min_delta = payload.toInt();
        configChangedMap[CONFIG_IDX_MIN_DELTA] = true;
        saveConfig();
        Serial.print("min_delta set to ");
        Serial.println(min_delta);
    }
    else if (topic.equals(baseTopic + "config/hysteresis/set"))
    {
        hysteresis = payload.toInt();
        configChangedMap[CONFIG_IDX_HYSTERESIS] = true;
        saveConfig();
        Serial.print("hysteresis set to ");
        Serial.println(hysteresis);
    }
    else if (topic.equals(baseTopic + "config/tempInside_min/set"))
    {
        tempInside_min = payload.toInt();
        configChangedMap[CONFIG_IDX_TEMPINSIDE_MIN] = true;
        saveConfig();
        Serial.print("tempInside_min set to ");
        Serial.println(tempInside_min);
    }
    else if (topic.equals(baseTopic + "config/tempOutside_min/set"))
    {
        tempOutside_min = payload.toInt();
        configChangedMap[CONFIG_IDX_TEMPOUTSIDE_MIN] = true;
        saveConfig();
        Serial.print("tempOutside_min set to ");
        Serial.println(tempOutside_min);
    }
    else if (topic.equals(baseTopic + "config/tempOutside_max/set"))
    {
        tempOutside_max = payload.toInt();
        configChangedMap[CONFIG_IDX_TEMPOUTSIDE_MAX] = true;
        saveConfig();
        Serial.print("tempOutside_max set to ");
        Serial.println(tempOutside_max);
    }
    else if (topic.equals(baseTopic + "config/correction_temp_inside/set"))
    {
        correction_temp_inside = payload.toFloat();
        configChangedMap[CONFIG_IDX_CORRECTION_TEMP_INSIDE] = true;
        saveConfig();
        Serial.print("correction_temp_inside set to ");
        Serial.println(correction_temp_inside);
    }
    else if (topic.equals(baseTopic + "config/correction_temp_outside/set"))
    {
        correction_temp_outside = payload.toFloat();
        configChangedMap[CONFIG_IDX_CORRECTION_TEMP_OUTSIDE] = true;
        saveConfig();
        Serial.print("correction_temp_outside set to ");
        Serial.println(correction_temp_outside);
    }
    else if (topic.equals(baseTopic + "config/correction_humidity_inside/set"))
    {
        correction_humidity_inside = payload.toFloat();
        configChangedMap[CONFIG_IDX_CORRECTION_HUMIDITY_INSIDE] = true;
        saveConfig();
        Serial.print("correction_humidity_inside set to ");
        Serial.println(correction_humidity_inside);
    }
    else if (topic.equals(baseTopic + "config/correction_humidity_outside/set"))
    {
        correction_humidity_outside = payload.toFloat();
        configChangedMap[CONFIG_IDX_CORRECTION_HUMIDITY_OUTSIDE] = true;
        saveConfig();
        Serial.print("correction_humidity_outside set to ");
        Serial.println(correction_humidity_outside);
    }
    else if (topic.equals(baseTopic + "config/minHumidityForcedVentilation/set"))
    {
        min_humidity_for_override = payload.toInt();
        configChangedMap[CONFIG_IDX_MIN_HUMIDITY_FOR_OVERRIDE] = true;
        saveConfig();
        Serial.print("min_humidity_for_override set to ");
        Serial.println(min_humidity_for_override);
    }
    else if (topic.equals(baseTopic + "config/maxHoursWithoutForcedVentilation/set"))
    {
        max_hours_without_ventilation = payload.toInt();
        configChangedMap[CONFIG_IDX_MAX_HOURS_WITHOUT_VENTILATION] = true;
        saveConfig();
        Serial.print("max_hours_without_ventilation set to ");
        Serial.println(max_hours_without_ventilation);
    }
    else if (topic.equals(baseTopic + "config/forcedVentilationMinutes/set"))
    {
        ventilation_override_minutes = payload.toInt();
        configChangedMap[CONFIG_IDX_VENTILATION_OVERRIDE_MINUTES] = true;
        saveConfig();
        Serial.print("ventilation_override_minutes set to ");
        Serial.println(ventilation_override_minutes);
    }
    else if (topic.equals(baseTopic + "config/reference_temp_diff_threshold/set"))
    {
        referenceTempDifferenceThreshold = payload.toFloat();
        configChangedMap[CONFIG_IDX_REFERENCE_TEMP_DIFFERENCE_THRESHOLD] = true;
        saveConfig();
        Serial.print("reference_temp_diff_threshold set to ");
        Serial.println(referenceTempDifferenceThreshold);
    }
    else if (topic.equals(baseTopic + "config/reset"))
    {
        if (payload == "true" or payload == "1")
        {
            resetConfig();
        }
    }
    else if (topic.equals(baseTopic + "sensor-outside/reference_temperature/set"))
    {
        setOutsideReferenceTemperature(payload.toFloat());
    }
    else
    {
        Serial.println("Unknown topic: " + topic);
    }

    stopSleeping = true;
}

/**
 * Publishes a configuration value if it has changed since the last publish.
 *
 * @param configMapIndex The index of the configuration value in the configuration map.
 * @param valueName The name of the configuration value.
 * @param value The new value of the configuration value.
 * @return True if the configuration value was published, false otherwise.
 */
void publishConfigValueIfChanged(short configMapIndex, String valueName, String value)
{
    if (configChangedMap[configMapIndex])
    {
        if (mqttClient.connected())
        {
            configChangedMap[configMapIndex] = !mqttClient.publish(baseTopic + "config/" + valueName, value, true, 1);
            Serial.println("config value published for " + valueName + ". result: " + (!configChangedMap[configMapIndex] ? "true" : "false"));
        }
    }
}

/**
 * This function publishes the current configuration values to the MQTT broker.
 * It publishes the requested mode, minimum delta temperature, hysteresis, minimum inside temperature,
 * minimum outside temperature, and maximum outside temperature.
 * If the MQTT client is not connected, this function does nothing.
 */
void publishConfigIfChanged()
{
    // only publish if value has changed
    publishConfigValueIfChanged(CONFIG_IDX_MODE, "mode", requestedMode);
    publishConfigValueIfChanged(CONFIG_IDX_MIN_DELTA, "deltaDPmin", String(min_delta));
    publishConfigValueIfChanged(CONFIG_IDX_HYSTERESIS, "hysteresis", String(hysteresis));
    publishConfigValueIfChanged(CONFIG_IDX_TEMPINSIDE_MIN, "tempInside_min", String(tempInside_min));
    publishConfigValueIfChanged(CONFIG_IDX_TEMPOUTSIDE_MIN, "tempOutside_min", String(tempOutside_min));
    publishConfigValueIfChanged(CONFIG_IDX_TEMPOUTSIDE_MAX, "tempOutside_max", String(tempOutside_max));
    publishConfigValueIfChanged(CONFIG_IDX_MIN_HUMIDITY_FOR_OVERRIDE, "minHumidityForcedVentilation", String(min_humidity_for_override));
    publishConfigValueIfChanged(CONFIG_IDX_MAX_HOURS_WITHOUT_VENTILATION, "maxHoursWithoutForcedVentilation", String(max_hours_without_ventilation));
    publishConfigValueIfChanged(CONFIG_IDX_VENTILATION_OVERRIDE_MINUTES, "forcedVentilationMinutes", String(ventilation_override_minutes));
    publishConfigValueIfChanged(CONFIG_IDX_CORRECTION_TEMP_INSIDE, "correction_temp_inside", String(correction_temp_inside));
    publishConfigValueIfChanged(CONFIG_IDX_CORRECTION_TEMP_OUTSIDE, "correction_temp_outside", String(correction_temp_outside));
    publishConfigValueIfChanged(CONFIG_IDX_CORRECTION_HUMIDITY_INSIDE, "correction_humidity_inside", String(correction_humidity_inside));
    publishConfigValueIfChanged(CONFIG_IDX_CORRECTION_HUMIDITY_OUTSIDE, "correction_humidity_outside", String(correction_humidity_outside));
    publishConfigValueIfChanged(CONFIG_IDX_REFERENCE_TEMP_DIFFERENCE_THRESHOLD, "reference_temp_diff_threshold", String(referenceTempDifferenceThreshold));
}

/**
 * @brief Blinks the built-in blue LED for a given amount of time.
 *
 * This function blinks the built-in blue LED on the ESP board for a given amount of time.
 * It turns on the LED for 50ms, then turns it off for 50ms, and repeats this process until
 * the specified sleep time has elapsed. The function subtracts 100ms from the sleep time
 * after each iteration to account for the time spent blinking the LED.
 *
 * @param sleepTimeMS The amount of time to sleep and blink the LED, in milliseconds.
 */
void sleepAndBlink(int sleepTimeMS)
{
    while (sleepTimeMS > 0)
    {
        digitalWrite(LED_BUILTIN_BLUE, LOW);  // turn on the LED
        delay(50);                            // wait for 50ms
        digitalWrite(LED_BUILTIN_BLUE, HIGH); // turn off the LED
        delay(50);                            // wait for 50ms
        sleepTimeMS -= 100;                   // subtract 100ms from the delay time
    }
}

/**
 * Returns the current time as a formatted string in ISO 8601 format.
 * The time is obtained from the NTP server using the timeClient object.
 *
 * @return A string representing the current time in ISO 8601 format.
 */
String getTimeString()
{
    String timeString;
    time_t epochTime = time(&epochTime);
    struct tm tm;
    localtime_r(&epochTime, &tm);
    int day = tm.tm_mday;
    int month = tm.tm_mon + 1;
    int year = tm.tm_year + 1900;
    int hour = tm.tm_hour;
    int minute = tm.tm_min;
    int second = tm.tm_sec;

    char buffer[20];
    sprintf(buffer, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month, day, hour, minute, second);
    timeString = String(buffer);

    return timeString;
}

/**
 * Saves the current configuration values to a JSON file on the LittleFS file system.
 *
 * @return True if the configuration was successfully saved, false otherwise.
 */
bool saveConfig()
{
    JsonDocument doc;
    doc["mode"] = requestedMode;
    doc["deltaDPmin"] = min_delta;
    doc["hysteresis"] = hysteresis;
    doc["tempInside_min"] = tempInside_min;
    doc["tempOutside_min"] = tempOutside_min;
    doc["tempOutside_max"] = tempOutside_max;
    doc["min_humidity_for_override"] = min_humidity_for_override;
    doc["max_hours_without_ventilation"] = max_hours_without_ventilation;
    doc["ventilation_override_minutes"] = ventilation_override_minutes;
    doc["correction_temp_inside"] = correction_temp_inside;
    doc["correction_temp_outside"] = correction_temp_outside;
    doc["correction_humidity_inside"] = correction_humidity_inside;
    doc["correction_humidity_outside"] = correction_humidity_outside;
    doc["reference_temp_diff_threshold"] = referenceTempDifferenceThreshold;

    // Open file for writing
    File configFile = LittleFS.open("/config.json", "w");
    if (!configFile)
    {
        Serial.println("Failed to open config file for writing");
        return false;
    }

    // Serialize JSON to file
    serializeJson(doc, configFile);
    configFile.close();
    Serial.println("Config saved to file");
    return true;
}

/**
 * Loads the configuration values from a JSON file on the LittleFS file system.
 * The function reads the configuration file and updates the relevant variables
 * with the values stored in the file.
 *
 * @return True if the configuration was successfully loaded, false otherwise.
 */
bool loadConfig()
{
    File configFile = LittleFS.open("/config.json", "r");
    if (!configFile)
    {
        Serial.println("Failed to open config file");
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, configFile);

    if (error)
    {
        Serial.print(F("Failed to deserialize config file: "));
        Serial.println(error.f_str());
        return false;
    }

    if (doc["mode"].is<String>())
        requestedMode = doc["mode"].as<String>();
    if (doc["deltaDPmin"].is<int>())
        min_delta = doc["deltaDPmin"].as<int>();
    if (doc["hysteresis"].is<int>())
        hysteresis = doc["hysteresis"].as<int>();
    if (doc["tempInside_min"].is<int>())
        tempInside_min = doc["tempInside_min"].as<int>();
    if (doc["tempOutside_min"].is<int>())
        tempOutside_min = doc["tempOutside_min"].as<int>();
    if (doc["tempOutside_max"].is<int>())
        tempOutside_max = doc["tempOutside_max"].as<int>();
    if (doc["min_humidity_for_override"].is<int>())
        min_humidity_for_override = doc["min_humidity_for_override"].as<int>();
    if (doc["max_hours_without_ventilation"].is<int>())
        max_hours_without_ventilation = doc["max_hours_without_ventilation"].as<int>();
    if (doc["ventilation_override_minutes"].is<int>())
        ventilation_override_minutes = doc["ventilation_override_minutes"].as<int>();
    if (doc["correction_temp_inside"].is<float>())
        correction_temp_inside = doc["correction_temp_inside"].as<float>();
    if (doc["correction_temp_outside"].is<float>())
        correction_temp_outside = doc["correction_temp_outside"].as<float>();
    if (doc["correction_humidity_inside"].is<float>())
        correction_humidity_inside = doc["correction_humidity_inside"].as<float>();
    if (doc["correction_humidity_outside"].is<float>())
        correction_humidity_outside = doc["correction_humidity_outside"].as<float>();
    if (doc["reference_temp_diff_threshold"].is<float>())
        referenceTempDifferenceThreshold = doc["reference_temp_diff_threshold"].as<float>();

    configFile.close();
    Serial.println("Config loaded from file:");
    Serial.println("- mode: " + requestedMode);
    Serial.println("- deltaDPmin: " + String(min_delta));
    Serial.println("- hysteresis: " + String(hysteresis));
    Serial.println("- tempInside_min: " + String(tempInside_min));
    Serial.println("- tempOutside_min: " + String(tempOutside_min));
    Serial.println("- tempOutside_max: " + String(tempOutside_max));
    Serial.println("- min_humidity_for_override: " + String(min_humidity_for_override));
    Serial.println("- max_hours_without_ventilation: " + String(max_hours_without_ventilation));
    Serial.println("- ventilation_override_minutes: " + String(ventilation_override_minutes));
    Serial.println("- correction_temp_inside: " + String(correction_temp_inside));
    Serial.println("- correction_temp_outside: " + String(correction_temp_outside));
    Serial.println("- correction_humidity_inside: " + String(correction_humidity_inside));
    Serial.println("- correction_humidity_outside: " + String(correction_humidity_outside));
    Serial.println("- reference_temp_diff_threshold: " + String(referenceTempDifferenceThreshold));

    return true;
}

/**
 * Resets the configuration values to their default values and saves them to storage. The default values are defined in the secrets.h file.
 *
 * @return True if the configuration was successfully saved, false otherwise.
 */
void resetConfig()
{
    requestedMode = "AUTO";
    min_delta = MIN_Delta;
    hysteresis = HYSTERESIS;
    tempInside_min = TEMPINSIDE_MIN;
    tempOutside_min = TEMPOUTSIDE_MIN;
    tempOutside_max = TEMPOUTSIDE_MAX;
    min_humidity_for_override = MIN_HUMIDITY_FOR_OVERRIDE;
    max_hours_without_ventilation = MAX_HOURS_WITHOUT_VENTILATION;
    ventilation_override_minutes = VENTILATION_OVERRIDE_MINUTES;
    correction_temp_inside = CORRECTION_TEMP_INSIDE;
    correction_temp_outside = CORRECTION_TEMP_OUTSIDE;
    correction_humidity_inside = CORRECTION_HUMIDITY_INSIDE;
    correction_humidity_outside = CORRECTION_HUMIDITY_OUTSIDE;
    referenceTempDifferenceThreshold = REFERENCE_TEMPERATURE_DIFFERENCE_THRESHOLD;
    Serial.println("Config reset to default values");
    // mark all config values as changed
    for (size_t i = 0; i < sizeof(configChangedMap); i++)
    {
        configChangedMap[i] = true;
    }

    saveConfig();
}
