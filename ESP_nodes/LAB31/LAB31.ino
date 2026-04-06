#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Firebase_ESP_Client.h>
#include <time.h>

#define ROOM_NAME "LAB31"

// WIFI
#define WIFI_SSID "group5"
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
bool scanMode = false;
bool isInside = false;
String currentFaculty = "";
String currentSubject = "";

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

  if (minutes >= 510 && minutes < 570) return "s1";
  if (minutes >= 570 && minutes < 630) return "s2";
  if (minutes >= 630 && minutes < 690) return "s3";
  if (minutes >= 690 && minutes < 750) return "s4";
  if (minutes >= 750 && minutes < 810) return "s5";
  if (minutes >= 810 && minutes < 870) return "s6";
  if (minutes >= 870 && minutes < 930) return "s7";

  return "";
}

// 🔍 UID → FACULTY
String findFacultyByUID(String uid){

  FirebaseJson json;
  if (!Firebase.RTDB.getJSON(&fbdo, "teachers")) {
  Serial.println("Teacher fetch failed");
  return "";
}

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

  auth.user.email = "test@test.com";
  auth.user.password = "123456";

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  configTime(19800, 0, "pool.ntp.org");
  Serial.print("Syncing time");

time_t now = time(nullptr);

while (now < 100000) {
  delay(500);
  Serial.print(".");
  now = time(nullptr);
}

Serial.println("\nTime synced!");

  delay(2000);

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(ROOM_NAME);
  lcd.setCursor(0,1);
  lcd.print("Press Button");
}

void loop() {

  // 🔘 BUTTON PRESS → ENABLE SCAN
  if (digitalRead(BUTTON_PIN) == LOW) {

    lcd.clear();
    lcd.print("Ready to Scan");

    scanMode = true;

    delay(300); // debounce

    while(digitalRead(BUTTON_PIN) == LOW); // wait release
  }

  // 🔍 ONLY SCAN WHEN BUTTON PRESSED
  if (scanMode) {

    while (!mfrc522.PICC_IsNewCardPresent()) {
  delay(50);
}

while (!mfrc522.PICC_ReadCardSerial()) {
  delay(50);
}

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
      scanMode = false;
      return;
    }

    String path = "classrooms/" + String(ROOM_NAME) + "/timetable/" + day + "/" + slot + "/faculty";

    if(!Firebase.RTDB.getString(&fbdo, path)){
      Serial.println("Firebase ERROR: " + fbdo.errorReason());
      lcd.clear();
      lcd.print("No Lecture Now");
      delay(2000);
      scanMode = false;
      lcd.clear();
lcd.print("Press Button");
      return;
    }

    String scheduledFaculty = fbdo.stringData();
    String subjectPath = "classrooms/" + String(ROOM_NAME) + "/timetable/" + day + "/" + slot + "/subject";
String subject = "";

if(Firebase.RTDB.getString(&fbdo, subjectPath)){
  subject = fbdo.stringData();
}
    String scannedFaculty = findFacultyByUID(uid);

    // 🔥 DEBUG
    Serial.println("----------");
    Serial.println("Path: " + path);
    Serial.println("Day: " + day);
    Serial.println("Slot: " + slot);
    Serial.println("Scheduled: " + scheduledFaculty);
    Serial.println("Scanned: " + scannedFaculty);
    Serial.println("----------");

    lcd.clear();


if(scannedFaculty == ""){
  lcd.print("Unknown Card");
  beepInvalid();
}

// ✅ ENTRY
else if(!isInside && scannedFaculty == scheduledFaculty){

  lcd.print("Lecture Started");
  delay(1500);

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Ongoing:");
  lcd.setCursor(0,1);
  lcd.print(subject);   // 🔥 SHOW SUBJECT

  beepValid();

  isInside = true;
  currentFaculty = scannedFaculty;
  currentSubject = subject;

  // 🔥 Firebase update
  Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/status", "Ongoing");

Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/faculty", scannedFaculty);

Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/subject", subject);
  Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/current_faculty", scannedFaculty);
}

// ✅ EXIT (same teacher scans again)
else if(isInside && scannedFaculty == currentFaculty){

  lcd.clear();
  lcd.print("Lecture Ended");
  delay(1500);

  lcd.clear();
  lcd.print("Room Free");

  beepValid();

  isInside = false;
  currentFaculty = "";
  currentSubject = "";

  // 🔥 Firebase update
  Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/status", "Free");

Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/faculty", "");

Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/subject", "");
  Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/current_faculty", "");
}

// ❌ WRONG PERSON
else{
  lcd.print("Access Denied");
  beepInvalid();
}

    delay(3000);

    

    scanMode = false;  // 🔥 RESET
    lcd.clear();
    lcd.print("Press Button");

    mfrc522.PICC_HaltA();
  }
  time_t now = time(nullptr);
Serial.println(ctime(&now));
delay(500);
}