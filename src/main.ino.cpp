# 1 "C:\\Users\\OLIANA~1\\AppData\\Local\\Temp\\tmpi3gthnbp"
#include <Arduino.h>
# 1 "C:/Dev/Privat/dewpoint-vent/src/main.ino"
#include <Arduino.h>
#include "DHT.h"
#include <ESP8266WiFi.h>
#include <MQTT.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <secrets.h>
#include <settings.h>
#include <ArduinoJson.h>
#include "FS.h"
#include <LittleFS.h>
#include <iterator>

#define RELAIS_ON HIGH
#define RELAIS_OFF LOW


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

long unsigned maxMilliSecondsWithoutWiFi = 300000;
long unsigned lastTimeWiFiOK = ULONG_MAX;
String startupTime;
bool ventilatorStatus = false;
long unsigned lastTimeVentilatorStatusChange;
int min_humidity_for_override = MIN_HUMIDITY_FOR_OVERRIDE;
int max_hours_without_ventilation = MAX_HOURS_WITHOUT_VENTILATION;
int ventilation_override_minutes = VENTILATION_OVERRIDE_MINUTES;
bool ventilationOverride = false;
String baseTopic = "dewppoint-ventilator";
String requestedMode = "AUTO";
int min_delta = MIN_Delta;
int hysteresis = HYSTERESIS;
int tempInside_min = TEMPINSIDE_MIN;
int tempOutside_min = TEMPOUTSIDE_MIN;
int tempOutside_max = TEMPOUTSIDE_MAX;
bool stopSleeping = false;
bool configChangedMap[9] = {true, true, true, true, true, true, true, true, true};
#define CONFIG_IDX_MODE 0
#define CONFIG_IDX_MIN_DELTA 1
#define CONFIG_IDX_HYSTERESIS 2
#define CONFIG_IDX_TEMPINSIDE_MIN 3
#define CONFIG_IDX_TEMPOUTSIDE_MIN 4
#define CONFIG_IDX_TEMPOUTSIDE_MAX 5
#define CONFIG_IDX_MIN_HUMIDITY_FOR_OVERRIDE 6
#define CONFIG_IDX_MAX_HOURS_WITHOUT_VENTILATION 7
#define CONFIG_IDX_VENTILATION_OVERRIDE_MINUTES 8
WiFiEventHandler wifiConnectHandler;

WiFiClient wifiClient;
MQTTClient mqttClient;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, TIME_SERVER, 3600, 60000);

DHT dhtInside(DHTPIN_INSIDE, DHTTYPE_INSIDE);
DHT dhtOutside(DHTPIN_OUTSIDE, DHTTYPE_OUTSIDE);






struct SensorValues
{
    float humidityInside;
    float tempInside;
    float humidityOutside;
    float tempOutside;

    bool sensorsOK;
    String errorReasoun;
};
# 104 "C:/Dev/Privat/dewpoint-vent/src/main.ino"
void setup();
void loop();
void calculateAndSetVentilatorStatus();
SensorValues getSensorValues();
void setVentilatorOn(bool running);
float calculateDewpoint(float t, float r);
void initializeWiFi();
void onWiFiConnect(const WiFiEventStationModeGotIP &event);
void connectMQTTIfDisconnected();
void mqttCallback(String &topic, String &payload);
void publishConfigValueIfChanged(short configMapIndex, String valueName, String value);
void publishConfigIfChanded();
bool connectNTPClient();
void sleepAndBlink(int sleepTimeMS);
String getTimeString();
bool saveConfig();
bool loadConfig();
void resetConfig();
#line 104 "C:/Dev/Privat/dewpoint-vent/src/main.ino"
void setup()
{
    ESP.wdtEnable(WDTO_8S);

    pinMode(LED_BUILTIN_RED, OUTPUT);
    pinMode(LED_BUILTIN_BLUE, OUTPUT);
    pinMode(RELAIPIN, OUTPUT);

    digitalWrite(LED_BUILTIN_RED, LOW);

    Serial.begin(115200);

    initializeWiFi();

    Serial.println(F("Testing sensors..."));

    dhtInside.begin();
    dhtOutside.begin();


#ifdef SECRET_MQTT_BASETOPIC
    baseTopic = SECRET_MQTT_BASETOPIC;
    if (baseTopic.endsWith("/") == false)
    {
        baseTopic.concat("/");
    }
#endif
    Serial.print("MQTT base topic set: ");
    Serial.println(baseTopic);


    Serial.println("Mounting FS...");
    if (!LittleFS.begin())
    {
        Serial.println("Failed to mount file system");
    }


    if (loadConfig() == false)
    {

        Serial.println("Failed to load config from file - saving default values in config");
        resetConfig();
    }


    lastTimeVentilatorStatusChange = millis() - max_hours_without_ventilation * 60 * 60 * 1000;

    Serial.println("Setup complete");
    Serial.println();
}
# 168 "C:/Dev/Privat/dewpoint-vent/src/main.ino"
void loop()
{

    if (WiFi.status() == WL_CONNECTED)
    {
        lastTimeWiFiOK = millis();
    }
    else if (millis() - lastTimeWiFiOK > maxMilliSecondsWithoutWiFi)
    {
        ESP.restart();
    }
    else
    {
        Serial.println("WiFi not connected for " + String(millis() - lastTimeWiFiOK) + "ms");
    }

    digitalWrite(LED_BUILTIN_BLUE, LOW);
    connectMQTTIfDisconnected();
    publishConfigIfChanded();
    stopSleeping = false;
    calculateAndSetVentilatorStatus();

    Serial.println();
    digitalWrite(LED_BUILTIN_BLUE, HIGH);


    for (short i = 0; i < 60; i++)
    {
        if (mqttClient.connected())
        {

            if (i % 10 == 0)
                mqttClient.publish(baseTopic + "log/heartbeat", getTimeString(), true, 0);
            mqttClient.loop();
            delay(10);
        }
        else if (i % 10 == 0)
        {
            Serial.print("MQTT connected: false, Wifi connected: ");
            Serial.print(WiFi.status() == WL_CONNECTED ? "true" : "false");
            Serial.println(" - no heartbeat sent");
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






void calculateAndSetVentilatorStatus()
{
    SensorValues sensorValues = getSensorValues();

    if (sensorValues.sensorsOK == false)
    {
        if (mqttClient.connected())
        {
            mqttClient.publish(baseTopic + "log/status", "sensors show errors: " + sensorValues.errorReasoun, false, 1);
            Serial.println(F("Error message sent"));
            delay(500);
        }

        Serial.println(F("Restarting in 5 seconds..."));
        delay(5000);
        ESP.restart();
    }
    else
    {
        if (mqttClient.connected())
        {
            mqttClient.publish(baseTopic + "log/status", "sensors OK", false, 1);
        }
    }

    ESP.wdtFeed();


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


    Serial.println(F("calculating dew point..."));
    float dewPoint_inside = calculateDewpoint(sensorValues.tempInside, sensorValues.humidityInside);
    float dewPoint_outside = calculateDewpoint(sensorValues.tempOutside, sensorValues.humidityOutside);


    Serial.print(F("sensor-inside dew point: "));
    Serial.print(dewPoint_inside);
    Serial.println(F("°C  "));

    Serial.print(F("sensor-outside dew point: "));
    Serial.print(dewPoint_outside);
    Serial.println(F("°C  "));


    float deltaDP = dewPoint_inside - dewPoint_outside;


    String ventilatorStatusReason = "Hysteresis phase";
    if (deltaDP > (min_delta + hysteresis))
    {
        ventilatorStatus = true;
        ventilatorStatusReason = "DeltaDP > (MIN_Delta + HYSTERESIS): " + String(deltaDP) + " > " + String(min_delta) + " + " + String(hysteresis);
    }
    else if (deltaDP <= (min_delta))
    {
        ventilatorStatus = false;
        ventilatorStatusReason = "DeltaDP < (MIN_Delta): " + String(deltaDP) + " < " + String(min_delta);
    }


    if (ventilatorStatus && sensorValues.tempInside < tempInside_min)
    {
        ventilatorStatus = false;
        ventilatorStatusReason = "tempInside < TEMPINSIDE_MIN: " + String(sensorValues.tempInside) + " < " + String(tempInside_min);
    }
    else if (ventilatorStatus && sensorValues.tempOutside < tempOutside_min)
    {
        ventilatorStatus = false;
        ventilatorStatusReason = "tempOutside < TEMPOUTSIDE_MIN: " + String(sensorValues.tempOutside) + " < " + String(tempOutside_min);
    }
    else if (ventilatorStatus && sensorValues.tempOutside > tempOutside_max)
    {
        ventilatorStatus = false;
        ventilatorStatusReason = "tempOutside > TEMPOUTSIDE_MAX: " + String(sensorValues.tempOutside) + " > " + String(tempOutside_max);
    }


    if (sensorValues.humidityInside >= min_humidity_for_override && ventilatorStatus == false && lastTimeVentilatorStatusChange < millis() - max_hours_without_ventilation * 60 * 60 * 1000)
    {
        ventilationOverride = true;
        ventilatorStatusReason = "ventilator off for " + String(max_hours_without_ventilation) + " hours - turning on";
    }

    if (ventilationOverride && lastTimeVentilatorStatusChange < millis() - ventilation_override_minutes * 60 * 60 * 1000)
    {
        ventilationOverride = false;
    }

    if (ventilationOverride)
    {
        ventilatorStatusReason = "ventilationOverride == true";
        ventilatorStatus = true;
    }
    else if (requestedMode != "AUTO")
    {
        ventilatorStatusReason = "requestedMode == " + requestedMode;
        ventilatorStatus = !(requestedMode == "OFF");
    }

    setVentilatorOn(ventilatorStatus);
    Serial.println("-> Reason: " + ventilatorStatusReason);


    if (mqttClient.connected())
    {
        mqttClient.publish(baseTopic + "sensor-inside/temperature", String(sensorValues.tempInside, 2), false, 1);
        mqttClient.publish(baseTopic + "sensor-inside/humidity", String(sensorValues.humidityInside, 2), false, 1);
        mqttClient.publish(baseTopic + "sensor-inside/dewpoint", String(dewPoint_inside, 2), false, 1);
        mqttClient.publish(baseTopic + "sensor-outside/temperature", String(sensorValues.tempOutside, 2), false, 1);
        mqttClient.publish(baseTopic + "sensor-outside/humidity", String(sensorValues.humidityOutside, 2), false, 1);
        mqttClient.publish(baseTopic + "sensor-outside/dewpoint", String(dewPoint_outside, 2), false, 1);
        mqttClient.publish(baseTopic + "log/ventilatorStatusReason", ventilatorStatusReason, false, 1);
        Serial.println(F("published to MQTT"));
    }
}







SensorValues getSensorValues()
{
    SensorValues result = {0, 0, 0, 0, true, ""};

    result.humidityInside = dhtInside.readHumidity();
    result.tempInside = dhtInside.readTemperature();
    result.humidityOutside = dhtOutside.readHumidity();
    result.tempOutside = dhtOutside.readTemperature();

    if (isnan(result.humidityInside) || isnan(result.tempInside) || result.humidityInside > 100 || result.humidityInside < 1 || result.tempInside < -40 || result.tempInside > 80)
    {
        Serial.println(F("Error reading from sensor inside:"));
        Serial.print(F("humidity: "));
        Serial.print(result.humidityInside);
        Serial.print(F("%  temperature: "));
        Serial.print(result.tempInside);
        Serial.println(F("°C"));
        Serial.println(result.humidityInside);
        result.sensorsOK = false;
        result.errorReasoun = "Error reading from sensor inside. ";
    }
    else
    {
        Serial.println(F("sensor inside OK"));
    }

    if (isnan(result.humidityOutside) || isnan(result.tempOutside) || result.humidityOutside > 100 || result.humidityOutside < 1 || result.tempOutside < -40 || result.tempOutside > 80)
    {
        Serial.println(F("Error reading from sensor outside."));
        Serial.print(F("humidity: "));
        Serial.print(result.humidityOutside);
        Serial.print(F("%  temperature: "));
        Serial.print(result.tempOutside);
        Serial.println(F("°C"));
        result.sensorsOK = false;
        result.errorReasoun.concat("Error reading from sensor outside. ");
    }
    else
    {
        Serial.println(F("sensor outside OK"));
    }

    return result;
}
# 415 "C:/Dev/Privat/dewpoint-vent/src/main.ino"
void setVentilatorOn(bool running)
{

    bool currentlyInRunningState = digitalRead(RELAIPIN) == RELAIS_ON;
    if (running == true)
    {
        digitalWrite(RELAIPIN, RELAIS_ON);
        Serial.println(F("-> ventilation ON"));
    }
    else
    {
        digitalWrite(RELAIPIN, RELAIS_OFF);
        Serial.println(F("-> ventilation OFF"));
    }

    if (currentlyInRunningState != running && mqttClient.connected())
    {

        lastTimeVentilatorStatusChange = millis();

        mqttClient.publish(baseTopic + "ventilation/state", running ? "ON" : "OFF", true, 1);
        mqttClient.publish(baseTopic + "ventilation/stateNum", running ? "1" : "0", true, 1);
    }
}
# 448 "C:/Dev/Privat/dewpoint-vent/src/main.ino"
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


    float sdd = 6.1078 * pow(10, (a * t) / (b + t));


    float dd = sdd * (r / 100);


    float v = log10(dd / 6.1078);


    float tt = (b * v) / (a - v);
    return {tt};
}







void initializeWiFi()
{
    Serial.println(F("connecting to WiFi..."));

    wifiConnectHandler = WiFi.onStationModeGotIP(onWiFiConnect);
    WiFi.setHostname(wifi_hostname);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);

#if defined(SECRET_WIFI_SSID) && defined(SECRET_WIFI_PASSWORD)
    WiFi.begin(ssid, password);
    short waitCounter = 6;
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
# 523 "C:/Dev/Privat/dewpoint-vent/src/main.ino"
void onWiFiConnect(const WiFiEventStationModeGotIP &event)
{
    Serial.println("Connected to Wi-Fi sucessfully.");
    stopSleeping = true;
}







void connectMQTTIfDisconnected()
{

    if (WiFi.status() == WL_CONNECTED)
    {

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


        if (mqttClient.connected() == false)
        {
            Serial.println(F("Connecting to MQTT..."));
            mqttClient.begin(mqtt_server, mqtt_port, wifiClient);
            mqttClient.setKeepAlive(20);
            mqttClient.setTimeout(2000);
            mqttClient.onMessage(mqttCallback);
            mqttClient.setCleanSession(false);

            if (mqttClient.connect(mqtt_clientID, mqtt_user, mqtt_password))
            {
                Serial.println(F("MQTT connected"));
                mqttClient.subscribe(baseTopic + "config/mode/set", 1);
                mqttClient.subscribe(baseTopic + "config/deltaDPmin/set", 1);
                mqttClient.subscribe(baseTopic + "config/hysteresis/set", 1);
                mqttClient.subscribe(baseTopic + "config/tempInside_min/set", 1);
                mqttClient.subscribe(baseTopic + "config/tempOutside_min/set", 1);
                mqttClient.subscribe(baseTopic + "config/tempOutside_max/set", 1);
                mqttClient.subscribe(baseTopic + "config/reset", 1);
                Serial.println("command topics subscribed");

                if (startupTime == NULL)
                {
                    if (connectNTPClient())
                    {
                        startupTime = getTimeString();

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
# 607 "C:/Dev/Privat/dewpoint-vent/src/main.ino"
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
    else if (topic.equals(baseTopic + "config/overrideMinHumidity/set"))
    {
        min_humidity_for_override = payload.toInt();
        configChangedMap[CONFIG_IDX_MIN_HUMIDITY_FOR_OVERRIDE] = true;
        saveConfig();
        Serial.print("min_humidity_for_override set to ");
        Serial.println(min_humidity_for_override);
    }
    else if (topic.equals(baseTopic + "config/overrideMaxHoursWithoutVentilation/set"))
    {
        max_hours_without_ventilation = payload.toInt();
        configChangedMap[CONFIG_IDX_MAX_HOURS_WITHOUT_VENTILATION] = true;
        saveConfig();
        Serial.print("max_hours_without_ventilation set to ");
        Serial.println(max_hours_without_ventilation);
    }
    else if (topic.equals(baseTopic + "config/overrideVentilationMinutes/set"))
    {
        ventilation_override_minutes = payload.toInt();
        configChangedMap[CONFIG_IDX_VENTILATION_OVERRIDE_MINUTES] = true;
        saveConfig();
        Serial.print("ventilation_override_minutes set to ");
        Serial.println(ventilation_override_minutes);
    }
    else if (topic.equals(baseTopic + "config/reset"))
    {
        if (payload == "true" or payload == "1")
        {
            resetConfig();
        }
    }
    else
    {
        Serial.println("Unknown topic: " + topic);
    }

    stopSleeping = true;
}
# 726 "C:/Dev/Privat/dewpoint-vent/src/main.ino"
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







void publishConfigIfChanded()
{

    publishConfigValueIfChanged(CONFIG_IDX_MODE, "mode", requestedMode);
    publishConfigValueIfChanged(CONFIG_IDX_MIN_DELTA, "deltaDPmin", String(min_delta));
    publishConfigValueIfChanged(CONFIG_IDX_HYSTERESIS, "hysteresis", String(hysteresis));
    publishConfigValueIfChanged(CONFIG_IDX_TEMPINSIDE_MIN, "tempInside_min", String(tempInside_min));
    publishConfigValueIfChanged(CONFIG_IDX_TEMPOUTSIDE_MIN, "tempOutside_min", String(tempOutside_min));
    publishConfigValueIfChanged(CONFIG_IDX_TEMPOUTSIDE_MAX, "tempOutside_max", String(tempOutside_max));
    publishConfigValueIfChanged(CONFIG_IDX_MIN_HUMIDITY_FOR_OVERRIDE, "overrideMinHumidity", String(min_humidity_for_override));
    publishConfigValueIfChanged(CONFIG_IDX_MAX_HOURS_WITHOUT_VENTILATION, "overrideMaxHoursWithoutVentilation", String(max_hours_without_ventilation));
    publishConfigValueIfChanged(CONFIG_IDX_VENTILATION_OVERRIDE_MINUTES, "overrideVentilationMinutes", String(ventilation_override_minutes));
}





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
# 804 "C:/Dev/Privat/dewpoint-vent/src/main.ino"
void sleepAndBlink(int sleepTimeMS)
{
    while (sleepTimeMS > 0)
    {
        digitalWrite(LED_BUILTIN_BLUE, LOW);
        delay(50);
        digitalWrite(LED_BUILTIN_BLUE, HIGH);
        delay(50);
        sleepTimeMS -= 100;
    }
}







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






bool saveConfig()
{
    StaticJsonDocument<200> doc;
    doc["mode"] = requestedMode;
    doc["deltaDPmin"] = min_delta;
    doc["hysteresis"] = hysteresis;
    doc["tempInside_min"] = tempInside_min;
    doc["tempOutside_min"] = tempOutside_min;
    doc["tempOutside_max"] = tempOutside_max;
    doc["min_humidity_for_override"] = min_humidity_for_override;
    doc["max_hours_without_ventilation"] = max_hours_without_ventilation;
    doc["ventilation_override_minutes"] = ventilation_override_minutes;


    File configFile = LittleFS.open("/config.json", "w");
    if (!configFile)
    {
        Serial.println("Failed to open config file for writing");
        return false;
    }


    serializeJson(doc, configFile);
    configFile.close();
    Serial.println("Config saved to file");
    return true;
}
# 881 "C:/Dev/Privat/dewpoint-vent/src/main.ino"
bool loadConfig()
{
    File configFile = LittleFS.open("/config.json", "r");
    if (!configFile)
    {
        Serial.println("Failed to open config file");
        return false;
    }

    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, configFile);

    if (error)
    {
        Serial.print(F("Failed to deserialize config file: "));
        Serial.println(error.f_str());
        return false;
    }

    if (doc.containsKey("mode"))
        requestedMode = doc["mode"].as<String>();
    if (doc.containsKey("deltaDPmin"))
        min_delta = doc["deltaDPmin"].as<int>();
    if (doc.containsKey("hysteresis"))
        hysteresis = doc["hysteresis"].as<int>();
    if (doc.containsKey("tempInside_min"))
        tempInside_min = doc["tempInside_min"].as<int>();
    if (doc.containsKey("tempOutside_min"))
        tempOutside_min = doc["tempOutside_min"].as<int>();
    if (doc.containsKey("tempOutside_max"))
        tempOutside_max = doc["tempOutside_max"].as<int>();
    if (doc.containsKey("min_humidity_for_override"))
        min_humidity_for_override = doc["min_humidity_for_override"].as<int>();
    if (doc.containsKey("max_hours_without_ventilation"))
        max_hours_without_ventilation = doc["max_hours_without_ventilation"].as<int>();
    if (doc.containsKey("ventilation_override_minutes"))
        ventilation_override_minutes = doc["ventilation_override_minutes"].as<int>();

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

    return true;
}






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
    Serial.println("Config reset to default values");

    for (size_t i = 0; i < sizeof(configChangedMap); i++)
    {
        configChangedMap[i] = true;
    }

    saveConfig();
}