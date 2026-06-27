#include <Arduino.h>

extern "C" {
#include "ad5940.h"
#include "Impedance.h"

uint32_t AD5940_MCUResourceInit(void *pCfg);
void AD5940_Main(void);
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("AD5941 impedance sweep on ESP32");
  Serial.println("SPI: SCLK=18 MOSI=23 MISO=19 CS=5, RESET=4, INT=2");

  uint32_t init_result = AD5940_MCUResourceInit(NULL);
  if(init_result != 0)
  {
    Serial.print("MCU resource init failed: 0x");
    Serial.println(init_result, HEX);
    while(true)
      delay(1000);
  }

  Serial.println("Starting 100 Hz to 100 kHz impedance sweep...");
  AD5940_Main();
}

void loop()
{
  delay(1000);
}
