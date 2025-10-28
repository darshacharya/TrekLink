// Slave_Enhanced.ino
// ESP32 + SX127x + BMP280/085 + Battery Monitor + LED + Button
// Author: Enhanced by Assistant
// JSON format data transmission with altitude and battery percentage

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_BMP085.h>
#include <ArduinoJson.h>

// ============ LORA PINS ============
#define LORA_NSS   5
#define LORA_RST   14
#define LORA_DIO0  26
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_FREQ  433E6

// ============ GPIO PINS ============
#define LED_PIN       25
#define BUTTON_PIN    4
#define BATTERY_PIN   34  // ADC1_CH6

// ============ NODE CONFIG ============
#define NODE_ID "NODE3"   // Change to "NODE1", "NODE2", "NODE3"

// ============ BATTERY CONFIG ============
#define R1 150000.0  // 150k ohm (Brown Green Yellow)
#define R2 100000.0  // 100k ohm (Brown Black Yellow)
#define VREF 3.6     // ESP32 ADC reference at 11dB attenuation
#define ADC_MAX 4095.0
#define BATT_FULL 8.4    // 2S Li-ion fully charged
#define BATT_EMPTY 5.5   // 2S Li-ion empty (adjust based on your usage)
#define ADC_SAMPLES 20   // More samples for stability

// Calibration factor (fine-tune if needed after testing)
#define VREF_CALIBRATION 1.0

// Sea level pressure for altitude calculation
#define SEA_LEVEL_PRESSURE 1013.25

Adafruit_BMP280 bmp280;
Adafruit_BMP085 bmp085;

bool haveBMP280 = false;
bool haveBMP085 = false;

bool alertFlag = false;
bool blinking = false;
unsigned long blinkStart = 0;
const unsigned long BLINK_MS = 10000UL;

// ============================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BATTERY_PIN, INPUT);

  // Configure ADC for better accuracy
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // 0-3.6V range (with non-linearity above 3.1V)

  // Initialize LoRa
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

  // Detect BMP sensor
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
  
  // Startup blink
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }
}

// ============================================
void loop() {
  // 1) Check button (with debounce)
  static unsigned long lastButtonTime = 0;
  if (digitalRead(BUTTON_PIN) == LOW && (millis() - lastButtonTime > 200)) {
    alertFlag = true;
    lastButtonTime = millis();
    Serial.println("Button pressed -> alertFlag set");
    digitalWrite(LED_PIN, HIGH);
    delay(50);
    digitalWrite(LED_PIN, LOW);
  }

  // 2) Check for incoming LoRa packets
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String payload = "";
    while (LoRa.available()) {
      payload += (char)LoRa.read();
    }
    payload.trim();
    
    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    Serial.print("[RX RSSI=");
    Serial.print(rssi);
    Serial.print(" SNR=");
    Serial.print(snr);
    Serial.print("] ");
    Serial.println(payload);

    // Handle master poll request
    if (payload == String("REQ:") + NODE_ID) {
      delay(random(50, 150)); // Random delay to avoid collisions
      sendSensorData();
    }
    // Handle broadcast alert
    else if (payload.startsWith("BROADCAST:ALERT")) {
      startBlinking();
    }
  }

  // 3) Handle LED blinking (non-blocking)
  if (blinking) {
    unsigned long now = millis();
    if (now - blinkStart < BLINK_MS) {
      // Fast blink: 250ms period (4Hz)
      digitalWrite(LED_PIN, ((now / 125) % 2) ? HIGH : LOW);
    } else {
      blinking = false;
      digitalWrite(LED_PIN, LOW);
      Serial.println("Blinking stopped");
    }
  }
}

// ============================================
// Read all sensors and send JSON data
void sendSensorData() {
  float temp = NAN;
  float pres = NAN;
  float alt = NAN;

  // Read BMP sensor
  if (haveBMP280) {
    temp = bmp280.readTemperature();
    pres = bmp280.readPressure() / 100.0; // Convert Pa to hPa
    alt = bmp280.readAltitude(SEA_LEVEL_PRESSURE);
  } else if (haveBMP085) {
    temp = bmp085.readTemperature();
    pres = bmp085.readPressure() / 100.0;
    alt = bmp085.readAltitude(SEA_LEVEL_PRESSURE * 100); // Needs Pa
  }

  // Read battery percentage
  float battPct = readBatteryPercent();

  // Create JSON (compact format)
  StaticJsonDocument<128> doc;
  doc["node"] = NODE_ID;
  doc["temp"] = isnan(temp) ? 0 : round(temp * 100) / 100.0;
  doc["pres"] = isnan(pres) ? 0 : round(pres * 10) / 10.0;
  doc["alt"] = isnan(alt) ? 0 : (int)round(alt);
  doc["bat"] = (int)round(battPct);
  doc["alert"] = alertFlag ? 1 : 0;

  String jsonStr;
  serializeJson(doc, jsonStr);

  // Send via LoRa
  LoRa.beginPacket();
  LoRa.print(jsonStr);
  LoRa.endPacket();

  Serial.print("Sent -> ");
  Serial.println(jsonStr);

  // Reset alert flag after sending
  alertFlag = false;
}

// ============================================
// Read battery voltage and calculate percentage

float readBatteryPercent() {
  const int numReadings = 20;
  int readings[numReadings];
  
  // Take readings
  for (int i = 0; i < numReadings; i++) {
    readings[i] = analogRead(BATTERY_PIN);
    delay(5); // Short delay between samples
  }

  // Sort for median
  for (int i = 0; i < numReadings - 1; i++) {
    for (int j = i + 1; j < numReadings; j++) {
      if (readings[i] > readings[j]) {
        int temp = readings[i];
        readings[i] = readings[j];
        readings[j] = temp;
      }
    }
  }

  // Use median (middle value)
  float adcValue = readings[numReadings / 2];
  
  // Rest of your voltage calculation...
  float vDivider = (adcValue / ADC_MAX) * 3.6 * VREF_CALIBRATION;
  float vBatt = vDivider * ((R1 + R2) / R2);

  // Clamp and calculate percent (same as before)
  if (vBatt > 8.4) vBatt = 8.4;
  if (vBatt < 5.5) vBatt = 5.5;

  float percent = ((vBatt - BATT_EMPTY) / (BATT_FULL - BATT_EMPTY)) * 100.0;
  if (percent > 100.0) percent = 100.0;
  if (percent < 0.0) percent = 0.0;

  return percent;
}

// ============================================
void startBlinking() {
  blinking = true;
  blinkStart = millis();
  Serial.println("ALERT broadcast received -> LED blinking for 10s");
}