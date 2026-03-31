#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);
#include <WiFi.h>
#include <esp_now.h>
#include <Firebase_ESP_Client.h>
#include <time.h>

#define SS_PIN 5
#define RST_PIN 4   // avoid conflict with LCD
#define BUTTON_PIN 12
///////////////////////
// WIFI
#define WIFI_SSID "Shoheb"
#define WIFI_PASSWORD "12345678"

MFRC522 rfid(SS_PIN, RST_PIN);
bool adminMode = true;

///////////////////////
// FIREBASE
#define API_KEY "AIzaSyAFSLRyN4W5VRg4atDPq7Pae0Kl8-RVP30"
#define DATABASE_URL "https://smart-classroom-allocation-default-rtdb.asia-southeast1.firebasedatabase.app"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

///////////////////////
// CLASSROOM MACs
uint8_t lab31MAC[]  = {0x00,0x70,0x07,0x3A,0x76,0xBC};
uint8_t cr125MAC[]  = {0xD4,0xE9,0xF4,0xBC,0x56,0x68};
uint8_t cr126MAC[]  = {0x28,0x05,0xA5,0x6E,0x6F,0x88};

///////////////////////
typedef struct {
  char room[10];
  char uid[20];
} ScanData;

typedef struct {
  char status[15];
  char faculty[20];
  char subject[20];
} ResponseData;

ScanData incomingData;
ResponseData response;

///////////////////////
// GET CURRENT DAY
String getCurrentDay() {
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);

  String days[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  return days[timeinfo->tm_wday];
}

///////////////////////
// GET CURRENT SLOT
String getCurrentSlot() {
  struct tm *timeinfo;
  time_t now = time(nullptr);
  timeinfo = localtime(&now);

  int minutes = timeinfo->tm_hour * 60 + timeinfo->tm_min;

  if (minutes >= 510 && minutes <= 570) return "8:30-9:30";
  if (minutes >= 570 && minutes <= 630) return "9:30-10:30";
  if (minutes >= 640 && minutes <= 700) return "10:40-11:40";
  if (minutes >= 700 && minutes <= 760) return "11:40-12:40";
  if (minutes >= 800 && minutes <= 860) return "1:20-2:20";
  if (minutes >= 860 && minutes <= 920) return "2:20-3:20";
  if (minutes >= 920 && minutes <= 980) return "3:20-4:20";

  return "";
}

///////////////////////
void addPeer(uint8_t *peerAddr) {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peerAddr, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
}

///////////////////////
void logToFirebase(String room, String faculty, String result, String slot) {

  String path = "logs/" + room + "/" + getCurrentDay() + "/" + slot;

  Firebase.RTDB.setString(&fbdo, path + "/faculty", faculty);
  Firebase.RTDB.setString(&fbdo, path + "/result", result);
}

///////////////////////
void onDataReceive(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if(adminMode) return;

  memcpy(&incomingData, data, sizeof(incomingData));

  String room = String(incomingData.room);
  String uid  = String(incomingData.uid);

  Serial.println("Scan Received");
  Serial.println(room);
  Serial.println(uid);

  String day  = getCurrentDay();
  String slot = getCurrentSlot();

  if (slot == "") {
    strcpy(response.status, "NO_LECTURE");
    esp_now_send(info->src_addr, (uint8_t *)&response, sizeof(response));
    return;
  }

  // Get scheduled faculty
  String timetablePath = "classrooms/" + room + "/timetable/" + day + "/" + slot + "/faculty";

  if (!Firebase.RTDB.getString(&fbdo, timetablePath)) {
    strcpy(response.status, "NO_LECTURE");
    esp_now_send(info->src_addr, (uint8_t *)&response, sizeof(response));
    return;
  }

  String scheduledFaculty = fbdo.stringData();

  // Get faculty from UID
  String facultyPath = "faculty/" + uid + "/name";

  if (!Firebase.RTDB.getString(&fbdo, facultyPath)) {

  Serial.println("UNKNOWN UID: " + uid);

  // 🔥 LCD DISPLAY HERE
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Unknown Card");

  lcd.setCursor(0,1);
  lcd.print(uid.substring(0,8));

  delay(3000);

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Waiting...");

  strcpy(response.status, "INVALID");
  esp_now_send(info->src_addr, (uint8_t *)&response, sizeof(response));

  logToFirebase(room, "UNKNOWN", "INVALID", slot);

  return;
}

  String scannedFaculty = fbdo.stringData();

  if (scannedFaculty == scheduledFaculty) {

    strcpy(response.status, "VALID");
    scannedFaculty.toCharArray(response.faculty, 20);

    // Update classroom status
    Firebase.RTDB.setString(&fbdo, "classrooms/" + room + "/status", "Ongoing");
    Firebase.RTDB.setString(&fbdo, "classrooms/" + room + "/current_faculty", scannedFaculty);

    logToFirebase(room, scannedFaculty, "VALID", slot);
  }
  else {
    strcpy(response.status, "INVALID");
    logToFirebase(room, scannedFaculty, "INVALID", slot);
  }

  esp_now_send(info->src_addr, (uint8_t *)&response, sizeof(response));
}

///////////////////////
void setup() {

  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  configTime(19800, 0, "pool.ntp.org");

  esp_now_init();
  esp_now_register_recv_cb(onDataReceive);

  addPeer(lab31MAC);
  addPeer(cr125MAC);
  addPeer(cr126MAC);
lcd.init();
lcd.backlight();

lcd.setCursor(0,0);
lcd.print("Master Ready");
  Serial.println("MASTER READY");

  SPI.begin();
rfid.PCD_Init();

pinMode(BUTTON_PIN, INPUT_PULLUP);

lcd.clear();
lcd.setCursor(0,0);
lcd.print("Scan Card...");
}

void loop() {

  // 🔘 Button reset
  if(digitalRead(BUTTON_PIN) == LOW){
    delay(200);

    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Scan Card...");

    adminMode = true;

    while(digitalRead(BUTTON_PIN) == LOW);
  }

  // 🪪 RFID scan
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    delay(10);
    return;
  }

  adminMode = true;  // 🔥 correct place

  String uid = "";

  for (byte i = 0; i < rfid.uid.size; i++) {
    uid += String(rfid.uid.uidByte[i], HEX);
  }

  uid.toUpperCase();

  Serial.println("ADMIN UID: " + uid);

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("UID:");

  lcd.setCursor(0,1);
  lcd.print(uid.substring(0,16));

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  delay(3000);
}