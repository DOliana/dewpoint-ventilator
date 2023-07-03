#include <Arduino.h>
#include "DHT.h"
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <secrets.h>

#define RELAIPIN 16 // connection for ventilator relais switch
#define DHTPIN_1 13 // data line for DHT sensor 1 (inside)
#define DHTPIN_2 14 // Datenleitung für den DHT-Sensor 2 (außen)

#define RELAIS_EIN LOW
#define RELAIS_AUS HIGH
bool rel;

#define DHTTYPE_1 DHT22 // DHT 22
#define DHTTYPE_2 DHT22 // DHT 22

// ******* Correction values for individual sensor values *******
#define CORRECTION_temp_1 0     // correction value for indoor sensor temperature
#define CORRECTION_temp_2 0.1   // correction value for outdoor sensor temperature
#define CORRECTION_humidity_1 0 // correction value for indoor sensor humidity
#define CORRECTION_humidity_2 0 // correction value for outdoor sensor humidity
//*******************************************************************

#define MIN_Delta 5.0   // minimum dew point difference at which the relay switches
#define HYSTERESE 1.0   // distance from switch-on and switch-off point
#define TEMP1_min 10.0  // minimum indoor temperature at which ventilation is activated
#define TEMP2_min -10.0 // minimum outdoor temperature at which ventilation is activated

// ********* Wifi + MQTT settings *********
const char *ssid = SECRET_WIFI_SSID;
const char *password = SECRET_WIFI_PASSWORD;
const char *wifi_hostname = "dewpoint-vent";

int wifiErrorCounter = 0;
// ****************************************

DHT dht1(DHTPIN_1, DHTTYPE_1); // The indoor sensor is now addressed with dht1
DHT dht2(DHTPIN_2, DHTTYPE_2); // The outdoor sensor is now addressed with dht2

bool errorOnInitialize = true;

void setup()
{
    ESP.wdtEnable(WDTO_8S);

    pinMode(LED_BUILTIN, OUTPUT);       // Define LED pin as output
    digitalWrite(0, HIGH); // Turn on LED to show some activity
    pinMode(RELAIPIN, OUTPUT);          // Define relay pin as output
    digitalWrite(RELAIPIN, RELAIS_AUS); // Turn off relay

    Serial.begin(115200);
    Serial.println(F("Testing sensors..."));

    dht1.begin(); // Start sensors
    dht2.begin();
}

void loop()
{
    digitalWrite(0, HIGH); // Turn on LED to show some activity
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
    }
    if (isnan(h1) || isnan(t1) || isnan(h2) || isnan(t2))
        errorOnInitialize = true;

    if (errorOnInitialize == true)
    {
        digitalWrite(RELAIPIN, RELAIS_AUS); // Turn off ventilator
        Serial.println(F("Restarting..."));
        while (1)
            ; // Endless loop to restart the CPU through the watchdog
    }
    // feed the watchdog
    ESP.wdtFeed();

    //********* Wifi connection *********
    // if connection is unsuccessful, try again every 20 loops
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println(F("WiFi disconnected"));
        if (wifiErrorCounter <= 0)
        {
            Serial.println(F("connecting to WiFi..."));
            WiFi.setHostname(wifi_hostname);
            WiFi.begin(ssid, password);
            int waitCounter = 0;
            while (WiFi.status() != WL_CONNECTED && waitCounter < 10)
            {
                delay(500);
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
            }
        }
        else
        {
            wifiErrorCounter--;
        }
    }

    //**** Calculate dew points********
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

    Serial.println(F("calculating dew point..."));
    float dewPoint_1 = calculateDewpoint(t1, h1);
    float dewPoint_2 = calculateDewpoint(t2, h2);

    Serial.print(F("Sensor-1 dew point: "));
    Serial.print(dewPoint_1);
    Serial.println(F("°C  "));

    Serial.print(F("Sensor-2 dew point: "));
    Serial.print(dewPoint_2);
    Serial.println(F("°C  "));

    ESP.wdtFeed();

    float DeltaTP = dewPoint_1 - dewPoint_2;

    if (DeltaTP > (MIN_Delta + HYSTERESE))
        rel = true;
    if (DeltaTP < (MIN_Delta))
        rel = false;
    if (t1 < TEMP1_min)
        rel = false;
    if (t2 < TEMP2_min)
        rel = false;

    if (rel == true)
    {
        digitalWrite(RELAIPIN, RELAIS_EIN); // Turn on relay
        Serial.println(F("-> ventilation ON"));
    }
    else
    {
        digitalWrite(RELAIPIN, RELAIS_AUS); // Turn off relay
        Serial.println(F("-> ventilation OFF"));
    }

    Serial.println();

    delay(4000);
    digitalWrite(BUILTIN_LED, LOW); // Turn off LED
    delay(1000);
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