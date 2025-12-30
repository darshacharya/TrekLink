// ============================================================
// Slave_Enhanced_Final.ino
// ESP32 + SX127x + BMP280/085 + GPS + Battery Monitor + LED + Button
// Author: Enhanced version by Assistant
// Features:
// - JSON LoRa telemetry: temperature, pressure, altitude, battery %, GPS lat/lon, alert flag
// - ADC calibration & hysteresis to stabilize high-impedance battery readings
// ============================================================

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_BMP085.h>
#include <ArduinoJson.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include "esp_adc_cal.h"

// ================= LORA CONFIG =================
#define LORA_NSS   5
#define LORA_RST   14
#define LORA_DIO0  26
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_FREQ  433E6

// ================= GPIO PINS =================
#define LED_PIN       25
#define BUTTON_PIN    4
#define BATTERY_PIN   34

// ================= GPS CONFIG =================
#define GPS_RX      16   // RX of ESP32 ← TX of GPS
#define BUTTON_PIN    4
#define BATTERY_PIN   34

// ================= GPS CONFIG =================
#define GPS_RX      16   // RX of ESP32 ← TX of GPS
#define GPS_TX      17   // TX of ESP32 → RX of GPS

// ================= NODE CONFIG =================
#define NODE_ID "NODE3"



#define GPS_TX      17   // TX of ESP32 → RX of GPS

// ================= NODE CONFIG =================
#define NODE_ID "NODE3"

// ================= BATTERY CONFIG =================
#define R1 150000.0
#define R2 100000.0
#define VREF_CALIBRATION 1.0
#define BATT_FULL 8.4
#define BATT_EMPTY 5.5
#define DEFAULT_VREF 1100  // mV, internal reference for ESP32 ADC

// ================= SENSOR CONFIG =================
#define SEA_LEVEL_PRESSURE 1013.25

// ================= OBJECTS =================
Adafruit_BMP280 bmp280;
Adafruit_BMP085 bmp085;
TinyGPSPlus gps;
HardwareSerial GPSSerial(1);
esp_adc_cal_characteristics_t *adc_chars;

// ================= FLAGS =================
bool haveBMP280 = false;
bool haveBMP085 = false;
bool alertFlag = false;
bool blinking = false;
unsigned long blinkStart = 0;
const unsigned long BLINK_MS = 10000UL;

// ============================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BATTERY_PIN, INPUT);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // --- Initialize ADC calibration ---
  setup_adc_cal();

  // --- Initialize GPS ---
  GPSSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("GPS initialized at 9600 baud");

  // --- Initialize LoRa ---
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa init failed!");
    while (true) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(200);
    }
  }
  Serial.println("LoRa init OK");

  // --- Detect BMP Sensor ---
  Wire.begin();
  if (bmp280.begin(0x76) || bmp280.begin(0x77)) {
    haveBMP280 = true;
    Serial.println("Detected BMP280");
  } else if (bmp085.begin()) {
    haveBMP085 = true;
    Serial.println("Detected BMP085");
  } else {
    Serial.println("WARNING: No BMP sensor detected!");
  }

  Serial.print("Node ");
  Serial.print(NODE_ID);
  Serial.println(" ready");

  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }
}

// ============================================================
void loop() {
  // --- Feed GPS parser ---
  while (GPSSerial.available() > 0) {
    gps.encode(GPSSerial.read());
  }

  // --- Check button press (debounced) ---
  static unsigned long lastButtonTime = 0;
  if (digitalRead(BUTTON_PIN) == LOW && (millis() - lastButtonTime > 200)) {
    alertFlag = true;
    lastButtonTime = millis();
    Serial.println("Button pressed -> alertFlag set");
    digitalWrite(LED_PIN, HIGH);
    delay(50);
    digitalWrite(LED_PIN, LOW);
  }

  // --- Handle incoming LoRa packets ---
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String payload = "";
    while (LoRa.available()) payload += (char)LoRa.read();
    payload.trim();

    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    Serial.print("[RX RSSI=");
    Serial.print(rssi);
    Serial.print(" SNR=");
    Serial.print(snr);
    Serial.print("] ");
    Serial.println(payload);

    if (payload == String("REQ:") + NODE_ID) {
      delay(random(50, 150));  // collision avoidance
      sendSensorData();
    } else if (payload.startsWith("BROADCAST:ALERT")) {
      startBlinking();
    }
  }

  // --- LED blinking (non-blocking) ---
  if (blinking) {
    unsigned long now = millis();
    if (now - blinkStart < BLINK_MS) {
      digitalWrite(LED_PIN, ((now / 125) % 2) ? HIGH : LOW);
    } else {
      blinking = false;
      digitalWrite(LED_PIN, LOW);
      Serial.println("Blinking stopped");
    }
  }
}

// ============================================================
// Send sensor data as JSON
void sendSensorData() {
  float temp = NAN, pres = NAN, alt = NAN;

  if (haveBMP280) {
    temp = bmp280.readTemperature();
    pres = bmp280.readPressure() / 100.0;
    alt = bmp280.readAltitude(SEA_LEVEL_PRESSURE);
  } else if (haveBMP085) {
    temp = bmp085.readTemperature();
    pres = bmp085.readPressure() / 100.0;
    alt = bmp085.readAltitude(SEA_LEVEL_PRESSURE * 100);
  }

  float battPct = readBatteryPercent();

  double lat = gps.location.isValid() ? gps.location.lat() : 0.0;
  double lon = gps.location.isValid() ? gps.location.lng() : 0.0;

  StaticJsonDocument<192> doc;
  doc["node"] = NODE_ID;
  doc["temp"] = isnan(temp) ? 0 : round(temp * 100) / 100.0;
  doc["pres"] = isnan(pres) ? 0 : round(pres * 10) / 10.0;
  doc["alt"]  = isnan(alt)  ? 0 : (int)round(alt);
  doc["bat"]  = (int)round(battPct);
  doc["alert"] = alertFlag ? 1 : 0;
  doc["lat"]  = lat;
  doc["lon"]  = lon;

  String jsonStr;
  serializeJson(doc, jsonStr);

  LoRa.beginPacket();
  LoRa.print(jsonStr);
  LoRa.endPacket();

  Serial.print("Sent -> ");
  Serial.println(jsonStr);

  alertFlag = false;
}

// ============================================================
// Initialize ADC calibration
void setup_adc_cal() {
  adc_chars = (esp_adc_cal_characteristics_t*)calloc(1, sizeof(esp_adc_cal_characteristics_t));
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, DEFAULT_VREF, adc_chars);
}

// ============================================================
// Read and stabilize battery voltage
float readBatteryPercent() {
  const int numReadings = 50;
  int readings[numReadings];

  // take multiple readings
  for (int i = 0; i < numReadings; i++) {
    readings[i] = analogRead(BATTERY_PIN);
    delay(3);
  }

  // sort for median
  for (int i = 0; i < numReadings - 1; i++) {
    for (int j = i + 1; j < numReadings; j++) {
      if (readings[i] > readings[j]) {
        int t = readings[i];
        readings[i] = readings[j];
        readings[j] = t;
      }
    }
  }

  int mid = numReadings / 2;
  int median = readings[mid];
  float avg = (readings[mid - 1] + median + readings[mid + 1]) / 3.0;

  uint32_t mv = esp_adc_cal_raw_to_voltage((uint32_t)avg, adc_chars);
  float vDivider = (mv / 1000.0);
  float vBatt = vDivider * ((R1 + R2) / R2) * VREF_CALIBRATION;

  if (vBatt > BATT_FULL) vBatt = BATT_FULL;
  if (vBatt < BATT_EMPTY) vBatt = BATT_EMPTY;

  float percent = ((vBatt - BATT_EMPTY) / (BATT_FULL - BATT_EMPTY)) * 100.0;
  if (percent > 100.0) percent = 100.0;
  if (percent < 0.0) percent = 0.0;

  // add hysteresis
  static float lastPercent = -1.0;
  if (lastPercent < 0) lastPercent = percent;
  if (fabs(percent - lastPercent) < 1.0) {
    percent = lastPercent;
  } else {
    lastPercent = percent;
  }

  return percent;
}

// ============================================================
// Start LED blinking for alert
void startBlinking() {
  blinking = true;
  blinkStart = millis();
  Serial.println("ALERT broadcast received -> LED blinking for 10s");
}
