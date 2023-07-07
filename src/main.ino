#include <Arduino.h>
#include "DHT.h"
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <secrets.h>
#include <NTPClient.h> // for time sync
#include <WiFiUdp.h>   // for time sync

#define RELAIPIN 16 // connection for ventilator relais switch
#define DHTPIN_1 13 // data line for DHT sensor 1 (inside)
#define DHTPIN_2 14 // data line for DHT sensor 2 (outside)

#define DHTTYPE_1 DHT22 // DHT 22
#define DHTTYPE_2 DHT22 // DHT 22

// ******* Correction values for individual sensor values ***********
#define CORRECTION_temp_1 0     // correction value for indoor sensor temperature
#define CORRECTION_temp_2 0.1   // correction value for outdoor sensor temperature
#define CORRECTION_humidity_1 0 // correction value for indoor sensor humidity
#define CORRECTION_humidity_2 0 // correction value for outdoor sensor humidity
//*******************************************************************

#define MIN_Delta 5.0   // minimum dew point difference at which the relay switches
#define HYSTERESE 1.0   // distance from switch-on and switch-off point
#define TEMP1_min 10.0  // minimum indoor temperature at which ventilation is activated
#define TEMP2_min -10.0 // minimum outdoor temperature at which ventilation is activated

// *************************** END OF SETTINGS SECTION ***************************
#define RELAIS_ON LOW
#define RELAIS_OFF HIGH
bool ventilatorStatus;

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

WiFiClient espClient;
PubSubClient client(espClient);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 3600, 60000);

DHT dht1(DHTPIN_1, DHTTYPE_1); // The indoor sensor is now addressed with dht1
DHT dht2(DHTPIN_2, DHTTYPE_2); // The outdoor sensor is now addressed with dht2

void setup()
{
    ESP.wdtEnable(WDTO_8S);

    pinMode(LED_BUILTIN, OUTPUT);       // Define LED pin as output
    digitalWrite(0, HIGH);              // Turn on LED to show some activity
    pinMode(RELAIPIN, OUTPUT);          // Define relay pin as output
    digitalWrite(RELAIPIN, RELAIS_OFF); // Turn off relay

    Serial.begin(115200);
    Serial.println(F("Testing sensors..."));

    dht1.begin(); // Start sensors
    dht2.begin();
}

void loop()
{
    digitalWrite(LED_BUILTIN, LOW);                         // Turn on LED when loop is active
    connectWifiIfNecessary();                               // Connect to Wifi if not connected do this at the beginning so it can run in the background
    float h1 = dht1.readHumidity() + CORRECTION_humidity_1; // Read indoor humidity and store it under "h1"
    float t1 = dht1.readTemperature() + CORRECTION_temp_1;  // Read indoor temperature and store it under "t1"
    float h2 = dht2.readHumidity() + CORRECTION_humidity_2; // Read outdoor humidity and store it under "h2"
    float t2 = dht2.readTemperature() + CORRECTION_temp_2;  // Read outdoor temperature and store it under "t2"

    if (errorOnInitialize == true) // Check if valid values are coming from the sensors
    {
        errorOnInitialize = false;
        if (isnan(h1) || isnan(t1) || h1 > 100 || h1 < 1 || t1 < -40 || t1 > 80)
        {
            Serial.println(F("Error reading from sensor 1."));
            errorOnInitialize = true;
        }
        else
        {
            Serial.println(F("Sensor 1 OK"));
        }

        delay(1000);

        if (isnan(h2) || isnan(t2) || h2 > 100 || h2 < 1 || t2 < -40 || t2 > 80)
        {
            Serial.println(F("Error reading from sensor 2."));
            errorOnInitialize = true;
        }
        else
        {
            Serial.println(F("Sensor 2 OK"));
        }

        delay(1000);

        // set baseTopic to use for MQTT messages

        String baseTopic = "";
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
    if (isnan(h1) || isnan(t1) || isnan(h2) || isnan(t2))
        errorOnInitialize = true;

    if (errorOnInitialize == true)
    {
        digitalWrite(RELAIPIN, RELAIS_OFF); // Turn off ventilator
        Serial.println(F("Restarting..."));
        while (1)
            ; // Endless loop to restart the CPU through the watchdog
    }

    ESP.wdtFeed(); // feed the watchdog

    //**** Print sensor values********
    Serial.print(F("Sensor-1: "));
    Serial.print(F("hunidity: "));
    Serial.print(h1);
    Serial.print(F("%  temperature: "));
    Serial.print(t1);
    Serial.println(F("°C"));

    Serial.print("Sensor-2: ");
    Serial.print(F("hunidity: "));
    Serial.print(h2);
    Serial.print(F("%  temperature: "));
    Serial.print(t2);
    Serial.println(F("°C"));

    //**** Calculate dew points********
    Serial.println(F("calculating dew point..."));
    float dewPoint_1 = calculateDewpoint(t1, h1);
    float dewPoint_2 = calculateDewpoint(t2, h2);

    //**** Print dew points********
    Serial.print(F("Sensor-1 dew point: "));
    Serial.print(dewPoint_1);
    Serial.println(F("°C  "));

    Serial.print(F("Sensor-2 dew point: "));
    Serial.print(dewPoint_2);
    Serial.println(F("°C  "));

    ESP.wdtFeed();

    //**** Calculate difference between dew points********
    float DeltaTP = dewPoint_1 - dewPoint_2;

    //**** decide if ventilator should run or not ********
    if (DeltaTP > (MIN_Delta + HYSTERESE))
        ventilatorStatus = true;
    if (DeltaTP < (MIN_Delta))
        ventilatorStatus = false;
    if (t1 < TEMP1_min)
        ventilatorStatus = false;
    if (t2 < TEMP2_min)
        ventilatorStatus = false;

    if (ventilatorStatus == true)
    {
        digitalWrite(RELAIPIN, RELAIS_ON); // Turn on relay
        Serial.println(F("-> ventilation ON"));
    }
    else
    {
        digitalWrite(RELAIPIN, RELAIS_OFF); // Turn off relay
        Serial.println(F("-> ventilation OFF"));
    }

    // **** publish vlaues via MQTT ********
    if (client.connected())
    {
        client.publish((baseTopic + "sensor-1/temperature").c_str(), String(t1, 0).c_str());
        client.publish((baseTopic + "sensor-1/humidity").c_str(), String(h1, 0).c_str());
        client.publish((baseTopic + "sensor-1/dewpoint").c_str(), String(dewPoint_1, 0).c_str());
        client.publish((baseTopic + "sensor-2/temperature").c_str(), String(t2, 0).c_str());
        client.publish((baseTopic + "sensor-2/humidity").c_str(), String(h2, 0).c_str());
        client.publish((baseTopic + "sensor-2/dewpoint").c_str(), String(dewPoint_2, 0).c_str());
        client.publish((baseTopic + "ventilation/state").c_str(), ventilatorStatus ? "ON" : "OFF");
        client.publish((baseTopic + "ventilation/stateNum").c_str(), ventilatorStatus ? "1" : "0");
        Serial.println(F("published to MQTT"));
    }

    Serial.println();

    // this is the first time the loop is run, so we can post the startup time to MQTT for monitoring reboots
    if (firstStartup)
    {
        if (connectNTPClient())
        {
            String startupTime = timeClient.getFormattedTime();

            Serial.print(F("Startup time: "));
            Serial.println(startupTime);
            client.publish((baseTopic + "startup").c_str(), startupTime.c_str());
            firstStartup = false;
        }
    }

    delay(100);                      // delay required for led to turn of
    digitalWrite(LED_BUILTIN, HIGH); // Turn off LED while sleeping

    delay(4900);
    ESP.wdtFeed();
}

float calculateDewpoint(float t, float r)
{
    float a, b;

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
#ifdef SECRET_WIFI_SSID &&SECRET_WIFI_PASSWORD
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
        if (client.connected() == false)
        {
            Serial.println(F("WiFi connected. Connecting to MQTT..."));
            client.setServer(mqtt_server, mqtt_port);
            if (client.connect(mqtt_clientID, mqtt_user, mqtt_password))
            {
                Serial.println(F("MQTT connected"));
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
            return true;
        }
        else
        {
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
        digitalWrite(LED_BUILTIN, HIGH); // turn on the LED
        delay(50);                       // wait for 50ms
        digitalWrite(LED_BUILTIN, LOW);  // turn off the LED
        delay(50);                       // wait for 50ms
        delayTime -= 100;                // subtract 100ms from the delay time
    }
}
