// //
// //    FILE: SHT85_demo.ino
// //  AUTHOR: Rob Tillaart
// // PURPOSE: demo
// //     URL: https://github.com/RobTillaart/SHT85
// //
// // TOPVIEW SHT85  (check datasheet)
// // SHT30 voltage (check datasheet): 2.4 - 5.5V
// //            +-------+
// // +-----\    | SDA 4 ----- Yellow ----
// // | +-+  ----+ GND 3 ----- Black  ----
// // | +-+  ----+ +5V 2 ----- Red    ----
// // +-----/    | SCL 1 ----- White  ----
// //            +-------+


// #include "SHT85.h"

// #define SHT85_ADDRESS         0x44

// uint32_t start;
// uint32_t stop;

// SHT31 sht(SHT85_ADDRESS);


// void setup()
// {
//   Serial.begin(115200);
//   Serial.println(__FILE__);
//   Serial.print("SHT_LIB_VERSION: \t");
//   Serial.println(SHT_LIB_VERSION);

//   Wire.begin();
//   Wire.setClock(100000);
//   Serial.print("SHT.begin: ");
//   Serial.println(sht.begin());

//   uint16_t stat = sht.readStatus();
//   Serial.print(stat, HEX);
//   Serial.println();

// //   uint32_t ser = sht.GetSerialNumber();
// //   Serial.print(ser, HEX);
// //   Serial.println();
//   delay(1000);
// }


// void loop()
// {
//   start = micros();
//   sht.read();         //  default = true/fast       slow = false
//   stop = micros();

//   Serial.print("\tt: ");
//   Serial.print((stop - start) * 0.001);
//   Serial.print("\tt: ");
//   Serial.print(sht.getTemperature(), 1);
//   Serial.print("\th: ");
//   Serial.print(sht.getHumidity(), 1);
//   Serial.print("\te: ");
//   Serial.println(sht.getError(), 1);
//   delay(3000);
// }


// //  -- END OF FILE --