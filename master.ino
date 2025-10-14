// ----- MASTER NODE FINAL CODE -----
// Author: Sudarshan T S
// Purpose: Poll multiple LoRa slave nodes sequentially, gather data, and handle alerts

#include <SPI.h>
#include <LoRa.h>

// ---------------- PIN CONFIG ----------------
#define LORA_NSS    5
#define LORA_RST    14
#define LORA_DIO0   26
#define LORA_SCK    18
#define LORA_MISO   19
#define LORA_MOSI   23

// ---------------- NODE CONFIG ----------------
const String nodes[] = {"NODE1", "NODE2", "NODE3"};
const int NODE_COUNT = sizeof(nodes) / sizeof(nodes[0]);

// ---------------- TIMING CONFIG ----------------
#define POLL_INTERVAL   3000    // 3 seconds between polls
#define RESPONSE_TIMEOUT 1500   // wait for reply
#define MAX_RETRIES       2

struct NodeState {
  bool awaiting = false;
  unsigned long lastSend = 0;
  int retries = 0;
};

NodeState nodeState[NODE_COUNT];
int currentIndex = 0;

unsigned long lastPollCycle = 0;
bool alertPending = false;

// ------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("LoRa Master initializing...");

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa init failed. Check wiring.");
    while (1);
  }

  Serial.println("LoRa Master ready!");
}

// ------------------------------------------------
void loop() {
  unsigned long now = millis();

  // --- Handle any incoming messages ---
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String incoming = "";
    while (LoRa.available()) incoming += (char)LoRa.read();
    incoming.trim();

    if (incoming.length() > 0) {
      Serial.print("[RX RSSI=");
      Serial.print(LoRa.packetRssi());
      Serial.print("] ");
      Serial.println(incoming);
      handleIncoming(incoming);
    }
  }

  // --- Poll logic ---
  if (!nodeState[currentIndex].awaiting &&
      (now - lastPollCycle >= POLL_INTERVAL)) {

    lastPollCycle = now;
    pollNode(currentIndex);
  }

  // --- Timeout handling ---
  for (int i = 0; i < NODE_COUNT; i++) {
    if (nodeState[i].awaiting &&
        (now - nodeState[i].lastSend > RESPONSE_TIMEOUT)) {

      Serial.print("Timeout for ");
      Serial.println(nodes[i]);
      nodeState[i].retries++;

      if (nodeState[i].retries < MAX_RETRIES) {
        Serial.print("Retrying ");
        Serial.print(nodes[i]);
        Serial.print(" (");
        Serial.print(nodeState[i].retries);
        Serial.println(")");
        pollNode(i);
      } else {
        Serial.print("Skipping ");
        Serial.println(nodes[i]);
        nodeState[i].awaiting = false;
        nodeState[i].retries = 0;
        currentIndex = (currentIndex + 1) % NODE_COUNT;
      }
    }
  }

  // --- Handle Alert Broadcast ---
  if (alertPending) {
    broadcastAlert();
    alertPending = false;
  }
}

// ------------------------------------------------
void pollNode(int index) {
  String req = "REQ:" + nodes[index];
  LoRa.beginPacket();
  LoRa.print(req);
  LoRa.endPacket();

  nodeState[index].awaiting = true;
  nodeState[index].lastSend = millis();

  Serial.print("Sent -> ");
  Serial.println(req);
}

// ------------------------------------------------
void handleIncoming(const String &payload) {
  if (payload.startsWith("DATA:")) {
    int p1 = payload.indexOf(':');
    int p2 = payload.indexOf(',', p1 + 1);
    String nodeId = (p2 > 0) ? payload.substring(p1 + 1, p2) : payload.substring(p1 + 1);
    nodeId.trim();

    for (int i = 0; i < NODE_COUNT; ++i) {
      if (nodes[i] == nodeId) {
        nodeState[i].awaiting = false;
        nodeState[i].retries = 0;
        break;
      }
    }

    Serial.print("Parsed DATA from ");
    Serial.println(nodeId);
    Serial.println(payload);

    if (payload.indexOf("Alert:1") != -1) {
      Serial.println("ALERT flag received from node -> scheduling broadcast");
      alertPending = true;
    }

    // ✅ Move to next node after success
    currentIndex = (currentIndex + 1) % NODE_COUNT;
  }
  else if (payload.startsWith("ALERT_FROM")) {
    Serial.println("ALERT_FROM received: " + payload);
    alertPending = true;
  }
  else {
    Serial.println("Unknown payload: " + payload);
  }
}

// ------------------------------------------------
void broadcastAlert() {
  Serial.println("=== Broadcasting ALERT to all nodes ===");
  LoRa.beginPacket();
  LoRa.print("BROADCAST:ALERT");
  LoRa.endPacket();
}











// _____________________________________
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

//   // Optional tuning for better reliability
//   LoRa.setSpreadingFactor(12);
//   LoRa.setSignalBandwidth(125E3);
//   LoRa.setCodingRate4(5);

//   Serial.println("Node A (Pinger) ready");
// }

// void loop() {
//   // Send "ping"
//   LoRa.beginPacket();
//   LoRa.print("ping");
//   LoRa.endPacket();
//   Serial.println("Sent: ping");

//   // Wait up to 2 seconds for "pong"
//   unsigned long start = millis();
//   while (millis() - start < 2000) {
//     int packetSize = LoRa.parsePacket();
//     if (packetSize) {
//       String msg = "";
//       while (LoRa.available()) msg += (char)LoRa.read();
//       Serial.println("Got reply: " + msg);
//       break;
//     }
//   }

//   delay(2000);
// }
