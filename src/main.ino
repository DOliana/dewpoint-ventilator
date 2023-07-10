#include <Arduino.h>
#include "DHT.h"
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <secrets.h>
#include <NTPClient.h> // for time sync
#include <WiFiUdp.h>   // for time sync

#define RELAIPIN 12 // connection for ventilator relais switch
#define DHTPIN_INSIDE 13 // data line for DHT sensor 1 (inside)
#define DHTPIN_OUTSIDE 14 // data line for DHT sensor 2 (outside)

#define DHTTYPE_INSIDE DHT22 // DHT 22
#define DHTTYPE_OUTSIDE DHT22 // DHT 22

#define LED_BUILTIN_RED LED_BUILTIN
#define LED_BUILTIN_BLUE 2

// ******* Correction values for individual sensor values ***********
#define CORRECTION_TEMP_INSIDE 0     // correction value for indoor sensor temperature
#define CORRECTION_TEMP_OUTSIDE -0.5   // correction value for outdoor sensor temperature
#define CORRECTION_HUMIDITY_INSIDE 0 // correction value for indoor sensor humidity
#define CORRECTION_HUMIDITY_OUTSIDE -0.6 // correction value for outdoor sensor humidity
//*******************************************************************

#define MIN_Delta 5.0   // minimum dew point difference at which the relay switches
#define HYSTERESIS 1.0   // distance from switch-on and switch-off point
#define TEMPINSIDE_MIN 10.0  // minimum indoor temperature at which ventilation is activated
#define TEMPOUTSIDE_MIN -10.0 // minimum outdoor temperature at which ventilation is activated
#define TEMPOUTSIDE_MAX 25.0  // maximum outdoor temperature at which ventilation is activated

// *************************** END OF SETTINGS SECTION ***************************
#define RELAIS_ON HIGH
#define RELAIS_OFF LOW

// ********* Wifi + MQTT settings (values defined in secret.h) ******
const char *mqtt_server = SECRET_MQTT_SERVER;
const char *mqtt_user = SECRET_MQTT_USER;
const char *mqtt_password = SECRET_MQTT_PASSWORD;
#ifdef SECRET_MQTT_BASETOPIC
const char *mqtt_baseTopic = SECRET_MQTT_BASETOPIC;
#else
const char *mqtt_baseTopic = "";
#endif
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

//*******************************************************************

bool errorOnInitialize = true;
bool firstStartup = true;
int wifiErrorCounter = 0;
String baseTopic = "";
String requestedMode = "AUTO";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 3600, 60000);

DHT dhtInside(DHTPIN_INSIDE, DHTTYPE_INSIDE); // The indoor sensor is now addressed with dhtInside
DHT dhtOutside(DHTPIN_OUTSIDE, DHTTYPE_OUTSIDE); // The outdoor sensor is now addressed with dhtOutside

void setup()
{
    ESP.wdtEnable(WDTO_8S);

    pinMode(LED_BUILTIN_RED, OUTPUT);  // Define LED pin as output
    pinMode(LED_BUILTIN_BLUE, OUTPUT); // Define LED pin as output

    digitalWrite(LED_BUILTIN_RED, LOW); // Turn on LED to show we have power
    pinMode(RELAIPIN, OUTPUT);          // Define relay pin as output
    digitalWrite(RELAIPIN, RELAIS_OFF); // Turn off relay

    Serial.begin(115200);
    Serial.println(F("Testing sensors..."));

    dhtInside.begin(); // Start sensors
    dhtOutside.begin();

    mqttClient.setCallback(mqttCallback);

    // set baseTopic to use for MQTT messages
#ifdef SECRET_MQTT_BASETOPIC
    baseTopic = mqtt_baseTopic;
    if (baseTopic.endsWith("/") == false)
    {
        baseTopic.concat("/");
    }
#endif
    Serial.print("MQTT base topic set: ");
    Serial.println(baseTopic);
}

void loop()
{
    digitalWrite(LED_BUILTIN_BLUE, LOW);                    // Turn on LED when loop is active
    connectWifiIfNecessary();                               // Connect to Wifi if not connected do this at the beginning so it can run in the background
    connectMQTTIfNecessary();                               // Connect to MQTT if not connected do this at the beginning so it can run in the background
    mqttClient.loop();                                      // Check for MQTT messages
    calculateAndSetVentilatorStatus();

    // this is the first time the loop is run, so we can post the startup time to MQTT for monitoring reboots
    if (firstStartup)
    {
        if (mqttClient.connected() && connectNTPClient())
        {
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
            String startupTime = String(buffer);

            Serial.print(F("Startup time: "));
            Serial.println(startupTime);
            mqttClient.publish((baseTopic + "startup").c_str(), startupTime.c_str(), true);
            firstStartup = false;
        }
    }

    Serial.println();

    delay(100);                           // delay required for led to turn of
    digitalWrite(LED_BUILTIN_BLUE, HIGH); // Turn off LED while sleeping

    delay(9900);
    ESP.wdtFeed();
}

void calculateAndSetVentilatorStatus(){

    float humidityInside = dhtInside.readHumidity() + CORRECTION_HUMIDITY_INSIDE; // Read indoor humidity and store it under "h1"
    float tempInside = dhtInside.readTemperature() + CORRECTION_TEMP_INSIDE;  // Read indoor temperature and store it under "t1"
    float humidityOutside = dhtOutside.readHumidity() + CORRECTION_HUMIDITY_OUTSIDE; // Read outdoor humidity and store it under "h2"
    float tempOutside = dhtOutside.readTemperature() + CORRECTION_TEMP_OUTSIDE;  // Read outdoor temperature and store it under "t2"

    String errorString = "";
    if (errorOnInitialize == true) // Check if valid values are coming from the sensors (only during first call)
    {
        errorOnInitialize = false;
        if (isnan(humidityInside) || isnan(tempInside) || humidityInside > 100 || humidityInside < 1 || tempInside < -40 || tempInside > 80)
        {
            Serial.println(F("Error reading from sensor inside."));
            errorString.concat("Error reading from sensor inside. ");
            errorOnInitialize = true;
        }
        else
        {
            Serial.println(F("sensor inside OK"));
        }

        delay(1000);

        if (isnan(humidityOutside) || isnan(tempOutside) || humidityOutside > 100 || humidityOutside < 1 || tempOutside < -40 || tempOutside > 80)
        {
            Serial.println(F("Error reading from sensor outside."));
            errorString.concat("Error reading from sensor outside. ");
            errorOnInitialize = true;
        }
        else
        {
            Serial.println(F("sensor outside OK"));
        }

        delay(1000);
    }
    if (isnan(humidityInside) || isnan(tempInside) || isnan(humidityOutside) || isnan(tempOutside))
        errorOnInitialize = true;

    if (errorOnInitialize == true)
    {
        digitalWrite(RELAIPIN, RELAIS_OFF); // Turn off ventilator
        
        if (mqttClient.connected())
        {
            mqttClient.publish((baseTopic + "status").c_str(), ("error during initialization: " + errorString).c_str());
            Serial.println(F("Error message sent"));
            delay(500);
        }
        Serial.println(F("Restarting..."));
        while (1)
            ; // Endless loop to restart the CPU through the watchdog
    } else {
        if (mqttClient.connected())
        {
            mqttClient.publish((baseTopic + "status").c_str(), "initialized");
        }
    }

    ESP.wdtFeed(); // feed the watchdog

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
    float DeltaTP = dewPoint_inside - dewPoint_outside;

    //**** decide if ventilator should run or not ********
    bool ventilatorStatus = false;
    String ventilatorStatusReason = "";
    if (DeltaTP > (MIN_Delta + HYSTERESIS))
    {
        ventilatorStatus = true;
        ventilatorStatusReason = "DeltaTP > (MIN_Delta + HYSTERESE)";
    }
    if (DeltaTP < (MIN_Delta))
    {
        ventilatorStatus = false;
        ventilatorStatusReason = "DeltaTP < (MIN_Delta)";
    }
    if (tempInside < TEMPINSIDE_MIN)
    {
        ventilatorStatus = false;
        ventilatorStatusReason = "tempInside < TEMPINSIDE_MIN";
    }
    if (tempOutside < TEMPOUTSIDE_MIN)
    {
        ventilatorStatus = false;
        ventilatorStatusReason = "tempOutside < TEMPOUTSIDE_MIN";
    }
    if (tempOutside > TEMPOUTSIDE_MAX)
    {
        ventilatorStatus = false;
        ventilatorStatusReason = "tempOutside > TEMPOUTSIDE_MAX";
    }

    if(requestedMode == "AUTO") {
        setVentilatorOn(ventilatorStatus);
    } else if (requestedMode == "ON") {
        ventilatorStatusReason = "requestedMode == ON";
        setVentilatorOn(true);
    } else if (requestedMode == "OFF") {
        ventilatorStatusReason = "requestedMode == OFF";
        setVentilatorOn(false);
    }
    if(mqttClient.connected()){
        mqttClient.publish((baseTopic + "mode").c_str(), requestedMode.c_str());
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

void setVentilatorOn(bool running){
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

    if(mqttClient.connected()){
        mqttClient.publish((baseTopic + "ventilation/state").c_str(), running ? "ON" : "OFF");
        mqttClient.publish((baseTopic + "ventilation/stateNum").c_str(), running ? "1" : "0");
    }
}

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

// This function checks if the WiFi is connected and tries to connect if it is not.
// If the connection is unsuccessful, it will retry every 20 loops.
void connectWifiIfNecessary()
{
    // if connection is unsuccessful, try again every 20 loops
    if (WiFi.status() != WL_CONNECTED)
    {
#if defined(SECRET_WIFI_SSID) && defined(SECRET_WIFI_PASSWORD)
        Serial.println(F("WiFi disconnected"));
        if (wifiErrorCounter <= 0)
        {
            Serial.println(F("connecting to WiFi..."));
            WiFi.setHostname(wifi_hostname);
            WiFi.begin(ssid, password);
            int waitCounter = 0;
            while (WiFi.status() != WL_CONNECTED && waitCounter < 10)
            {
                blinkDelay(500);
                Serial.print(".");
                waitCounter++;
            }

            if (WiFi.status() != WL_CONNECTED)
            {
                Serial.println(F("WiFi connection failed"));
                wifiErrorCounter = 20;
            }
            else
            {
                Serial.println(F("WiFi connected"));
                connectMQTTIfNecessary();
            }
        }
        else
        {
            wifiErrorCounter--;
            Serial.print("retrying wifi connection in ");
            Serial.print(wifiErrorCounter);
            Serial.println(" loops");
        }
#else
        Serial.println("WiFi not configured");
#endif
    }
    else
    {
        // wifi connection continues even if we don't wait for it - we need to check periodically
        // even if WiFi connection failed in the first place.
        connectMQTTIfNecessary();
    }
}

// This function connects to the MQTT server if necessary
void connectMQTTIfNecessary()
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
            Serial.println(F("WiFi connected. Connecting to MQTT..."));
            mqttClient.setServer(mqtt_server, mqtt_port);
            if (mqttClient.connect(mqtt_clientID, mqtt_user, mqtt_password))
            {
                Serial.println(F("MQTT connected"));
                mqttClient.subscribe((baseTopic + "mode/set").c_str());
            }
            else
            {
                Serial.println(F("MQTT connection failed"));
            }
        }
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

// This function blinks the built-in LED with a given delay time
void blinkDelay(int delayTime)
{
    while (delayTime > 0)
    {
        digitalWrite(LED_BUILTIN_BLUE, LOW);  // turn on the LED
        delay(50);                            // wait for 50ms
        digitalWrite(LED_BUILTIN_BLUE, HIGH); // turn off the LED
        delay(50);                            // wait for 50ms
        delayTime -= 100;                     // subtract 100ms from the delay time
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    String payloadStr = "";
    for (unsigned int i = 0; i < length; i++) {
        payloadStr += (char)payload[i];
    }
    Serial.println(payloadStr);

    // Switch on the LED if an 1 was received as first character
    if (payloadStr == "AUTO") {
        requestedMode = "AUTO";
        Serial.println("Mode set to AUTO");
    } else if (payloadStr == "ON") {
        requestedMode = "ON";
        Serial.println("Mode set to ON");
    } else if (payloadStr == "OFF") {
        requestedMode = "OFF";
        Serial.println("Mode set to OFF");
    } else {
        requestedMode = "AUTO";
        Serial.println("Unknown mode");
    }
}