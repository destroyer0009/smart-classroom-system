#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Firebase_ESP_Client.h>
#include <time.h>

#define ROOM_NAME "CR126"

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
String lastLine1 = "";
String lastLine2 = "";

FirebaseJson teachersCache;
bool teachersLoaded = false;

String cachedSlot = "";
String cachedFaculty = "";
String cachedSubject = "";
String cachedDate = "";
    FirebaseJson lateJson;
    unsigned long lastLateFetch = 0;
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
void updateLCD(String line1, String line2) {

  if(line1 != lastLine1){
    lcd.setCursor(0,0);
    lcd.print("                ");
    lcd.setCursor(0,0);
    lcd.print(line1);
    lastLine1 = line1;
  }

  if(line2 != lastLine2){
    lcd.setCursor(0,1);
    lcd.print("                ");
    lcd.setCursor(0,1);
    lcd.print(line2);
    lastLine2 = line2;
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
  if (slot == "s3") return 640;
  if (slot == "s4") return 700;
  if (slot == "s5") return 800;
  if (slot == "s6") return 860;
  if (slot == "s7") return 920;
  if (slot == "s8") return 980;
  return -1;
}

// 🔍 UID → FACULTY
String findFacultyByUID(String uid){

  if(!teachersLoaded){
    if (Firebase.RTDB.getJSON(&fbdo, "teachers")) {
      teachersCache = fbdo.jsonObject();
      teachersLoaded = true;
    } else {
      Serial.println("Teacher fetch failed");
      return "";
    }
  }

  FirebaseJson &json = teachersCache;

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
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);

  int current = t->tm_hour * 60 + t->tm_min;

  if(current >= 510 && current < 570) return "s1";
  if(current >= 570 && current < 630) return "s2";

  // if(current >= 630 && current < 640) return "break";

  if(current >= 640 && current < 700) return "s3";
  if(current >= 700 && current < 760) return "s4";

  if(current >= 760 && current < 800) return "lunch";

  if(current >= 800 && current < 860) return "s5";
  if(current >= 860 && current < 920) return "s6";
  if(current >= 920 && current < 980) return "s7";
  if(current >= 980 && current < 1040) return "s8";

  return "none";
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

  
}

void loop() {

    String day = getCurrentDay();
    String slot = getCurrentSlot();
if(WiFi.status() != WL_CONNECTED){
  Serial.println("WiFi Lost - Running Offline");
  return;
}
    time_t nowDate = time(nullptr);
struct tm *t = localtime(&nowDate);

String todayDate = String(t->tm_year + 1900) + "-";
todayDate += (t->tm_mon + 1 < 10 ? "0" : "") + String(t->tm_mon + 1) + "-";
todayDate += (t->tm_mday < 10 ? "0" : "") + String(t->tm_mday);


    Serial.println("DAY: " + day);
    Serial.println("SLOT: " + slot);

      // 🔥 ALWAYS SHOW CURRENT LECTURE (NO BUTTON NEEDED)

    if(slot == "lunch" || slot == "none"){
      updateLCD(ROOM_NAME, "No Active Slot");
      scanMode = false;
return;
    }

    static String lastSlot = "";

if(slot != lastSlot && lastSlot != ""){
  Firebase.RTDB.deleteNode(&fbdo, 
  "classrooms/" + String(ROOM_NAME) + "/live");
}

lastSlot = slot;

if(slot != cachedSlot || todayDate != cachedDate){

  Serial.println("🔄 Fetching from Firebase...");

  cachedSlot = slot;
  cachedDate = todayDate;

  cachedFaculty = "";
  cachedSubject = "";

  String specialPath = "classrooms/" + String(ROOM_NAME) + 
  "/specialBookings/" + todayDate + "/" + slot + "/faculty";

  if(Firebase.RTDB.get(&fbdo, specialPath) && 
     fbdo.dataType() == "string" && fbdo.stringData() != ""){

    cachedFaculty = fbdo.stringData();

    String subPath = "classrooms/" + String(ROOM_NAME) + 
    "/specialBookings/" + todayDate + "/" + slot + "/subject";

    if(Firebase.RTDB.getString(&fbdo, subPath)){
      cachedSubject = fbdo.stringData();
    }
  }
  else{
    String path = "classrooms/" + String(ROOM_NAME) + 
    "/timetable/" + day + "/" + slot + "/faculty";

    if(Firebase.RTDB.getString(&fbdo, path)){
      cachedFaculty = fbdo.stringData();
    }

    String subjectPath = "classrooms/" + String(ROOM_NAME) + 
    "/timetable/" + day + "/" + slot + "/subject";

    if(Firebase.RTDB.getString(&fbdo, subjectPath)){
      cachedSubject = fbdo.stringData();
    }
  }
}

String scheduledFaculty = cachedFaculty;
String subjectName = cachedSubject;

    // 📺 DISPLAY
String line1 = ROOM_NAME;
String line2 = "";

if(isInside){
  line2 = currentSubject;
}
else if(slot != "none" && slot != "lunch" && scheduledFaculty != ""){
  if(subjectName != ""){
    line2 = subjectName;
  } else {
    line2 = "Lecture";
  }
}
else{
  line2 = "No Lecture";
}

updateLCD(line1, line2);


    time_t nowTime;

      // 🔘 BUTTON PRESS → ENABLE SCAN
      if (digitalRead(BUTTON_PIN) == LOW && !scanMode) {
  updateLCD("Ready", "Scan Card");
  scanMode = true;
  delay(400);
}

      // 🔍 ONLY SCAN WHEN BUTTON PRESSED
      if (scanMode) {

      slot = getCurrentSlot();   // 🔥 ADD THIS

      if(slot == "lunch" || slot == "none"){
        lcd.clear();
        lcd.print("Outside Time");
        scanMode = false;
        return;
      }

      if (!mfrc522.PICC_IsNewCardPresent() || 
    !mfrc522.PICC_ReadCardSerial()) {
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
    Serial.println(uid);

    Serial.println("UID: " + uid);

   updateLCD("Checking", "Please Wait");

    time_t nowTime = time(nullptr);
    struct tm *tNow = localtime(&nowTime);

    int currentMinutes = tNow->tm_hour * 60 + tNow->tm_min;
    int slotStart = getSlotStartMinutes(slot);
    bool isLateAllowed = false;
// // 🔥 RE-FETCH DATA FOR SCAN (VERY IMPORTANT)

// // Check special booking again
// String specialPath = "classrooms/" + String(ROOM_NAME) + 
// "/specialBookings/" + todayDate + "/" + slot + "/faculty";

// if(Firebase.RTDB.get(&fbdo, specialPath) && fbdo.dataType() == "string" && fbdo.stringData() != ""){
//   cachedFaculty = fbdo.stringData();

//   String subPath = "classrooms/" + String(ROOM_NAME) + 
//   "/specialBookings/" + todayDate + "/" + slot + "/subject";

//   if(Firebase.RTDB.getString(&fbdo, subPath)){
//     cachedSubject = fbdo.stringData();
//   }
// }
// else{
//   // fallback to timetable
//   String path = "classrooms/" + String(ROOM_NAME) + 
//   "/timetable/" + day + "/" + slot + "/faculty";

//   if(Firebase.RTDB.getString(&fbdo, path)){
//     cachedFaculty = fbdo.stringData();
//   }

//   String subjectPath = "classrooms/" + String(ROOM_NAME) + 
//   "/timetable/" + day + "/" + slot + "/subject";

//   if(Firebase.RTDB.getString(&fbdo, subjectPath)){
//     cachedSubject = fbdo.stringData();
//   }
// }

   if(cachedFaculty == ""){
  updateLCD("No Lecture", "Now");
  scanMode = false;
  return;
}
    
    String scannedFaculty = findFacultyByUID(uid);


        // 🔔 SHOW WAITING STATE
   updateLCD("Lecture Time", "Waiting...");
    delay(500);


        // 🔔 CHECK LATE



  if(millis() - lastLateFetch > 5000){
    if(Firebase.RTDB.getJSON(&fbdo, "late_notifications")){
        lateJson = fbdo.jsonObject();
    }
    lastLateFetch = millis();
}

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
          isLateAllowed = true;   // ✅ IMPORTANT

          updateLCD("Late", minData.stringValue + "m");
          delay(1000);

          
      }
    }

      lateJson.iteratorEnd();
      

        // 🔥 DEBUG
        Serial.println("----------");
        Serial.println("Day: " + day);
        Serial.println("Slot: " + slot);
        Serial.println("Scheduled: " + cachedFaculty);
        Serial.println("Scanned: " + scannedFaculty);
        Serial.println("----------");



        // ⏰ TIMING CONTROL
  

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
      updateLCD("Access", "Unknown Card");
      beepInvalid();
    }

    // ✅ ENTRY

    else if(!isInside && scannedFaculty == cachedFaculty){
      String cancelPath = "cancelled_lectures/" + todayDate + "/" + String(ROOM_NAME) + "/" + slot;

if(slot != "lunch" && slot != "none" && cachedFaculty != "" &&
   Firebase.RTDB.get(&fbdo, cancelPath) &&
   fbdo.dataType() == "string"){

  updateLCD("Lecture","Cancelled");
  beepInvalid();
  scanMode = false;
  return;
}

      // ⏰ 30 MIN WINDOW LOGIC

      // ❌ Too early
if(currentMinutes < slotStart){
  updateLCD("Access", "Too Early");
  beepInvalid();
  scanMode = false;
  return;
}

// ❌ Slot over ONLY if late not allowed
if(currentMinutes > slotStart + 30 && !isLateAllowed){
  updateLCD("Slot","Over");

  Firebase.RTDB.deleteNode(&fbdo, 
  "classrooms/" + String(ROOM_NAME) + "/live");

  scanMode = false;
  return;
}

      // ✅ ORIGINAL ENTRY CODE CONTINUES
      Firebase.RTDB.deleteNode(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live");

  updateLCD("Lecture","Started");
  delay(1500);

      updateLCD("Ongoing", cachedSubject.substring(0,16));

      beepValid();

      isInside = true;
      currentFaculty = scannedFaculty;
      if(cachedSubject != ""){
        currentSubject = cachedSubject;
      } else {
        currentSubject = "Lecture";
      }
      


      // 🔥 Firebase update
    //   Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/status", "Ongoing");

    // Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/faculty", scannedFaculty);

    // Firebase.RTDB.setString(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live/subject", subject);
    String basePath = String("classrooms/") + ROOM_NAME + "/live/";

    time_t now;
    time(&now);

    long long timestamp = (long long)now * 1000;   // 🔥 convert to milliseconds

    Firebase.RTDB.setString(&fbdo, basePath + "status", "Ongoing");
    Firebase.RTDB.setString(&fbdo, basePath + "subject", currentSubject);
    Firebase.RTDB.setString(&fbdo, basePath + "faculty", scannedFaculty);
    Firebase.RTDB.setString(&fbdo, basePath + "slot", slot);
    Firebase.RTDB.setDouble(&fbdo, basePath + "timestamp", timestamp); // 🔥 FIX

    Firebase.RTDB.setString(&fbdo, 
    "classrooms/" + String(ROOM_NAME) + "/current_faculty", scannedFaculty);

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

      updateLCD("Lecture", "Ended");
      delay(1500);

      updateLCD("Room","Free");

      beepValid();

      // ⏱️ CHECK TIME
      time_t currentTime = time(nullptr);
      struct tm *t = localtime(&currentTime);

      long long timestamp = 0;

      if(Firebase.RTDB.getDouble(&fbdo,
      "classrooms/" + String(ROOM_NAME) + "/live/timestamp")){

        timestamp = (long long)(fbdo.doubleData() / 1000);
      }

      if(timestamp > 0 && (currentTime - timestamp < 1800)){
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
      updateLCD("Access", "Denied");
      beepInvalid();
      delay(1500);
    }

        delay(3000);

        

        scanMode = false;  // 🔥 RESET
        

        mfrc522.PICC_HaltA();

        nowTime = time(nullptr);
    Serial.println(ctime(&nowTime));
    delay(500);

      }
      String slotCheck = getCurrentSlot();

    if(slotCheck == "lunch" || slotCheck == "none"){
      Firebase.RTDB.deleteNode(&fbdo, 
      "classrooms/" + String(ROOM_NAME) + "/live");
    }

    delay(200);
}