#include <Arduino.h>      // for Arduino functions
#include "DHT.h"          // for DHT sensors
#include <ESP8266WiFi.h>  // for WiFi
#include <PubSubClient.h> // for MQTT
#include <NTPClient.h>    // for time sync
#include <WiFiUdp.h>      // for time sync
#include <secrets.h>      // for WiFi and MQTT secrets
#include <settings.h>     // for settings
#include <ArduinoJson.h>  // for JSON parsing (config)
#include "FS.h"           // for file system (config)
#include <LittleFS.h>     // for file system (config)

#define RELAIS_ON HIGH // define the relais on value
#define RELAIS_OFF LOW // define the relais off value

const char *config_path = "/config.json";

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

long unsigned maxMilliSecondsWithoutWiFi = 300000; // maximum time without wifi after which we perform a full reboot
long unsigned lastTimeWiFiOK = ULONG_MAX;          // used to keep track of the last time the WiFi was connected
bool errorOnInitialize = true;                     // used to prevent the microcontroller from freezing when the DHT sensors are not initialized correctly
String startupTime;                                // startup time - if set its been sent. Used to prevent sending the startup message multiple times
bool ventilatorStatus = false;                     // needs to be a global variable, so the state is saved across loops
String baseTopic = "dewppoint-ventilator";         // used to store the MQTT base topic
String requestedMode = "AUTO";                     // default mode after reboot is AUTO
int min_delta = MIN_Delta;                         // minimum difference between the dew points inside and outside to turn on the ventilator
int hysteresis = HYSTERESIS;                       // hysteresis for turning off the ventilator
int tempInside_min = TEMPINSIDE_MIN;               // minimum temperature inside to turn on the ventilator
int tempOutside_min = TEMPOUTSIDE_MIN;             // minimum temperature outside to turn on the ventilator
int tempOutside_max = TEMPOUTSIDE_MAX;             // maximum temperature outside to turn on the ventilator
bool stopSleeping = false;                         // a simple flag to prevent the microcontroller from going to sleep - set from a different thread on wifi-connected
WiFiEventHandler wifiConnectHandler;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, TIME_SERVER, 3600, 60000);

DHT dhtInside(DHTPIN_INSIDE, DHTTYPE_INSIDE);    // The indoor sensor is now addressed with dhtInside
DHT dhtOutside(DHTPIN_OUTSIDE, DHTTYPE_OUTSIDE); // The outdoor sensor is now addressed with dhtOutside

/**
 * This struct that represents the result of checking the DHT sensors.
 * It contains a boolean field "sensorsOK" indicating whether the sensors are working correctly,
 * and a string field "errorReason" containing any error messages if applicable.
 */
struct SensorCheckResult
{
    bool sensorsOK;
    String errorReasoun;
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

    Serial.begin(115200); // Start serial communication

    setVentilatorOn(ventilatorStatus); // Turn off ventilator
    initializeWiFi();                  // Initialize WiFi connection

    Serial.println(F("Testing sensors..."));

    dhtInside.begin();  // Start indoor sensor
    dhtOutside.begin(); // Start outdoor sensor

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

    // load config from file if it exists
    if (loadConfig() == false)
    {
        // default values will be used instead
        Serial.println("Failed to load config from file - saving default values in config");
        resetConfig();
    }
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
    else if (millis() - lastTimeWiFiOK > maxMilliSecondsWithoutWiFi)
    {
        ESP.restart();
    }

    digitalWrite(LED_BUILTIN_BLUE, LOW); // Turn on LED when loop is active
    connectMQTTIfDisconnected();         // Connect to MQTT if not connected do this at the beginning so it can run in the background
    publishConfig();
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
                mqttClient.publish((baseTopic + "log/heartbeat").c_str(), getTimeString().c_str(), true);
            mqttClient.loop(); // Check for MQTT messages
            // if an MQTT command was received, stop sleeping and process the command
        }
        if (stopSleeping)
        {
            stopSleeping = false;
            break;
        }
        delay(1000);
        ESP.wdtFeed();
    }
}

/**
 * Calculates the difference between the dew points inside and outside, and sets the ventilator status accordingly.
 * If the difference is greater than the minimum delta plus the hysteresis, the ventilator is turned on.
 * Otherwise, the ventilator is turned off.
 */
void calculateAndSetVentilatorStatus()
{
    if (errorOnInitialize == true) // Check if valid values are coming from the sensors (only during first call)
    {
        SensorCheckResult sensorStatus = checkSensors();
        errorOnInitialize = sensorStatus.sensorsOK == false;

        if (sensorStatus.sensorsOK == false)
        {
            digitalWrite(RELAIPIN, RELAIS_OFF); // Turn off ventilator

            if (mqttClient.connected())
            {
                mqttClient.publish((baseTopic + "log/status").c_str(), ("error during initialization: " + sensorStatus.errorReasoun).c_str());
                Serial.println(F("Error message sent"));
                delay(500); // wait for message to be sent
            }

            Serial.println(F("Restarting..."));
            ESP.restart();
        }
        else
        {
            if (mqttClient.connected())
            {
                mqttClient.publish((baseTopic + "log/status").c_str(), "initialized");
            }
        }
    }

    ESP.wdtFeed(); // feed the watchdog

    float humidityInside = dhtInside.readHumidity() + CORRECTION_HUMIDITY_INSIDE;    // Read indoor humidity and store it under "h1"
    float tempInside = dhtInside.readTemperature() + CORRECTION_TEMP_INSIDE;         // Read indoor temperature and store it under "t1"
    float humidityOutside = dhtOutside.readHumidity() + CORRECTION_HUMIDITY_OUTSIDE; // Read outdoor humidity and store it under "h2"
    float tempOutside = dhtOutside.readTemperature() + CORRECTION_TEMP_OUTSIDE;      // Read outdoor temperature and store it under "t2"

    //**** Print sensor values********
    Serial.print(F("sensor-inside: "));
    Serial.print(F("hunidity: "));
    Serial.print(humidityInside);
    Serial.print(F("%  temperature: "));
    Serial.print(tempInside);
    Serial.println(F("°C"));

    Serial.print("sensor-outside: ");
    Serial.print(F("hunidity: "));
    Serial.print(humidityOutside);
    Serial.print(F("%  temperature: "));
    Serial.print(tempOutside);
    Serial.println(F("°C"));

    //**** Calculate dew points********
    Serial.println(F("calculating dew point..."));
    float dewPoint_inside = calculateDewpoint(tempInside, humidityInside);
    float dewPoint_outside = calculateDewpoint(tempOutside, humidityOutside);

    //**** Print dew points********
    Serial.print(F("sensor-inside dew point: "));
    Serial.print(dewPoint_inside);
    Serial.println(F("°C  "));

    Serial.print(F("sensor-outside dew point: "));
    Serial.print(dewPoint_outside);
    Serial.println(F("°C  "));

    //**** Calculate difference between dew points********
    float deltaTP = dewPoint_inside - dewPoint_outside;

    //**** decide if ventilator should run or not ********
    String ventilatorStatusReason = "Hysteresis phase";
    if (deltaTP > (min_delta + hysteresis))
    {
        ventilatorStatus = true;
        ventilatorStatusReason = "DeltaTP > (MIN_Delta + HYSTERESIS): " + String(deltaTP) + " > " + String(min_delta) + " + " + String(hysteresis);
    }
    else if (deltaTP <= (min_delta))
    {
        ventilatorStatus = false;
        ventilatorStatusReason = "DeltaTP < (MIN_Delta): " + String(deltaTP) + " < " + String(min_delta);
    }

    // check overrides
    if (ventilatorStatus && tempInside < tempInside_min)
    {
        ventilatorStatus = false;
        ventilatorStatusReason = "tempInside < TEMPINSIDE_MIN: " + String(tempInside) + " < " + String(tempInside_min);
    }
    else if (ventilatorStatus && tempOutside < tempOutside_min)
    {
        ventilatorStatus = false;
        ventilatorStatusReason = "tempOutside < TEMPOUTSIDE_MIN: " + String(tempOutside) + " < " + String(tempOutside_min);
    }
    else if (ventilatorStatus && tempOutside > tempOutside_max)
    {
        ventilatorStatus = false;
        ventilatorStatusReason = "tempOutside > TEMPOUTSIDE_MAX: " + String(tempOutside) + " > " + String(tempOutside_max);
    }

    if (requestedMode == "ON")
    {
        ventilatorStatusReason = "requestedMode == ON";
        setVentilatorOn(true);
    }
    else if (requestedMode == "OFF")
    {
        ventilatorStatusReason = "requestedMode == OFF";
        setVentilatorOn(false);
    }
    else // mode == auto or unknown
    {
        setVentilatorOn(ventilatorStatus);
    }

    // **** publish vlaues via MQTT ********
    if (mqttClient.connected())
    {
        mqttClient.publish((baseTopic + "sensor-inside/temperature").c_str(), String(tempInside, 2).c_str());
        mqttClient.publish((baseTopic + "sensor-inside/humidity").c_str(), String(humidityInside, 2).c_str());
        mqttClient.publish((baseTopic + "sensor-inside/dewpoint").c_str(), String(dewPoint_inside, 2).c_str());
        mqttClient.publish((baseTopic + "sensor-outside/temperature").c_str(), String(tempOutside, 2).c_str());
        mqttClient.publish((baseTopic + "sensor-outside/humidity").c_str(), String(humidityOutside, 2).c_str());
        mqttClient.publish((baseTopic + "sensor-outside/dewpoint").c_str(), String(dewPoint_outside, 2).c_str());
        mqttClient.publish((baseTopic + "ventilation/reason").c_str(), ventilatorStatusReason.c_str());
        Serial.println(F("published to MQTT"));
    }
}

/**
 * Checks the readings from the DHT sensors and returns a SensorCheckResult struct indicating whether the sensors are working correctly.
 * If any of the readings are NaN or outside of the expected range, the function sets the "sensorsOK" field of the result to false and adds an error message to the "errorReason" field.
 *
 * @return A SensorCheckResult struct indicating whether the sensors are working correctly and any error messages if applicable.
 */
SensorCheckResult checkSensors()
{
    SensorCheckResult result = {true, ""};

    float humidityInside = dhtInside.readHumidity();   // Read indoor humidity and store it under "h1"
    float tempInside = dhtInside.readTemperature();    // Read indoor temperature and store it under "t1"
    float humidityOutside = dhtOutside.readHumidity(); // Read outdoor humidity and store it under "h2"
    float tempOutside = dhtOutside.readTemperature();  // Read outdoor temperature and store it under "t2"

    if (isnan(humidityInside) || isnan(tempInside) || humidityInside > 100 || humidityInside < 1 || tempInside < -40 || tempInside > 80)
    {
        Serial.println(F("Error reading from sensor inside."));
        result.sensorsOK = false;
        result.errorReasoun = "Error reading from sensor inside. ";
    }
    else
    {
        Serial.println(F("sensor inside OK"));
    }

    if (isnan(humidityOutside) || isnan(tempOutside) || humidityOutside > 100 || humidityOutside < 1 || tempOutside < -40 || tempOutside > 80)
    {
        Serial.println(F("Error reading from sensor outside."));
        result.sensorsOK = false;
        result.errorReasoun.concat("Error reading from sensor outside. ");
    }
    else
    {
        Serial.println(F("sensor outside OK"));
    }

    return result;
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

    if (mqttClient.connected())
    {
        mqttClient.publish((baseTopic + "ventilation/state").c_str(), running ? "ON" : "OFF");
        mqttClient.publish((baseTopic + "ventilation/stateNum").c_str(), running ? "1" : "0");
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
            mqttClient.setServer(mqtt_server, mqtt_port);
            mqttClient.setCallback(mqttCallback);

            if (mqttClient.connect(mqtt_clientID, mqtt_user, mqtt_password))
            {
                Serial.println(F("MQTT connected"));
                mqttClient.subscribe((baseTopic + "config/mode/set").c_str());
                mqttClient.subscribe((baseTopic + "config/deltaTPmin/set").c_str());
                mqttClient.subscribe((baseTopic + "config/hysteresis/set").c_str());
                mqttClient.subscribe((baseTopic + "config/tempInside_min/set").c_str());
                mqttClient.subscribe((baseTopic + "config/tempOutside_min/set").c_str());
                mqttClient.subscribe((baseTopic + "config/tempOutside_max/set").c_str());
                mqttClient.subscribe((baseTopic + "config/reset").c_str());
                Serial.println("command topics subscribed");

                if (startupTime == NULL)
                {
                    if (mqttClient.connected() && connectNTPClient())
                    {
                        startupTime = getTimeString();

                        Serial.print(F("Startup time: "));
                        Serial.println(startupTime);
                        mqttClient.publish((baseTopic + "log/startup").c_str(), startupTime.c_str(), true);
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
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    String payloadStr = "";
    for (unsigned int i = 0; i < length; i++)
    {
        payloadStr += (char)payload[i];
    }
    Serial.println(payloadStr);

    if (strcmp(topic, (baseTopic + "config/mode/set").c_str()) == 0)
    {
        // code to execute if topic equals baseTopic + "mode/set"
        if (payloadStr == "AUTO")
        {
            requestedMode = "AUTO";
            Serial.println("Mode set to AUTO");
        }
        else if (payloadStr == "ON")
        {
            requestedMode = "ON";
            Serial.println("Mode set to ON");
        }
        else if (payloadStr == "OFF")
        {
            requestedMode = "OFF";
            Serial.println("Mode set to OFF");
        }
        else
        {
            requestedMode = "AUTO";
            Serial.println("Unknown mode");
        }
    }
    else if (strcmp(topic, (baseTopic + "config/deltaTPmin/set").c_str()) == 0)
    {
        // code to execute if topic equals baseTopic + "config/deltaTPmin/set"
        min_delta = payloadStr.toInt();
        Serial.print("min_delta set to ");
        Serial.println(min_delta);
    }
    else if (strcmp(topic, (baseTopic + "config/hysteresis/set").c_str()) == 0)
    {
        // code to execute if topic equals baseTopic + "config/hysteresis/set"
        hysteresis = payloadStr.toInt();
        Serial.print("hysteresis set to ");
        Serial.println(hysteresis);
    }
    else if (strcmp(topic, (baseTopic + "config/tempInside_min/set").c_str()) == 0)
    {
        // code to execute if topic equals baseTopic + "config/tempInside_min/set"
        tempInside_min = payloadStr.toInt();
        Serial.print("tempInside_min set to ");
        Serial.println(tempInside_min);
    }
    else if (strcmp(topic, (baseTopic + "config/tempOutside_min/set").c_str()) == 0)
    {
        // code to execute if topic equals baseTopic + "config/tempOutside_min/set"
        tempOutside_min = payloadStr.toInt();
        Serial.print("tempOutside_min set to ");
        Serial.println(tempOutside_min);
    }
    else if (strcmp(topic, (baseTopic + "config/tempOutside_max/set").c_str()) == 0)
    {
        // code to execute if topic equals baseTopic + "config/tempOutside_max/set"
        tempOutside_max = payloadStr.toInt();
        Serial.print("tempOutside_max set to ");
        Serial.println(tempOutside_max);
    }
    else if (strcmp(topic, (baseTopic + "config/reset").c_str()) == 0)
    {
        // code to execute if topic equals baseTopic + "config/reset"
        if (payloadStr == "true" or payloadStr == "1")
        {
            resetConfig();
        }
    }

    stopSleeping = true;
}

/**
 * This function publishes the current configuration values to the MQTT broker.
 * It publishes the requested mode, minimum delta temperature, hysteresis, minimum inside temperature,
 * minimum outside temperature, and maximum outside temperature.
 * If the MQTT client is not connected, this function does nothing.
 */
void publishConfig()
{
    if (mqttClient.connected())
    {
        mqttClient.publish((baseTopic + "config/mode").c_str(), requestedMode.c_str());
        mqttClient.publish((baseTopic + "config/deltaTPmin").c_str(), String(min_delta).c_str());
        mqttClient.publish((baseTopic + "config/hysteresis").c_str(), String(hysteresis).c_str());
        mqttClient.publish((baseTopic + "config/tempInside_min").c_str(), String(tempInside_min).c_str());
        mqttClient.publish((baseTopic + "config/tempOutside_min").c_str(), String(tempOutside_min).c_str());
        mqttClient.publish((baseTopic + "config/tempOutside_max").c_str(), String(tempOutside_max).c_str());
        Serial.println("config published");
    }
}

/**
 * This function connects to the NTP server and updates the time.
 * It returns true if the time was successfully updated, false otherwise.
 */
bool connectNTPClient()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println(F("Connecting to NTP server..."));
        timeClient.begin();
        int retries = 0;
        while (!timeClient.update() && retries < 10)
        {
            timeClient.forceUpdate();
            retries++;
            delay(500);
        }

        // if time was successfully updated, return true
        if (retries < 10)
        {
            Serial.println(F("NTP time updated"));
            return true;
        }
        else
        {
            Serial.println(F("NTP time update failed"));
            return false;
        }
    }
    else
    {
        return false;
    }
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
    time_t epochTime = timeClient.getEpochTime();
    struct tm *ptm = gmtime((time_t *)&epochTime);

    int day = ptm->tm_mday;
    int month = ptm->tm_mon + 1;
    int year = ptm->tm_year + 1900;
    int hour = ptm->tm_hour;
    int minute = ptm->tm_min;
    int second = ptm->tm_sec;

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
    StaticJsonDocument<200> doc;
    doc["mode"] = requestedMode;
    doc["deltaTPmin"] = min_delta;
    doc["hysteresis"] = hysteresis;
    doc["tempInside_min"] = tempInside_min;
    doc["tempOutside_min"] = tempOutside_min;
    doc["tempOutside_max"] = tempOutside_max;

    // Open file for writing
    File configFile = LittleFS.open(config_path, "w");
    if (!configFile)
    {
        Serial.println("Failed to open config file for writing");
        return false;
    }

    // Serialize JSON to file
    serializeJson(doc, configFile);
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
    File configFile = LittleFS.open(config_path, "r");
    if (!configFile)
    {
        Serial.println("Failed to open config file");
        return false;
    }

    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, configFile);

    if (error)
    {
        Serial.print(F("Failed to open & deserialize config file: "));
        Serial.println(error.f_str());
        return false;
    }

    requestedMode = doc["mode"].as<String>();
    min_delta = doc["deltaTPmin"].as<float>();
    hysteresis = doc["hysteresis"].as<float>();
    tempInside_min = doc["tempInside_min"].as<float>();
    tempOutside_min = doc["tempOutside_min"].as<float>();
    tempOutside_max = doc["tempOutside_max"].as<float>();

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
    Serial.println("Config reset to default values");
    saveConfig();
}