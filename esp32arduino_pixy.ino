#include <Pixy2.h>
#include <SPI.h>

// Use VSPI pins on ESP32
static const int PIXY_SCK  = 18;
static const int PIXY_MISO = 19;
static const int PIXY_MOSI = 23;
static const int PIXY_CS   = 5;

Pixy2 pixy;

void setup() {
  Serial.begin(115200);

  // Initialize ESP32 VSPI with our pins
  SPI.begin(PIXY_SCK, PIXY_MISO, PIXY_MOSI, PIXY_CS);

  // Some Pixy2 Arduino libs let you override SS via this define:
  //  #define PIXY_SPI_SS 5
  // (If your library version needs it, put the define above the Pixy2 include.)
  
  pixy.init();   // defaults to SPI on Arduino/ESP32
}

void loop() {
  // get color-connected-components (CCC) blocks
  pixy.ccc.getBlocks();

  if (pixy.ccc.numBlocks) {
    for (int i = 0; i < pixy.ccc.numBlocks; i++) {
      auto &b = pixy.ccc.blocks[i];

      // Replace '6' with whatever signature ID you taught for blue
      if (b.m_signature == 6) {
        Serial.print("BLUE @ (");
        Serial.print(b.m_x); Serial.print(", ");
        Serial.print(b.m_y); Serial.print(")  w=");
        Serial.print(b.m_width); Serial.print(" h=");
        Serial.println(b.m_height);

        // TODO: do something—toggle a pin, send UART, etc.
        // digitalWrite(LED_BUILTIN, HIGH);
      }
    }
  }
}
