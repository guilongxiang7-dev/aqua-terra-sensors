#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <SoftwareSerial.h>

/*
Arduino board B - four-in-one soil sensor (SN-3001-TR RS485 model)

Wiring (UNO/NANO, 5 V):
  OLED SSD1306 128x64 I2C
    SDA -> A4
    SCL -> A5
    VCC -> 5V
    GND -> GND

  UART-to-RS485 converter (the sensor uses RS485 and Arduino uses TTL UART, so this converter is required)
    Converter TTL side:
      VCC -> 5V
      GND -> GND
      TXD -> D8   converter TX connects to Arduino software RX
      RXD -> D9   converter RX connects to Arduino software TX
    Converter RS485 side:
      A   -> soil sensor RS485-A (yellow/green)
      B   -> soil sensor RS485-B (blue)
    The automatic-direction module has no DE/RE wiring; keep RS485_AUTO_DIR=1 below
    If the module has DE/RE pins (MAX485 type):
      Connect DE and RE together to D7 and set RS485_AUTO_DIR to 0

  Four-in-one soil probe five-pin connector (DC 4.5-30 V, default address 1, default 4800 baud)
    Brown VCC -> 5 V (external 5-24 V is possible with common Arduino ground)
    Black GND -> GND
    Yellow 485-A -> converter A
    Blue 485-B -> converter B

  USB serial output at 9600 baud:
    moisture;temperature;conductivity;pH;

Registers (function code 03):
  0000H moisture x10  %
  0001H temperature   x10  degC (signed two's complement)
  0002H conductivity      uS/cm
  0003H PH     x10

Library: U8g2
*/

#define RS485_RX_PIN 8
#define RS485_TX_PIN 9
#define RS485_DE_PIN 7
#define RS485_AUTO_DIR 1   // 1=automatic-direction UART-RS485 converter; 0=MAX485 with DE/RE on D7
#define SOIL_ADDR 0x01
#define SOIL_BAUD 4800

SoftwareSerial rs485(RS485_RX_PIN, RS485_TX_PIN);
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, SCL, SDA, U8X8_PIN_NONE);

int cnt = 0;

float tu_h = 0;
float tu_t = 0;
int tu_ec = 0;
float tu_ph = 0;
uint8_t tu_has = 0;     // a valid reading has been received
uint8_t tu_h_hold = 0;  // large moisture jump awaiting confirmation
uint8_t tu_t_hold = 0;  // large temperature jump awaiting confirmation

/**
 * @brief Print hexadecimal data to USB serial for RS485 diagnostics
 */
void log_hex(const __FlashStringHelper *tag, uint8_t *buf, uint8_t n)
{
  uint8_t i;
  Serial.print(tag);
  Serial.print(' ');
  Serial.print(n);
  Serial.print(F(":"));
  for (i = 0; i < n; i++) {
    Serial.print(' ');
    if (buf[i] < 16) {
      Serial.print('0');
    }
    Serial.print(buf[i], HEX);
  }
  Serial.print(F("\r\n"));
}

uint16_t crc16_modbus(uint8_t *buf, uint8_t len)
{
  uint16_t crc = 0xFFFF;
  uint8_t i, j;
  for (i = 0; i < len; i++) {
    crc ^= buf[i];
    for (j = 0; j < 8; j++) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001;
      }
      else {
        crc = crc >> 1;
      }
    }
  }
  return crc;
}

void rs485_tx(uint8_t *cmd, uint8_t len)
{
  uint8_t i;
#if RS485_AUTO_DIR == 0
  digitalWrite(RS485_DE_PIN, HIGH);
  delay(2);
#endif
  for (i = 0; i < len; i++) {
    rs485.write(cmd[i]);
  }
  rs485.flush();
  delay(2);
#if RS485_AUTO_DIR == 0
  digitalWrite(RS485_DE_PIN, LOW);
#endif
}

uint8_t rs485_rx(uint8_t *buf, uint8_t maxlen, uint16_t timeout_ms)
{
  uint8_t n = 0;
  unsigned long t0 = millis();
  while ((millis() - t0) < timeout_ms) {
    while (rs485.available() && n < maxlen) {
      buf[n++] = (uint8_t)rs485.read();
      t0 = millis();
    }
    if (n >= 5 && (millis() - t0) > 20) {
      break;
    }
  }
  return n;
}

/**
 * @brief A large change must occur twice before acceptance; small changes are averaged
 * @param shown current displayed value
 * @param now new sample
 * @param lim permitted single-step change
 * @param hold confirmation flag
 */
float tu_smooth(float shown, float now, float lim, uint8_t *hold)
{
  float d = now - shown;
  if (d < 0) {
    d = -d;
  }
  if (tu_has && d > lim) {
    if (*hold == 0) {
      *hold = 1;
      return shown;
    }
  }
  *hold = 0;
  if (!tu_has) {
    return now;
  }
  return shown * 0.5 + now * 0.5;
}

/**
 * @brief Find a valid soil response in the receive buffer, including converter echo handling
 * @return 1=parsing successful
 */
uint8_t tu_parse(uint8_t *rx, uint8_t n, float *h, float *t, int *ec, float *ph)
{
  uint8_t i;
  uint16_t crc, crc_rx;

  for (i = 0; i + 12 < n; i++) {
    if (rx[i] != SOIL_ADDR || rx[i + 1] != 0x03 || rx[i + 2] != 0x08) {
      continue;
    }
    crc = crc16_modbus(&rx[i], 11);
    crc_rx = (uint16_t)rx[i + 11] | ((uint16_t)rx[i + 12] << 8);
    if (crc != crc_rx) {
      continue;
    }
    *h = (((uint16_t)rx[i + 3] << 8) | rx[i + 4]) / 10.0;
    *t = ((int16_t)(((uint16_t)rx[i + 5] << 8) | rx[i + 6])) / 10.0;
    *ec = (int)(((uint16_t)rx[i + 7] << 8) | rx[i + 8]);
    *ph = (((uint16_t)rx[i + 9] << 8) | rx[i + 10]) / 10.0;
    return 1;
  }
  return 0;
}

void tu_read(void)
{
  uint8_t cmd[8];
  uint8_t rx[32];
  uint8_t n, k;
  uint16_t crc;
  float h, t, ph;
  int ec;

  // datasheet example: 01 03 00 00 00 04 44 09  read four registers
  cmd[0] = SOIL_ADDR;
  cmd[1] = 0x03;
  cmd[2] = 0x00;
  cmd[3] = 0x00;
  cmd[4] = 0x00;
  cmd[5] = 0x04;
  crc = crc16_modbus(cmd, 6);
  cmd[6] = (uint8_t)(crc & 0xFF);
  cmd[7] = (uint8_t)(crc >> 8);

  for (k = 0; k < 2; k++) {
    while (rs485.available()) {
      rs485.read();
    }

    rs485_tx(cmd, 8);
    n = rs485_rx(rx, sizeof(rx), 300);
    log_hex(F("TX"), cmd, 8);
    log_hex(F("RX"), rx, n);
    if (tu_parse(rx, n, &h, &t, &ec, &ph)) {
      Serial.print(F("RAW H="));
      Serial.print(h, 1);
      Serial.print(F(" T="));
      Serial.print(t, 1);
      Serial.print(F(" EC="));
      Serial.print(ec);
      Serial.print(F(" PH="));
      Serial.println(ph, 1);

      tu_h = tu_smooth(tu_h, h, 4.0, &tu_h_hold);
      tu_t = tu_smooth(tu_t, t, 1.5, &tu_t_hold);
      tu_ec = ec;
      tu_ph = tu_has ? (tu_ph * 0.5 + ph * 0.5) : ph;
      tu_has = 1;

      Serial.print(F("OUT H="));
      Serial.print(tu_h, 1);
      Serial.print(F(" T="));
      Serial.print(tu_t, 1);
      Serial.print(F(" EC="));
      Serial.print(tu_ec);
      Serial.print(F(" PH="));
      Serial.print(tu_ph, 1);
      Serial.print(F(" holdH="));
      Serial.print(tu_h_hold);
      Serial.print(F(" holdT="));
      Serial.println(tu_t_hold);
      return;
    }
    Serial.println(F("PARSE fail"));
  }
}

void OLED_setup()
{
  uint8_t ok = u8g2.begin();
  Serial.print(F("OLED "));
  Serial.println(ok ? F("ok") : F("fail"));
}

void OLED_loop()
{
  u8g2.setFont(u8g2_font_gb16st_t_1);
  u8g2.firstPage();
  do {
    u8g2.drawStr(0, 16, "T:");
    u8g2.setCursor(20, 16);
    u8g2.print(tu_t, 1);
    u8g2.drawStr(72, 16, "H:");
    u8g2.setCursor(92, 16);
    u8g2.print(tu_h, 1);

    u8g2.drawStr(0, 32, "PH:");
    u8g2.setCursor(28, 32);
    u8g2.print(tu_ph, 1);

    u8g2.drawStr(0, 48, "EC:");
    u8g2.setCursor(28, 48);
    u8g2.print(tu_ec);
  } while (u8g2.nextPage());
}

void setup()
{
  Serial.begin(9600);

#if RS485_AUTO_DIR == 0
  pinMode(RS485_DE_PIN, OUTPUT);
  digitalWrite(RS485_DE_PIN, LOW);
#endif
  rs485.begin(SOIL_BAUD);

  OLED_setup();

  Serial.println(F("turang B log 9600"));
}

void loop()
{
  tu_read();
  OLED_loop();

  //serial output
  if (cnt++, cnt > 2) {
    cnt = 0;
    Serial.print(tu_h, 1);
    Serial.print(';');
    Serial.print(tu_t, 1);
    Serial.print(';');
    Serial.print(tu_ec);
    Serial.print(';');
    Serial.print(tu_ph, 1);
    Serial.print(';');
    Serial.print("\r\n");
  }
}
