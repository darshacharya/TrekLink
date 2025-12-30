// ============================================================
// Master_Enhanced.ino - SPIFFS Version (No SD Card Required!)
// ESP32 + SX127x + Internal Flash Logging + Buzzer + LED Alert
// Polls slaves, logs JSON data to internal flash, handles alerts
// ============================================================
//
// HARDWARE CONNECTIONS:
// ============================================================
//
// LoRa SX127x Module (VSPI):
//   NSS/CS   -> GPIO 5
//   RST      -> GPIO 14
//   DIO0     -> GPIO 26
//   SCK      -> GPIO 18
//   MISO     -> GPIO 19
//   MOSI     -> GPIO 23
//   VCC      -> 3.3V
//   GND      -> GND
//
// Buzzer & LED:
//   BUZZER   -> GPIO 21 (active buzzer, with GND)
//   LED      -> GPIO 22 (with 220Ω resistor to GND)
//
// NO SD CARD NEEDED! Uses internal ESP32 flash storage (SPIFFS)
//
// IMPORTANT:
// - Go to Tools > Partition Scheme > "Default 4MB with spiffs"
// - First upload: Use Tools > ESP32 Sketch Data Upload (to format SPIFFS)
//   OR the code will auto-format on first run
//
// ============================================================

#include <SPI.h>
#include <LoRa.h>
#include <SPIFFS.h>  // Internal flash file system
#include <ArduinoJson.h>

// ---------- LORA (VSPI) ----------
#define LORA_NSS   5
#define LORA_RST   14
#define LORA_DIO0  26
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23

// ---------- ALERT HARDWARE ----------
#define BUZZER_PIN 21
#define LED_PIN    22

// ---------- NODES ----------
const String nodes[] = {"NODE1", "NODE2", "NODE3"};
const int NODE_COUNT = sizeof(nodes) / sizeof(nodes[0]);

// ---------- TIMING ----------
#define POLL_INTERVAL     3000
#define RESPONSE_TIMEOUT  1500
#define MAX_RETRIES       2

// ---------- ALERT SETTINGS ----------
#define ALERT_BEEP_COUNT  3
#define ALERT_BEEP_MS     200
#define BUTTON_BEEP_MS    100
#define LED_BLINK_MS      300
#define LED_ALERT_DURATION 5000

// ---------- LOGGING SETTINGS ----------
#define MAX_LOG_SIZE      500000  // 500KB max log size
#define LOG_BUFFER_SIZE   10      // Buffer N entries before writing

struct NodeState {
  bool awaiting = false;
  unsigned long lastSend = 0;
  int retries = 0;
};

NodeState nodeState[NODE_COUNT];
int currentIndex = 0;
unsigned long lastPollCycle = 0;

bool alertPending = false;
bool ledBlinking = false;
unsigned long ledBlinkStart = 0;
unsigned long lastLedToggle = 0;

bool storageOK = false;
String logFileName = "/data.txt";
String logBuffer = "";
int bufferCount = 0;

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n╔════════════════════════════════╗");
  Serial.println("║   LoRa Master Node v3.0        ║");
  Serial.println("║   (SPIFFS Internal Storage)    ║");
  Serial.println("╚════════════════════════════════╝\n");

  // ---------------- GPIO Setup ----------------
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  beep(BUTTON_BEEP_MS);
  delay(100);
  beep(BUTTON_BEEP_MS);
  delay(200);

  // ---------------- LoRa Init ----------------
  Serial.println("→ Initializing LoRa...");
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("❌ LoRa init failed!");
    while (true) {
      beep(50);
      delay(400);
    }
  }
  
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setSyncWord(0x12);
  
  Serial.println("✓ LoRa initialized (433MHz)\n");

  // ---------------- SPIFFS Init ----------------
  Serial.println("→ Initializing internal storage (SPIFFS)...");
  
  if (!SPIFFS.begin(true)) {  // true = format if mount fails
    Serial.println("❌ SPIFFS mount failed!");
    Serial.println("   Logging DISABLED\n");
    storageOK = false;
    beep(50);
    delay(100);
    beep(50);
  } else {
    storageOK = true;
    Serial.println("✓ SPIFFS mounted successfully");
    
    size_t totalBytes = SPIFFS.totalBytes();
    size_t usedBytes = SPIFFS.usedBytes();
    
    Serial.printf("  Total: %u bytes (%.1f KB)\n", totalBytes, totalBytes/1024.0);
    Serial.printf("  Used:  %u bytes (%.1f KB)\n", usedBytes, usedBytes/1024.0);
    Serial.printf("  Free:  %u bytes (%.1f KB)\n", totalBytes-usedBytes, (totalBytes-usedBytes)/1024.0);
    
    // Check if log file exists
    if (!SPIFFS.exists(logFileName)) {
      writeLogHeader();
      Serial.println("  Created new log file");
    } else {
      File f = SPIFFS.open(logFileName, "r");
      if (f) {
        Serial.printf("  Existing log: %u bytes\n", f.size());
        f.close();
      }
    }
    Serial.println();
    beep(BUTTON_BEEP_MS);
  }

  // ---------------- Ready ----------------
  Serial.println("╔════════════════════════════════╗");
  Serial.println("║   System Ready - Polling...    ║");
  Serial.println("╚════════════════════════════════╝\n");
  
  beep(BUTTON_BEEP_MS);
  delay(100);
  beep(BUTTON_BEEP_MS);
}

// ============================================================
void loop() {
  unsigned long now = millis();

  // ---- Handle incoming packets ----
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String incoming;
    while (LoRa.available()) {
      incoming += (char)LoRa.read();
    }
    incoming.trim();

    if (incoming.length()) {
      int rssi = LoRa.packetRssi();
      float snr = LoRa.packetSnr();

      Serial.printf("📡 [RX] RSSI=%d dBm, SNR=%.1f dB\n", rssi, snr);
      handleIncoming(incoming, rssi, snr);
    }
  }

  // ---- Poll cycle ----
  if (!nodeState[currentIndex].awaiting && (now - lastPollCycle >= POLL_INTERVAL)) {
    lastPollCycle = now;
    pollNode(currentIndex);
  }

  // ---- Timeout handling ----
  for (int i = 0; i < NODE_COUNT; i++) {
    if (nodeState[i].awaiting && (now - nodeState[i].lastSend > RESPONSE_TIMEOUT)) {
      Serial.printf("⏱  Timeout: %s\n", nodes[i].c_str());
      nodeState[i].retries++;

      if (nodeState[i].retries < MAX_RETRIES) {
        Serial.printf("🔄 Retry %s (%d/%d)\n", nodes[i].c_str(), nodeState[i].retries, MAX_RETRIES);
        pollNode(i);
      } else {
        Serial.printf("❌ %s unreachable\n\n", nodes[i].c_str());
        logTimeout(nodes[i]);
        nodeState[i].awaiting = false;
        nodeState[i].retries = 0;
        currentIndex = (currentIndex + 1) % NODE_COUNT;
      }
    }
  }

  // ---- Handle alert broadcast ----
  if (alertPending) {
    broadcastAlert();
    alertPending = false;
  }

  // ---- LED blinking ----
  if (ledBlinking) {
    if (now - ledBlinkStart < LED_ALERT_DURATION) {
      if (now - lastLedToggle >= LED_BLINK_MS) {
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        lastLedToggle = now;
      }
    } else {
      ledBlinking = false;
      digitalWrite(LED_PIN, LOW);
    }
  }
}

// ============================================================
void pollNode(int index) {
  String req = "REQ:" + nodes[index];
  LoRa.beginPacket();
  LoRa.print(req);
  LoRa.endPacket();

  nodeState[index].awaiting = true;
  nodeState[index].lastSend = millis();

  Serial.printf("📤 Poll → %s\n", nodes[index].c_str());
}

// ============================================================
void handleIncoming(const String &payload, int rssi, float snr) {
  if (!payload.startsWith("{") || !payload.endsWith("}")) {
    Serial.println("⚠️  Non-JSON message\n");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.printf("⚠️  JSON parse failed: %s\n\n", error.c_str());
    return;
  }

  const char* nodeId = doc["node"];
  if (!nodeId) {
    Serial.println("⚠️  Missing 'node' field\n");
    return;
  }

  // Mark node as responded
  for (int i = 0; i < NODE_COUNT; i++) {
    if (nodes[i] == nodeId) {
      nodeState[i].awaiting = false;
      nodeState[i].retries = 0;
      break;
    }
  }

  // Display data
  Serial.println("┌────────────────────────────────┐");
  Serial.printf("│ Node:      %-19s │\n", nodeId);
  Serial.printf("│ Temp:      %.2f °C             │\n", doc["temp"].as<float>());
  Serial.printf("│ Pressure:  %.2f hPa           │\n", doc["pres"].as<float>());
  Serial.printf("│ Altitude:  %d m                │\n", doc["alt"].as<int>());
  Serial.printf("│ Battery:   %d %%                │\n", doc["bat"].as<int>());
  Serial.printf("│ Alert:     %s                  │\n", doc["alert"].as<int>() ? "YES ⚠️ " : "No");
  Serial.println("└────────────────────────────────┘");

  // Log to internal storage
  logData(payload, nodeId, rssi, snr);

  // Alert handling
  if (doc["alert"].as<int>() == 1) {
    Serial.println("\n🚨 ⚠️  ALERT FLAG DETECTED! ⚠️  🚨\n");
    alertPending = true;
    ledBlinking = true;
    ledBlinkStart = millis();
    lastLedToggle = millis();
  }

  currentIndex = (currentIndex + 1) % NODE_COUNT;
  Serial.println();
}

// ============================================================
void broadcastAlert() {
  Serial.println("╔════════════════════════════════╗");
  Serial.println("║  🚨 BROADCASTING ALERT 🚨      ║");
  Serial.println("╚════════════════════════════════╝");

  for (int i = 0; i < ALERT_BEEP_COUNT; i++) {
    beep(ALERT_BEEP_MS);
    delay(ALERT_BEEP_MS);
  }

  LoRa.beginPacket();
  LoRa.print("BROADCAST:ALERT");
  LoRa.endPacket();

  Serial.println("Alert broadcast sent\n");
}

// ============================================================
void beep(int duration) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(duration);
  digitalWrite(BUZZER_PIN, LOW);
}

// ============================================================
void writeLogHeader() {
  if (!storageOK) return;
  
  File file = SPIFFS.open(logFileName, "w");
  if (file) {
    file.println("===========================================");
    file.println("     LoRa Sensor Network Data Log");
    file.println("===========================================");
    file.println("Timestamp,NodeID,Temp,Pressure,Altitude,Battery,Alert,RSSI,SNR,JSON");
    file.close();
  }
}

// ============================================================
void logData(const String &jsonStr, const String &nodeId, int rssi, float snr) {
  if (!storageOK) {
    Serial.println("⚠️  Logging skipped (storage unavailable)");
    return;
  }

  StaticJsonDocument<256> doc;
  deserializeJson(doc, jsonStr);

  // Build CSV line
  String csvLine = String(millis()) + "," +
                   nodeId + "," +
                   String(doc["temp"].as<float>(), 2) + "," +
                   String(doc["pres"].as<float>(), 2) + "," +
                   String(doc["alt"].as<int>()) + "," +
                   String(doc["bat"].as<int>()) + "," +
                   String(doc["alert"].as<int>()) + "," +
                   String(rssi) + "," +
                   String(snr, 1) + "," +
                   jsonStr;

  // Add to buffer
  logBuffer += csvLine + "\n";
  bufferCount++;

  // Write buffer when full or check file size
  if (bufferCount >= LOG_BUFFER_SIZE) {
    flushLogBuffer();
    checkLogSize();
  }

  Serial.println("💾 Data logged to flash");
}

// ============================================================
void flushLogBuffer() {
  if (!storageOK || logBuffer.length() == 0) return;

  File file = SPIFFS.open(logFileName, "a");
  if (file) {
    file.print(logBuffer);
    file.close();
    logBuffer = "";
    bufferCount = 0;
  } else {
    Serial.println("❌ Flash write failed");
  }
}

// ============================================================
void checkLogSize() {
  if (!storageOK) return;

  File file = SPIFFS.open(logFileName, "r");
  if (file) {
    size_t fileSize = file.size();
    file.close();

    // If file too large, archive it
    if (fileSize > MAX_LOG_SIZE) {
      Serial.println("⚠️  Log file too large, archiving...");
      
      // Delete old archive if exists
      if (SPIFFS.exists("/data_old.txt")) {
        SPIFFS.remove("/data_old.txt");
      }
      
      // Rename current to old
      SPIFFS.rename(logFileName, "/data_old.txt");
      
      // Create new log
      writeLogHeader();
      
      Serial.println("✓ Log rotated");
    }
  }
}

// ============================================================
void logTimeout(const String &nodeId) {
  if (!storageOK) return;

  String csvLine = String(millis()) + "," + nodeId + ",TIMEOUT,,,,,,,";
  logBuffer += csvLine + "\n";
  bufferCount++;

  if (bufferCount >= LOG_BUFFER_SIZE) {
    flushLogBuffer();
  }
}