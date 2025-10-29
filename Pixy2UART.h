//
// begin license header
//
// This file is part of Pixy CMUcam5 or "Pixy" for short
//
// All Pixy source code is provided under the terms of the
// GNU General Public License v2 (http://www.gnu.org/licenses/gpl-2.0.html).
// Those wishing to use Pixy source code, software and/or
// technologies under different licensing terms should contact us at
// cmucam@cs.cmu.edu. Such licensing terms are available for
// all portions of the Pixy codebase presented here.
//
// end license header
//
// UART link class.
// On ESP32 we use HardwareSerial2 with selectable RX/TX pins.
// On AVR/others we use Serial1 as in the original library.

#ifndef _PIXY2UART_H
#define _PIXY2UART_H

#include "TPixy2.h"
#include <Arduino.h>

// ---------- Defaults you can override in your sketch BEFORE including Pixy2UART.h ----------
#ifndef PIXY_UART_BAUDRATE
#define PIXY_UART_BAUDRATE 115200
#endif

#ifdef ARDUINO_ARCH_ESP32
  // Default pins for ESP32 DevKit V1 (UART2)
  #ifndef PIXY2_UART_RX_PIN
  #define PIXY2_UART_RX_PIN 16
  #endif
  #ifndef PIXY2_UART_TX_PIN
  #define PIXY2_UART_TX_PIN 17
  #endif
#endif
// -----------------------------------------------------------------------------------------

class Link2UART
{
public:
  // arg: baud rate or PIXY_DEFAULT_ARGVAL to use default
  int8_t open(uint32_t arg)
  {
    uint32_t baud = (arg == PIXY_DEFAULT_ARGVAL) ? PIXY_UART_BAUDRATE : arg;

#ifdef ARDUINO_ARCH_ESP32
    // Use UART2 on ESP32 with configured pins.
    // NOTE: Ensure PixyMon is set to Interface=UART, Baud=baud, and USB is unplugged.
    Serial2.begin(baud, SERIAL_8N1, PIXY2_UART_RX_PIN, PIXY2_UART_TX_PIN);
#else
    // Non-ESP32 (e.g., AVR Mega) uses Serial1 like the original code.
    Serial1.begin(baud);
#endif
    return 0;
  }

  void close() { }

  // Receive exactly len bytes with ~2ms timeout per byte.
  int16_t recv(uint8_t *buf, uint8_t len, uint16_t *cs = NULL)
  {
    if (cs) *cs = 0;

    for (uint8_t i = 0; i < len; i++)
    {
      int16_t c = -1;
      uint16_t spins = 0;

      // ~2ms timeout at 10 us per check (200 iterations)
      while (true)
      {
#ifdef ARDUINO_ARCH_ESP32
        c = Serial2.read();
#else
        c = Serial1.read();
#endif
        if (c >= 0) break;
        if (spins++ >= 200) return -1;
        delayMicroseconds(10);
      }

      buf[i] = static_cast<uint8_t>(c);
      if (cs) *cs += buf[i];
    }
    return len;
  }

  int16_t send(uint8_t *buf, uint8_t len)
  {
#ifdef ARDUINO_ARCH_ESP32
    Serial2.write(buf, len);
#else
    Serial1.write(buf, len);
#endif
    return len;
  }

private:
  uint8_t m_addr; // unused, kept for API parity
};

// Type alias the same way the library does
typedef TPixy2<Link2UART> Pixy2UART;

#endif // _PIXY2UART_H
