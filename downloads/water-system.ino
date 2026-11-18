#include <Arduino.h>
#include <DHT.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <SoftwareSerial.h>
#include <OneWire.h>
#include <DallasTemperature.h>

/*
Arduino board A - water monitoring

Wiring (UNO/NANO, 5 V):
  OLED SSD1306 128x64 I2C
    SDA -> A4
    SCL -> A5
    VCC -> 5V
    GND -> GND

  DHT11 air temperature and humidity
    DATA -> D2
    VCC  -> 5V
    GND  -> GND

  DS18B20 water temperature (requires 4.7 kOhm pull-up to 5 V; many modules include it)
    DQ  -> D3
    VCC -> 5V
    GND -> GND

  Analogue TDS channel
    A0  -> TDS analogue output
    VCC -> 5V
    GND -> GND

  Turbidity module analogue output
    A1  -> turbidity analogue output
    VCC -> 5V
    GND -> GND

  Serial pH module: 9600 baud, 8N1, 5 V only
    Datasheet pin convention: TX connects to controller transmit and RX to controller receive (opposite to the usual crossed convention)
    pH module TX -> D9 Arduino software TX
    pH module RX -> D8 Arduino software RX
    If pH cannot be read, swap these two lines
    Connect the electrode to the pH module BNC socket
    datasheet example: send 01 03 00 00 00 02 C4 0B, pH = raw value / 100

  USB serial debugging at 9600 baud (UNO has no Serial1; USB/D0/D1 use Serial):
    Open the IDE serial monitor at 9600 baud to view I2C, OLED, DHT, DS, TDS, turbidity and pH logs

Libraries: U8g2, DHT sensor library, OneWire, DallasTemperature
*/

#define DHTPIN 2
#define DHTTYPE DHT11
#define ONE_WIRE_BUS 3
#define TDS_PIN A0
#define NTU_PIN A1
#define PH_RX_PIN 8
#define PH_TX_PIN 9

#define VREF 5.0
#define SCOUNT 30   // reference TDS implementation with a 30-point median filter

DHT dht(DHTPIN, DHTTYPE);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
SoftwareSerial phSerialA(PH_RX_PIN, PH_TX_PIN);  /**< RX=D8 TX=D9 */
SoftwareSerial phSerialB(PH_TX_PIN, PH_RX_PIN);  /**< RX=D9 TX=D8, swapped-pin detection */
SoftwareSerial *phSerial = &phSerialA;
/* Hardware I2C: address 0x3C detected; software I2C would conflict with Wire on A4/A5 */
U8G2_SSD1306_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

int t = 0, h = 0;
int tt = 0;
float wendu = 25.0;
float PH_Value = 0;
float tdsValue = 0;      /**< reference implementation output in ppm */
float ntuVolt = 0;       /**< turbidity output in volts; higher voltage indicates clearer water */
int mode = 0;
int cnt = 0;
uint16_t loop_n = 0;     /**< main-loop counter for serial-log comparison */
uint8_t oled_addr = 0;   /**< first I2C address detected; 0 means not found */
uint8_t oled_ok = 0;     /**< u8g2.begin return status; 1 indicates successful initialization */
uint8_t ph_swap = 0;     /**< 1=pH receives on D9 and transmits on D8 */
uint8_t ds_cnt = 0;      /**< number of DS18B20 devices on the bus */
uint8_t ds_ok = 0;       /**< 1=a valid water temperature was read in this cycle */

int analogBuffer[SCOUNT];
uint16_t t_ph_Value, t_temp;

/**
 * @brief Print remaining AVR SRAM
 * @return remaining bytes
 * @note U8g2 or serial communication may fail below 200 bytes
 */
int freeRam(void)
{
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

/**
 * @brief Hexadecimal dump to USB serial
 * @param tag line prefix
 * @param buf data
 * @param n byte count
 */
void dbg_hex(const __FlashStringHelper *tag, uint8_t *buf, uint8_t n)
{
  uint8_t i;
  Serial.print(tag);
  Serial.print(F(" n="));
  Serial.print(n);
  Serial.print(F(" "));
  for (i = 0; i < n; i++) {
    if (buf[i] < 16) {
      Serial.print('0');
    }
    Serial.print(buf[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}

/**
 * @brief Scan hardware I2C on A4/A5
 * @note SSD1306 commonly uses 0x3C; keep TWI active and use the same hardware I2C bus for display
 */
void i2c_scan(void)
{
  uint8_t addr, n = 0;
  int a4, a5;

  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
  delay(5);
  a4 = analogRead(SDA);
  a5 = analogRead(SCL);
  Serial.print(F("[I2C] A4="));
  Serial.print(a4);
  Serial.print(F(" A5="));
  Serial.println(a5);

  Wire.begin();
  for (addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("[I2C] found 0x"));
      Serial.println(addr, HEX);
      if (oled_addr == 0) {
        oled_addr = addr;
      }
      n++;
      delay(1);
    }
  }
  if (n == 0) {
    Serial.println(F("[I2C] none"));
  }
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

int getMedianNum(int bArray[], int iFilterLen)
{
  int bTab[SCOUNT];
  int i, j, bTemp;
  for (i = 0; i < iFilterLen; i++) {
    bTab[i] = bArray[i];
  }
  for (j = 0; j < iFilterLen - 1; j++) {
    for (i = 0; i < iFilterLen - j - 1; i++) {
      if (bTab[i] > bTab[i + 1]) {
        bTemp = bTab[i];
        bTab[i] = bTab[i + 1];
        bTab[i + 1] = bTemp;
      }
    }
  }
  if ((iFilterLen & 1) > 0) {
    bTemp = bTab[(iFilterLen - 1) / 2];
  }
  else {
    bTemp = (bTab[iFilterLen / 2] + bTab[iFilterLen / 2 - 1]) / 2;
  }
  return bTemp;
}

void ph_send(uint8_t *cmd, uint8_t len)
{
  uint8_t i;
  while (phSerial->available()) {
    phSerial->read();
  }
  for (i = 0; i < len; i++) {
    phSerial->write(cmd[i]);
  }
}

uint8_t ph_recv(uint8_t *buf, uint8_t maxlen, uint16_t timeout_ms)
{
  uint8_t n = 0;
  unsigned long t0 = millis();
  while ((millis() - t0) < timeout_ms) {
    while (phSerial->available() && n < maxlen) {
      buf[n++] = (uint8_t)phSerial->read();
      t0 = millis();
    }
    if (n >= 5 && (millis() - t0) > 20) {
      break;
    }
  }
  return n;
}

/**
 * @brief Switch pH software-serial pin orientation
 */
void ph_use_swap(uint8_t swap)
{
  ph_swap = swap;
  if (ph_swap) {
    phSerial = &phSerialB;
    phSerialB.listen();
  }
  else {
    phSerial = &phSerialA;
    phSerialA.listen();
  }
  Serial.print(F("[PH] RX=D"));
  Serial.print(ph_swap ? PH_TX_PIN : PH_RX_PIN);
  Serial.print(F(" TX=D"));
  Serial.println(ph_swap ? PH_RX_PIN : PH_TX_PIN);
}

void ph_read(void)
{
  // datasheet example: 01 03 00 00 00 02 C4 0B
  uint8_t cmd[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B};
  uint8_t rx[16];
  uint8_t n;
  uint16_t crc, crc_rx;

  ph_send(cmd, 8);
  n = ph_recv(rx, sizeof(rx), 200);

  dbg_hex(F("[PH] TX"), cmd, 8);
  dbg_hex(F("[PH] RX"), rx, n);

  if (n == 0) {
    Serial.println(F("[PH] FAIL timeout, try swap D8/D9"));
    ph_use_swap(ph_swap ? 0 : 1);
    return;
  }
  if (n < 9) {
    Serial.println(F("[PH] FAIL short"));
    return;
  }
  if (!(rx[0] == 0x01 && rx[1] == 0x03 && rx[2] == 0x04)) {
    Serial.println(F("[PH] FAIL header"));
    return;
  }
  crc = crc16_modbus(rx, 7);
  crc_rx = (uint16_t)rx[7] | ((uint16_t)rx[8] << 8);
  Serial.print(F("[PH] crc calc="));
  Serial.print(crc, HEX);
  Serial.print(F(" rx="));
  Serial.println(crc_rx, HEX);
  if (crc != crc_rx) {
    Serial.println(F("[PH] FAIL crc"));
    return;
  }
  t_ph_Value = ((uint16_t)rx[3] << 8) | rx[4];
  t_temp = ((uint16_t)rx[5] << 8) | rx[6];
  PH_Value = t_ph_Value / 100.0;
  Serial.print(F("[PH] OK raw="));
  Serial.print(t_ph_Value);
  Serial.print(F(" ph="));
  Serial.print(PH_Value, 2);
  Serial.print(F(" t_raw="));
  Serial.println(t_temp);
}

void DH111()
{
  float hh = dht.readHumidity();
  float tt_air = dht.readTemperature();

  Serial.print(F("[DHT] H="));
  Serial.print(hh);
  Serial.print(F(" T="));
  Serial.print(tt_air);
  if (isnan(hh) || isnan(tt_air)) {
    Serial.println(F(" FAIL"));
    return;
  }
  Serial.println(F(" OK"));
  if (hh >= 0 && hh <= 100) {
    h = (int)hh;
  }
  if (tt_air >= -20 && tt_air <= 80) {
    t = (int)tt_air;
  }
}

void w18B20_loop()
{
  uint8_t present;
  float tempC;

  present = oneWire.reset();
  if (ds_cnt == 0) {
    sensors.begin();
  }
  ds_cnt = sensors.getDeviceCount();
  tempC = sensors.getTempCByIndex(0);
  Serial.print(F("[DS] present="));
  Serial.print(present);
  Serial.print(F(" n="));
  Serial.print(ds_cnt);
  Serial.print(F(" C="));
  Serial.print(tempC);
  if (tempC != DEVICE_DISCONNECTED_C) {
    wendu = tempC;
    tt = (int)tempC;
    ds_ok = 1;
    Serial.println(F(" OK"));
  }
  else {
    ds_ok = 0;
    Serial.println(F(" FAIL, check DQ=D3 VCC=5V 4.7k"));
  }
  sensors.requestTemperatures();
}

void TDS_caiji()
{
  int i;
  int med;
  float averageVoltage;
  float compensationCoefficient;
  float compensationVolatge;
  float temperature = 25.0;

  for (i = 0; i < SCOUNT; i++) {
    analogBuffer[i] = analogRead(TDS_PIN);
    delay(40);
  }

  if (wendu > -20 && wendu < 80) {
    temperature = wendu;
  }

  med = getMedianNum(analogBuffer, SCOUNT);
  averageVoltage = med * (float)VREF / 1024.0;
  compensationCoefficient = 1.0 + 0.02 * (temperature - 25.0);
  compensationVolatge = averageVoltage / compensationCoefficient;
  tdsValue = (133.42 * compensationVolatge * compensationVolatge * compensationVolatge
              - 255.86 * compensationVolatge * compensationVolatge
              + 857.39 * compensationVolatge) * 0.5;
  if (tdsValue < 0) {
    tdsValue = 0;
  }
  if (tdsValue > 1400) {
    tdsValue = 1400;
  }

  Serial.print(F("[TDS] adc0="));
  Serial.print(analogBuffer[0]);
  Serial.print(F(" med="));
  Serial.print(med);
  Serial.print(F(" V="));
  Serial.print(averageVoltage, 3);
  Serial.print(F(" tds="));
  Serial.print((int)tdsValue);
  if (med == 0) {
    Serial.println(F(" (0=probe not immersed or A0 not connected)"));
  }
  else {
    Serial.println();
  }
}

void NTU_caiji()
{
  int adc;
  adc = analogRead(NTU_PIN);
  /* Turbidity conversion: voltage = adc * (5.0 / 1024.0); display voltage only */
  ntuVolt = adc * (5.0 / 1024.0);

  Serial.print(F("[TURB] adc="));
  Serial.print(adc);
  Serial.print(F(" V="));
  Serial.println(ntuVolt, 3);
}

void OLED_setup()
{
  i2c_scan();
  if (oled_addr == 0) {
    oled_addr = 0x3C;
  }
  u8g2.setI2CAddress(oled_addr << 1);
  oled_ok = u8g2.begin();
  u8g2.setBusClock(100000);
  u8g2.setContrast(255);
  u8g2.setPowerSave(0);
  Serial.print(F("[OLED] addr=0x"));
  Serial.print(oled_addr, HEX);
  Serial.print(F(" begin="));
  Serial.println(oled_ok);

  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.firstPage();
  do {
    u8g2.drawFrame(0, 0, 128, 64);
    u8g2.drawStr(8, 16, "OLED splash");
    u8g2.drawStr(8, 32, "shui A DBG");
  } while (u8g2.nextPage());
  Serial.println(F("[OLED] splash done"));
}

void OLED_loop()
{
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.firstPage();
  do {
    u8g2.setCursor(0, 14);
    u8g2.print(F("Air:"));
    u8g2.print(t);
    u8g2.print(F("    Humidity:"));
    u8g2.print(h);

    u8g2.setCursor(0, 30);
    u8g2.print(F("Water:"));
    if (ds_ok) {
      u8g2.print(wendu, 1);
    }
    else {
      u8g2.print(F("--"));
    }
    u8g2.print(F("  pH:"));
    u8g2.print(PH_Value, 2);

    u8g2.setCursor(0, 46);
    u8g2.print(F("TDS:"));
    u8g2.print((int)tdsValue);
    u8g2.print(F(" ppm"));

    u8g2.setCursor(0, 62);
    u8g2.print(F("Turbidity:"));
    u8g2.print(ntuVolt, 2);
    u8g2.print(F(" V"));
  } while (u8g2.nextPage());
}

void setup()
{
  /* UNO/NANO has one hardware UART: Serial=USB=D0/D1 for debugging */
  Serial.begin(9600);
  delay(300);
  Serial.println();
  Serial.println(F("======== shui A DBG ========"));
  Serial.print(F("[SYS] ram="));
  Serial.println(freeRam());

  phSerialA.begin(9600);
  phSerialB.begin(9600);
  ph_use_swap(0);

  pinMode(TDS_PIN, INPUT);
  pinMode(NTU_PIN, INPUT);
  Serial.print(F("[ADC] A0="));
  Serial.print(analogRead(TDS_PIN));
  Serial.print(F(" A1="));
  Serial.println(analogRead(NTU_PIN));

  OLED_setup();

  dht.begin();
  Serial.println(F("[DHT] begin D2"));

  pinMode(ONE_WIRE_BUS, INPUT_PULLUP);
  sensors.begin();
  ds_cnt = sensors.getDeviceCount();
  sensors.requestTemperatures();
  Serial.print(F("[DS] begin D3 cnt="));
  Serial.println(ds_cnt);
  if (ds_cnt == 0) {
    Serial.println(F("[DS] no device: DQ=D3 + 4.7k to 5V"));
  }

  Serial.print(F("[SYS] setup ram="));
  Serial.println(freeRam());
  Serial.println(F("[SYS] setup done"));
}

void loop()
{
  unsigned long t0 = millis();

  loop_n++;
  Serial.println(F("----"));
  Serial.print(F("[LOOP] n="));
  Serial.print(loop_n);
  Serial.print(F(" ms="));
  Serial.print(t0);
  Serial.print(F(" ram="));
  Serial.println(freeRam());

  OLED_loop();
  Serial.println(F("[OLED] draw done"));
  DH111();
  w18B20_loop();
  TDS_caiji();
  NTU_caiji();
  ph_read();

  Serial.print(F("[VAL] Wt="));
  Serial.print(wendu, 1);
  Serial.print(F(" TDS="));
  Serial.print((int)tdsValue);
  Serial.print(F(" PH="));
  Serial.print(PH_Value, 2);
  Serial.print(F(" Turb="));
  Serial.print(ntuVolt, 2);
  Serial.print(F(" At="));
  Serial.print(t);
  Serial.print(F(" H="));
  Serial.print(h);
  Serial.print(F(" mode="));
  Serial.print(mode);
  Serial.print(F(" dt="));
  Serial.println(millis() - t0);
}
