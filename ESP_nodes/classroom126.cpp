#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Firebase_ESP_Client.h>
#include <time.h>

#define ROOM_NAME "CR126"

// WIFI
#define WIFI_SSID "Shoheb"
#define WIFI_PASSWORD "12345678"

// FIREBASE
#define API_KEY "AIzaSyAXNrkME8ssbzJxfUrEpzSNDCa7MEpOgrY"
#define DATABASE_URL "https://sample-final-proj-default-rtdb.asia-southeast1.firebasedatabase.app/"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// RFID
#define SS_PIN 5
#define RST_PIN 4

// BUTTON + BUZZER
#define BUTTON_PIN 17
#define BUZZER_PIN 2

MFRC522 mfrc522(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// 🔊 BUZZER
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

// 📅 DAY
String getCurrentDay() {
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);

  String days[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  return days[timeinfo->tm_wday];
}

// ⏰ SLOT (IMPORTANT: s1, s2...)
String getCurrentSlot() {
  struct tm *timeinfo;
  time_t now = time(nullptr);
  timeinfo = localtime(&now);

  int minutes = timeinfo->tm_hour * 60 + timeinfo->tm_min;

  if (minutes >= 510 && minutes <= 570) return "s1";
  if (minutes >= 570 && minutes <= 630) return "s2";
  if (minutes >= 640 && minutes <= 700) return "s3";
  if (minutes >= 700 && minutes <= 760) return "s4";
  if (minutes >= 800 && minutes <= 860) return "s5";
  if (minutes >= 860 && minutes <= 920) return "s6";
  if (minutes >= 920 && minutes <= 980) return "s7";

  return "";
}

// 🔍 UID → FACULTY
String findFacultyByUID(String uid){

  FirebaseJson json;
  Firebase.RTDB.getJSON(&fbdo, "teachers");

  json = fbdo.jsonObject();

  size_t count = json.iteratorBegin();

  for (size_t i = 0; i < count; i++) {
    String key, value;
    int type;

    json.iteratorGet(i, type, key, value);

    if(type == FirebaseJson::JSON_OBJECT){

      FirebaseJson subObj;
      subObj.setJsonData(value);

      FirebaseJsonData rfidData;
      subObj.get(rfidData, "rfid");

      if(rfidData.stringValue == uid){
        return key;
      }
    }
  }

  json.iteratorEnd();
  return "";
}

void setup() {

  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  SPI.begin(18, 19, 23, 5);
  mfrc522.PCD_Init();

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  lcd.print("Connecting WiFi");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  lcd.clear();
  lcd.print("WiFi Connected");

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  configTime(19800, 0, "pool.ntp.org");

  delay(2000);

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(ROOM_NAME);
  lcd.setCursor(0,1);
  lcd.print("Scan Card...");
}

void loop() {

  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  String uid = "";

  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }

  uid.toUpperCase();

  Serial.println("UID: " + uid);

  lcd.clear();
  lcd.print("Checking...");

  String day = getCurrentDay();
  String slot = getCurrentSlot();

  if(slot == ""){
    lcd.clear();
    lcd.print("No Lecture");
    delay(2000);
    return;
  }

  String path = "classrooms/" + String(ROOM_NAME) + "/timetable/" + day + "/" + slot + "/faculty";

  if(!Firebase.RTDB.getString(&fbdo, path)){
    lcd.clear();
    lcd.print("No Data");
    delay(2000);
    return;
  }

  String scheduledFaculty = fbdo.stringData();
  String scannedFaculty = findFacultyByUID(uid);

  lcd.clear();

  if(scannedFaculty == ""){
    lcd.print("Unknown Card");
    beepInvalid();
  }
  else if(scannedFaculty == scheduledFaculty){
    lcd.print("Access Granted");
    lcd.setCursor(0,1);
    lcd.print(scannedFaculty);
    beepValid();
  }
  else{
    lcd.print("Access Denied");
    beepInvalid();
  }

  delay(3000);

  lcd.clear();
  lcd.print("Scan Card...");

  mfrc522.PICC_HaltA();
}