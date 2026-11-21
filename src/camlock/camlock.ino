/* ==================================================================
   CamLock v2.0 — ตู้เช่ากล้องอัตโนมัติ
   ESP32-WROOM-32E (แผง CAMERA LOCKER v1.0)  +  ThingsBoard Cloud (MQTT)
   วิชา CPE23367 · มหาวิทยาลัยศรีปทุม

   ระบบทั้งหมดอยู่ในบอร์ดนี้: สมาชิก กล้อง การเช่า การชำระเงิน บันทึกการทำงาน
   เว็บแอป (GitHub Pages) ส่งคำสั่งผ่าน ThingsBoard RPC มาที่บอร์ด
   บอร์ดตอบกลับทาง RPC และส่งสถานะตู้ขึ้น telemetry

   ขาที่ใช้ (ตรงกับแผง CAMERA LOCKER v1.0)
     GPIO26  RELAY_DRV  ออก  สั่งรีเลย์  (HIGH = กลอนเปิด)
     GPIO27  DOOR_GPIO  เข้า reed switch (LOW = ประตูปิด)
     GPIO25  STATUS     ออก  ไฟสถานะ

   ไลบรารี (Arduino IDE > Library Manager)
     - PubSubClient  (Nick O'Leary)
     - ArduinoJson   (Benoit Blanchon) รุ่น 6 หรือ 7 ก็ได้
   บอร์ด: esp32 by Espressif (core 2.x หรือ 3.x) > ESP32 Dev Module

   บัญชีผู้ดูแลเริ่มต้น: admin / 1234  (เปลี่ยนทันทีในหน้าตั้งค่าของแอป)
   ================================================================== */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_arduino_version.h>
#include <time.h>
#include <sys/time.h>

/* ---------- ตั้งค่าตรงนี้ ---------- */
#define WIFI_SSID         "ESP_CAMLOCK"
#define WIFI_PASS         "0649682424"
#define TB_HOST           "thingsboard.cloud"     // หรือ IP ของเซิร์ฟเวอร์ที่ติดตั้งเอง
#define TB_PORT           1883
#define TB_TOKEN          "MTyY8KeuoPiXMB24eeWB"  // Access token ของอุปกรณ์ใน ThingsBoard
#define LOCKER_ID         "A"

// ยังไม่ได้ติด reed switch ให้ตั้งเป็น 0: ระบบจะถือว่าประตูเปิดตอนปลดล็อก
// และปิด/ล็อกเองหลัง AUTO_CLOSE_MS (ผู้ใช้ต้องปิดประตูภายในเวลานี้)
// ติด reed switch ที่ J4 แล้วค่อยเปลี่ยนเป็น 1
#define HAS_REED_SWITCH   0
#define AUTO_CLOSE_MS     10000
/* ---------------------------------- */

#define FW_VERSION        "2.1.0"
#define ADMIN_SID         "admin"
#define ADMIN_DEFAULT_PW  "1234"
#define TZ_INFO           "ICT-7"

#define PIN_RELAY         26
#define PIN_DOOR          27
#define PIN_STATUS_LED    25
#define RELAY_ON          HIGH
#define RELAY_OFF         LOW
#define DOOR_CLOSED_LEVEL LOW

#define MAX_USERS         30
#define MAX_CAMS          8
#define MAX_RENTALS       40
#define MAX_SESSIONS      10
#define MAX_LOG           50
#define LOG_LEN           96
#define MIN_PULSE_GAP_MS  2000
#define MAX_UNLOCK_MS     8000      // จ่ายไฟกลอนนานสุด กันคอยล์ร้อน
#define FAIL_LIMIT        5
#define LOCKOUT_MS        60000UL
#define SESSION_TTL_MS    (12UL * 3600UL * 1000UL)
#define UNPAID_CANCEL_SEC (30 * 60)
#define RETURN_WAIT_MS    90000UL
#define LATE_GRACE_SEC    (10 * 60)
#define WDT_TIMEOUT_S     15
#define TELEMETRY_MS      10000UL
#define MQTT_BUFFER       24576     // คำตอบ RPC ใหญ่สุด (รายการผู้ดูแล) ต้องพอดีบัฟเฟอร์นี้

// ---------- โครงสร้างข้อมูล (ต้องอยู่ก่อนฟังก์ชันแรก เพราะ Arduino IDE สร้าง prototype ไว้ตรงนั้น) ----------
enum UserFlag   : uint8_t { UF_USED = 1, UF_ADMIN = 2, UF_DISABLED = 4 };
enum CamStatus  : uint8_t { CAM_AVAILABLE = 0, CAM_RENTED = 1, CAM_MAINT = 2 };
enum RentStatus : uint8_t { RS_EMPTY = 0, RS_AWAIT = 1, RS_VERIFY = 2, RS_READY = 3, RS_ACTIVE = 4, RS_RETURNED = 5, RS_CANCELLED = 6 };
enum RentFlag   : uint8_t { RF_REFUNDED = 1, RF_PAY_NOTIFIED = 2 };

struct User {
  char     sid[16];
  char     name[48];
  char     phone[16];
  uint8_t  salt[8];
  uint8_t  hash[32];
  uint8_t  flags;
  uint32_t created;
};
struct Camera {
  uint8_t  used;
  uint8_t  status;
  char     name[40];
  char     lens[40];
  char     specs[72];     // คั่นด้วย |
  uint16_t price;         // บาท/ชม.
  uint16_t deposit;
};
struct Rental {
  uint16_t id;
  uint8_t  user;
  uint8_t  cam;
  uint8_t  status;
  uint8_t  flags;
  uint16_t hours;
  uint16_t price;
  uint16_t deposit;
  uint32_t created, pickedAt, dueAt, returnedAt;
  uint32_t amount;        // ยอดที่ต้องจ่ายทั้งหมด (ค่าเช่า + มัดจำ + ต่อเวลา + ค่าปรับ)
  uint32_t paid;          // ยอดที่ผู้ดูแลยืนยันว่าได้รับแล้ว
  uint32_t lateFee;
};
struct Session { char token[33]; int8_t user; uint32_t lastMs; };
struct LogEntry { uint32_t epoch, ms; char msg[LOG_LEN]; };

WiFiClient   net;
PubSubClient mqtt(net);

// พารามิเตอร์ของ RPC ที่กำลังทำงาน
#if ARDUINOJSON_VERSION_MAJOR >= 7
JsonDocument reqDoc;
#else
DynamicJsonDocument reqDoc(4096);
#endif
JsonVariantConst REQ;

String arg(const char *name) {
  JsonVariantConst v = REQ[name];
  if (v.isNull()) return "";
  if (v.is<const char *>()) return String(v.as<const char *>());
  if (v.is<bool>()) return v.as<bool>() ? "1" : "0";
  String s; serializeJson(v, s);      // ตัวเลข -> ข้อความ
  return s;
}
bool hasArg(const char *name) { return !REQ[name].isNull(); }


// ============================================================
//  SHA-256 (ใช้เก็บรหัสผ่านแบบเข้ารหัส ไม่เก็บตัวจริง)
// ============================================================
static const uint32_t K256[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256Block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
    uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],k=h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = k + (ROR(e,6) ^ ROR(e,11) ^ ROR(e,25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    uint32_t t2 = (ROR(a,2) ^ ROR(a,13) ^ ROR(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
    k=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
  }
  h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=k;
}
void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  uint8_t buf[64]; size_t i = 0;
  for (; i + 64 <= len; i += 64) sha256Block(h, data + i);
  size_t r = len - i; memcpy(buf, data + i, r); buf[r++] = 0x80;
  if (r > 56) { memset(buf + r, 0, 64 - r); sha256Block(h, buf); r = 0; }
  memset(buf + r, 0, 56 - r);
  uint64_t bits = (uint64_t)len * 8;
  for (int j = 0; j < 8; j++) buf[63 - j] = bits >> (j * 8);
  sha256Block(h, buf);
  for (int j = 0; j < 8; j++) { out[j*4] = h[j] >> 24; out[j*4+1] = h[j] >> 16; out[j*4+2] = h[j] >> 8; out[j*4+3] = h[j]; }
}

// ============================================================
//  โครงสร้างข้อมูล (เก็บถาวรใน flash)
// ============================================================


// ---------------- ตัวแปรระบบ ----------------
Preferences prefs;

User     users[MAX_USERS];
Camera   cams[MAX_CAMS];
Rental   rentals[MAX_RENTALS];
Session  sessions[MAX_SESSIONS];
LogEntry logs[MAX_LOG];
uint8_t  logHead = 0, logCount = 0;
uint16_t nextRentalId = 1;
uint32_t totalUnlocks = 0;

// การตั้งค่า
String   lockerName, lockerLoc, lockerNote, promptpay;
bool     requireApprove = true;
uint16_t unlockMs = 3000, alarmSec = 60;

// สถานะฮาร์ดแวร์
bool     relayActive = false;
uint32_t relayOffAt = 0, lastPulseAt = 0, lastUnlockCmdAt = 0;
bool     doorClosed = true, doorRawLast = true, doorAlarm = false;
uint32_t doorChangedAt = 0, doorOpenedAt = 0;

// การคืนของที่กำลังดำเนินอยู่
int      returningIdx = -1;
uint32_t returningAt = 0;
bool     returningOpened = false;

uint8_t  loginFails = 0;
uint32_t loginLockUntil = 0;
uint32_t lastHousekeep = 0;
bool     telemetryDirty = true, attributesDirty = true;

// ============================================================
//  ยูทิลิตี้
// ============================================================
uint32_t nowEpoch() { time_t t = time(nullptr); return (t > 1700000000) ? (uint32_t)t : 0; }
bool timeSynced() { return nowEpoch() != 0; }

String jsonEscape(const char *s) {
  String o; o.reserve(strlen(s) + 4);
  for (const char *p = s; *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}
String jstr(const String &s) { return "\"" + jsonEscape(s.c_str()) + "\""; }
String jstr(const char *s)   { return "\"" + jsonEscape(s) + "\""; }
const char *jbool(bool b)    { return b ? "true" : "false"; }

// คัดลอกสตริงโดยไม่ตัดกลางตัวอักษรไทย (UTF-8)
void copyStr(char *dst, size_t n, const String &src) {
  strncpy(dst, src.c_str(), n - 1);
  dst[n - 1] = 0;
  size_t len = strlen(dst), i = 0, ok = 0;
  while (i < len) {
    uint8_t c = dst[i];
    size_t w = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
    if (i + w > len) break;
    i += w; ok = i;
  }
  dst[ok] = 0;
}

void addLog(const String &msg) {
  LogEntry &e = logs[logHead];
  e.epoch = nowEpoch(); e.ms = millis();
  copyStr(e.msg, LOG_LEN, msg);
  logHead = (logHead + 1) % MAX_LOG;
  if (logCount < MAX_LOG) logCount++;
  Serial.printf("[LOG] %s\n", msg.c_str());
}

String randomHex(int bytes) {
  String s; char b[3];
  for (int i = 0; i < bytes; i++) { sprintf(b, "%02x", (unsigned)(esp_random() & 0xFF)); s += b; }
  return s;
}

void hashPassword(const String &pw, const uint8_t salt[8], uint8_t out[32]) {
  uint8_t buf[8 + 72];
  size_t n = pw.length(); if (n > 72) n = 72;
  memcpy(buf, salt, 8); memcpy(buf + 8, pw.c_str(), n);
  sha256(buf, 8 + n, out);
  for (int i = 0; i < 999; i++) sha256(out, 32, out);   // ทำซ้ำให้เดายากขึ้น
}
void setPassword(User &u, const String &pw) {
  for (int i = 0; i < 8; i++) u.salt[i] = esp_random() & 0xFF;
  hashPassword(pw, u.salt, u.hash);
}
bool checkPassword(const User &u, const String &pw) {
  uint8_t h[32]; hashPassword(pw, u.salt, h);
  uint8_t d = 0; for (int i = 0; i < 32; i++) d |= h[i] ^ u.hash[i];
  return d == 0;
}

// ============================================================
//  บันทึก / โหลด
// ============================================================
void saveUsers()   { prefs.begin("camlock", false); prefs.putBytes("users", users, sizeof(users)); prefs.end(); }
void saveCams()    { prefs.begin("camlock", false); prefs.putBytes("cams", cams, sizeof(cams)); prefs.end(); }
void saveRentals() { prefs.begin("camlock", false); prefs.putBytes("rent", rentals, sizeof(rentals)); prefs.putUShort("nextRid", nextRentalId); prefs.end(); }
void saveSettings() {
  prefs.begin("camlock", false);
  prefs.putString("lname", lockerName); prefs.putString("lloc", lockerLoc); prefs.putString("lnote", lockerNote);
  prefs.putString("pp", promptpay);    prefs.putBool("approve", requireApprove);
  prefs.putUShort("unlockMs", unlockMs); prefs.putUShort("alarmSec", alarmSec);
  prefs.end();
}

void seedCamera(int i, const char *n, const char *l, const char *s, uint16_t p, uint16_t d) {
  Camera &c = cams[i]; c.used = 1; c.status = CAM_AVAILABLE;
  copyStr(c.name, sizeof(c.name), n); copyStr(c.lens, sizeof(c.lens), l); copyStr(c.specs, sizeof(c.specs), s);
  c.price = p; c.deposit = d;
}

void loadAll() {
  memset(users, 0, sizeof(users)); memset(cams, 0, sizeof(cams)); memset(rentals, 0, sizeof(rentals));
  prefs.begin("camlock", true);
  lockerName = prefs.getString("lname", "ตู้ A");
  lockerLoc  = prefs.getString("lloc", "อาคาร 11 ชั้น 1");
  lockerNote = prefs.getString("lnote", "หน้าห้องแล็บ · เปิด 24 ชม.");
  promptpay  = prefs.getString("pp", "");
  requireApprove = prefs.getBool("approve", true);
  unlockMs   = prefs.getUShort("unlockMs", 3000);
  alarmSec   = prefs.getUShort("alarmSec", 60);
  nextRentalId = prefs.getUShort("nextRid", 1);
  totalUnlocks = prefs.getUInt("unlocks", 0);
  bool hasUsers = prefs.getBytesLength("users") == sizeof(users);
  bool hasCams  = prefs.getBytesLength("cams") == sizeof(cams);
  if (hasUsers) prefs.getBytes("users", users, sizeof(users));
  if (hasCams)  prefs.getBytes("cams", cams, sizeof(cams));
  if (prefs.getBytesLength("rent") == sizeof(rentals)) prefs.getBytes("rent", rentals, sizeof(rentals));
  prefs.end();
  if (unlockMs < 500 || unlockMs > MAX_UNLOCK_MS) unlockMs = 3000;
  if (alarmSec < 10) alarmSec = 60;

  if (!hasUsers) {   // ครั้งแรก: สร้างบัญชีผู้ดูแล
    User &a = users[0];
    copyStr(a.sid, sizeof(a.sid), ADMIN_SID); copyStr(a.name, sizeof(a.name), "ผู้ดูแลระบบ");
    a.flags = UF_USED | UF_ADMIN; setPassword(a, ADMIN_DEFAULT_PW);
    saveUsers();
  }
  if (!hasCams) {    // ครั้งแรก: ใส่กล้องตัวอย่าง แก้/ลบได้ในหน้าผู้ดูแล
    seedCamera(0, "Canon EOS R50", "RF-S 18-45mm", "APS-C 24.2MP|4K 30p|375 g", 60, 3000);
    seedCamera(1, "Sony ZV-E10", "E 16-50mm", "APS-C 24.2MP|4K 24p|343 g", 55, 3000);
    seedCamera(2, "Fujifilm X-T30 II", "XC 15-45mm", "APS-C 26.1MP|ฟิล์ม 18 แบบ|378 g", 70, 3500);
    seedCamera(3, "GoPro HERO12", "ที่จับกันน้ำ", "5.3K 60p|กันน้ำ 10 ม.|154 g", 40, 1500);
    saveCams();
  }
}

bool defaultAdminPassword() {
  for (auto &u : users) if ((u.flags & UF_ADMIN) && strcmp(u.sid, ADMIN_SID) == 0) return checkPassword(u, ADMIN_DEFAULT_PW);
  return false;
}

// ============================================================
//  กลอนและประตู
// ============================================================
// สถานะประตู: อ่านจาก reed switch หรือจำลองจากเวลาปลดล็อก
uint32_t virtualOpenUntil = 0;
bool readDoorClosed() {
#if HAS_REED_SWITCH
  return digitalRead(PIN_DOOR) == DOOR_CLOSED_LEVEL;
#else
  return virtualOpenUntil == 0 || (int32_t)(millis() - virtualOpenUntil) >= 0;
#endif
}

String unlockDoor(const String &who) {
  uint32_t now = millis();
  if (relayActive) return "กำลังปลดล็อกอยู่แล้ว";
  if (lastPulseAt && now - lastPulseAt < MIN_PULSE_GAP_MS) return "รอสักครู่แล้วลองใหม่";
  digitalWrite(PIN_RELAY, RELAY_ON);
  relayActive = true; relayOffAt = now + unlockMs; lastPulseAt = now; lastUnlockCmdAt = now;
  virtualOpenUntil = now + AUTO_CLOSE_MS;
  totalUnlocks++;
  prefs.begin("camlock", false); prefs.putUInt("unlocks", totalUnlocks); prefs.end();
  addLog("ปลดล็อก โดย " + who);
  telemetryDirty = true;
  return "";
}

void finishReturn() {
  Rental &r = rentals[returningIdx];
  uint32_t ep = nowEpoch();
  r.status = RS_RETURNED; r.returnedAt = ep;
  if (ep && r.dueAt && ep > r.dueAt + LATE_GRACE_SEC) {
    uint32_t lateH = (ep - r.dueAt + 3599) / 3600;
    r.lateFee = lateH * r.price;
    r.amount += r.lateFee;
  }
  if (r.cam < MAX_CAMS && cams[r.cam].used && cams[r.cam].status == CAM_RENTED) { cams[r.cam].status = CAM_AVAILABLE; saveCams(); }
  saveRentals();
  addLog(String("คืนกล้องสำเร็จ #") + r.id + (r.lateFee ? " (คืนช้า ค่าปรับ " + String(r.lateFee) + " บาท)" : ""));
  returningIdx = -1;
}

void updateRelay() {
  if (relayActive && (int32_t)(millis() - relayOffAt) >= 0) { digitalWrite(PIN_RELAY, RELAY_OFF); relayActive = false; telemetryDirty = true; }
}

void updateDoor() {
  bool raw = readDoorClosed();
  uint32_t now = millis();
  if (raw != doorRawLast) { doorRawLast = raw; doorChangedAt = now; }
  if (raw != doorClosed && now - doorChangedAt > 60) {
    doorClosed = raw;
    telemetryDirty = true;
    if (!doorClosed) {
      doorOpenedAt = now;
      bool authorized = lastUnlockCmdAt && (now - lastUnlockCmdAt < 15000);
      addLog(authorized ? "ประตูเปิด" : "⚠ ประตูถูกเปิดโดยไม่มีคำสั่งปลดล็อก");
      if (returningIdx >= 0) returningOpened = true;
    } else {
      addLog("ประตูปิด (เปิดไป " + String((now - doorOpenedAt) / 1000) + " วินาที)");
      doorAlarm = false;
      if (returningIdx >= 0 && returningOpened) finishReturn();
    }
  }
  if (!doorClosed && !doorAlarm && now - doorOpenedAt > (uint32_t)alarmSec * 1000UL) {
    doorAlarm = true; telemetryDirty = true;
    addLog("⚠ ประตูเปิดค้างเกิน " + String(alarmSec) + " วินาที");
  }
  if (returningIdx >= 0 && !returningOpened && now - returningAt > RETURN_WAIT_MS) {
    addLog("ไม่ได้เปิดประตู ยกเลิกการคืน #" + String(rentals[returningIdx].id));
    returningIdx = -1;
  }
}

void updateStatusLed() {
  uint32_t t = millis(); bool on;
  if (relayActive) on = true;
  else if (doorAlarm) on = (t / 120) % 2;                       // กะพริบเร็ว = ประตูเปิดค้าง
  else if (mqtt.connected()) on = true;                          // ติดค้าง = ออนไลน์กับ ThingsBoard
  else if (WiFi.status() == WL_CONNECTED) on = (t / 700) % 2;    // กะพริบช้า = มี WiFi แต่ยังต่อ ThingsBoard ไม่ได้
  else on = (t / 1500) % 10 == 0;                                // แวบ ๆ = ไม่มี WiFi
  digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
}

// งานเบื้องหลังทุก 10 วินาที: ยกเลิกการจองที่ไม่จ่ายเงิน
void housekeeping() {
  uint32_t ep = nowEpoch(); if (!ep) return;
  bool changed = false, camChanged = false;
  for (auto &r : rentals) {
    if (r.status == RS_AWAIT && r.created && ep > r.created + UNPAID_CANCEL_SEC) {
      r.status = RS_CANCELLED; changed = true;
      if (r.cam < MAX_CAMS && cams[r.cam].status == CAM_RENTED) { cams[r.cam].status = CAM_AVAILABLE; camChanged = true; }
      addLog("ยกเลิกอัตโนมัติ #" + String(r.id) + " (ไม่ชำระเงินใน 30 นาที)");
    }
  }
  if (changed) { saveRentals(); telemetryDirty = true; }
  if (camChanged) saveCams();
}

// ============================================================
//  เซสชันผู้ใช้
// ============================================================
int currentUser() {
  String t = arg("t");
  if (t.length() != 32) return -1;
  for (auto &s : sessions) {
    if (s.user >= 0 && t == s.token) {
      if (millis() - s.lastMs > SESSION_TTL_MS) { s.user = -1; return -1; }
      User &u = users[s.user];
      if (!(u.flags & UF_USED) || (u.flags & UF_DISABLED)) { s.user = -1; return -1; }
      s.lastMs = millis();
      return s.user;
    }
  }
  return -1;
}
String newSession(int user) {
  int slot = 0; uint32_t oldest = UINT32_MAX;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].user < 0) { slot = i; break; }
    if (sessions[i].lastMs < oldest) { oldest = sessions[i].lastMs; slot = i; }
  }
  String tok = randomHex(16);
  copyStr(sessions[slot].token, sizeof(sessions[slot].token), tok);
  sessions[slot].user = user; sessions[slot].lastMs = millis();
  return tok;
}
void dropSessionsOf(int user) { for (auto &s : sessions) if (s.user == user) s.user = -1; }

// ============================================================
//  ตัวช่วยส่งคำตอบ
// ============================================================
// คำตอบของ RPC ที่กำลังทำงาน (ส่งกลับไป ThingsBoard หลัง handler จบ)
String RESP;
void sendJson(int code, const String &body) { (void)code; RESP = body; }
void sendErr(int code, const String &msg) { sendJson(code, "{\"ok\":false,\"code\":" + String(code) + ",\"msg\":" + jstr(msg) + "}"); }
void sendOk(const String &msg) { sendJson(200, "{\"ok\":true,\"msg\":" + jstr(msg) + "}"); }

int requireUser() {
  int u = currentUser();
  if (u < 0) sendErr(401, "กรุณาเข้าสู่ระบบใหม่");
  return u;
}
int requireAdmin() {
  int u = currentUser();
  if (u < 0) { sendErr(401, "กรุณาเข้าสู่ระบบใหม่"); return -1; }
  if (!(users[u].flags & UF_ADMIN)) { sendErr(403, "เฉพาะผู้ดูแล"); return -1; }
  return u;
}

// ถ้าตู้ยังไม่รู้เวลา (ไม่มีเน็ต) ให้ยืมเวลาจากมือถือที่เปิดแอป
void syncTimeFromClient() {
  if (timeSynced() || !hasArg("now")) return;
  long long c = atoll(arg("now").c_str());
  if (c > 1700000000LL && c < 4000000000LL) {
    struct timeval tv = { (time_t)c, 0 };
    settimeofday(&tv, nullptr);
    addLog("ตั้งเวลาตู้จากมือถือ");
  }
}

const char *rentStatusName(uint8_t s) {
  switch (s) {
    case RS_AWAIT: return "await"; case RS_VERIFY: return "verify"; case RS_READY: return "ready";
    case RS_ACTIVE: return "active"; case RS_RETURNED: return "returned"; case RS_CANCELLED: return "cancelled";
    default: return "empty";
  }
}
const char *camStatusName(uint8_t s) { return s == CAM_RENTED ? "rented" : s == CAM_MAINT ? "maint" : "available"; }

String userJson(int i) {
  User &u = users[i];
  int count = 0, late = 0; uint32_t hrs = 0;
  for (auto &r : rentals) if (r.status != RS_EMPTY && r.user == i) {
    if (r.status == RS_RETURNED || r.status == RS_ACTIVE) { count++; hrs += r.hours; }
    if (r.lateFee) late++;
  }
  return "{\"idx\":" + String(i) + ",\"sid\":" + jstr(u.sid) + ",\"name\":" + jstr(u.name) + ",\"phone\":" + jstr(u.phone) +
         ",\"admin\":" + jbool(u.flags & UF_ADMIN) + ",\"disabled\":" + jbool(u.flags & UF_DISABLED) +
         ",\"created\":" + String(u.created) + ",\"rentals\":" + String(count) + ",\"hours\":" + String(hrs) + ",\"late\":" + String(late) + "}";
}
String camJson(int i) {
  Camera &c = cams[i];
  return "{\"id\":" + String(i) + ",\"name\":" + jstr(c.name) + ",\"lens\":" + jstr(c.lens) + ",\"specs\":" + jstr(c.specs) +
         ",\"price\":" + String(c.price) + ",\"deposit\":" + String(c.deposit) + ",\"status\":\"" + camStatusName(c.status) + "\"}";
}
String rentalJson(int i) {
  Rental &r = rentals[i];
  bool validCam = r.cam < MAX_CAMS && cams[r.cam].used;
  bool validUser = r.user < MAX_USERS && (users[r.user].flags & UF_USED);
  return "{\"id\":" + String(r.id) + ",\"cam\":" + String(r.cam) + ",\"camName\":" + jstr(validCam ? cams[r.cam].name : "(ลบแล้ว)") +
         ",\"user\":" + String(r.user) + ",\"sid\":" + jstr(validUser ? users[r.user].sid : "-") + ",\"uname\":" + jstr(validUser ? users[r.user].name : "(ลบแล้ว)") +
         ",\"status\":\"" + rentStatusName(r.status) + "\",\"hours\":" + String(r.hours) + ",\"price\":" + String(r.price) +
         ",\"deposit\":" + String(r.deposit) + ",\"created\":" + String(r.created) + ",\"picked\":" + String(r.pickedAt) +
         ",\"due\":" + String(r.dueAt) + ",\"returned\":" + String(r.returnedAt) + ",\"amount\":" + String(r.amount) +
         ",\"paid\":" + String(r.paid) + ",\"lateFee\":" + String(r.lateFee) + ",\"refunded\":" + jbool(r.flags & RF_REFUNDED) +
         ",\"notified\":" + jbool(r.flags & RF_PAY_NOTIFIED) + ",\"returning\":" + jbool(returningIdx == i) + "}";
}

int findRental(uint32_t id) {
  for (int i = 0; i < MAX_RENTALS; i++) if (rentals[i].status != RS_EMPTY && rentals[i].id == id) return i;
  return -1;
}
int openRentalOf(int user) {
  for (int i = 0; i < MAX_RENTALS; i++) {
    Rental &r = rentals[i];
    if (r.user == user && (r.status == RS_AWAIT || r.status == RS_VERIFY || r.status == RS_READY || r.status == RS_ACTIVE)) return i;
  }
  return -1;
}

// ============================================================
//  API สาธารณะ
// ============================================================
void handleStatus() {
  syncTimeFromClient();
  int avail = 0, total = 0;
  for (auto &c : cams) if (c.used) { total++; if (c.status == CAM_AVAILABLE) avail++; }
  String j = "{\"fw\":\"" FW_VERSION "\"";
  j += ",\"locker\":{\"name\":" + jstr(lockerName) + ",\"loc\":" + jstr(lockerLoc) + ",\"note\":" + jstr(lockerNote) + "}";
  j += ",\"door\":\"" + String(doorClosed ? "closed" : "open") + "\"";
  j += ",\"unlocking\":" + String(jbool(relayActive));
  j += ",\"alarm\":" + String(jbool(doorAlarm));
  j += ",\"doorOpenSec\":" + String(doorClosed ? 0 : (millis() - doorOpenedAt) / 1000);
  j += ",\"available\":" + String(avail) + ",\"cams\":" + String(total);
  j += ",\"ssid\":" + jstr(WiFi.SSID());
  j += ",\"ip\":" + jstr(WiFi.localIP().toString());
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"lockerId\":\"" LOCKER_ID "\"";
  j += ",\"epoch\":" + String(nowEpoch());
  j += ",\"uptime\":" + String(millis() / 1000);
  j += ",\"heap\":" + String(ESP.getFreeHeap());
  j += ",\"unlocks\":" + String(totalUnlocks);
  j += ",\"promptpay\":" + jstr(promptpay);
  j += ",\"approve\":" + String(jbool(requireApprove));
  j += ",\"unlockMs\":" + String(unlockMs);
  j += ",\"returning\":" + String(jbool(returningIdx >= 0));
  j += "}";
  sendJson(200, j);
}

bool validSid(const String &s) {
  if (s.length() < 3 || s.length() > 15) return false;
  for (unsigned i = 0; i < s.length(); i++) { char c = s[i]; if (!isalnum((unsigned char)c) && c != '-' && c != '_') return false; }
  return true;
}

void handleRegister() {
  syncTimeFromClient();
  String sid = arg("sid"); sid.trim(); sid.toLowerCase();
  String name = arg("name"); name.trim();
  String phone = arg("phone"); phone.trim();
  String pw = arg("pass");
  if (!validSid(sid)) { sendErr(400, "รหัสนักศึกษาต้องเป็นตัวเลข/อักษรอังกฤษ 3–15 ตัว"); return; }
  if (name.length() < 2) { sendErr(400, "กรุณาใส่ชื่อ-นามสกุล"); return; }
  if (phone.length() < 9) { sendErr(400, "กรุณาใส่เบอร์โทร"); return; }
  if (pw.length() < 6) { sendErr(400, "รหัสผ่านอย่างน้อย 6 ตัว"); return; }
  int slot = -1;
  for (int i = 0; i < MAX_USERS; i++) {
    if ((users[i].flags & UF_USED) && sid == users[i].sid) { sendErr(409, "รหัสนี้สมัครไว้แล้ว"); return; }
    if (slot < 0 && !(users[i].flags & UF_USED)) slot = i;
  }
  if (slot < 0) { sendErr(409, "สมาชิกเต็มแล้ว ติดต่อผู้ดูแล"); return; }
  User &u = users[slot];
  memset(&u, 0, sizeof(User));
  copyStr(u.sid, sizeof(u.sid), sid); copyStr(u.name, sizeof(u.name), name); copyStr(u.phone, sizeof(u.phone), phone);
  u.flags = UF_USED; u.created = nowEpoch();
  setPassword(u, pw);
  saveUsers();
  addLog("สมัครสมาชิกใหม่: " + sid);
  String tok = newSession(slot);
  sendJson(200, "{\"ok\":true,\"token\":\"" + tok + "\",\"user\":" + userJson(slot) + "}");
}

void handleLogin() {
  syncTimeFromClient();
  uint32_t now = millis();
  if (loginLockUntil && (int32_t)(loginLockUntil - now) > 0) {
    sendErr(429, "ใส่รหัสผิดหลายครั้ง รอ " + String((loginLockUntil - now) / 1000 + 1) + " วินาที"); return;
  }
  String sid = arg("sid"); sid.trim(); sid.toLowerCase();
  String pw = arg("pass");
  for (int i = 0; i < MAX_USERS; i++) {
    User &u = users[i];
    if ((u.flags & UF_USED) && sid == u.sid) {
      if (!checkPassword(u, pw)) break;
      if (u.flags & UF_DISABLED) { sendErr(403, "บัญชีนี้ถูกระงับ ติดต่อผู้ดูแล"); return; }
      loginFails = 0;
      String tok = newSession(i);
      addLog("เข้าสู่ระบบ: " + sid);
      sendJson(200, "{\"ok\":true,\"token\":\"" + tok + "\",\"user\":" + userJson(i) + "}");
      return;
    }
  }
  loginFails++;
  addLog("เข้าสู่ระบบไม่สำเร็จ: " + sid);
  if (loginFails >= FAIL_LIMIT) { loginLockUntil = now + LOCKOUT_MS; loginFails = 0; addLog("⚠ ล็อกการเข้าสู่ระบบ 60 วินาที"); }
  sendErr(403, "รหัสนักศึกษาหรือรหัสผ่านไม่ถูกต้อง");
}

void handleLogout() {
  String t = arg("t");
  for (auto &s : sessions) if (s.user >= 0 && t == s.token) s.user = -1;
  sendOk("ออกจากระบบแล้ว");
}

// ============================================================
//  API ผู้ใช้
// ============================================================
void handleMe() {
  int u = requireUser(); if (u < 0) return;
  syncTimeFromClient();
  String j = "{\"ok\":true,\"user\":" + userJson(u) + ",\"rentals\":[";
  bool first = true;
  for (int i = 0; i < MAX_RENTALS; i++) if (rentals[i].status != RS_EMPTY && rentals[i].user == u) {
    if (!first) { j += ","; } first = false; j += rentalJson(i);
  }
  j += "],\"cams\":[";
  first = true;
  for (int i = 0; i < MAX_CAMS; i++) if (cams[i].used) { if (!first) j += ","; first = false; j += camJson(i); }
  j += "]}";
  sendJson(200, j);
}

void handleRent() {
  int u = requireUser(); if (u < 0) return;
  if (!timeSynced()) { sendErr(503, "ตู้ยังไม่ได้ตั้งเวลา ลองรีเฟรชหน้าแล้วลองใหม่"); return; }
  if (openRentalOf(u) >= 0) { sendErr(409, "คุณมีรายการเช่าที่ยังไม่จบอยู่แล้ว"); return; }
  long cam = arg("cam").toInt(), hours = arg("hours").toInt();
  if (cam < 0 || cam >= MAX_CAMS || !cams[cam].used) { sendErr(404, "ไม่พบกล้อง"); return; }
  if (cams[cam].status != CAM_AVAILABLE) { sendErr(409, "กล้องตัวนี้ไม่ว่างแล้ว"); return; }
  if (hours < 1 || hours > 168) { sendErr(400, "จำนวนชั่วโมงไม่ถูกต้อง"); return; }

  int slot = -1;
  for (int i = 0; i < MAX_RENTALS; i++) if (rentals[i].status == RS_EMPTY) { slot = i; break; }
  if (slot < 0) {   // เต็ม: ทับรายการที่จบแล้วอันเก่าที่สุด
    for (int i = 0; i < MAX_RENTALS; i++) {
      Rental &r = rentals[i];
      if ((r.status == RS_CANCELLED || (r.status == RS_RETURNED && (r.flags & RF_REFUNDED || r.paid >= r.amount))) &&
          (slot < 0 || r.id < rentals[slot].id)) slot = i;
    }
  }
  if (slot < 0) { sendErr(409, "ระบบเต็ม กรุณาติดต่อผู้ดูแล"); return; }

  Camera &c = cams[cam];
  Rental &r = rentals[slot];
  memset(&r, 0, sizeof(Rental));
  r.id = nextRentalId++; r.user = u; r.cam = cam; r.hours = hours;
  r.price = c.price; r.deposit = c.deposit;
  r.amount = (uint32_t)c.price * hours + c.deposit;
  r.created = nowEpoch();
  r.status = RS_AWAIT;
  c.status = CAM_RENTED;
  saveRentals(); saveCams();
  addLog("จอง #" + String(r.id) + " " + users[u].sid + " → " + c.name + " " + String(hours) + " ชม.");
  sendJson(200, "{\"ok\":true,\"rental\":" + rentalJson(slot) + "}");
}

int ownRental(int u) {
  int i = findRental(arg("rid").toInt());
  if (i < 0 || rentals[i].user != u) { sendErr(404, "ไม่พบรายการ"); return -1; }
  return i;
}

void handlePayNotify() {
  int u = requireUser(); if (u < 0) return;
  int i = ownRental(u); if (i < 0) return;
  Rental &r = rentals[i];
  if (r.paid >= r.amount) { sendOk("ชำระครบแล้ว"); return; }
  r.flags |= RF_PAY_NOTIFIED;
  if (!requireApprove) {
    r.paid = r.amount; r.flags &= ~RF_PAY_NOTIFIED;
    if (r.status == RS_AWAIT || r.status == RS_VERIFY) r.status = RS_READY;
    addLog("ชำระเงิน #" + String(r.id) + " (อนุมัติอัตโนมัติ)");
  } else {
    if (r.status == RS_AWAIT) r.status = RS_VERIFY;
    addLog("แจ้งชำระเงิน #" + String(r.id) + " รอผู้ดูแลตรวจ");
  }
  saveRentals();
  sendJson(200, "{\"ok\":true,\"rental\":" + rentalJson(i) + "}");
}

void handleCancel() {
  int u = requireUser(); if (u < 0) return;
  int i = ownRental(u); if (i < 0) return;
  Rental &r = rentals[i];
  if (r.status != RS_AWAIT && r.status != RS_VERIFY && r.status != RS_READY) { sendErr(409, "ยกเลิกไม่ได้แล้ว"); return; }
  r.status = RS_CANCELLED;
  if (r.cam < MAX_CAMS && cams[r.cam].status == CAM_RENTED) { cams[r.cam].status = CAM_AVAILABLE; saveCams(); }
  saveRentals();
  addLog("ผู้ใช้ยกเลิก #" + String(r.id) + (r.paid ? " (ต้องคืนเงิน " + String(r.paid) + " บาท)" : ""));
  sendOk(r.paid ? "ยกเลิกแล้ว ผู้ดูแลจะคืนเงินให้" : "ยกเลิกแล้ว");
}

// เปิดตู้: รับกล้อง (READY) หรือคืนกล้อง (ACTIVE)
void handleOpen() {
  int u = requireUser(); if (u < 0) return;
  int i = ownRental(u); if (i < 0) return;
  Rental &r = rentals[i];
  if (!timeSynced()) { sendErr(503, "ตู้ยังไม่ได้ตั้งเวลา ลองรีเฟรชหน้าแล้วลองใหม่"); return; }
  String action;
  if (r.status == RS_READY) {
    String err = unlockDoor(String(users[u].sid) + " (รับกล้อง #" + r.id + ")");
    if (err.length()) { sendErr(409, err); return; }
    r.status = RS_ACTIVE; r.pickedAt = nowEpoch(); r.dueAt = r.pickedAt + (uint32_t)r.hours * 3600;
    saveRentals();
    action = "pickup";
  } else if (r.status == RS_ACTIVE) {
    if (returningIdx >= 0 && returningIdx != i) { sendErr(409, "มีคนกำลังคืนของอยู่ รอสักครู่"); return; }
    String err = unlockDoor(String(users[u].sid) + " (คืนกล้อง #" + r.id + ")");
    if (err.length()) { sendErr(409, err); return; }
    returningIdx = i; returningAt = millis(); returningOpened = false;
    action = "return";
  } else if (r.status == RS_AWAIT || r.status == RS_VERIFY) {
    sendErr(402, requireApprove && r.status == RS_VERIFY ? "รอผู้ดูแลยืนยันการชำระเงิน" : "กรุณาชำระเงินก่อน"); return;
  } else { sendErr(409, "รายการนี้จบแล้ว"); return; }
  sendJson(200, "{\"ok\":true,\"action\":\"" + action + "\",\"unlockMs\":" + String(unlockMs) + ",\"rental\":" + rentalJson(i) + "}");
}

void handleExtend() {
  int u = requireUser(); if (u < 0) return;
  int i = ownRental(u); if (i < 0) return;
  Rental &r = rentals[i];
  long h = arg("hours").toInt();
  if (h < 1 || h > 24) { sendErr(400, "ต่อได้ครั้งละ 1–24 ชม."); return; }
  if (r.status != RS_ACTIVE) { sendErr(409, "ต่อเวลาได้เฉพาะตอนกำลังเช่า"); return; }
  if (returningIdx == i) { sendErr(409, "กำลังคืนกล้องอยู่"); return; }
  if (r.hours + h > 336) { sendErr(400, "เช่ารวมได้สูงสุด 14 วัน"); return; }
  r.hours += h; r.dueAt += h * 3600; r.amount += (uint32_t)r.price * h;
  if (!requireApprove) r.paid = r.amount;
  saveRentals();
  addLog("ต่อเวลา #" + String(r.id) + " +" + String(h) + " ชม.");
  sendJson(200, "{\"ok\":true,\"rental\":" + rentalJson(i) + "}");
}

void handlePassword() {
  int u = requireUser(); if (u < 0) return;
  if (!checkPassword(users[u], arg("old"))) { sendErr(403, "รหัสผ่านเดิมไม่ถูกต้อง"); return; }
  String n = arg("new");
  if (n.length() < 4) { sendErr(400, "รหัสผ่านใหม่สั้นเกินไป"); return; }
  setPassword(users[u], n); saveUsers();
  addLog("เปลี่ยนรหัสผ่าน: " + String(users[u].sid));
  sendOk("เปลี่ยนรหัสผ่านแล้ว");
}

// ============================================================
//  API ผู้ดูแล
// ============================================================
void handleAdminOverview() {
  if (requireAdmin() < 0) return;
  String j; j.reserve(24000);
  j = "{\"ok\":true,\"defaultPw\":" + String(jbool(defaultAdminPassword())) + ",\"users\":[";
  bool first = true;
  for (int i = 0; i < MAX_USERS; i++) if (users[i].flags & UF_USED) { if (!first) j += ","; first = false; j += userJson(i); }
  j += "],\"cams\":[";
  first = true;
  for (int i = 0; i < MAX_CAMS; i++) if (cams[i].used) { if (!first) j += ","; first = false; j += camJson(i); }
  j += "],\"rentals\":[";
  first = true;
  for (int i = 0; i < MAX_RENTALS; i++) if (rentals[i].status != RS_EMPTY) { if (!first) j += ","; first = false; j += rentalJson(i); }
  j += "]}";
  sendJson(200, j);
}

void handleAdminUnlock() {
  int a = requireAdmin(); if (a < 0) return;
  String err = unlockDoor("ผู้ดูแล " + String(users[a].sid));
  if (err.length()) sendErr(409, err); else sendOk("ปลดล็อกแล้ว");
}

void handleAdminRental() {
  if (requireAdmin() < 0) return;
  int i = findRental(arg("rid").toInt());
  if (i < 0) { sendErr(404, "ไม่พบรายการ"); return; }
  Rental &r = rentals[i];
  String act = arg("action");
  bool freeCam = false;
  if (act == "confirm") {             // ยืนยันได้รับเงินครบ
    r.paid = r.amount; r.flags &= ~RF_PAY_NOTIFIED;
    if (r.status == RS_AWAIT || r.status == RS_VERIFY) r.status = RS_READY;
  } else if (act == "reject") {       // ยังไม่ได้รับเงิน
    r.flags &= ~RF_PAY_NOTIFIED;
    if (r.status == RS_VERIFY) { r.status = RS_AWAIT; r.created = nowEpoch(); }
  } else if (act == "returned") {     // ผู้ดูแลรับคืนเอง
    if (returningIdx == i) returningIdx = -1;
    if (r.status == RS_ACTIVE) { r.status = RS_RETURNED; r.returnedAt = nowEpoch(); freeCam = true; }
  } else if (act == "cancel") {
    if (r.status == RS_AWAIT || r.status == RS_VERIFY || r.status == RS_READY) { r.status = RS_CANCELLED; freeCam = true; }
    else { sendErr(409, "ยกเลิกได้เฉพาะรายการที่ยังไม่รับกล้อง"); return; }
  } else if (act == "refund") {       // คืนมัดจำ (หักยอดค้าง/ค่าปรับแล้ว) = ปิดบัญชีรายการนี้
    if (r.status != RS_RETURNED) { sendErr(409, "คืนมัดจำได้หลังคืนกล้องแล้ว"); return; }
    r.flags |= RF_REFUNDED; r.paid = r.amount;
  } else if (act == "delete") {
    if (r.status == RS_ACTIVE || r.status == RS_READY) { sendErr(409, "ลบไม่ได้ รายการยังไม่จบ"); return; }
    if (r.status == RS_AWAIT || r.status == RS_VERIFY) freeCam = true;
    uint16_t id = r.id; uint8_t cam = r.cam;
    memset(&r, 0, sizeof(Rental));
    if (freeCam && cam < MAX_CAMS && cams[cam].status == CAM_RENTED) { cams[cam].status = CAM_AVAILABLE; saveCams(); }
    saveRentals(); addLog("ผู้ดูแลลบรายการ #" + String(id)); sendOk("ลบแล้ว"); return;
  } else { sendErr(400, "คำสั่งไม่ถูกต้อง"); return; }
  if (freeCam && r.cam < MAX_CAMS && cams[r.cam].status == CAM_RENTED) { cams[r.cam].status = CAM_AVAILABLE; saveCams(); }
  saveRentals();
  addLog("ผู้ดูแล " + act + " #" + String(r.id));
  sendOk("บันทึกแล้ว");
}

void handleAdminCamSave() {
  if (requireAdmin() < 0) return;
  long id = hasArg("id") && arg("id").length() ? arg("id").toInt() : -1;
  String name = arg("name"); name.trim();
  if (name.length() == 0) { sendErr(400, "กรุณาใส่ชื่อกล้อง"); return; }
  long price = arg("price").toInt(), dep = arg("deposit").toInt();
  if (price < 0 || price > 60000 || dep < 0 || dep > 60000) { sendErr(400, "ราคาไม่ถูกต้อง"); return; }
  if (id < 0) { for (int i = 0; i < MAX_CAMS; i++) if (!cams[i].used) { id = i; break; } }
  if (id < 0 || id >= MAX_CAMS) { sendErr(409, "กล้องเต็มแล้ว (สูงสุด " + String(MAX_CAMS) + " ตัว)"); return; }
  Camera &c = cams[id];
  bool isNew = !c.used;
  c.used = 1;
  copyStr(c.name, sizeof(c.name), name);
  copyStr(c.lens, sizeof(c.lens), arg("lens"));
  copyStr(c.specs, sizeof(c.specs), arg("specs"));
  c.price = price; c.deposit = dep;
  String st = arg("status");
  if (c.status != CAM_RENTED) c.status = (st == "maint") ? CAM_MAINT : CAM_AVAILABLE;
  if (isNew) c.status = (st == "maint") ? CAM_MAINT : CAM_AVAILABLE;
  saveCams();
  addLog(String(isNew ? "เพิ่มกล้อง: " : "แก้ไขกล้อง: ") + c.name);
  sendOk("บันทึกแล้ว");
}

void handleAdminCamDelete() {
  if (requireAdmin() < 0) return;
  long id = arg("id").toInt();
  if (id < 0 || id >= MAX_CAMS || !cams[id].used) { sendErr(404, "ไม่พบกล้อง"); return; }
  if (cams[id].status == CAM_RENTED) { sendErr(409, "กล้องถูกเช่าอยู่ ลบไม่ได้"); return; }
  addLog(String("ลบกล้อง: ") + cams[id].name);
  memset(&cams[id], 0, sizeof(Camera));
  saveCams();
  sendOk("ลบแล้ว");
}

void handleAdminUser() {
  int a = requireAdmin(); if (a < 0) return;
  long idx = arg("idx").toInt();
  if (idx < 0 || idx >= MAX_USERS || !(users[idx].flags & UF_USED)) { sendErr(404, "ไม่พบสมาชิก"); return; }
  User &u = users[idx];
  String act = arg("action");
  if (idx == a && act != "password") { sendErr(409, "จัดการบัญชีตัวเองจากหน้าบัญชี"); return; }
  if (act == "disable") { u.flags |= UF_DISABLED; dropSessionsOf(idx); }
  else if (act == "enable") u.flags &= ~UF_DISABLED;
  else if (act == "admin") u.flags |= UF_ADMIN;
  else if (act == "unadmin") u.flags &= ~UF_ADMIN;
  else if (act == "password") {
    String p = arg("pass");
    if (p.length() < 4) { sendErr(400, "รหัสผ่านใหม่สั้นเกินไป"); return; }
    setPassword(u, p); dropSessionsOf(idx);
  } else if (act == "delete") {
    if (openRentalOf(idx) >= 0) { sendErr(409, "สมาชิกมีรายการเช่าค้างอยู่"); return; }
    dropSessionsOf(idx); addLog(String("ลบสมาชิก: ") + u.sid);
    memset(&u, 0, sizeof(User)); saveUsers(); sendOk("ลบแล้ว"); return;
  } else { sendErr(400, "คำสั่งไม่ถูกต้อง"); return; }
  saveUsers();
  addLog("ผู้ดูแล " + act + " สมาชิก " + String(u.sid));
  sendOk("บันทึกแล้ว");
}

void handleAdminSettings() {
  if (requireAdmin() < 0) return;
  sendJson(200, "{\"ok\":true,\"lockerName\":" + jstr(lockerName) + ",\"lockerLoc\":" + jstr(lockerLoc) +
                ",\"lockerNote\":" + jstr(lockerNote) + ",\"promptpay\":" + jstr(promptpay) +
                ",\"approve\":" + jbool(requireApprove) + ",\"unlockMs\":" + String(unlockMs) +
                ",\"alarmSec\":" + String(alarmSec) + ",\"ssid\":" + jstr(WIFI_SSID) + "}");
}

void handleAdminSettingsSave() {
  if (requireAdmin() < 0) return;
  if (hasArg("unlockMs")) {
    long v = arg("unlockMs").toInt();
    if (v < 500 || v > MAX_UNLOCK_MS) { sendErr(400, "เวลาปลดล็อกต้องอยู่ระหว่าง 0.5–8 วินาที"); return; }
    unlockMs = v;
  }
  if (hasArg("alarmSec")) {
    long v = arg("alarmSec").toInt();
    if (v < 10 || v > 3600) { sendErr(400, "เวลาแจ้งเตือนต้องอยู่ระหว่าง 10–3600 วินาที"); return; }
    alarmSec = v;
  }
  if (hasArg("promptpay")) {
    String p = arg("promptpay"); String d;
    for (unsigned i = 0; i < p.length(); i++) if (isDigit(p[i])) d += p[i];
    if (d.length() && d.length() != 10 && d.length() != 13 && d.length() != 15) { sendErr(400, "พร้อมเพย์ต้องเป็นเบอร์ 10 หลัก หรือเลขบัตร 13 หลัก"); return; }
    promptpay = d;
  }
  if (hasArg("lockerName")) { String s = arg("lockerName"); s.trim(); if (s.length()) lockerName = s; }
  if (hasArg("lockerLoc"))  { lockerLoc = arg("lockerLoc"); lockerLoc.trim(); }
  if (hasArg("lockerNote")) { lockerNote = arg("lockerNote"); lockerNote.trim(); }
  if (hasArg("approve")) requireApprove = arg("approve") == "1";
  saveSettings();
  attributesDirty = true;
  addLog("ผู้ดูแลบันทึกการตั้งค่า");
  sendOk("บันทึกการตั้งค่าแล้ว");
}

void handleAdminLog() {
  if (requireAdmin() < 0) return;
  String j = "{\"ok\":true,\"now\":" + String(millis()) + ",\"items\":[";
  for (int i = 0; i < logCount; i++) {
    int idx = (logHead - 1 - i + MAX_LOG) % MAX_LOG;
    if (i) j += ",";
    j += "{\"e\":" + String(logs[idx].epoch) + ",\"ms\":" + String(logs[idx].ms) + ",\"m\":" + jstr(logs[idx].msg) + "}";
  }
  j += "]}";
  sendJson(200, j);
}


// ============================================================
//  RPC จาก ThingsBoard
//  แอปเรียก  POST /api/plugins/rpc/twoway/{deviceId}  {"method":"login","params":{...}}
//  บอร์ดรับที่ v1/devices/me/rpc/request/{id}  แล้วตอบที่ v1/devices/me/rpc/response/{id}
// ============================================================


// getStatus: คงไว้ให้วิดเจ็ตบนแดชบอร์ด ThingsBoard เดิมใช้ได้
void handleGetStatus() {
  sendJson(200, String("{\"ok\":true,\"doorOpen\":") + jbool(!doorClosed) + ",\"lockActive\":" + jbool(relayActive) + "}");
}

typedef void (*Handler)();
struct Route { const char *method; Handler fn; };
const Route ROUTES[] = {
  {"status", handleStatus},           {"getStatus", handleGetStatus},           {"register", handleRegister},       {"login", handleLogin},
  {"logout", handleLogout},           {"me", handleMe},                   {"rent", handleRent},
  {"pay", handlePayNotify},           {"cancel", handleCancel},           {"open", handleOpen},
  {"extend", handleExtend},           {"password", handlePassword},
  {"adminOverview", handleAdminOverview}, {"adminUnlock", handleAdminUnlock}, {"adminRental", handleAdminRental},
  {"adminCamera", handleAdminCamSave},    {"adminCameraDelete", handleAdminCamDelete}, {"adminUser", handleAdminUser},
  {"adminSettings", handleAdminSettings}, {"adminSettingsSave", handleAdminSettingsSave}, {"adminLog", handleAdminLog},
};

void publishTelemetry() {
  int avail = 0, total = 0;
  for (auto &c : cams) if (c.used) { total++; if (c.status == CAM_AVAILABLE) avail++; }
  String j = "{\"doorOpen\":" + String(jbool(!doorClosed)) + ",\"lockActive\":" + jbool(relayActive) +
             ",\"alarm\":" + jbool(doorAlarm) + ",\"returning\":" + jbool(returningIdx >= 0) +
             ",\"available\":" + String(avail) + ",\"camCount\":" + String(total) +
             ",\"unlockCount\":" + String(totalUnlocks) + ",\"rssi\":" + String(WiFi.RSSI()) +
             ",\"uptime\":" + String(millis() / 1000) + ",\"heap\":" + String(ESP.getFreeHeap()) + "}";
  mqtt.publish("v1/devices/me/telemetry", j.c_str());
  telemetryDirty = false;
}

void publishAttributes() {
  String j = "{\"lockerId\":\"" LOCKER_ID "\",\"firmware\":\"" FW_VERSION "\",\"ip\":" + jstr(WiFi.localIP().toString()) +
             ",\"ssid\":" + jstr(WIFI_SSID) + ",\"lockerName\":" + jstr(lockerName) + ",\"lockerLoc\":" + jstr(lockerLoc) +
             ",\"lockerNote\":" + jstr(lockerNote) + ",\"promptpay\":" + jstr(promptpay) +
             ",\"approve\":" + jbool(requireApprove) + ",\"unlockMs\":" + String(unlockMs) +
             ",\"reedSwitch\":" + jbool(HAS_REED_SWITCH) + ",\"autoCloseSec\":" + String(AUTO_CLOSE_MS / 1000) + "}";
  mqtt.publish("v1/devices/me/attributes", j.c_str());
  attributesDirty = false;
}

void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  static const char *PREFIX = "v1/devices/me/rpc/request/";
  if (strncmp(topic, PREFIX, strlen(PREFIX)) != 0) return;
  String requestId = String(topic + strlen(PREFIX));

  RESP = "";
  reqDoc.clear();
  DeserializationError err = deserializeJson(reqDoc, payload, len);
  JsonVariantConst root = reqDoc.as<JsonVariantConst>();
  const char *method = err ? nullptr : root["method"].as<const char *>();
  if (!method) {
    sendErr(400, "คำสั่งไม่ถูกต้อง");
  } else {
    // ThingsBoard ส่ง params มาได้ทั้งแบบ object และแบบ string ที่เป็น JSON
#if ARDUINOJSON_VERSION_MAJOR >= 7
    static JsonDocument pdoc;
#else
    static DynamicJsonDocument pdoc(2048);
#endif
    REQ = root["params"];
    if (REQ.is<const char *>()) { pdoc.clear(); if (!deserializeJson(pdoc, REQ.as<const char *>())) REQ = pdoc.as<JsonVariantConst>(); }
    bool found = false;
    for (auto &r : ROUTES) if (strcmp(method, r.method) == 0) { r.fn(); found = true; break; }
    if (!found) sendErr(404, String("ไม่รู้จักคำสั่ง ") + method);
  }
  if (RESP.length() == 0) sendErr(500, "ไม่มีคำตอบ");
  String out = "v1/devices/me/rpc/response/" + requestId;
  if (!mqtt.publish(out.c_str(), (const uint8_t *)RESP.c_str(), RESP.length()))
    Serial.printf("[RPC] ส่งคำตอบไม่สำเร็จ (%u ไบต์) เพิ่ม MQTT_BUFFER\n", RESP.length());
  telemetryDirty = true;   // สถานะอาจเปลี่ยน ส่งค่าใหม่ทันที
  RESP = "";
}

// ============================================================
//  WiFi + MQTT (ไม่บล็อก loop เพื่อให้กลอนปิดตรงเวลาเสมอ)
// ============================================================
uint32_t lastWifiTry = 0, lastMqttTry = 0, lastTelemetry = 0;
bool wifiWasUp = false, ntpStarted = false;

void maintainNetwork() {
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasUp) { wifiWasUp = false; addLog("WiFi หลุด"); }
    if (now - lastWifiTry > 15000 || lastWifiTry == 0) {
      lastWifiTry = now;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      Serial.printf("กำลังต่อ WiFi %s ...\n", WIFI_SSID);
    }
    return;
  }
  if (!wifiWasUp) {
    wifiWasUp = true;
    addLog("เชื่อม WiFi " WIFI_SSID " IP " + WiFi.localIP().toString());
    if (!ntpStarted) { configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.navy.mi.th"); ntpStarted = true; }
  }
  if (!mqtt.connected()) {
    if (now - lastMqttTry < 5000 && lastMqttTry != 0) return;
    lastMqttTry = now;
    String clientId = "camlock-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.printf("กำลังต่อ ThingsBoard %s ...\n", TB_HOST);
    esp_task_wdt_reset();
    if (mqtt.connect(clientId.c_str(), TB_TOKEN, NULL)) {      // username = access token
      mqtt.subscribe("v1/devices/me/rpc/request/+");
      addLog("เชื่อม ThingsBoard แล้ว");
      publishAttributes();
      publishTelemetry();
    } else {
      Serial.printf("ต่อ ThingsBoard ไม่ได้ (state %d) เช็ค TB_TOKEN\n", mqtt.state());
    }
    return;
  }
  mqtt.loop();
  if (attributesDirty) publishAttributes();
  if (telemetryDirty || now - lastTelemetry >= TELEMETRY_MS) { lastTelemetry = now; publishTelemetry(); }
}

// ============================================================
//  Watchdog
// ============================================================
void setupWatchdog() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_S * 1000; cfg.idle_core_mask = 0; cfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  pinMode(PIN_RELAY, OUTPUT); digitalWrite(PIN_RELAY, RELAY_OFF);   // ตั้ง LOW ก่อนอย่างอื่น กันกลอนเปิดตอนบูต
  pinMode(PIN_STATUS_LED, OUTPUT); digitalWrite(PIN_STATUS_LED, LOW);
  pinMode(PIN_DOOR, INPUT_PULLUP);                                   // บนแผงมี R5 10k ดึงขึ้นอยู่แล้ว

  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== CamLock v" FW_VERSION " · ตู้ " LOCKER_ID " ===");
  setupWatchdog();
  for (auto &s : sessions) s.user = -1;
  loadAll();

  doorClosed = doorRawLast = readDoorClosed();
  doorOpenedAt = millis();
  addLog(String("เปิดเครื่อง ประตู") + (doorClosed ? "ปิดอยู่" : "เปิดอยู่"));

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("camlock");
  WiFi.setAutoReconnect(true);
  mqtt.setServer(TB_HOST, TB_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(MQTT_BUFFER);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);
}

void loop() {
  esp_task_wdt_reset();
  maintainNetwork();
  updateRelay();
  updateDoor();
  updateStatusLed();
  if (millis() - lastHousekeep > 10000) { lastHousekeep = millis(); housekeeping(); }
  delay(2);
}
