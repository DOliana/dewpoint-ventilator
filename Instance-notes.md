# notes for instace at home

- [huzzah feather](#huzzah-feather)
- [SHT30 sensor](#sht30-sensor)

## huzzah feather

![Huzzah ESP8266 Pinout](doc/adafruit_products_Huzzah_ESP8266_Pinout_v1.2-1.png)

I2C pins: 4, 5 (lower right in picture)

## SHT30 sensor

SHT30 voltage (check datasheet): 2.4 - 5.5V

wiring:

```
           +-------+
+-----\    | SDA 4 ----- Yellow ----
| +-+  ----+ GND 3 ----- Black  ----
| +-+  ----+ +5V 2 ----- Red    ----
+-----/    | SCL 1 ----- White  ----
           +-------+
```