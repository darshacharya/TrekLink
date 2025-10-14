// /* Slave_AutoBMP.ino
//    ESP32 + SX127x, BMP085 or BMP280 autodetect
// */
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_BMP085.h>

#define LORA_NSS   5
#define LORA_RST   14
#define LORA_DIO0  26
#define LORA_FREQ  433E6

#define LED_PIN    25
#define BUTTON_PIN 4

// Change this per node
#define NODE_ID "NODE1"   // set to "NODE1", "NODE2", "NODE3"

Adafruit_BMP280 bmp280;
Adafruit_BMP085 bmp085;

bool haveBMP280 = false;
bool haveBMP085 = false;

bool alertFlag = false;      // set when button pressed; sent to master on next poll
bool blinking = false;
unsigned long blinkStart = 0;
const unsigned long BLINK_MS = 10000UL;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa init failed!");
    while (true);
  }
  Serial.println("LoRa init OK");

  // Try detect BMP280 first (addresses 0x76/0x77)
  Wire.begin(); // default SDA/SCL (21/22 on most ESP32)
  if (bmp280.begin(0x76) || bmp280.begin(0x77)) {
    haveBMP280 = true;
    Serial.println("Detected BMP280");
  } else if (bmp085.begin()) {
    haveBMP085 = true;
    Serial.println("Detected BMP085");
  } else {
    Serial.println("No BMP detected");
  }

  Serial.print("Node ");
  Serial.print(NODE_ID);
  Serial.println(" ready");
}

void loop() {
  // 1) read button (non-blocking)
  if (digitalRead(BUTTON_PIN) == LOW) {
    // simple debounce/time window could be added; keep it basic
    alertFlag = true;
    Serial.println("Button -> alertFlag set");
    delay(50); // small debounce
  }

  // 2) check incoming packets
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String payload = "";
    while (LoRa.available()) payload += (char)LoRa.read();
    int rssi = LoRa.packetRssi();

    Serial.print("[RX RSSI=");
    Serial.print(rssi);
    Serial.print("] ");
    Serial.println(payload);

    // If master requests us
    if (payload == String("REQ:") + NODE_ID) {
      sendSensorData();
    }
    // If broadcast alert received
    else if (payload.startsWith("ALERT_ON")) {
      startBlinking();
    } else {
      // ignore other messages
    }
  }

  // 3) handle blinking non-blocking
  if (blinking) {
    unsigned long now = millis();
    if (now - blinkStart < BLINK_MS) {
      // fast blink pattern without blocking
      digitalWrite(LED_PIN, ((now / 250) % 2) ? HIGH : LOW);
    } else {
      blinking = false;
      digitalWrite(LED_PIN, LOW);
    }
  }
}

// Read sensors and send DATA message
void sendSensorData() {
  float temp = NAN;
  float pres = NAN;

  if (haveBMP280) {
    temp = bmp280.readTemperature();
    pres = bmp280.readPressure() / 100.0; // hPa
  } else if (haveBMP085) {
    temp = bmp085.readTemperature();
    pres = bmp085.readPressure() / 100.0;
  }

  String msg = "DATA:" + String(NODE_ID);
  msg += ",Temp:" + String(temp, 2);
  msg += ",Pres:" + String(pres, 2);
  msg += ",Alert:" + String(alertFlag ? 1 : 0);
  delay(random(30, 120));
  LoRa.beginPacket();
  LoRa.print(msg);
  LoRa.endPacket();

  Serial.print("Sent -> ");
  Serial.println(msg);
  alertFlag = false; // reset after sending
}

void startBlinking() {
  blinking = true;
  blinkStart = millis();
  Serial.println("ALERT broadcast received -> blinking");
}












// ________________________________
// #include <SPI.h>
// #include <LoRa.h>

// #define NSS   5
// #define RST   14
// #define DIO0  26
// #define FREQ  433E6

// void setup() {
//   Serial.begin(115200);
//   SPI.begin(18, 19, 23, NSS);
//   LoRa.setPins(NSS, RST, DIO0);

//   if (!LoRa.begin(FREQ)) {
//     Serial.println("LoRa init failed!");
//     while (true);
//   }

//   LoRa.setSpreadingFactor(12);
//   LoRa.setSignalBandwidth(125E3);
//   LoRa.setCodingRate4(5);

//   Serial.println("Node B (Ponger) ready");
// }

// void loop() {
//   int packetSize = LoRa.parsePacket();
//   if (packetSize) {
//     String msg = "";
//     while (LoRa.available()) msg += (char)LoRa.read();
//     Serial.println("Received: " + msg);

//     if (msg == "ping") {
//       delay(200);  // prevent collision
//       LoRa.beginPacket();
//       LoRa.print("pong");
//       LoRa.endPacket();
//       Serial.println("Sent: pong");
//     }
//   }
// }
