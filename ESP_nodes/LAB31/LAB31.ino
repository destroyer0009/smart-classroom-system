/*
  ========================================================
   Smart Classroom Allocation System — ESP32 Firmware
   CORRECTED VERSION — All bugs fixed, improvements added
  ========================================================

  FIXES:
  [1]  findFacultyByUID — iteratorEnd() now called before early return (was causing memory leak / crash)
  [2]  time_t nowTime — removed duplicate outer declaration (was shadowed and unused)
  [3]  WiFi loss — now delays + attempts reconnect instead of rapid-spinning
  [4]  scanMode — reset to false on WiFi loss and lunch/none early exits
  [5]  Button press — pre-checks slot before activating scanMode
  [6]  Boot recovery — reads Firebase live node on startup to restore isInside state
  [7]  Teachers cache — auto-refreshes every 30 minutes
  [8]  Exit log time — now uses sprintf (consistent zero-padding like entry log)
  [9]  delay(200) — added on all early-return paths to prevent CPU spinning
  [10] day variable — refreshed inside loop from current time (midnight-safe)
  [11] BUG FIX — recoverStateFromFirebase() now validates stored slot vs current slot
       (stale live node from a previous slot/reboot is now properly cleaned up)
  [12] BUG FIX — current_faculty in Firebase is now cleared on slot change AND
       during lunch/none, not only on explicit faculty exit scan
*/

#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Firebase_ESP_Client.h>
#include <time.h>

// ── ROOM NAME ──────────────────────────────────────────
#define ROOM_NAME "CR125"

// ── WIFI ────────────────────────────────────────────────
#define WIFI_SSID     "group5"
#define WIFI_PASSWORD "12345678"

// ── FIREBASE ────────────────────────────────────────────
#define API_KEY      "AIzaSyAXNrkME8ssbzJxfUrEpzSNDCa7MEpOgrY"
#define DATABASE_URL "https://sample-final-proj-default-rtdb.asia-southeast1.firebasedatabase.app/"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ── PINS ────────────────────────────────────────────────
#define SS_PIN     5
#define RST_PIN    4
#define BUTTON_PIN 17
#define BUZZER_PIN 2

// ── HARDWARE ────────────────────────────────────────────
MFRC522           mfrc522(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ── STATE ───────────────────────────────────────────────
bool   scanMode       = false;
bool   isInside       = false;
String currentFaculty = "";
String currentSubject = "";

// ── LCD CACHE (avoid flicker) ───────────────────────────
String lastLine1 = "";
String lastLine2 = "";

// ── FIREBASE CACHE ──────────────────────────────────────
FirebaseJson teachersCache;
bool          teachersLoaded    = false;
unsigned long teachersCacheTime = 0;
const unsigned long TEACHERS_REFRESH_MS = 1800000UL; // 30 minutes

FirebaseJson  lateJson;
unsigned long lastLateFetch = 0;

String cachedSlot    = "";
String cachedFaculty = "";
String cachedSubject = "";
String cachedDate    = "";

// ── TIMING ──────────────────────────────────────────────
unsigned long lastScanTime   = 0;
unsigned long lastButtonPress = 0;

// ═══════════════════════════════════════════════════════
//  HELPER — clear both live node and current_faculty
// ═══════════════════════════════════════════════════════
void clearRoomState() {
  Firebase.RTDB.deleteNode(&fbdo, "classrooms/" + String(ROOM_NAME) + "/live");
  Firebase.RTDB.setString(&fbdo,
    "classrooms/" + String(ROOM_NAME) + "/current_faculty", "");
  isInside       = false;
  currentFaculty = "";
  currentSubject = "";
}

// ═══════════════════════════════════════════════════════
//  BUZZER
// ═══════════════════════════════════════════════════════
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

// ═══════════════════════════════════════════════════════
//  LCD (only redraws changed lines to avoid flicker)
// ═══════════════════════════════════════════════════════
void updateLCD(String line1, String line2) {
  if (line1 != lastLine1) {
    lcd.setCursor(0, 0);
    lcd.print("                ");
    lcd.setCursor(0, 0);
    lcd.print(line1);
    lastLine1 = line1;
  }
  if (line2 != lastLine2) {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);
    lcd.print(line2);
    lastLine2 = line2;
  }
}

// ═══════════════════════════════════════════════════════
//  TIME HELPERS
// ═══════════════════════════════════════════════════════
String getCurrentDay() {
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  String days[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  return days[t->tm_wday];
}

String getTodayDate() {
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  char buf[11];
  sprintf(buf, "%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
  return String(buf);
}

String getCurrentSlot() {
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  int current = t->tm_hour * 60 + t->tm_min;

  if (current >= 510 && current < 570)  return "s1";
  if (current >= 570 && current < 630)  return "s2";
  if (current >= 640 && current < 700)  return "s3";
  if (current >= 700 && current < 760)  return "s4";
  if (current >= 760 && current < 800)  return "lunch";
  if (current >= 800 && current < 860)  return "s5";
  if (current >= 860 && current < 920)  return "s6";
  if (current >= 920 && current < 980)  return "s7";
  if (current >= 980 && current < 1040) return "s8";
  return "none";
}

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

// ═══════════════════════════════════════════════════════
//  FIND FACULTY BY UID
//  FIX [1]: iteratorEnd() is now called before every return
// ═══════════════════════════════════════════════════════
String findFacultyByUID(String uid) {
  bool shouldRefresh = !teachersLoaded ||
                       (millis() - teachersCacheTime > TEACHERS_REFRESH_MS);

  if (shouldRefresh) {
    if (Firebase.RTDB.getJSON(&fbdo, "teachers")) {
      teachersCache     = fbdo.jsonObject();
      teachersLoaded    = true;
      teachersCacheTime = millis();
    } else {
      Serial.println("Teacher fetch failed: " + fbdo.errorReason());
      return "";
    }
  }

  FirebaseJson &json = teachersCache;
  size_t count = json.iteratorBegin();

  for (size_t i = 0; i < count; i++) {
    String key, value;
    int    type;
    json.iteratorGet(i, type, key, value);

    if (type == FirebaseJson::JSON_OBJECT) {
      FirebaseJson subObj;
      subObj.setJsonData(value);

      FirebaseJsonData rfidData;
      subObj.get(rfidData, "rfid");

      if (rfidData.stringValue == uid) {
        json.iteratorEnd(); // FIX [1]: MUST call before returning
        return key;
      }
    }
  }

  json.iteratorEnd();
  return "";
}

// ═══════════════════════════════════════════════════════
//  BOOT STATE RECOVERY
//  FIX [6] + FIX [11]:
//   — Reads live node on startup to restore isInside
//   — NEW: validates stored slot against current slot
//     so stale live data from a previous session is
//     cleaned up instead of blindly restored
// ═══════════════════════════════════════════════════════
void recoverStateFromFirebase() {
  String statusPath = "classrooms/" + String(ROOM_NAME) + "/live/status";

  if (Firebase.RTDB.getString(&fbdo, statusPath) && fbdo.stringData() == "Ongoing") {

    // ── FIX [11]: validate that stored slot == current slot ──
    String storedSlot = "";
    String slotPath   = "classrooms/" + String(ROOM_NAME) + "/live/slot";
    if (Firebase.RTDB.getString(&fbdo, slotPath)) {
      storedSlot = fbdo.stringData();
    }

    String currentSlotNow = getCurrentSlot();

    if (storedSlot == "" || storedSlot != currentSlotNow) {
      // Slot has changed or is unknown — live node is stale, clean everything
      Serial.println("Boot recovery: stale live node detected (stored=" +
                     storedSlot + " current=" + currentSlotNow + "). Clearing.");
      clearRoomState();
      return;
    }

    // Slot still matches — safe to restore state
    isInside = true;

    String facPath = "classrooms/" + String(ROOM_NAME) + "/live/faculty";
    if (Firebase.RTDB.getString(&fbdo, facPath)) {
      currentFaculty = fbdo.stringData();
    }
    String subPath = "classrooms/" + String(ROOM_NAME) + "/live/subject";
    if (Firebase.RTDB.getString(&fbdo, subPath)) {
      currentSubject = fbdo.stringData();
    }
    Serial.println("State recovered — faculty: " + currentFaculty +
                   " subject: " + currentSubject + " slot: " + storedSlot);

  } else {
    // No live session — ensure Firebase is clean on fresh boot
    clearRoomState();
  }
}

// ═══════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  SPI.begin(18, 19, 23, 5);
  mfrc522.PCD_Init();

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  updateLCD("Connecting...", "WiFi");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int wifiTries = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTries < 20) {
    delay(500);
    wifiTries++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    updateLCD("WiFi Failed", "Check Settings");
    delay(3000);
  } else {
    updateLCD("WiFi OK", "Connecting FB...");
  }

  // ── Firebase ──────────────────────────────────────
  auth.user.email    = "test@test.com";
  auth.user.password = "123456";
  config.api_key     = API_KEY;
  config.database_url = DATABASE_URL;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // ── NTP Time ──────────────────────────────────────
  configTime(19800, 0, "pool.ntp.org");
  updateLCD("Syncing Time", "Please Wait...");

  time_t now = time(nullptr);
  int tries = 0;
  while (now < 100000 && tries < 30) {
    delay(500);
    now = time(nullptr);
    tries++;
  }

  if (now < 100000) {
    Serial.println("Time sync failed!");
    updateLCD("Time Sync", "Failed!");
    delay(2000);
  } else {
    Serial.println("Time synced!");
  }

  // ── Recover state ─────────────────────────────────
  // FIX [6] + FIX [11]: validates slot before restoring
  recoverStateFromFirebase();

  updateLCD(ROOM_NAME, "Ready");
  delay(1500);
}

// ═══════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════
void loop() {

  // ── WiFi check ──────────────────────────────────────
  // FIX [3]: delay + reconnect instead of rapid-spinning
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Lost - Attempting reconnect...");
    updateLCD("WiFi Lost", "Reconnecting...");
    scanMode = false; // FIX [4]
    WiFi.reconnect();
    delay(3000);
    return;
  }

  // ── Current time context ─────────────────────────────
  // FIX [10]: re-evaluated each loop (midnight-safe)
  String day       = getCurrentDay();
  String slot      = getCurrentSlot();
  String todayDate = getTodayDate();

  Serial.println("DAY: " + day + " | SLOT: " + slot + " | DATE: " + todayDate);

  // ── Outside active hours ─────────────────────────────
  // FIX [12]: clearRoomState() now also clears current_faculty in Firebase
  if (slot == "lunch" || slot == "none") {
    updateLCD(ROOM_NAME, slot == "lunch" ? "Lunch Break" : "No Active Slot");
    scanMode = false; // FIX [4]

    // FIX [12]: was only deleting live node — current_faculty was left behind
    clearRoomState();

    delay(200); // FIX [9]
    return;
  }

  // ── Detect slot change → clear live data ─────────────
  static String lastSlot = "";
  if (slot != lastSlot && lastSlot != "") {
    Serial.println("Slot changed: " + lastSlot + " → " + slot + ". Clearing room state.");

    // FIX [12]: was only deleting live node — current_faculty was left behind
    clearRoomState();
  }
  lastSlot = slot;

  // ── Fetch timetable for current slot (cached) ─────────
  if (slot != cachedSlot || todayDate != cachedDate) {
    Serial.println("Fetching from Firebase...");
    cachedSlot    = slot;
    cachedDate    = todayDate;
    cachedFaculty = "";
    cachedSubject = "";

    // Check special bookings first
    String specialPath = "classrooms/" + String(ROOM_NAME)
                       + "/specialBookings/" + todayDate + "/" + slot + "/faculty";

    if (Firebase.RTDB.get(&fbdo, specialPath) &&
        fbdo.dataType() == "string" && fbdo.stringData() != "") {
      cachedFaculty = fbdo.stringData();

      String subPath = "classrooms/" + String(ROOM_NAME)
                     + "/specialBookings/" + todayDate + "/" + slot + "/subject";
      if (Firebase.RTDB.getString(&fbdo, subPath)) {
        cachedSubject = fbdo.stringData();
      }
    } else {
      // Fallback to regular timetable
      String path = "classrooms/" + String(ROOM_NAME)
                  + "/timetable/" + day + "/" + slot + "/faculty";
      if (Firebase.RTDB.getString(&fbdo, path)) {
        cachedFaculty = fbdo.stringData();
      }

      String subjectPath = "classrooms/" + String(ROOM_NAME)
                         + "/timetable/" + day + "/" + slot + "/subject";
      if (Firebase.RTDB.getString(&fbdo, subjectPath)) {
        cachedSubject = fbdo.stringData();
      }
    }
  }

  // ── LCD display ───────────────────────────────────────
  String line2 = "";
  if (isInside) {
    line2 = currentSubject.length() > 0 ? currentSubject : "Ongoing";
  } else if (cachedFaculty != "") {
    line2 = cachedSubject.length() > 0 ? cachedSubject : "Lecture";
  } else {
    line2 = "No Lecture";
  }
  updateLCD(ROOM_NAME, line2);

  // ── Button press → enable scan ────────────────────────
  // FIX [5]: pre-check slot before activating scanMode
  if (digitalRead(BUTTON_PIN) == LOW && millis() - lastButtonPress > 1000) {
    lastButtonPress = millis();

    String currentSlotNow = getCurrentSlot();
    if (currentSlotNow == "lunch" || currentSlotNow == "none") {
      updateLCD("Outside", "Lecture Hours");
      delay(1500);
      updateLCD(ROOM_NAME, line2);
    } else if (!scanMode) {
      updateLCD("Ready", "Scan Card");
      scanMode = true;
    }
  }

  // ── SCAN MODE ─────────────────────────────────────────
  if (scanMode) {
    slot = getCurrentSlot(); // re-check (time may have passed)

    if (slot == "lunch" || slot == "none") {
      updateLCD("Outside", "Lecture Hours");
      scanMode = false;
      delay(200); // FIX [9]
      return;
    }

    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
      delay(50); // FIX [9]: small yield when polling
      return;
    }

    // Debounce rapid scans
    if (millis() - lastScanTime < 3000) {
      mfrc522.PICC_HaltA();
      return;
    }
    lastScanTime = millis();

    // Read UID
    String uid = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
      uid += String(mfrc522.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();
    Serial.println("UID: " + uid);

    updateLCD("Checking...", "Please Wait");

    // Current time for slot-timing checks
    time_t nowTime = time(nullptr); // FIX [2]: single declaration (no outer shadow)
    struct tm *tNow = localtime(&nowTime);
    int currentMinutes = tNow->tm_hour * 60 + tNow->tm_min;
    int slotStart      = getSlotStartMinutes(slot);

    // No lecture scheduled
    if (cachedFaculty == "") {
      updateLCD("No Lecture", "Now");
      beepInvalid();
      scanMode = false;
      delay(1500);
      return;
    }

    // Look up who scanned
    String scannedFaculty = findFacultyByUID(uid);

    // ── Late-notification check (re-fetch every 5 seconds) ──
    bool isLateAllowed = false;

    if (millis() - lastLateFetch > 5000) {
      if (Firebase.RTDB.getJSON(&fbdo, "late_notifications")) {
        lateJson      = fbdo.jsonObject();
        lastLateFetch = millis();
      }
    }

    size_t lcount = lateJson.iteratorBegin();
    for (size_t i = 0; i < lcount; i++) {
      String key, value;
      int    type;
      lateJson.iteratorGet(i, type, key, value);

      FirebaseJson obj;
      obj.setJsonData(value);

      FirebaseJsonData roomData, slotData, dateData, minData;
      obj.get(roomData, "room");
      obj.get(slotData, "slot");
      obj.get(dateData, "date");
      obj.get(minData,  "minutes");

      if (roomData.stringValue == ROOM_NAME &&
          slotData.stringValue == slot      &&
          dateData.stringValue == todayDate) {
        isLateAllowed = true;
        updateLCD("Late:", minData.stringValue + " min");
        delay(1000);
      }
    }
    lateJson.iteratorEnd();

    // ── Debug ──────────────────────────────────────────────
    Serial.println("--- SCAN ---");
    Serial.println("Day: "       + day);
    Serial.println("Slot: "      + slot);
    Serial.println("Scheduled: " + cachedFaculty);
    Serial.println("Scanned: "   + scannedFaculty);
    Serial.println("LateOK: "    + String(isLateAllowed));
    Serial.println("------------");

    // ══════════════════════════════════════════════════════
    //  CASE 1: Unknown RFID card
    // ══════════════════════════════════════════════════════
    if (scannedFaculty == "") {
      updateLCD("Access", "Unknown Card");
      beepInvalid();

      FirebaseJson logJson;
      time_t t1 = time(nullptr);
      struct tm *lt1 = localtime(&t1);
      char tbuf[6];
      sprintf(tbuf, "%02d:%02d", lt1->tm_hour, lt1->tm_min);
      logJson.set("teacher", "Unknown Card");
      logJson.set("room",    ROOM_NAME);
      logJson.set("time",    String(tbuf));
      logJson.set("status",  "Invalid");
      Firebase.RTDB.pushJSON(&fbdo, "/logs/" + todayDate, &logJson);
    }

    // ══════════════════════════════════════════════════════
    //  CASE 2: Entry — correct teacher, not yet inside
    // ══════════════════════════════════════════════════════
    else if (!isInside && scannedFaculty == cachedFaculty) {

      // Check if lecture was cancelled
      String cancelPath = "cancelled_lectures/" + todayDate
                        + "/" + String(ROOM_NAME) + "/" + slot;
      if (Firebase.RTDB.get(&fbdo, cancelPath) &&
          fbdo.dataType() != "null" && fbdo.dataType() != "") {
        updateLCD("Lecture", "Cancelled");
        beepInvalid();
        scanMode = false;
        delay(2000);
        return;
      }

      // Too early (before slot start)
      if (currentMinutes < slotStart) {
        updateLCD("Access", "Too Early");
        beepInvalid();
        scanMode = false;
        delay(1500);
        return;
      }

      // Slot window expired (30-min grace) unless late is allowed
      if (currentMinutes > slotStart + 30 && !isLateAllowed) {
        updateLCD("Slot", "Over");
        clearRoomState(); // FIX [12]: clears both live + current_faculty
        scanMode = false;
        delay(1500);
        return;
      }

      // ✅ Valid entry
      clearRoomState(); // reset any leftover state before writing fresh
      updateLCD("Lecture", "Started");
      beepValid();
      delay(1500);

      isInside       = true;
      currentFaculty = scannedFaculty;
      currentSubject = (cachedSubject != "") ? cachedSubject : "Lecture";

      // Write live node
      String basePath = String("classrooms/") + ROOM_NAME + "/live/";
      long long ts    = (long long)time(nullptr) * 1000;

      Firebase.RTDB.setString(&fbdo, basePath + "status",    "Ongoing");
      Firebase.RTDB.setString(&fbdo, basePath + "subject",   currentSubject);
      Firebase.RTDB.setString(&fbdo, basePath + "faculty",   scannedFaculty);
      Firebase.RTDB.setString(&fbdo, basePath + "slot",      slot);
      Firebase.RTDB.setDouble(&fbdo, basePath + "timestamp", (double)ts);

      Firebase.RTDB.setString(&fbdo,
        "classrooms/" + String(ROOM_NAME) + "/current_faculty", scannedFaculty);

      // Log Entry
      FirebaseJson logJson;
      time_t t2 = time(nullptr);
      struct tm *lt2 = localtime(&t2);
      char tbuf[6];
      sprintf(tbuf, "%02d:%02d", lt2->tm_hour, lt2->tm_min);
      logJson.set("teacher", scannedFaculty);
      logJson.set("room",    ROOM_NAME);
      logJson.set("time",    String(tbuf));
      logJson.set("status",  "Entry");
      Firebase.RTDB.pushJSON(&fbdo, "/logs/" + todayDate, &logJson);

      updateLCD("Ongoing", currentSubject.substring(0, 16));
    }

    // ══════════════════════════════════════════════════════
    //  CASE 3: Exit — same teacher scans again
    // ══════════════════════════════════════════════════════
    else if (isInside && scannedFaculty == currentFaculty) {

      updateLCD("Lecture", "Ended");
      beepValid();
      delay(1500);

      // Check if teacher left early (< 30 min)
      time_t entryTs = 0;
      if (Firebase.RTDB.getDouble(&fbdo,
          "classrooms/" + String(ROOM_NAME) + "/live/timestamp")) {
        entryTs = (time_t)((long long)(fbdo.doubleData() / 1000));
      }
      time_t now2 = time(nullptr);
      if (entryTs > 0 && (now2 - entryTs < 1800)) {
        Serial.println("⚠ Teacher left early (< 30 min)");
      }

      // Log Exit before clearing state (need scannedFaculty name)
      FirebaseJson logJson;
      time_t t3 = time(nullptr);
      struct tm *lt3 = localtime(&t3);
      char tbuf[6];
      sprintf(tbuf, "%02d:%02d", lt3->tm_hour, lt3->tm_min);
      logJson.set("teacher", scannedFaculty);
      logJson.set("room",    ROOM_NAME);
      logJson.set("time",    String(tbuf));
      logJson.set("status",  "Exit");
      Firebase.RTDB.pushJSON(&fbdo, "/logs/" + todayDate, &logJson);

      // clearRoomState() sets isInside=false, currentFaculty="",
      // deletes live node, and clears current_faculty in Firebase
      clearRoomState();

      updateLCD(ROOM_NAME, "Room Free");
    }

    // ══════════════════════════════════════════════════════
    //  CASE 4: Wrong person
    // ══════════════════════════════════════════════════════
    else {
      updateLCD("Access", "Denied");
      beepInvalid();
      delay(1500);
    }

    delay(2000); // show result on LCD before resetting
    scanMode = false;
    mfrc522.PICC_HaltA();

    // Debug print
    time_t debugNow = time(nullptr);
    Serial.println("After scan: " + String(ctime(&debugNow)));
  }

  delay(200); // FIX [9]: prevent CPU hogging between loop iterations
}
