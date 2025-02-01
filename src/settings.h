#define RELAIPIN 12       // connection for ventilator relais switch
#define DHTPIN_INSIDE 13  // data line for DHT sensor 1 (inside)
#define DHTPIN_OUTSIDE 14 // data line for DHT sensor 2 (outside)

#define DHTTYPE_INSIDE DHT22  // DHT 22
#define DHTTYPE_OUTSIDE DHT22 // DHT 22

#define LED_BUILTIN_RED LED_BUILTIN
#define LED_BUILTIN_BLUE 2

// ******* Correction values for individual sensor values ***********
#define CORRECTION_TEMP_INSIDE 0                      // correction value for indoor sensor temperature
#define CORRECTION_TEMP_OUTSIDE 0                     // correction value for outdoor sensor temperature
#define CORRECTION_HUMIDITY_INSIDE 0                  // correction value for indoor sensor humidity
#define CORRECTION_HUMIDITY_OUTSIDE 0                 // correction value for outdoor sensor humidity
#define REFERENCE_TEMPERATURE_DIFFERENCE_THRESHOLD -2 // difference between the reference temperature and the outside temperature at which the reference temperature is used
//*******************************************************************

#define MIN_Delta 5.0                    // minimum dew point difference at which the relay switches
#define HYSTERESIS 1.0                   // distance from switch-on and switch-off point
#define TEMPINSIDE_MIN 10.0              // minimum indoor temperature at which ventilation is activated
#define TEMPOUTSIDE_MIN -10.0            // minimum outdoor temperature at which ventilation is activated
#define TEMPOUTSIDE_MAX 25.0             // maximum outdoor temperature at which ventilation is activated
#define MIN_HUMIDITY_FOR_OVERRIDE 80     // if the humidity inside is above this value, the ventilator will be turned on regardless of the dew point difference
#define MAX_HOURS_WITHOUT_VENTILATION 12 // after this time, the ventilator will be turned on for at least VENTILATION_OVERRIDE_TIME minutes
#define VENTILATION_OVERRIDE_MINUTES 30  // amount of minutes to override the ventilation status

#define TIME_SERVER "pool.ntp.org" // NTP server
#define TIME_ZONE "CET-1CEST,M3.5.0,M10.5.0/3" // Timezone https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
