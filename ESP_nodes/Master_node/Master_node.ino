/*
  ========================================================
   Smart Classroom Allocation — ESP-NOW MASTER NODE
  ========================================================

  WIRING:
    RFID:       SS=5, RST=4, SPI (SCK=18, MISO=19, MOSI=23)
    LCD:        I2C 0x27, 16x2, SDA=21 SCL=22
    ENROLL_BTN: GPIO 0  (BOOT button — hold 2s to toggle)
    BUZZER:     GPIO 2

  ENROLL MODE:
    Hold BOOT button 2s → enters enroll mode
    Scan any card → UID shown on LCD
    Known card   → shows teacher name on line 2
    Unknown card → shows "Not Registered" on line 2
    You then go to your website and add the UID there.
    Short-press button → exit enroll mode
*/

#include <WiFi.h>
#include <esp_now.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Firebase_ESP_Client.h>
#include <time.h>

// ── WIFI ────────────────────────────────────────────────
#define WIFI_SSID      "group5"
#define WIFI_PASSWORD  "12345678"
#define ESPNOW_CHANNEL 1

// ── FIREBASE ────────────────────────────────────────────
#define API_KEY      "AIzaSyAXNrkME8ssbzJxfUrEpzSNDCa7MEpOgrY"
#define DATABASE_URL "https://sample-final-proj-default-rtdb.asia-southeast1.firebasedatabase.app/"

FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;

// ── PINS ────────────────────────────────────────────────
#define ADMIN_SS_PIN   5
#define ADMIN_RST_PIN  4
#define ENROLL_BTN     17    // BOOT button
#define ADMIN_BUZZER   2

// ── HARDWARE ────────────────────────────────────────────
MFRC522           adminRFID(ADMIN_SS_PIN, ADMIN_RST_PIN);
LiquidCrystal_I2C adminLCD(0x27, 16, 2);

// ── LCD CACHE ────────────────────────────────────────────
String lastL1 = "", lastL2 = "";

void updateLCD(String l1, String l2) {
  if (l1 != lastL1) {
    adminLCD.setCursor(0, 0);
    adminLCD.print("                ");
    adminLCD.setCursor(0, 0);
    adminLCD.print(l1);
    lastL1 = l1;
  }
  if (l2 != lastL2) {
    adminLCD.setCursor(0, 1);
    adminLCD.print("                ");
    adminLCD.setCursor(0, 1);
    adminLCD.print(l2);
    lastL2 = l2;
  }
}

// ── SLAVE MACs ───────────────────────────────────────────
uint8_t slaveMACs[][6] = {
  {0xD4, 0xE9, 0xF4, 0xBC, 0x56, 0x68},  // CR125
  {0x00, 0x70, 0x07, 0xE1, 0xFD, 0x9C},  // CR126
  {0x28, 0x05, 0xA5, 0x6E, 0x6F, 0x88},  // LAB31
};
#define SLAVE_COUNT (sizeof(slaveMACs) / sizeof(slaveMACs[0]))

// ── PACKET STRUCTS ───────────────────────────────────────
typedef struct ScanPacket {
  char room[8];
  char uid[16];
} ScanPacket;

typedef struct ResponsePacket {
  char    line1[17];
  char    line2[17];
  uint8_t beep;
} ResponsePacket;

// ── TEACHER CACHE ────────────────────────────────────────
FirebaseJson  teachersCache;
bool          teachersLoaded    = false;
unsigned long teachersCacheTime = 0;
const unsigned long TEACHERS_REFRESH_MS = 1800000UL;

// ── ENROLL MODE STATE ────────────────────────────────────
bool          enrollMode        = false;
unsigned long btnHoldStart      = 0;
bool          btnWasLow         = false;
unsigned long lastBtnRelease    = 0;
unsigned long lastEnrollScan    = 0;    // debounce for enroll RFID reads
const unsigned long BTN_LONG_MS = 2000UL;
const unsigned long BTN_DEBOUNCE = 250UL;
const unsigned long SCAN_DEBOUNCE = 3000UL;

// ── PER-ROOM STATE ───────────────────────────────────────
struct RoomState {
  String roomName, currentFaculty, currentSubject;
  String cachedSlot, cachedDate, cachedFaculty, cachedSubject;
  bool   isInside;
};

RoomState rooms[8];
int       roomCount = 0;

RoomState* getRoom(const char* name) {
  for (int i = 0; i < roomCount; i++)
    if (rooms[i].roomName == String(name)) return &rooms[i];
  if (roomCount < 8) {
    rooms[roomCount] = { String(name), "", "", "", "", "", "", false };
    return &rooms[roomCount++];
  }
  return nullptr;
}

// ── ESP-NOW QUEUE ────────────────────────────────────────
#define QUEUE_SIZE 8
volatile ScanPacket scanQueue[QUEUE_SIZE];
volatile uint8_t    senderMAC[QUEUE_SIZE][6];
volatile int        qHead = 0, qTail = 0;

void IRAM_ATTR onDataReceive(const uint8_t* mac, const uint8_t* data, int len) {
  if (len != sizeof(ScanPacket)) return;
  int next = (qTail + 1) % QUEUE_SIZE;
  if (next == qHead) return;
  memcpy((void*)&scanQueue[qTail], data, sizeof(ScanPacket));
  memcpy((void*)senderMAC[qTail], mac, 6);
  qTail = next;
}

// ── BUZZER ───────────────────────────────────────────────
void beepValid()   { digitalWrite(ADMIN_BUZZER, HIGH); delay(200); digitalWrite(ADMIN_BUZZER, LOW); }
void beepInvalid() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(ADMIN_BUZZER, HIGH); delay(150);
    digitalWrite(ADMIN_BUZZER, LOW);  delay(150);
  }
}
void beepMode() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(ADMIN_BUZZER, HIGH); delay(80);
    digitalWrite(ADMIN_BUZZER, LOW);  delay(80);
  }
}

// ── SEND RESPONSE TO SLAVE ───────────────────────────────
void sendResponse(const uint8_t* mac, const char* l1, const char* l2, uint8_t beep) {
  ResponsePacket pkt;
  strncpy(pkt.line1, l1, 16); pkt.line1[16] = 0;
  strncpy(pkt.line2, l2, 16); pkt.line2[16] = 0;
  pkt.beep = beep;
  esp_now_send(mac, (uint8_t*)&pkt, sizeof(pkt));
}

// ── TIME HELPERS ─────────────────────────────────────────
String getCurrentDay() {
  time_t now = time(nullptr); struct tm *t = localtime(&now);
  String days[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  return days[t->tm_wday];
}
String getTodayDate() {
  time_t now = time(nullptr); struct tm *t = localtime(&now);
  char buf[11]; sprintf(buf, "%04d-%02d-%02d", t->tm_year+1900, t->tm_mon+1, t->tm_mday);
  return String(buf);
}
String getCurrentSlot() {
  time_t now = time(nullptr); struct tm *t = localtime(&now);
  int cur = t->tm_hour * 60 + t->tm_min;
  if (cur >= 510  && cur < 570)  return "s1";
  if (cur >= 570  && cur < 630)  return "s2";
  if (cur >= 640  && cur < 700)  return "s3";
  if (cur >= 700  && cur < 760)  return "s4";
  if (cur >= 760  && cur < 800)  return "lunch";
  if (cur >= 800  && cur < 860)  return "s5";
  if (cur >= 860  && cur < 920)  return "s6";
  if (cur >= 920  && cur < 980)  return "s7";
  if (cur >= 980  && cur < 1040) return "s8";
  return "none";
}
int getSlotStartMinutes(String slot) {
  if (slot=="s1") return 510; if (slot=="s2") return 570;
  if (slot=="s3") return 640; if (slot=="s4") return 700;
  if (slot=="s5") return 800; if (slot=="s6") return 860;
  if (slot=="s7") return 920; if (slot=="s8") return 980;
  return -1;
}

// ── FIND FACULTY BY UID ──────────────────────────────────
String findFacultyByUID(String uid) {
  bool stale = !teachersLoaded || (millis() - teachersCacheTime > TEACHERS_REFRESH_MS);
  if (stale) {
    if (Firebase.RTDB.getJSON(&fbdo, "teachers")) {
      teachersCache     = fbdo.jsonObject();
      teachersLoaded    = true;
      teachersCacheTime = millis();
    } else return "";
  }
  FirebaseJson &json = teachersCache;
  size_t count = json.iteratorBegin();
  for (size_t i = 0; i < count; i++) {
    String key, value; int type;
    json.iteratorGet(i, type, key, value);
    if (type == FirebaseJson::JSON_OBJECT) {
      FirebaseJson sub; sub.setJsonData(value);
      FirebaseJsonData d; sub.get(d, "rfid");
      if (d.stringValue == uid) { json.iteratorEnd(); return key; }
    }
  }
  json.iteratorEnd();
  return "";
}

// ── READ RFID ────────────────────────────────────────────
String readRFID() {
  if (!adminRFID.PICC_IsNewCardPresent()) return "";
  if (!adminRFID.PICC_ReadCardSerial())   return "";
  String uid = "";
  for (byte i = 0; i < adminRFID.uid.size; i++) {
    if (adminRFID.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(adminRFID.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  adminRFID.PICC_HaltA();
  adminRFID.PCD_StopCrypto1();
  return uid;
}

// ═══════════════════════════════════════════════════════
//  ENROLL BUTTON HANDLER
//  Normal mode  : hold 2s → enter enroll
//  Enroll mode  : any press → exit
// ═══════════════════════════════════════════════════════
void handleEnrollButton() {
  bool low = (digitalRead(ENROLL_BTN) == LOW);

  if (low && !btnWasLow) {
    btnHoldStart = millis();
    btnWasLow    = true;
  }

  if (!low && btnWasLow) {
    unsigned long held = millis() - btnHoldStart;
    btnWasLow = false;

    if (millis() - lastBtnRelease < BTN_DEBOUNCE) return;
    lastBtnRelease = millis();

    if (!enrollMode && held >= BTN_LONG_MS) {
      // Enter enroll mode
      enrollMode = true;
      beepMode();
      updateLCD("ENROLL MODE", "Scan a card...");
      Serial.println("[ENROLL] Entered enroll mode");

    } else if (enrollMode) {
      // Exit enroll mode
      enrollMode = false;
      beepMode();
      updateLCD("Master Ready", "Hold btn=Enroll");
      Serial.println("[ENROLL] Exited enroll mode");
    }
  }
}

// ═══════════════════════════════════════════════════════
//  ENROLL MODE — just scan and display UID
// ═══════════════════════════════════════════════════════
void handleEnrollScan() {
  String uid = readRFID();
  if (uid.length() == 0) return;

  // Debounce repeated reads of the same card
  if (millis() - lastEnrollScan < SCAN_DEBOUNCE) return;
  lastEnrollScan = millis();

  Serial.println("[ENROLL] Card UID: " + uid);

  String name = findFacultyByUID(uid);

  if (name != "") {
    // Known card — show name
    name.replace("_", " ");
    updateLCD(uid, name.substring(0, 16));
    beepValid();
    Serial.println("[ENROLL] Known: " + name);
  } else {
    // Unknown card — show UID so admin can copy it to website
    updateLCD(uid, "Not Registered");
    beepInvalid();
    Serial.println("[ENROLL] Unknown card — add UID on website");
  }
}

// ═══════════════════════════════════════════════════════
//  ROOM CACHE + STATE RECOVERY
// ═══════════════════════════════════════════════════════
void refreshRoomCache(RoomState* r, String slot, String day, String date) {
  if (slot == r->cachedSlot && date == r->cachedDate) return;
  r->cachedSlot = slot; r->cachedDate = date;
  r->cachedFaculty = ""; r->cachedSubject = "";

  String sp = "classrooms/" + r->roomName + "/specialBookings/" + date + "/" + slot + "/faculty";
  if (Firebase.RTDB.get(&fbdo, sp) && fbdo.dataType() == "string" && fbdo.stringData() != "") {
    r->cachedFaculty = fbdo.stringData();
    String ss = "classrooms/" + r->roomName + "/specialBookings/" + date + "/" + slot + "/subject";
    if (Firebase.RTDB.getString(&fbdo, ss)) r->cachedSubject = fbdo.stringData();
  } else {
    String p = "classrooms/" + r->roomName + "/timetable/" + day + "/" + slot + "/faculty";
    if (Firebase.RTDB.getString(&fbdo, p)) r->cachedFaculty = fbdo.stringData();
    String ps = "classrooms/" + r->roomName + "/timetable/" + day + "/" + slot + "/subject";
    if (Firebase.RTDB.getString(&fbdo, ps)) r->cachedSubject = fbdo.stringData();
  }
}

void recoverState(RoomState* r) {
  if (Firebase.RTDB.getString(&fbdo, "classrooms/" + r->roomName + "/live/status") &&
      fbdo.stringData() == "Ongoing") {
    r->isInside = true;
    if (Firebase.RTDB.getString(&fbdo, "classrooms/" + r->roomName + "/live/faculty"))
      r->currentFaculty = fbdo.stringData();
    if (Firebase.RTDB.getString(&fbdo, "classrooms/" + r->roomName + "/live/subject"))
      r->currentSubject = fbdo.stringData();
  } else {
    Firebase.RTDB.deleteNode(&fbdo, "classrooms/" + r->roomName + "/live");
    r->isInside = false;
  }
}

// ═══════════════════════════════════════════════════════
//  PROCESS SLAVE SCAN
// ═══════════════════════════════════════════════════════
void processScan(const uint8_t* mac, const ScanPacket& pkt) {
  String uid  = String(pkt.uid);
  String room = String(pkt.room);
  Serial.printf("[MASTER] Scan from %s | UID: %s\n", room.c_str(), uid.c_str());

  RoomState* r = getRoom(pkt.room);
  if (!r) { sendResponse(mac, "System Error", "Too many rooms", 0); return; }

  String slot      = getCurrentSlot();
  String day       = getCurrentDay();
  String todayDate = getTodayDate();

  if (slot == "lunch" || slot == "none") {
    sendResponse(mac, "Outside", slot == "lunch" ? "Lunch Break" : "No Active Slot", 0);
    return;
  }

  static String lastSlotPerRoom[8];
  int idx = r - rooms;
  if (slot != lastSlotPerRoom[idx] && lastSlotPerRoom[idx] != "") {
    Firebase.RTDB.deleteNode(&fbdo, "classrooms/" + room + "/live");
    r->isInside = false; r->currentFaculty = ""; r->currentSubject = "";
  }
  lastSlotPerRoom[idx] = slot;

  refreshRoomCache(r, slot, day, todayDate);
  if (r->cachedFaculty == "") { sendResponse(mac, "No Lecture", "Now", 0); return; }

  time_t nowTime = time(nullptr); struct tm *tNow = localtime(&nowTime);
  int currentMinutes = tNow->tm_hour * 60 + tNow->tm_min;
  int slotStart      = getSlotStartMinutes(slot);
  String scannedFaculty = findFacultyByUID(uid);

  // Late notification check
  bool isLateAllowed = false;
  FirebaseJson lateJson;
  if (Firebase.RTDB.getJSON(&fbdo, "late_notifications")) {
    lateJson = fbdo.jsonObject();
    size_t lc = lateJson.iteratorBegin();
    for (size_t i = 0; i < lc; i++) {
      String key, value; int type;
      lateJson.iteratorGet(i, type, key, value);
      FirebaseJson obj; obj.setJsonData(value);
      FirebaseJsonData rd, sd, dd;
      obj.get(rd, "room"); obj.get(sd, "slot"); obj.get(dd, "date");
      if (rd.stringValue == room && sd.stringValue == slot && dd.stringValue == todayDate)
        isLateAllowed = true;
    }
    lateJson.iteratorEnd();
  }

  char tbuf[6];
  time_t tlog = time(nullptr); struct tm *lt = localtime(&tlog);
  sprintf(tbuf, "%02d:%02d", lt->tm_hour, lt->tm_min);

  // CASE 1: Unknown card
  if (scannedFaculty == "") {
    FirebaseJson log;
    log.set("teacher", "Unknown Card"); log.set("room", room);
    log.set("time", String(tbuf));      log.set("status", "Invalid");
    Firebase.RTDB.pushJSON(&fbdo, "/logs/" + todayDate, &log);
    sendResponse(mac, "Access", "Unknown Card", 0);
    return;
  }

  // CASE 2: Entry
  if (!r->isInside && scannedFaculty == r->cachedFaculty) {
    String cancelPath = "cancelled_lectures/" + todayDate + "/" + room + "/" + slot;
    if (Firebase.RTDB.get(&fbdo, cancelPath) &&
        fbdo.dataType() != "null" && fbdo.dataType() != "") {
      sendResponse(mac, "Lecture", "Cancelled", 0); return;
    }
    if (currentMinutes < slotStart) { sendResponse(mac, "Access", "Too Early", 0); return; }
    if (currentMinutes > slotStart + 30 && !isLateAllowed) {
      Firebase.RTDB.deleteNode(&fbdo, "classrooms/" + room + "/live");
      sendResponse(mac, "Slot", "Over", 0); return;
    }

    r->isInside       = true;
    r->currentFaculty = scannedFaculty;
    r->currentSubject = (r->cachedSubject != "") ? r->cachedSubject : "Lecture";

    Firebase.RTDB.deleteNode(&fbdo, "classrooms/" + room + "/live");
    String base = "classrooms/" + room + "/live/";
    Firebase.RTDB.setString(&fbdo, base + "status",   "Ongoing");
    Firebase.RTDB.setString(&fbdo, base + "faculty",   scannedFaculty);
    Firebase.RTDB.setString(&fbdo, base + "subject",   r->currentSubject);
    Firebase.RTDB.setString(&fbdo, base + "slot",      slot);
    Firebase.RTDB.setDouble(&fbdo, base + "timestamp",  (double)((long long)time(nullptr)*1000));
    Firebase.RTDB.setString(&fbdo, "classrooms/" + room + "/current_faculty", scannedFaculty);

    FirebaseJson log;
    log.set("teacher", scannedFaculty); log.set("room", room);
    log.set("time", String(tbuf));      log.set("status", "Entry");
    Firebase.RTDB.pushJSON(&fbdo, "/logs/" + todayDate, &log);

    sendResponse(mac, "Started!", r->currentSubject.substring(0, 16).c_str(), 1);
    return;
  }

  // CASE 3: Exit
  if (r->isInside && scannedFaculty == r->currentFaculty) {
    time_t entryTs = 0;
    if (Firebase.RTDB.getDouble(&fbdo, "classrooms/" + room + "/live/timestamp"))
      entryTs = (time_t)((long long)(fbdo.doubleData() / 1000));
    if (entryTs > 0 && (time(nullptr) - entryTs < 1800))
      Serial.println("Warning: early exit < 30 min — " + room);

    r->isInside = false; r->currentFaculty = ""; r->currentSubject = "";
    Firebase.RTDB.deleteNode(&fbdo, "classrooms/" + room + "/live");
    Firebase.RTDB.setString(&fbdo, "classrooms/" + room + "/current_faculty", "");

    FirebaseJson log;
    log.set("teacher", scannedFaculty); log.set("room", room);
    log.set("time", String(tbuf));      log.set("status", "Exit");
    Firebase.RTDB.pushJSON(&fbdo, "/logs/" + todayDate, &log);

    sendResponse(mac, "Lecture Ended", "Room Free", 1);
    return;
  }

  // CASE 4: Wrong person
  sendResponse(mac, "Access", "Denied", 0);
}

// ═══════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  pinMode(ENROLL_BTN,   INPUT_PULLUP);
  pinMode(ADMIN_BUZZER, OUTPUT);

  SPI.begin(18, 19, 23, ADMIN_SS_PIN);
  adminRFID.PCD_Init();
  delay(50);

  Wire.begin(21, 22);
  adminLCD.init();
  adminLCD.backlight();
  updateLCD("Master Boot", "Starting...");

  WiFi.mode(WIFI_STA);
  Serial.printf("Master MAC: %s\n", WiFi.macAddress().c_str());

  updateLCD("Connecting...", "WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 30) delay(500);
  if (WiFi.status() != WL_CONNECTED) {
    updateLCD("WiFi FAILED", "Halting");
    while (1) delay(1000);
  }
  updateLCD("WiFi OK", WiFi.localIP().toString());
  delay(800);

  if (esp_now_init() != ESP_OK) {
    updateLCD("ESP-NOW", "Init Failed");
    while (1) delay(1000);
  }
  esp_now_register_recv_cb(onDataReceive);

  esp_now_peer_info_t peer = {};
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt  = false;
  for (int i = 0; i < SLAVE_COUNT; i++) {
    memcpy(peer.peer_addr, slaveMACs[i], 6);
    esp_now_add_peer(&peer);
  }

  auth.user.email    = "test@test.com";
  auth.user.password = "123456";
  config.api_key     = API_KEY;
  config.database_url = DATABASE_URL;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  updateLCD("Firebase", "Connecting...");
  delay(1500);

  configTime(19800, 0, "pool.ntp.org");
  updateLCD("Syncing Time", "Please Wait...");
  time_t now = time(nullptr); int t = 0;
  while (now < 100000 && t++ < 30) { delay(500); now = time(nullptr); }

  const char* roomNames[] = { "CR125", "CR126", "LAB31" };
  for (int i = 0; i < 4; i++) {
    RoomState* r = getRoom(roomNames[i]);
    if (r) recoverState(r);
  }

  updateLCD("Master Ready", "Hold btn=Enroll");
  Serial.println("Ready. Hold GPIO0 2s to enter enroll mode.");
}

// ═══════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════
void loop() {
  handleEnrollButton();

  if (enrollMode) {
    handleEnrollScan();
    delay(30);
    return;   // pause slave processing while in enroll
  }

  if (WiFi.status() != WL_CONNECTED) {
    updateLCD("WiFi Lost", "Reconnecting...");
    WiFi.reconnect();
    delay(3000);
    return;
  }

  while (qHead != qTail) {
    ScanPacket pkt; uint8_t mac[6];
    memcpy(&pkt, (void*)&scanQueue[qHead], sizeof(ScanPacket));
    memcpy(mac,  (void*)senderMAC[qHead],  6);
    qHead = (qHead + 1) % QUEUE_SIZE;
    processScan(mac, pkt);
  }

  delay(50);
}
