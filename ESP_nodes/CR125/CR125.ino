/*
  ========================================================
   Smart Classroom Allocation — ESP-NOW SLAVE NODE
  ========================================================

  ROLE:
  - Reads RFID cards
  - Sends {room, uid} to Master via ESP-NOW
  - Receives LCD response packet from Master
  - Drives LCD + buzzer based on Master's response
  - NO WiFi, NO Firebase — pure local hardware

  EACH SLAVE NEEDS:
  - ROOM_NAME defined uniquely per node
  - MASTER_MAC set to the actual master ESP32 MAC

  HOW TO GET MASTER MAC:
    Flash master firmware first. Read Serial output:
    "Master MAC: XX:XX:XX:XX:XX:XX"
    Paste that into MASTER_MAC below.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ── ROOM IDENTITY ───────────────────────────────────────
#define ROOM_NAME "CR125"   // ← CHANGE PER SLAVE

// ── MASTER MAC ──────────────────────────────────────────
//  Get this by reading Serial on the master at boot
uint8_t masterMAC[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x00};

// ── PINS ────────────────────────────────────────────────
#define SS_PIN     5
#define RST_PIN    4
#define BUTTON_PIN 17
#define BUZZER_PIN 2
#define ESPNOW_CHANNEL 1    // must match master

// ── HARDWARE ────────────────────────────────────────────
MFRC522           mfrc522(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ─────────────────────────────────────────────────────────
//  PACKET STRUCTURES  (identical to master)
// ─────────────────────────────────────────────────────────
typedef struct ScanPacket {
  char room[8];
  char uid[16];
} ScanPacket;

typedef struct ResponsePacket {
  char line1[17];
  char line2[17];
  uint8_t beep;
} ResponsePacket;

// ─────────────────────────────────────────────────────────
//  STATE
// ─────────────────────────────────────────────────────────
bool     scanMode        = false;
bool     waitingResponse = false;
unsigned long lastScanTime    = 0;
unsigned long lastButtonPress = 0;
unsigned long waitStart       = 0;
#define RESPONSE_TIMEOUT_MS 6000  // give up if master silent for 6s

// LCD anti-flicker cache
String lastLine1 = "", lastLine2 = "";

// ─────────────────────────────────────────────────────────
//  BUZZER
// ─────────────────────────────────────────────────────────
void beepValid() {
  digitalWrite(BUZZER_PIN, HIGH); delay(200); digitalWrite(BUZZER_PIN, LOW);
}

void beepInvalid() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(150);
    digitalWrite(BUZZER_PIN, LOW);  delay(150);
  }
}

// ─────────────────────────────────────────────────────────
//  LCD
// ─────────────────────────────────────────────────────────
void updateLCD(String l1, String l2) {
  if (l1 != lastLine1) {
    lcd.setCursor(0, 0); lcd.print("                ");
    lcd.setCursor(0, 0); lcd.print(l1); lastLine1 = l1;
  }
  if (l2 != lastLine2) {
    lcd.setCursor(0, 1); lcd.print("                ");
    lcd.setCursor(0, 1); lcd.print(l2); lastLine2 = l2;
  }
}

// ─────────────────────────────────────────────────────────
//  ESP-NOW RECEIVE CALLBACK (runs in ISR context — keep short)
// ─────────────────────────────────────────────────────────
volatile bool   hasResponse   = false;
volatile ResponsePacket pendingResponse;

void onDataReceive(const uint8_t* mac, const uint8_t* data, int len) {
  if (len != sizeof(ResponsePacket)) return;
  memcpy((void*)&pendingResponse, data, sizeof(ResponsePacket));
  hasResponse = true;
}

// ─────────────────────────────────────────────────────────
//  SEND SCAN TO MASTER
// ─────────────────────────────────────────────────────────
void sendScanToMaster(String uid) {
  ScanPacket pkt;
  strncpy(pkt.room, ROOM_NAME, sizeof(pkt.room));
  strncpy(pkt.uid,  uid.c_str(), sizeof(pkt.uid));
  pkt.uid[sizeof(pkt.uid)-1] = 0;

  esp_err_t result = esp_now_send(masterMAC, (uint8_t*)&pkt, sizeof(pkt));
  if (result == ESP_OK) {
    Serial.printf("[%s] Sent UID %s to master\n", ROOM_NAME, uid.c_str());
    waitingResponse = true;
    waitStart       = millis();
  } else {
    Serial.println("ESP-NOW send failed!");
    updateLCD("Master", "Unreachable");
    beepInvalid();
    delay(2000);
    updateLCD(ROOM_NAME, "Ready");
    scanMode = false;
  }
}

// ─────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  SPI.begin(18, 19, 23, 5);
  mfrc522.PCD_Init();

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  // Print own MAC so you can paste it into master's slaveMACs[]
  WiFi.mode(WIFI_STA);
  Serial.printf("[%s] My MAC: %s\n", ROOM_NAME, WiFi.macAddress().c_str());

  // Set channel to match master/router
  // Forcing channel so ESP-NOW doesn't drift after boot
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    updateLCD("ESP-NOW", "Init Failed!");
    Serial.println("ESP-NOW init failed");
    while (1) delay(1000);
  }
  esp_now_register_recv_cb(onDataReceive);

  // Register master as peer
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, masterMAC, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt  = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Failed to add master peer");
    updateLCD("Peer", "Add Failed");
    delay(3000);
  }

  updateLCD(ROOM_NAME, "Ready");
  Serial.printf("[%s] Slave ready\n", ROOM_NAME);
}

// ─────────────────────────────────────────────────────────
//  MAIN LOOP
// ─────────────────────────────────────────────────────────
void loop() {

  // ── Handle incoming response from master ──────────────
  if (hasResponse) {
    hasResponse      = false;
    waitingResponse  = false;

    ResponsePacket rsp;
    memcpy(&rsp, (void*)&pendingResponse, sizeof(ResponsePacket));

    updateLCD(String(rsp.line1), String(rsp.line2));

    if (rsp.beep == 1) beepValid();
    else               beepInvalid();

    delay(2500);
    updateLCD(ROOM_NAME, "Ready");
    scanMode = false;
    return;
  }

  // ── Timeout waiting for master ─────────────────────────
  if (waitingResponse && millis() - waitStart > RESPONSE_TIMEOUT_MS) {
    waitingResponse = false;
    scanMode        = false;
    updateLCD("No Response", "From Master");
    beepInvalid();
    delay(2000);
    updateLCD(ROOM_NAME, "Ready");
    return;
  }

  // ── Don't do anything else while waiting ─────────────
  if (waitingResponse) {
    delay(50);
    return;
  }

  // ── Button → enable scan mode ─────────────────────────
  if (digitalRead(BUTTON_PIN) == LOW && millis() - lastButtonPress > 1000) {
    lastButtonPress = millis();
    if (!scanMode) {
      updateLCD("Ready", "Scan Card");
      scanMode = true;
    }
  }

  // ── Scan mode: read RFID, send to master ─────────────
  if (scanMode) {
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
      delay(50);
      return;
    }

    // Debounce
    if (millis() - lastScanTime < 3000) {
      mfrc522.PICC_HaltA();
      return;
    }
    lastScanTime = millis();

    // Build UID string
    String uid = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
      uid += String(mfrc522.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();
    Serial.printf("[%s] Scanned UID: %s\n", ROOM_NAME, uid.c_str());

    updateLCD("Checking...", "Please Wait");
    sendScanToMaster(uid);

    mfrc522.PICC_HaltA();
  }

  delay(50);
}
