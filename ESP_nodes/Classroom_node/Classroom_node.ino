#include <WiFi.h>
#include <esp_now.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

///////////////////////
// ====== EDIT THESE ======
#define ROOM_NAME "Lab31"   
// =========================

///////////////////////
// RFID PINS
#define SS_PIN 5
#define RST_PIN 4

// BUTTON
#define BUTTON_PIN 15

// BUZZER
#define BUZZER_PIN 2

MFRC522 mfrc522(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);

///////////////////////
// 🔴 PUT MASTER MAC ADDRESS HERE
uint8_t masterAddress[] = {0x00, 0x70, 0x07, 0xE1, 0xFD, 0x9C};
// Example: 24:6F:28:AA:BB:CC
// ================================

///////////////////////
// STRUCTURES
typedef struct {
  char room[10];
  char uid[20];
} ScanData;

typedef struct {
  char status[15];   // VALID / INVALID / NO_LECTURE
  char faculty[20];
  char subject[20];
} ResponseData;

ScanData scanData;
ResponseData responseData;

///////////////////////
// BUZZER FUNCTIONS
void beepValid() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(200);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepInvalid() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    delay(150);
  }
}

///////////////////////
// RECEIVE DATA FROM MASTER
void  onDataReceive(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {

  memcpy(&responseData, incomingData, sizeof(responseData));

  lcd.clear();

  if (strcmp(responseData.status, "VALID") == 0) {
    lcd.setCursor(0, 0);
    lcd.print("Lecture Verified");
    lcd.setCursor(0, 1);
    lcd.print(responseData.faculty);
    beepValid();
  }
  else if (strcmp(responseData.status, "INVALID") == 0) {
    lcd.setCursor(0, 0);
    lcd.print("Invalid Faculty");
    lcd.setCursor(0, 1);
    lcd.print("Wrong Person");
    beepInvalid();
  }
  else {   // NO_LECTURE
    lcd.setCursor(0, 0);
    lcd.print("No Lecture Now");
    // 🔵 No beep here (as requested)
  }

  delay(3000);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(ROOM_NAME);
  lcd.setCursor(0, 1);
  lcd.print("Scan Card...");
}

///////////////////////
void setup() {

  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // SPI RFID
  SPI.begin(18, 19, 23, 5);
  mfrc522.PCD_Init();

  // LCD
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print(ROOM_NAME);
  lcd.setCursor(0, 1);
  lcd.print("Scan Card...");

  // ESP-NOW
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }

  esp_now_register_recv_cb(onDataReceive);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  esp_now_add_peer(&peerInfo);
}

///////////////////////
void loop() {

  // BUTTON (Optional refresh message)
  if (digitalRead(BUTTON_PIN) == LOW) {
    lcd.clear();
    lcd.print("Refreshing...");
    delay(1000);
    lcd.clear();
    lcd.print("Scan Card...");
  }

  // RFID CHECK
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  String uidString = "";

  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uidString += "0";
    uidString += String(mfrc522.uid.uidByte[i], HEX);
  }

  uidString.toUpperCase();

  Serial.print("UID: ");
  Serial.println(uidString);

  strcpy(scanData.room, ROOM_NAME);
  uidString.toCharArray(scanData.uid, 20);

  esp_now_send(masterAddress, (uint8_t *)&scanData, sizeof(scanData));

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Checking...");

  mfrc522.PICC_HaltA();
}