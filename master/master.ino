// // Master_Enhanced.ino
// ESP32 + SX127x + SD Card + Buzzer
// Polls slaves, logs JSON data to SD card, handles alerts
// Author: Enhanced by Assistant

#include <SPI.h>
#include <LoRa.h>
#include <SD.h>
#include <ArduinoJson.h>

// ============ LORA PINS ============
#define LORA_NSS    5
#define LORA_RST    14
#define LORA_DIO0   26
#define LORA_SCK    18
#define LORA_MISO   19
#define LORA_MOSI   23

// ============ SD CARD PINS ============
#define SD_CS       15    // Chip Select for SD card

// ============ OTHER PINS ============
#define BUZZER_PIN  27    // Active buzzer

// ============ NODE CONFIG ============
const String nodes[] = {"NODE1", "NODE2", "NODE3"};
const int NODE_COUNT = sizeof(nodes) / sizeof(nodes[0]);

// ============ TIMING CONFIG ============
#define POLL_INTERVAL     3000    // 3 seconds between polls
#define RESPONSE_TIMEOUT  1500    // Wait 1.5s for slave reply
#define MAX_RETRIES       2       // Retry twice before skip

// ============ BUZZER CONFIG ============
#define ALERT_BEEP_COUNT  3
#define ALERT_BEEP_MS     200
#define BUTTON_BEEP_MS    100

struct NodeState {
  bool awaiting = false;
  unsigned long lastSend = 0;
  int retries = 0;
};

NodeState nodeState[NODE_COUNT];
int currentIndex = 0;
unsigned long lastPollCycle = 0;
bool alertPending = false;

bool sdCardOK = false;
String logFileName = "/data.txt";

// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== LoRa Master Node ===");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Startup beep
  beep(BUTTON_BEEP_MS);
  delay(100);
  beep(BUTTON_BEEP_MS);

  // Initialize LoRa
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("ERROR: LoRa init failed!");
    while (1) {
      beep(50);
      delay(500);
    }
  }
  Serial.println("✓ LoRa initialized");

  // Initialize SD Card (shared SPI bus)
  if (!SD.begin(SD_CS)) {
    Serial.println("WARNING: SD Card init failed!");
    Serial.println("Data logging disabled.");
    sdCardOK = false;
  } else {
    sdCardOK = true;
    Serial.println("✓ SD Card initialized");
    
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
      Serial.println("No SD card attached");
      sdCardOK = false;
    } else {
      Serial.print("SD Card Type: ");
      if (cardType == CARD_MMC) Serial.println("MMC");
      else if (cardType == CARD_SD) Serial.println("SDSC");
      else if (cardType == CARD_SDHC) Serial.println("SDHC");
      
      uint64_t cardSize = SD.cardSize() / (1024 * 1024);
      Serial.printf("SD Card Size: %lluMB\n", cardSize);
      
      // Write header if file doesn't exist
      if (!SD.exists(logFileName)) {
        writeLogHeader();
      }
    }
  }

  Serial.println("=== Master Ready ===\n");
  beep(BUTTON_BEEP_MS);
}

// ============================================
void loop() {
  unsigned long now = millis();

  // Handle incoming LoRa packets
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String incoming = "";
    while (LoRa.available()) {
      incoming += (char)LoRa.read();
    }
    incoming.trim();

    if (incoming.length() > 0) {
      int rssi = LoRa.packetRssi();
      float snr = LoRa.packetSnr();
      
      Serial.print("[RX RSSI=");
      Serial.print(rssi);
      Serial.print(" SNR=");
      Serial.print(snr);
      Serial.print("] ");
      Serial.println(incoming);
      
      handleIncoming(incoming, rssi, snr);
    }
  }

  // Poll next node when ready
  if (!nodeState[currentIndex].awaiting &&
      (now - lastPollCycle >= POLL_INTERVAL)) {
    lastPollCycle = now;
    pollNode(currentIndex);
  }

  // Handle timeouts
  for (int i = 0; i < NODE_COUNT; i++) {
    if (nodeState[i].awaiting &&
        (now - nodeState[i].lastSend > RESPONSE_TIMEOUT)) {
      
      Serial.print("⏱ Timeout: ");
      Serial.println(nodes[i]);
      
      nodeState[i].retries++;

      if (nodeState[i].retries < MAX_RETRIES) {
        Serial.print("🔄 Retry ");
        Serial.print(nodes[i]);
        Serial.print(" (");
        Serial.print(nodeState[i].retries);
        Serial.println(")");
        pollNode(i);
      } else {
        Serial.print("❌ Skipping ");
        Serial.println(nodes[i]);
        logTimeout(nodes[i]);
        nodeState[i].awaiting = false;
        nodeState[i].retries = 0;
        currentIndex = (currentIndex + 1) % NODE_COUNT;
      }
    }
  }

  // Handle alert broadcast
  if (alertPending) {
    broadcastAlert();
    alertPending = false;
  }
}

// ============================================
void pollNode(int index) {
  String req = "REQ:" + nodes[index];
  
  LoRa.beginPacket();
  LoRa.print(req);
  LoRa.endPacket();

  nodeState[index].awaiting = true;
  nodeState[index].lastSend = millis();

  Serial.print("📤 Poll -> ");
  Serial.println(req);
}

// ============================================
void handleIncoming(const String &payload, int rssi, float snr) {
  // Check if it's JSON data
  if (payload.startsWith("{") && payload.endsWith("}")) {
    
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print("JSON parse error: ");
      Serial.println(error.c_str());
      return;
    }

    // Extract node ID
    const char* nodeId = doc["node"];
    if (nodeId == nullptr) {
      Serial.println("Missing 'node' field in JSON");
      return;
    }

    // Mark node as responded
    for (int i = 0; i < NODE_COUNT; i++) {
      if (nodes[i] == String(nodeId)) {
        nodeState[i].awaiting = false;
        nodeState[i].retries = 0;
        break;
      }
    }

    // Display parsed data
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━");
    Serial.print("Node: ");
    Serial.println(nodeId);
    Serial.print("Temp: ");
    Serial.print(doc["temp"].as<float>());
    Serial.println(" °C");
    Serial.print("Pressure: ");
    Serial.print(doc["pres"].as<float>());
    Serial.println(" hPa");
    Serial.print("Altitude: ");
    Serial.print(doc["alt"].as<int>());
    Serial.println(" m");
    Serial.print("Battery: ");
    Serial.print(doc["bat"].as<int>());
    Serial.println(" %");
    Serial.print("Alert: ");
    Serial.println(doc["alert"].as<int>());
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━");

    // Log to SD card
    logData(payload, nodeId, rssi, snr);

    // Check for alert
    if (doc["alert"].as<int>() == 1) {
      Serial.println("⚠️ ALERT FLAG DETECTED!");
      beep(BUTTON_BEEP_MS);
      delay(100);
      beep(BUTTON_BEEP_MS);
      alertPending = true;
    }

    // Move to next node
    currentIndex = (currentIndex + 1) % NODE_COUNT;
  }
  else {
    Serial.print("Non-JSON message: ");
    Serial.println(payload);
  }
}

// ============================================
void broadcastAlert() {
  Serial.println("\n🚨 BROADCASTING ALERT TO ALL NODES 🚨");
  
  // Alert beeps
  for (int i = 0; i < ALERT_BEEP_COUNT; i++) {
    beep(ALERT_BEEP_MS);
    delay(ALERT_BEEP_MS);
  }

  // Send broadcast
  LoRa.beginPacket();
  LoRa.print("BROADCAST:ALERT");
  LoRa.endPacket();
  
  Serial.println("Alert broadcast sent\n");
}

// ============================================
void beep(int duration) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(duration);
  digitalWrite(BUZZER_PIN, LOW);
}

// ============================================
void writeLogHeader() {
  File file = SD.open(logFileName, FILE_WRITE);
  if (file) {
    file.println("=== LoRa Sensor Network Data Log ===");
    file.println("Timestamp,NodeID,Temp,Pressure,Altitude,Battery,Alert,RSSI,SNR,JSON");
    file.close();
    Serial.println("Log header written");
  } else {
    Serial.println("Failed to write log header");
  }
}

// ============================================
void logData(const String &jsonStr, const String &nodeId, int rssi, float snr) {
  if (!sdCardOK) return;

  StaticJsonDocument<256> doc;
  deserializeJson(doc, jsonStr);

  File file = SD.open(logFileName, FILE_APPEND);
  if (file) {
    // Timestamp (millis since boot)
    file.print(millis());
    file.print(",");
    file.print(nodeId);
    file.print(",");
    file.print(doc["temp"].as<float>());
    file.print(",");
    file.print(doc["pres"].as<float>());
    file.print(",");
    file.print(doc["alt"].as<int>());
    file.print(",");
    file.print(doc["bat"].as<int>());
    file.print(",");
    file.print(doc["alert"].as<int>());
    file.print(",");
    file.print(rssi);
    file.print(",");
    file.print(snr);
    file.print(",");
    file.println(jsonStr);
    file.close();
    
    Serial.println("✓ Logged to SD");
  } else {
    Serial.println("❌ SD write failed");
  }
}

// ============================================
void logTimeout(const String &nodeId) {
  if (!sdCardOK) return;

  File file = SD.open(logFileName, FILE_APPEND);
  if (file) {
    file.print(millis());
    file.print(",");
    file.print(nodeId);
    file.println(",TIMEOUT,,,,,,,");
    file.close();
  }
}