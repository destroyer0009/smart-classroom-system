
// hello


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
unsigned long lastScanTime = 0;
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
  time_t currentTime = time(nullptr);
  struct tm *t = localtime(&currentTime);

  String days[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  return days[t->tm_wday];
}

// ⏰ SLOT (IMPORTANT: s1, s2...)
int getSlotStartMinutes(String slot) {
  if (slot == "s1") return 510;
  if (slot == "s2") return 570;
  if (slot == "s3") return 630;
  if (slot == "s4") return 690;
  if (slot == "s5") return 750;
  if (slot == "s6") return 810;
  if (slot == "s7") return 870;
  return -1;
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

String getCurrentSlot() {
  int h = hour();
  int m = minute();
  int current = h * 60 + m;

  if(current >= 510 && current < 570) return "s1";
  if(current >= 570 && current < 630) return "s2";
  if(current >= 630 && current < 690) return "s3";
  if(current >= 690 && current < 750) return "s4";
  if(current >= 750 && current < 810) return "s5";
  if(current >= 810 && current < 870) return "s6";
  if(current >= 870 && current < 930) return "s7";
  if(current >= 930 && current < 990) return "s8";

  return "";
}
void setup() {

  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);  

  SPI.begin(18, 19, 23, 5);
  mfrc522.PCD_Init();

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi"); 

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi Connected");

    // 🔴 MOVE THIS UP
    auth.user.email = "test@test.com";
    auth.user.password = "123456";

    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;

    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    // 🔴 CLEAN TIME CODE
    configTime(0, 0, "pool.ntp.org");
    Serial.print("Syncing time");

    time_t now = time(nullptr);

    while (now < 100000) {
      delay(500);
      Serial.print(".");
      now = time(nullptr);
    }

    Serial.println("\nTime synced!");

  delay(2000);

  
}

void loop() {

    String day = getCurrentDay();
    String slot = getCurrentSlot();

    String scheduledFaculty = "";
    String subjectName = "";

    Serial.println("DAY: " + day);
    Serial.println("SLOT: " + slot);

      // 🔥 ALWAYS SHOW CURRENT LECTURE (NO BUTTON NEEDED)
    lcd.setCursor(0,0);
    lcd.print("                ");
    lcd.setCursor(0,0);
    lcd.print(ROOM_NAME);

    if(slot == ""){
      lcd.setCursor(0,0);
      lcd.print(ROOM_NAME + "      ");
      lcd.setCursor(0,1);
      lcd.print("No Active Slot   ");
      // return;
    }

    // 📅 DATE
    time_t nowDate = time(nullptr);
    struct tm *t = localtime(&nowDate);

    String todayDate = String(t->tm_year + 1900) + "-";
    todayDate += (t->tm_mon + 1 < 10 ? "0" : "") + String(t->tm_mon + 1) + "-";
    todayDate += (t->tm_mday < 10 ? "0" : "") + String(t->tm_mday);

    scheduledFaculty = "";
    subjectName = "";

    // 🔥 CHECK SPECIAL BOOKING
    String specialPath = "classrooms/" + String(ROOM_NAME) + "/specialBookings/" + todayDate + "/" + slot + "/faculty";

    if(Firebase.RTDB.getString(&fbdo, specialPath) && fbdo.stringData() != ""){
      scheduledFaculty = fbdo.stringData();

      String subPath = "classrooms/" + String(ROOM_NAME) + "/specialBookings/" + todayDate + "/" + slot + "/subject";
      if(Firebase.RTDB.getString(&fbdo, subPath)){
        subjectName = fbdo.stringData();
      }
    }
    else{
      // 🔁 TIMETABLE
      String path = "classrooms/" + String(ROOM_NAME) + "/timetable/" + day + "/" + slot + "/faculty";

      if(Firebase.RTDB.getString(&fbdo, path) && fbdo.stringData() != ""){
        scheduledFaculty = fbdo.stringData();

      String subjectPath = "classrooms/" + String(ROOM_NAME) + "/timetable/" + day + "/" + slot + "/subject";
        if(Firebase.RTDB.getString(&fbdo, subjectPath) && fbdo.stringData() != ""){
          subjectName = fbdo.stringData();
        }
      }
    }

    // 📺 DISPLAY
    lcd.setCursor(0,0);
    lcd.print("                ");
    lcd.setCursor(0,0);
    lcd.print(ROOM_NAME);

    lcd.setCursor(0,1);

    if(isInside){
      lcd.setCursor(0,1);
      lcd.print("                ");
      lcd.setCursor(0,1);
      lcd.print(currentSubject.substring(0,16));
    }
    else if(scheduledFaculty != ""){
      lcd.setCursor(0,1);
      lcd.print("                ");
      lcd.setCursor(0,1);
      lcd.print(subjectName.substring(0,16));
    }
    else{
      lcd.setCursor(0,1);
      lcd.print("No Lecture      ");
    }
    time_t nowTime;

      // 🔘 BUTTON PRESS → ENABLE SCAN
      if (digitalRead(BUTTON_PIN) == LOW) {

        lcd.clear();
        lcd.print("Ready to Scan");

        scanMode = true;

        delay(300); // debounce

      }

      // 🔍 ONLY SCAN WHEN BUTTON PRESSED
      if (scanMode) {

      slot = getCurrentSlot();   // 🔥 ADD THIS

      if(slot == ""){
        lcd.clear();
        lcd.print("Outside Time");
        scanMode = false;
        // return;
      }

      if (!mfrc522.PICC_IsNewCardPresent()) {
        return;
      }

      if (!mfrc522.PICC_ReadCardSerial()) {
        return;
      }

    if(millis() - lastScanTime < 3000) return;
    lastScanTime = millis();
    String uid = "";

    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
      uid += String(mfrc522.uid.uidByte[i], HEX);
    }

    uid.toUpperCase();

    Serial.println("UID: " + uid);

    lcd.clear();
    lcd.print("Checking...");

    time_t nowTime = time(nullptr);
    struct tm *tNow = localtime(&nowTime);

    int currentMinutes = tNow->tm_hour * 60 + tNow->tm_min;
    int slotStart = getSlotStartMinutes(slot);
    // 📅 GET TODAY DATE
    String todayDate = "";



    // 🔥 CHECK SPECIAL BOOKING FIRST
    String specialPath = "classrooms/" + String(ROOM_NAME) + "/specialBookings/" + todayDate + "/" + slot + "/faculty";

    if(Firebase.RTDB.getString(&fbdo, specialPath) && fbdo.stringData() != ""){
      scheduledFaculty = fbdo.stringData();

      String subPath = "classrooms/" + String(ROOM_NAME) + "/specialBookings/" + todayDate + "/" + slot + "/subject";
      if(Firebase.RTDB.getString(&fbdo, subPath)){
        subjectName = fbdo.stringData();
      }
    }
    else{
      // 🔁 NORMAL TIMETABLE
      String path = "classrooms/" + String(ROOM_NAME) + "/timetable/" + day + "/" + slot + "/faculty";

      if(!Firebase.RTDB.getString(&fbdo, path) || fbdo.stringData() == ""){
        lcd.clear();
        lcd.print("No Lecture Now");
        delay(2000);
        scanMode = false;
        lcd.clear();
        lcd.print("Press Button");
        return;
      }

      scheduledFaculty = fbdo.stringData();

      String subjectPath = "classrooms/" + String(ROOM_NAME) + "/timetable/" + day + "/" + slot + "/subject";

      if(Firebase.RTDB.getString(&fbdo, subjectPath) && fbdo.stringData() != ""){
        subjectName = fbdo.stringData();
      }
    }
    String scannedFaculty = findFacultyByUID(uid);


        // 🔔 SHOW WAITING STATE
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Lecture Time");
    lcd.setCursor(0,1);
    lcd.print("Waiting...");
    delay(1000);


        


    // ❌ Too Early

        // 🔔 CHECK LATE
    FirebaseJson lateJson;

    if(Firebase.RTDB.getJSON(&fbdo, "late_notifications")){
      lateJson = fbdo.jsonObject();

      size_t count = lateJson.iteratorBegin();

      for (size_t i = 0; i < count; i++) {
        String key, value;
        int type;

        lateJson.iteratorGet(i, type, key, value);

        FirebaseJson obj;
        obj.setJsonData(value);

        FirebaseJsonData roomData, slotData, dateData, minData;

        obj.get(roomData, "room");
        obj.get(slotData, "slot");
        obj.get(dateData, "date");
        obj.get(minData, "minutes");

        if(roomData.stringValue == ROOM_NAME &&
          slotData.stringValue == slot &&
          dateData.stringValue == todayDate){

            lcd.clear();
            lcd.print("Late ");
            lcd.print(minData.stringValue + "m");
            delay(2000);
        }
      }

      lateJson.iteratorEnd();
    }

        // 🔥 DEBUG
        Serial.println("----------");
        Serial.println("Day: " + day);
        Serial.println("Slot: " + slot);
        Serial.println("Scheduled: " + scheduledFaculty);
        Serial.println("Scanned: " + scannedFaculty);
        Serial.println("----------");



        // ⏰ TIMING CONTROL
    int earlyAllow = 10;   // 10 min before allowed
    int lateAllow = 30;    // 10 min after allowed

    if(scannedFaculty == ""){

    FirebaseJson logJson;

    time_t time1 = time(nullptr);
    struct tm *t1 = localtime(&time1);

    String timeStr = String(t1->tm_hour) + ":" + String(t1->tm_min);

    logJson.set("teacher", "Unknown Card");
    logJson.set("room", ROOM_NAME);
    logJson.set("time", timeStr);
    logJson.set("status", "Invalid");


    // 🔥 DATE-WISE PATH
    String path = "/logs/" + todayDate;

    // ✅ PUSH
    Firebase.RTDB.pushJSON(&fbdo, path, &logJson);
      lcd.print("Unknown Card");
      beepInvalid();
    }

    // ✅ ENTRY

    else if(!isInside && scannedFaculty == scheduledFaculty){
      String cancelPath = "cancelled_lectures/" + todayDate + "/" + String(ROOM_NAME) + "/" + slot;

    if(slot != "" && scheduledFaculty != "" &&
      Firebase.RTDB.getString(&fbdo, cancelPath) &&
      fbdo.stringData() == "Manually Cancelled"){
      lcd.clear();
      lcd.print("Lecture Cancelled");
      beepInvalid();
      scanMode = false;
      return;
    }

      // ⏰ 30 MIN WINDOW LOGIC

      if(currentMinutes < slotStart){
        lcd.clear();
        lcd.print("Too Early");
        beepInvalid();
        scanMode = false;
        return;
      }

      if(currentMinutes >= slotStart && currentMinutes <= slotStart + 30){
        lcd.clear();
        lcd.print("Entry Allowed");
        delay(1000);
      }

      if(currentMinutes > slotStart + 30){
      lcd.clear();
      lcd.print("Slot Over");

      Firebase.RTDB.deleteNode(&fbdo, 
      "classrooms/" + String(ROOM_NAME) + "/live");

      scanMode = false;
      return;
    }

      // ✅ ORIGINAL ENTRY CODE CONTINUES
      lcd.clear();
      lcd.print("Lecture Started");
      delay(1500);

      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("Ongoing:      ");
      lcd.setCursor(0,1);
      lcd.print(subjectName);

      beepValid();

      isInside = true;
      currentFaculty = scannedFaculty;
      if(subjectName != ""){
        currentSubject = subjectName;
      } else {
        currentSubject = "Lecture";
      }
      


      // 🔥 Firebase update
    //   Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/status", "Ongoing");

    // Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/faculty", scannedFaculty);

    // Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/subject", subject);
    String basePath = String("classrooms/") + ROOM_NAME + "/live/";

    Firebase.RTDB.setString(&fbdo, basePath + "status", "Ongoing");
    Firebase.RTDB.setString(&fbdo, basePath + "subject", currentSubject);
    Firebase.RTDB.setString(&fbdo, basePath + "faculty", scannedFaculty);
    Firebase.RTDB.setString(&fbdo, basePath + "slot", slot);

      Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/current_faculty", scannedFaculty);
      Firebase.RTDB.setInt(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/startTime", time(nullptr));

      FirebaseJson logJson;

    time_t time2 = time(nullptr);
    struct tm *t2 = localtime(&time2);

    char buffer[6];
    sprintf(buffer, "%02d:%02d", t2->tm_hour, t2->tm_min);
    String timeStr = String(buffer);

    logJson.set("teacher", scannedFaculty);
    logJson.set("room", ROOM_NAME);
    logJson.set("time", timeStr);
    logJson.set("status", "Entry");


    // 🔥 DATE-WISE PATH
    String path = "/logs/" + todayDate;

    // ✅ PUSH
    Firebase.RTDB.pushJSON(&fbdo, path, &logJson);
    }

    // ✅ EXIT (same teacher scans again)
    else if(isInside && scannedFaculty == currentFaculty){

      lcd.clear();
      lcd.print("Lecture Ended");
      delay(1500);

      lcd.clear();
      lcd.print("Room Free");

      beepValid();

      // ⏱️ CHECK TIME
      time_t currentTime = time(nullptr);
      struct tm *t = localtime(&currentTime);

      Firebase.RTDB.getInt(&fbdo,
      "classrooms/" + String(ROOM_NAME) + "/live/startTime");

      int startTime = fbdo.intData();

      if(currentTime - startTime < 1800){
        Serial.println("Left Early");
      }

      isInside = false;
      currentFaculty = "";
      currentSubject = "";

      // 🔥 Firebase update
    Firebase.RTDB.deleteNode(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live");


      Firebase.RTDB.setString(&fbdo, 
      "classrooms/" + String(ROOM_NAME) + "/current_faculty", "");

      FirebaseJson logJson;

    time_t time3 = time(nullptr);
    struct tm *t3 = localtime(&time3);

    String timeStr = String(t3->tm_hour) + ":" + String(t3->tm_min);

    logJson.set("teacher", scannedFaculty);
    logJson.set("room", ROOM_NAME);
    logJson.set("time", timeStr);
    logJson.set("status", "Exit");


    // 🔥 DATE-WISE PATH
    String path = "/logs/" + todayDate;

    // ✅ PUSH
    Firebase.RTDB.pushJSON(&fbdo, path, &logJson);
    }

    // ❌ WRONG PERSON
    else{
      lcd.print("Access Denied");
      beepInvalid();
    }

        delay(3000);

        

        scanMode = false;  // 🔥 RESET
        

        mfrc522.PICC_HaltA();

        nowTime = time(nullptr);
    Serial.println(ctime(&nowTime));
    delay(500);

      }
      String slotCheck = getCurrentSlot();

    if(slotCheck == ""){
      Firebase.RTDB.deleteNode(&fbdo, 
      "classrooms/" + String(ROOM_NAME) + "/live");
    }

}