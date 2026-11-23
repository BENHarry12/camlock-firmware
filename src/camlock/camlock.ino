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
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include <esp_arduino_version.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <sys/time.h>

/* ---------- ตั้งค่าตรงนี้ ---------- */
#define WIFI_SSID         "ESP_CAMLOCK"
#define WIFI_PASS         "0649682424"
#define TB_HOST           "thingsboard.cloud"     // หรือ IP ของเซิร์ฟเวอร์ที่ติดตั้งเอง
#define TB_PORT           1883
#define TB_TOKEN          "MTyY8KeuoPiXMB24eeWB"  // Access token ของอุปกรณ์ใน ThingsBoard
#define LOCKER_ID         "A"

// ยังไม่ได้ติด reed switch ให้ตั้งเป็น 0: ระบบถือว่าประตูเปิดตลอดที่กลอนปลดล็อก
// และถือว่าปิดเมื่อกลอนล็อกกลับ (ผู้ใช้กด "ล็อกตู้" ในแอป หรือครบเวลาเปิดค้าง)
// ติด reed switch ที่ J4 แล้วค่อยเปลี่ยนเป็น 1
#define HAS_REED_SWITCH   0

// EasySlip (ตรวจสลิปโอนเงินอัตโนมัติ) — ใส่ API key ที่นี่ หรือใส่ในหน้า ตั้งค่า ของแอป (เก็บในบอร์ด ไม่โชว์ในเว็บ)
// อย่า push key จริงขึ้น GitHub สาธารณะ
#define EASYSLIP_KEY      ""

// บัญชีพร้อมเพย์ที่รับเงิน (ค่าเริ่มต้น แก้ได้ในหน้า ตั้งค่า ของแอป)
#define SHOP_PROMPTPAY    "0649682424"
#define SHOP_PP_NAME      "ภานุวัฒน์ ลิ้มประยูร (Panuwat Limprayoon)"
/* ---------------------------------- */

#define FW_VERSION        "3.1.0"
#define ADMIN_SID         "admin"
#define ADMIN_DEFAULT_PW  "1234"
// บัญชีทดสอบ (ระบบจำลอง): เช่าได้เลย ไม่ต้องยืนยันตัวตน ไม่ต้องชำระเงิน — กลอนเปิดจริง
#define TEST_SID          "testcam"
#define TEST_PW           "testcam"
#define TZ_INFO           "ICT-7"

#define PIN_RELAY         26
#define PIN_DOOR          27
#define PIN_STATUS_LED    25
#define RELAY_ON          HIGH
#define RELAY_OFF         LOW
#define DOOR_CLOSED_LEVEL LOW

#define MAX_USERS         60
#define MAX_CAMS          8
#define MAX_RENTALS       40
#define MAX_SESSIONS      60
#define MAX_LOG           50
#define LOG_LEN           96
#define MIN_PULSE_GAP_MS  2000
#define DEFAULT_HOLD_MS   30000     // กลอนเปิดค้างรอหยิบ/ใส่กล้อง (ผู้ใช้กด "ล็อกตู้" ในแอปให้ล็อกก่อนได้)
#define MAX_UNLOCK_MS     60000     // เปิดค้างได้นานสุด กันคอยล์กลอนร้อน (เช็คสเปกกลอนของคุณ)
#define FAIL_LIMIT        5
#define LOCKOUT_MS        60000UL
#define SESSION_TTL_MS    (12UL * 3600UL * 1000UL)
#define UNPAID_CANCEL_SEC (30 * 60)
#define RETURN_WAIT_MS    90000UL
#define MAX_WTX           40        // ประวัติกระเป๋าเงินล่าสุด
#define WDT_TIMEOUT_S     15
#define TELEMETRY_MS      20000UL   // heartbeat (ค่าเปลี่ยนจะส่งทันที)
#define MQTT_BUFFER       49152     // คำตอบ RPC ใหญ่สุด (รายการผู้ดูแล) ต้องพอดีบัฟเฟอร์นี้

// ---------- โครงสร้างข้อมูล (ต้องอยู่ก่อนฟังก์ชันแรก เพราะ Arduino IDE สร้าง prototype ไว้ตรงนั้น) ----------
enum UserFlag   : uint8_t { UF_USED = 1, UF_ADMIN = 2, UF_DISABLED = 4, UF_TEST = 8 };
enum CamStatus  : uint8_t { CAM_AVAILABLE = 0, CAM_RENTED = 1, CAM_MAINT = 2 };
enum RentStatus : uint8_t { RS_EMPTY = 0, RS_AWAIT = 1, RS_VERIFY = 2, RS_READY = 3, RS_ACTIVE = 4, RS_RETURNED = 5, RS_CANCELLED = 6 };
enum RentFlag   : uint8_t { RF_REFUNDED = 1, RF_PAY_NOTIFIED = 2, RF_PHOTO_PICK = 4, RF_PHOTO_RET = 8, RF_FORFEIT = 16, RF_LOST = 32 };

enum Verify      : uint8_t { VF_NONE = 0, VF_REJECTED = 1, VF_PENDING = 2, VF_OK = 3 };

struct User {
  char     sid[16];        // ชื่อผู้ใช้ (ใช้เข้าสู่ระบบ)
  char     name[48];
  char     phone[16];
  char     studentId[16];  // ไม่บังคับ
  uint8_t  salt[8];
  uint8_t  hash[32];
  uint8_t  flags;
  uint8_t  verify;         // สถานะยืนยันตัวตน
  uint32_t created;
};
struct UserV1 {            // รูปแบบเก่า (v2.0–2.2) ใช้ย้ายข้อมูลอัตโนมัติ
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
struct WalletTx { uint32_t epoch; int32_t amount; uint8_t user; char note[27]; };   // รายการเข้า/ออกของกระเป๋าเงิน

WiFiClient   net;
PubSubClient mqtt(net);

// พารามิเตอร์ของ RPC ที่กำลังทำงาน
#if ARDUINOJSON_VERSION_MAJOR >= 7
JsonDocument reqDoc;
#else
DynamicJsonDocument reqDoc(24576);   // รองรับรูปยืนยันตัวตน ~20KB
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
bool     fsOk = false;          // LittleFS พร้อมใช้

// การตั้งค่า
String   lockerName, lockerLoc, lockerNote, promptpay, ppName, slipKey;
// ผลตรวจสลิปล่าสุดของแต่ละรายการเช่า (0 = ไม่มี, 1 = กำลังตรวจ, 2 = ผ่าน, 3 = ไม่ผ่าน)
uint8_t  slipState[MAX_RENTALS];
String   slipMsg[MAX_RENTALS];
int      slipJob = -1;            // รายการที่รอตรวจ (ทำใน loop ไม่บล็อก RPC)
uint8_t  slipJobKind = 0;         // 0 = ชำระรายการเช่า, 1 = เติมเงินเข้ากระเป๋า
String   slipPayload;
// กระเป๋าเงิน (บาท) ของสมาชิกแต่ละคน เก็บใน NVS แยกจากข้อมูลสมาชิก
uint32_t wallet[MAX_USERS];
uint8_t  topState[MAX_USERS];     // ผลเติมเงินด้วยสลิปล่าสุด (0/1/2/3 เหมือน slipState)
String   topMsg[MAX_USERS];
WalletTx wtx[MAX_WTX];
uint8_t  wtxHead = 0, wtxCount = 0;
// รุ่นของรูปกล้อง / รูปโปรไฟล์ (0 = ไม่มีรูป) ให้แอปรู้ว่าต้องโหลดรูปใหม่ไหม
uint32_t camPh[MAX_CAMS], avPh[MAX_USERS];
String   camUrl[MAX_CAMS];          // ลิงก์รูปกล้องที่ผู้ดูแลใส่ (เก็บ NVS คีย์ cu0..cu7)
// ค่าปรับคืนช้า: นาทีละกี่บาท, ผ่อนผันกี่นาที, เกินกี่ชั่วโมงถือว่าไม่คืน (ยึดมัดจำ)
uint16_t lateFeeMin = 100, lateGraceMin = 0, lostHours = 24;
bool     requireApprove = true;
uint16_t unlockMs = DEFAULT_HOLD_MS, alarmSec = 90;

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

#define EV_QUEUE 8
uint8_t evQueue[EV_QUEUE], evHead = 0, evCount = 0;
uint32_t evSeq = 0;

void addLog(const String &msg) {
  LogEntry &e = logs[logHead];
  e.epoch = nowEpoch(); e.ms = millis();
  copyStr(e.msg, LOG_LEN, msg);
  logHead = (logHead + 1) % MAX_LOG;
  if (logCount < MAX_LOG) logCount++;
  Serial.printf("[LOG] %s\n", msg.c_str());
  // คิวเหตุการณ์ไว้ส่งขึ้น ThingsBoard ทันที (แดชบอร์ดผู้ดูแลเห็นสด)
  if (evCount < EV_QUEUE) { evQueue[(evHead + evCount) % EV_QUEUE] = logHead == 0 ? MAX_LOG - 1 : logHead - 1; evCount++; }
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
// สมาชิกเก็บเป็นไฟล์ใน LittleFS (ใหญ่เกินกว่าจะเขียนซ้ำใน NVS ได้ปลอดภัย)
void saveUsers() {
  if (fsOk) {
    File f = LittleFS.open("/users.tmp", "w");
    if (f) {
      size_t n = f.write((const uint8_t *)users, sizeof(users)); f.close();
      if (n == sizeof(users)) { LittleFS.remove("/users.bin"); LittleFS.rename("/users.tmp", "/users.bin"); return; }
    }
    addLog("⚠ บันทึกสมาชิกลงไฟล์ไม่สำเร็จ");
    return;
  }
  prefs.begin("camlock", false); prefs.putBytes("users", users, sizeof(users)); prefs.end();
}
void saveCams()    { prefs.begin("camlock", false); prefs.putBytes("cams", cams, sizeof(cams)); prefs.end(); }
void saveRentals() { prefs.begin("camlock", false); prefs.putBytes("rent", rentals, sizeof(rentals)); prefs.putUShort("nextRid", nextRentalId); prefs.end(); }
void saveSettings() {
  prefs.begin("camlock", false);
  prefs.putString("lname", lockerName); prefs.putString("lloc", lockerLoc); prefs.putString("lnote", lockerNote);
  prefs.putString("pp", promptpay);    prefs.putBool("approve", requireApprove);
  prefs.putUShort("holdMs", unlockMs); prefs.putUShort("alarmSec", alarmSec);
  prefs.putString("slipKey", slipKey); prefs.putString("ppName", ppName);
  prefs.putUShort("lateMin", lateFeeMin); prefs.putUShort("graceMin", lateGraceMin); prefs.putUShort("lostH", lostHours);
  prefs.end();
}
void saveWallet() {
  prefs.begin("camlock", false);
  prefs.putBytes("wallet", wallet, sizeof(wallet));
  prefs.putBytes("wtx", wtx, sizeof(wtx)); prefs.putUChar("wtxH", wtxHead); prefs.putUChar("wtxC", wtxCount);
  prefs.end();
}
void savePhotoVersions() {
  prefs.begin("camlock", false);
  prefs.putBytes("camPh", camPh, sizeof(camPh)); prefs.putBytes("avPh", avPh, sizeof(avPh));
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
  if (!promptpay.length()) promptpay = SHOP_PROMPTPAY;
  ppName     = prefs.getString("ppName", "");
  if (!ppName.length()) ppName = SHOP_PP_NAME;
  slipKey    = prefs.getString("slipKey", "");
  requireApprove = prefs.getBool("approve", true);
  unlockMs   = prefs.getUShort("holdMs", DEFAULT_HOLD_MS);
  alarmSec   = prefs.getUShort("alarmSec", 90);
  lateFeeMin = prefs.getUShort("lateMin", 100);
  lateGraceMin = prefs.getUShort("graceMin", 0);
  lostHours  = prefs.getUShort("lostH", 24);
  memset(wallet, 0, sizeof(wallet)); memset(wtx, 0, sizeof(wtx)); memset(camPh, 0, sizeof(camPh)); memset(avPh, 0, sizeof(avPh));
  if (prefs.getBytesLength("wallet") == sizeof(wallet)) prefs.getBytes("wallet", wallet, sizeof(wallet));
  if (prefs.getBytesLength("wtx") == sizeof(wtx)) { prefs.getBytes("wtx", wtx, sizeof(wtx)); wtxHead = prefs.getUChar("wtxH", 0) % MAX_WTX; uint8_t c = prefs.getUChar("wtxC", 0); wtxCount = c > MAX_WTX ? MAX_WTX : c; }
  if (prefs.getBytesLength("camPh") == sizeof(camPh)) prefs.getBytes("camPh", camPh, sizeof(camPh));
  if (prefs.getBytesLength("avPh") == sizeof(avPh)) prefs.getBytes("avPh", avPh, sizeof(avPh));
  for (int i = 0; i < MAX_CAMS; i++) camUrl[i] = prefs.getString(("cu" + String(i)).c_str(), "");
  nextRentalId = prefs.getUShort("nextRid", 1);
  totalUnlocks = prefs.getUInt("unlocks", 0);
  // โหลดสมาชิก: รองรับจำนวนช่องเดิมที่น้อยกว่า และรูปแบบเก่า (UserV1) — ไม่ทำข้อมูลหาย
  size_t ulen = prefs.getBytesLength("users");
  bool hasUsers = false, migrated = false, fromNvs = false;
  if (fsOk && LittleFS.exists("/users.bin")) {
    File f = LittleFS.open("/users.bin", "r");
    size_t n = f.size();
    if (n && n % sizeof(User) == 0 && n <= sizeof(users)) { f.read((uint8_t *)users, n); hasUsers = true; migrated = n != sizeof(users); }
    f.close();
  }
  if (hasUsers) { /* โหลดจากไฟล์แล้ว */ }
  else if (ulen && ulen % sizeof(User) == 0 && ulen <= sizeof(users) && ulen != 30 * sizeof(UserV1)) {
    prefs.getBytes("users", users, ulen);
    hasUsers = true; migrated = true; fromNvs = true;
  } else if (ulen && ulen % sizeof(UserV1) == 0 && ulen / sizeof(UserV1) <= MAX_USERS) {
    int n = ulen / sizeof(UserV1);
    UserV1 *old = new UserV1[n];
    prefs.getBytes("users", old, ulen);
    for (int i = 0; i < n; i++) {
      memcpy(users[i].sid, old[i].sid, 16); memcpy(users[i].name, old[i].name, 48); memcpy(users[i].phone, old[i].phone, 16);
      memcpy(users[i].salt, old[i].salt, 8); memcpy(users[i].hash, old[i].hash, 32);
      users[i].flags = old[i].flags; users[i].created = old[i].created;
      users[i].verify = (old[i].flags & UF_USED) ? VF_OK : VF_NONE;   // สมาชิกเดิมถือว่ายืนยันแล้ว
    }
    delete[] old;
    hasUsers = migrated = fromNvs = true;
  }
  bool hasCams  = prefs.getBytesLength("cams") == sizeof(cams);
  if (hasCams)  prefs.getBytes("cams", cams, sizeof(cams));
  if (prefs.getBytesLength("rent") == sizeof(rentals)) prefs.getBytes("rent", rentals, sizeof(rentals));
  prefs.end();
  if (unlockMs < 3000 || unlockMs > MAX_UNLOCK_MS) unlockMs = DEFAULT_HOLD_MS;
  if (alarmSec < 10) alarmSec = 60;

  if (migrated) saveUsers();
  if (fromNvs && fsOk) { prefs.begin("camlock", false); prefs.remove("users"); prefs.end(); }   // ย้ายไปไฟล์แล้ว คืนที่ NVS
  if (!hasUsers) {   // ครั้งแรก: สร้างบัญชีผู้ดูแล
    User &a = users[0];
    copyStr(a.sid, sizeof(a.sid), ADMIN_SID); copyStr(a.name, sizeof(a.name), "ผู้ดูแลระบบ");
    a.flags = UF_USED | UF_ADMIN; a.verify = VF_OK; setPassword(a, ADMIN_DEFAULT_PW);
    saveUsers();
  }
  // บัญชีทดสอบ: สร้างให้ถ้ายังไม่มี (ลบในแอปแล้วรีสตาร์ทบอร์ดจะกลับมา)
  bool hasTest = false;
  for (auto &u : users) if ((u.flags & UF_USED) && strcmp(u.sid, TEST_SID) == 0) { hasTest = true; if (!(u.flags & UF_TEST)) { u.flags |= UF_TEST; saveUsers(); } }
  if (!hasTest) {
    for (auto &t : users) if (!(t.flags & UF_USED)) {
      memset(&t, 0, sizeof(User));
      copyStr(t.sid, sizeof(t.sid), TEST_SID); copyStr(t.name, sizeof(t.name), "บัญชีทดสอบ (ไม่ต้องชำระเงิน)");
      t.flags = UF_USED | UF_TEST; t.verify = VF_OK; setPassword(t, TEST_PW);
      saveUsers(); break;
    }
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
  virtualOpenUntil = relayOffAt;
  totalUnlocks++;
  prefs.begin("camlock", false); prefs.putUInt("unlocks", totalUnlocks); prefs.end();
  addLog("ปลดล็อก โดย " + who);
  telemetryDirty = true;
  return "";
}

// ค่าปรับคืนช้า ณ เวลา ep: นาทีละ lateFeeMin บาท นับตั้งแต่เลยกำหนด (ถ้าเกินช่วงผ่อนผัน)
uint32_t lateFeeAt(const Rental &r, uint32_t ep) {
  if (!ep || !r.dueAt || !lateFeeMin || ep <= r.dueAt + (uint32_t)lateGraceMin * 60) return 0;
  uint32_t mins = (ep - r.dueAt + 59) / 60;
  return mins * (uint32_t)lateFeeMin;
}
// มัดจำที่คืนได้ = ยอดที่จ่ายเกินค่าเช่า/ค่าปรับ · ชำรุด/ไม่คืน/คืนไปแล้ว = 0
uint32_t refundableOf(const Rental &r) {
  if (r.flags & (RF_REFUNDED | RF_FORFEIT | RF_LOST)) return 0;
  long v = (long)r.paid - ((long)r.amount - (long)r.deposit);
  if (v < 0) v = 0;
  if (v > (long)r.deposit) v = r.deposit;
  return v;
}
void applyLateFee(Rental &r, uint32_t ep) {
  uint32_t fee = lateFeeAt(r, ep);
  if (fee) { r.lateFee = fee; r.amount += fee; }
}
// ยอดที่ต้องจ่ายจริง: หลังคืนกล้อง ค่าปรับหักจากมัดจำที่ถืออยู่ก่อน ส่วนที่เกินจึงต้องจ่ายเพิ่ม
uint32_t debtOf(const Rental &r) {
  uint32_t owed = r.amount > r.paid ? r.amount - r.paid : 0;
  if (r.status == RS_RETURNED) owed = owed > r.deposit ? owed - r.deposit : 0;
  return owed;
}
// เพิ่ม/หักเงินในกระเป๋า + บันทึกประวัติ
void walletAdd(int u, long amount, const String &note) {
  if (u < 0 || u >= MAX_USERS) return;
  long v = (long)wallet[u] + amount; if (v < 0) v = 0;
  wallet[u] = v;
  WalletTx &t = wtx[wtxHead];
  t.epoch = nowEpoch(); t.amount = amount; t.user = u; copyStr(t.note, sizeof(t.note), note);
  wtxHead = (wtxHead + 1) % MAX_WTX; if (wtxCount < MAX_WTX) wtxCount++;
  saveWallet();
  addLog(String(amount >= 0 ? "เงินเข้ากระเป๋า " : "หักกระเป๋า ") + users[u].sid + " " + String(amount) + " บาท (" + note + ")");
}

void finishReturn() {
  Rental &r = rentals[returningIdx];
  uint32_t ep = nowEpoch();
  r.status = RS_RETURNED; r.returnedAt = ep;
  applyLateFee(r, ep);
  if (r.cam < MAX_CAMS && cams[r.cam].used && cams[r.cam].status == CAM_RENTED) { cams[r.cam].status = CAM_AVAILABLE; saveCams(); }
  saveRentals();
  addLog(String("คืนกล้องสำเร็จ #") + r.id + (r.lateFee ? " (คืนช้า ค่าปรับ " + String(r.lateFee) + " บาท)" : ""));
  returningIdx = -1;
}

// ล็อกตู้ทันที (ผู้ใช้หยิบ/ใส่กล้องเสร็จแล้วกดในแอป)
void lockNow(const String &who) {
  if (relayActive) { digitalWrite(PIN_RELAY, RELAY_OFF); relayActive = false; telemetryDirty = true; }
  if (virtualOpenUntil && (int32_t)(virtualOpenUntil - millis()) > 0) virtualOpenUntil = millis();
  addLog("ล็อกตู้ โดย " + who);
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
#if HAS_REED_SWITCH
      // มี reed switch: ประตูเปิดแล้ว ไม่ต้องจ่ายไฟกลอนต่อ (ประตูปิดเมื่อไหร่ สลักเด้งล็อกเอง)
      if (relayActive && (int32_t)(relayOffAt - now) > 1000) relayOffAt = now + 1000;
#endif
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
  else if (WiFi.status() != WL_CONNECTED) on = (t / 50) % 2;    // กะพริบรัวมาก (10 ครั้ง/วินาที) = ยังไม่ต่อ WiFi
  else if (doorAlarm) { uint32_t p = t % 1000; on = p < 100 || (p >= 200 && p < 300); }   // กะพริบ 2 ทีแล้วเว้น = ประตูเปิดค้าง
  else if (mqtt.connected()) on = true;                          // ติดค้าง = ออนไลน์กับ ThingsBoard
  else on = (t / 700) % 2;                                       // กะพริบช้า = มี WiFi แต่ยังต่อ ThingsBoard ไม่ได้
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
    // เลยกำหนดคืนนานเกิน lostHours ชั่วโมง = ถือว่าไม่คืนกล้อง ยึดมัดจำ
    if (r.status == RS_ACTIVE && lostHours && r.dueAt && !(r.flags & RF_LOST) && ep > r.dueAt + (uint32_t)lostHours * 3600) {
      r.flags |= RF_LOST; changed = true;
      addLog("⚠ ไม่คืนกล้อง #" + String(r.id) + " เกิน " + String(lostHours) + " ชม. ยึดมัดจำ");
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

const char *verifyName(uint8_t v) { return v == VF_OK ? "ok" : v == VF_PENDING ? "pending" : v == VF_REJECTED ? "rejected" : "none"; }

// ---------- รูปยืนยันตัวตน เก็บใน LittleFS (ลบทิ้งเมื่อผู้ดูแลอนุมัติ/ปฏิเสธ) ----------
String photoPath(int idx, const String &kind) { return "/v" + String(idx) + (kind == "id" ? "_id.txt" : "_sf.txt"); }
void deletePhotos(int idx) {
  if (!fsOk) return;
  LittleFS.remove(photoPath(idx, "id")); LittleFS.remove(photoPath(idx, "selfie"));
}

// ---------- รูปหลักฐานตอนรับ/คืนกล้อง ----------
String evidencePath(uint16_t rid, const String &kind) { return "/r" + String(rid) + (kind == "return" ? "_r.txt" : "_p.txt"); }
void deleteEvidence(uint16_t rid) {
  if (!fsOk || !rid) return;
  LittleFS.remove(evidencePath(rid, "pickup")); LittleFS.remove(evidencePath(rid, "return"));
}

String userJson(int i) {
  User &u = users[i];
  int count = 0, late = 0; uint32_t hrs = 0;
  for (auto &r : rentals) if (r.status != RS_EMPTY && r.user == i) {
    if (r.status == RS_RETURNED || r.status == RS_ACTIVE) { count++; hrs += r.hours; }
    if (r.lateFee) late++;
  }
  return "{\"idx\":" + String(i) + ",\"sid\":" + jstr(u.sid) + ",\"name\":" + jstr(u.name) + ",\"phone\":" + jstr(u.phone) +
         ",\"studentId\":" + jstr(u.studentId) + ",\"verify\":\"" + verifyName(u.verify) + "\"" +
         ",\"admin\":" + jbool(u.flags & UF_ADMIN) + ",\"disabled\":" + jbool(u.flags & UF_DISABLED) + ",\"test\":" + jbool(u.flags & UF_TEST) +
         ",\"created\":" + String(u.created) + ",\"rentals\":" + String(count) + ",\"hours\":" + String(hrs) + ",\"late\":" + String(late) +
         ",\"wallet\":" + String(wallet[i]) + ",\"av\":" + String(avPh[i]) + "}";
}
String camJson(int i) {
  Camera &c = cams[i];
  return "{\"id\":" + String(i) + ",\"name\":" + jstr(c.name) + ",\"lens\":" + jstr(c.lens) + ",\"specs\":" + jstr(c.specs) +
         ",\"price\":" + String(c.price) + ",\"deposit\":" + String(c.deposit) + ",\"status\":\"" + camStatusName(c.status) + "\",\"ph\":" + String(camPh[i]) + ",\"url\":" + jstr(camUrl[i]) + "}";
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
         ",\"notified\":" + jbool(r.flags & RF_PAY_NOTIFIED) +
         ",\"photoPick\":" + jbool(r.flags & RF_PHOTO_PICK) + ",\"photoRet\":" + jbool(r.flags & RF_PHOTO_RET) + ",\"returning\":" + jbool(returningIdx == i) +
         ",\"slip\":" + String(slipState[i]) + ",\"slipMsg\":" + jstr(slipMsg[i]) +
         ",\"forfeit\":" + jbool(r.flags & RF_FORFEIT) + ",\"lost\":" + jbool(r.flags & RF_LOST) +
         ",\"lateNow\":" + String(r.status == RS_ACTIVE ? lateFeeAt(r, nowEpoch()) : 0) + ",\"refundable\":" + String(refundableOf(r)) + "}";
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
  j += ",\"promptpay\":" + jstr(promptpay) + ",\"ppName\":" + jstr(ppName);
  j += ",\"easyslip\":" + String(jbool(easySlipKey().length() > 0));
  j += ",\"approve\":" + String(jbool(requireApprove));
  j += ",\"unlockMs\":" + String(unlockMs);
  j += ",\"lateFeeMin\":" + String(lateFeeMin) + ",\"lateGraceMin\":" + String(lateGraceMin) + ",\"lostHours\":" + String(lostHours);
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
  String stu = arg("studentId"); stu.trim();
  if (!validSid(sid)) { sendErr(400, "ชื่อผู้ใช้ต้องเป็นอักษรอังกฤษ/ตัวเลข 3–15 ตัว (ใช้ _ หรือ - ได้)"); return; }
  if (stu.length() > 15) { sendErr(400, "รหัสนักศึกษายาวเกินไป"); return; }
  if (name.length() < 2) { sendErr(400, "กรุณาใส่ชื่อ-นามสกุล"); return; }
  if (phone.length() < 9) { sendErr(400, "กรุณาใส่เบอร์โทร"); return; }
  if (pw.length() < 6) { sendErr(400, "รหัสผ่านอย่างน้อย 6 ตัว"); return; }
  int slot = -1;
  for (int i = 0; i < MAX_USERS; i++) {
    if ((users[i].flags & UF_USED) && sid == users[i].sid) { sendErr(409, "ชื่อผู้ใช้นี้มีคนใช้แล้ว"); return; }
    if (slot < 0 && !(users[i].flags & UF_USED)) slot = i;
  }
  if (slot < 0) { sendErr(409, "สมาชิกเต็มแล้ว ติดต่อผู้ดูแล"); return; }
  User &u = users[slot];
  memset(&u, 0, sizeof(User));
  copyStr(u.sid, sizeof(u.sid), sid); copyStr(u.name, sizeof(u.name), name); copyStr(u.phone, sizeof(u.phone), phone);
  copyStr(u.studentId, sizeof(u.studentId), stu);
  u.flags = UF_USED; u.verify = VF_NONE; u.created = nowEpoch();
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
  sendErr(403, "ชื่อผู้ใช้หรือรหัสผ่านไม่ถูกต้อง");
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
  j += "],\"wallet\":" + String(wallet[u]) + ",\"topup\":{\"st\":" + String(topState[u]) + ",\"msg\":" + jstr(topMsg[u]) + "},\"wtx\":[";
  first = true;
  for (int k = 0; k < wtxCount; k++) {
    WalletTx &t = wtx[(wtxHead - 1 - k + MAX_WTX) % MAX_WTX];
    if (t.user != u) continue;
    if (!first) j += ","; first = false;
    j += "{\"e\":" + String(t.epoch) + ",\"a\":" + String(t.amount) + ",\"n\":" + jstr(t.note) + "}";
  }
  j += "]}";
  sendJson(200, j);
}

void handleRent() {
  int u = requireUser(); if (u < 0) return;
  if (!timeSynced()) { sendErr(503, "ตู้ยังไม่ได้ตั้งเวลา ลองรีเฟรชหน้าแล้วลองใหม่"); return; }
  bool isTest = users[u].flags & UF_TEST;
  if (!(users[u].flags & UF_ADMIN) && !isTest && users[u].verify != VF_OK) {
    sendErr(403, users[u].verify == VF_PENDING ? "รอผู้ดูแลตรวจการยืนยันตัวตน" : "กรุณายืนยันตัวตนก่อนเช่า"); return;
  }
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
  deleteEvidence(r.id);          // ทับรายการเก่า: ลบรูปหลักฐานของรายการนั้นด้วย
  memset(&r, 0, sizeof(Rental));
  r.id = nextRentalId++; r.user = u; r.cam = cam; r.hours = hours;
  r.price = isTest ? 0 : c.price; r.deposit = isTest ? 0 : c.deposit;   // บัญชีทดสอบ: ยอด 0 ไม่ปนรายได้ ค่าปรับก็เป็น 0
  r.amount = (uint32_t)r.price * hours + r.deposit;
  r.created = nowEpoch();
  r.status = isTest ? RS_READY : RS_AWAIT;
  c.status = CAM_RENTED;
  saveRentals(); saveCams();
  addLog("จอง #" + String(r.id) + " " + users[u].sid + " → " + c.name + " " + String(hours) + " ชม." + (isTest ? " (ทดสอบ ข้ามการชำระเงิน)" : ""));
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

// ---------- ตรวจสลิปด้วย EasySlip ----------
String easySlipKey() { return slipKey.length() ? slipKey : String(EASYSLIP_KEY); }

// paySlip: แอปอ่าน QR บนสลิปแล้วส่ง payload มา บอร์ดเอาไปถาม EasySlip (ทำใน loop แล้วตอบผลผ่าน rental.slip)
void handlePaySlip() {
  int u = requireUser(); if (u < 0) return;
  int i = ownRental(u); if (i < 0) return;
  Rental &r = rentals[i];
  if (!easySlipKey().length()) { sendErr(503, "ยังไม่ได้ตั้งค่า EasySlip ใช้ปุ่ม \"ฉันชำระแล้ว\" แทน"); return; }
  if (!debtOf(r)) { sendOk("ชำระครบแล้ว"); return; }
  if (r.status == RS_CANCELLED || r.status == RS_EMPTY) { sendErr(409, "รายการนี้ไม่ต้องชำระแล้ว"); return; }
  String pl = arg("payload"); pl.trim();
  if (pl.length() < 10 || pl.length() > 255) { sendErr(400, "อ่าน QR บนสลิปไม่ได้ ลองถ่ายให้ชัดขึ้น"); return; }
  if (slipJob >= 0) { sendErr(429, "กำลังตรวจสลิปอื่นอยู่ ลองใหม่ในไม่กี่วินาที"); return; }
  slipJob = i; slipJobKind = 0; slipPayload = pl;
  slipState[i] = 1; slipMsg[i] = "กำลังตรวจสลิป…";
  sendJson(200, "{\"ok\":true,\"pending\":true,\"rental\":" + rentalJson(i) + "}");
}

// topupSlip: เติมเงินเข้ากระเป๋าด้วยสลิป (ยอดในสลิป = ยอดที่เติม)
void handleTopupSlip() {
  int u = requireUser(); if (u < 0) return;
  if (!easySlipKey().length()) { sendErr(503, "ยังไม่ได้ตั้งค่า EasySlip ติดต่อผู้ดูแลเพื่อเติมเงิน"); return; }
  String pl = arg("payload"); pl.trim();
  if (pl.length() < 10 || pl.length() > 255) { sendErr(400, "อ่าน QR บนสลิปไม่ได้ ลองถ่ายให้ชัดขึ้น"); return; }
  if (slipJob >= 0) { sendErr(429, "กำลังตรวจสลิปอื่นอยู่ ลองใหม่ในไม่กี่วินาที"); return; }
  slipJob = u; slipJobKind = 1; slipPayload = pl;
  topState[u] = 1; topMsg[u] = "กำลังตรวจสลิป…";
  sendJson(200, "{\"ok\":true,\"pending\":true}");
}

// payWallet: จ่ายยอดค้างของรายการด้วยเงินในกระเป๋า
void handlePayWallet() {
  int u = requireUser(); if (u < 0) return;
  int i = ownRental(u); if (i < 0) return;
  Rental &r = rentals[i];
  if (r.status == RS_CANCELLED) { sendErr(409, "รายการนี้ถูกยกเลิกแล้ว"); return; }
  uint32_t owed = debtOf(r);
  if (!owed) { sendOk("ชำระครบแล้ว"); return; }
  if (wallet[u] < owed) { sendErr(402, "เงินในกระเป๋าไม่พอ (มี " + String(wallet[u]) + " บาท ต้องจ่าย " + String(owed) + " บาท)"); return; }
  r.paid += owed; r.flags &= ~RF_PAY_NOTIFIED;
  if (r.status == RS_AWAIT || r.status == RS_VERIFY) r.status = RS_READY;
  saveRentals();
  walletAdd(u, -(long)owed, "ชำระรายการ #" + String(r.id));
  telemetryDirty = true;
  sendJson(200, "{\"ok\":true,\"wallet\":" + String(wallet[u]) + ",\"rental\":" + rentalJson(i) + "}");
}

// เลขที่มองเห็นในบัญชีที่ปิดบัง (เช่น 08xxxxxx89) ต้องตรงกับพร้อมเพย์ของร้าน นับจากท้าย
bool maskedMatch(const String &masked, const String &full) {
  String m, f;
  for (unsigned k = 0; k < masked.length(); k++) { char c = masked[k]; if (isDigit(c) || c == 'x' || c == 'X' || c == '*') m += c; }
  for (unsigned k = 0; k < full.length(); k++) if (isDigit(full[k])) f += full[k];
  if (!m.length() || !f.length()) return false;
  int digits = 0;
  for (int a = m.length() - 1, b = f.length() - 1; a >= 0 && b >= 0; a--, b--) {
    if (isDigit(m[a])) { if (m[a] != f[b]) return false; digits++; }
  }
  return digits >= 2;
}
// เบอร์ 0812345678 ในสลิปบางธนาคารขึ้นเป็น 66812345678
bool receiverMatches(const String &acc) {
  if (!promptpay.length()) return false;         // ยังไม่ตั้งพร้อมเพย์ของร้าน เทียบผู้รับไม่ได้ ให้ผู้ดูแลตรวจ
  if (maskedMatch(acc, promptpay)) return true;
  if (promptpay.length() == 10 && promptpay[0] == '0') return maskedMatch(acc, "66" + promptpay.substring(1));
  return false;
}
// สำรอง: บางธนาคารไม่ส่งเลขพร้อมเพย์ของผู้รับ ใช้ชื่อแทน (ชื่อจริงคำแรกต้องตรง)
bool nameMatches(const String &rname) {
  if (!ppName.length() || !rname.length()) return false;
  String first = ppName; int sp = first.indexOf(' '); if (sp > 0) first = first.substring(0, sp);
  if (first.length() < 3) return false;
  return rname.indexOf(first) >= 0;
}
bool slipRefUsed(const String &ref) {
  if (!fsOk || !LittleFS.exists("/slips.txt")) return false;
  File f = LittleFS.open("/slips.txt", "r");
  while (f.available()) { String l = f.readStringUntil('\n'); l.trim(); if (l == ref) { f.close(); return true; } }
  f.close(); return false;
}
void slipRefSave(const String &ref) {
  if (!fsOk) return;
  if (LittleFS.exists("/slips.txt")) { File c = LittleFS.open("/slips.txt", "r"); size_t n = c.size(); c.close(); if (n > 16000) LittleFS.remove("/slips.txt"); }
  File f = LittleFS.open("/slips.txt", "a"); if (f) { f.println(ref); f.close(); }
}
String urlEncode(const String &s) {
  String o; const char *hex = "0123456789ABCDEF";
  for (unsigned k = 0; k < s.length(); k++) {
    char c = s[k];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { o += '%'; o += hex[(c >> 4) & 15]; o += hex[c & 15]; }
  }
  return o;
}
uint8_t curSlipKind = 0;
void slipFail(int i, const String &msg) {
  if (curSlipKind == 1) {
    topState[i] = 3; topMsg[i] = msg;
    addLog("สลิปเติมเงิน " + String(users[i].sid) + " ไม่ผ่าน: " + msg);
  } else {
    slipState[i] = 3; slipMsg[i] = msg;
    addLog("สลิป #" + String(rentals[i].id) + " ไม่ผ่าน: " + msg);
  }
}

void processSlipJob() {
  if (slipJob < 0) return;
  int i = slipJob; slipJob = -1; curSlipKind = slipJobKind;
  Rental &r = rentals[curSlipKind == 1 ? 0 : i];
  if (WiFi.status() != WL_CONNECTED) { slipFail(i, "ตู้ไม่มีอินเทอร์เน็ต ลองใหม่อีกครั้ง"); telemetryDirty = true; return; }
  esp_task_wdt_reset();
  WiFiClientSecure tls; tls.setInsecure();   // ไม่ตรวจใบรับรอง (บอร์ดไม่มีที่เก็บ CA) ข้อมูลยังเข้ารหัสอยู่
  HTTPClient http; http.setTimeout(8000); http.setConnectTimeout(6000);
  String url = "https://developer.easyslip.com/api/v1/verify?payload=" + urlEncode(slipPayload);
  if (!http.begin(tls, url)) { slipFail(i, "เชื่อม EasySlip ไม่ได้"); return; }
  http.addHeader("Authorization", "Bearer " + easySlipKey());
  int code = http.GET();
  esp_task_wdt_reset();
  String body = code > 0 ? http.getString() : "";
  http.end();
  esp_task_wdt_reset();
  if (code <= 0) { slipFail(i, "เชื่อม EasySlip ไม่ได้ (" + String(code) + ")"); telemetryDirty = true; return; }

#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument d;
#else
  DynamicJsonDocument d(4096);
#endif
  if (deserializeJson(d, body)) { slipFail(i, "คำตอบจาก EasySlip ไม่ถูกต้อง (HTTP " + String(code) + ")"); telemetryDirty = true; return; }
  if (code != 200 || (d["status"] | 0) != 200) {
    String m = d["message"] | "error";
    String th = m == "slip_not_found" ? "ไม่พบสลิปนี้ในระบบธนาคาร (สลิปปลอมหรือเพิ่งโอน ลองใหม่ใน 1 นาที)" :
                m == "invalid_payload" ? "QR บนสลิปไม่ถูกต้อง" :
                m == "duplicate_slip" ? "สลิปนี้ถูกใช้ไปแล้ว" :
                m == "quota_exceeded" ? "โควตา EasySlip หมด แจ้งผู้ดูแล" :
                m == "unauthorized" ? "API key ของ EasySlip ไม่ถูกต้อง แจ้งผู้ดูแล" :
                m == "application_expired" || m == "application_deactivated" ? "บัญชี EasySlip หมดอายุ แจ้งผู้ดูแล" :
                "EasySlip ตอบว่า " + m;
    slipFail(i, th); telemetryDirty = true; return;
  }
  JsonVariant dt = d["data"];
  String ref = dt["transRef"] | "";
  uint32_t amt = (uint32_t)((dt["amount"]["amount"] | 0.0) + 0.001);
  String rname = dt["receiver"]["account"]["name"]["th"] | (dt["receiver"]["account"]["name"]["en"] | "");
  String proxy = dt["receiver"]["account"]["proxy"]["account"] | "";
  String bankAc = dt["receiver"]["account"]["bank"]["account"] | "";
  if (!ref.length()) { slipFail(i, "สลิปไม่มีเลขอ้างอิง"); return; }
  if (slipRefUsed(ref)) { slipFail(i, "สลิปนี้ถูกใช้ไปแล้ว"); telemetryDirty = true; return; }
  bool recvOk = receiverMatches(proxy) || (bankAc.length() && receiverMatches(bankAc)) ||
                (!proxy.length() && nameMatches(rname));
  if (curSlipKind == 1) {           // ---- เติมเงินเข้ากระเป๋า
    if (!recvOk) { slipFail(i, "ชื่อ/บัญชีผู้รับในสลิปไม่ตรงกับร้าน (" + rname + ")"); return; }
    if (amt < 1) { slipFail(i, "ยอดในสลิปไม่ถูกต้อง"); return; }
    slipRefSave(ref);
    walletAdd(i, amt, "เติมเงินด้วยสลิป");
    topState[i] = 2; topMsg[i] = "เติมเงินสำเร็จ " + String(amt) + " บาท";
    telemetryDirty = true;
    return;
  }
  uint32_t owed = debtOf(r);
  if (!recvOk) {
    // ผู้รับไม่ตรงพร้อมเพย์ของร้าน ส่งให้ผู้ดูแลตรวจเอง
    r.flags |= RF_PAY_NOTIFIED; if (r.status == RS_AWAIT) r.status = RS_VERIFY; saveRentals();
    slipFail(i, "ชื่อ/บัญชีผู้รับในสลิปไม่ตรงกับร้าน (" + rname + ") ส่งให้ผู้ดูแลตรวจแล้ว");
    telemetryDirty = true; return;
  }
  if (amt < owed) {
    slipFail(i, "ยอดในสลิป " + String(amt) + " บาท น้อยกว่ายอดที่ต้องชำระ " + String(owed) + " บาท");
    telemetryDirty = true; return;
  }
  slipRefSave(ref);
  r.paid += owed; r.flags &= ~RF_PAY_NOTIFIED;
  if (r.status == RS_AWAIT || r.status == RS_VERIFY) r.status = RS_READY;
  saveRentals();
  slipState[i] = 2; slipMsg[i] = "ตรวจสลิปผ่าน ยอด " + String(amt) + " บาท";
  if (amt > owed && r.user < MAX_USERS) { walletAdd(r.user, amt - owed, "เงินเกินจากสลิป #" + String(r.id)); slipMsg[i] += " (ส่วนเกินเข้ากระเป๋า)"; }
  addLog("ชำระด้วยสลิป #" + String(r.id) + " " + String(amt) + " บาท (EasySlip ref " + ref + ")");
  telemetryDirty = true;
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
  uint32_t cost = (uint32_t)r.price * h;
  bool useWallet = arg("wallet") == "1";
  if (useWallet && wallet[u] < cost) { sendErr(402, "เงินในกระเป๋าไม่พอ (มี " + String(wallet[u]) + " บาท ต้องจ่าย " + String(cost) + " บาท)"); return; }
  r.hours += h; r.dueAt += h * 3600; r.amount += cost;
  if (useWallet && cost) { r.paid += cost; walletAdd(u, -(long)cost, "ต่อเวลา #" + String(r.id)); }
  else if (!requireApprove) r.paid = r.amount;
  saveRentals();
  addLog("ต่อเวลา #" + String(r.id) + " +" + String(h) + " ชม." + (useWallet ? " (จ่ายจากกระเป๋า)" : ""));
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
  String j; j.reserve(40000);
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
    if (r.status == RS_ACTIVE) { r.status = RS_RETURNED; r.returnedAt = nowEpoch(); applyLateFee(r, r.returnedAt); freeCam = true; }
  } else if (act == "cancel") {
    if (r.status == RS_AWAIT || r.status == RS_VERIFY || r.status == RS_READY) { r.status = RS_CANCELLED; freeCam = true; }
    else { sendErr(409, "ยกเลิกได้เฉพาะรายการที่ยังไม่รับกล้อง"); return; }
  } else if (act == "refund") {       // ตรวจสภาพผ่าน: คืนมัดจำ (หักค่าปรับแล้ว) เข้ากระเป๋าผู้เช่า
    if (r.status != RS_RETURNED) { sendErr(409, "คืนมัดจำได้หลังคืนกล้องแล้ว"); return; }
    if (r.flags & RF_REFUNDED) { sendErr(409, "รายการนี้ปิดไปแล้ว"); return; }
    uint32_t amt = refundableOf(r);
    r.flags |= RF_REFUNDED;
    if (amt) { r.amount -= amt; r.paid -= amt; walletAdd(r.user, amt, "คืนมัดจำ #" + String(r.id)); }
  } else if (act == "forfeit") {      // ตรวจแล้วกล้องชำรุด/เสียหาย: ไม่คืนมัดจำทั้งสิ้น
    if (r.status != RS_RETURNED && r.status != RS_ACTIVE) { sendErr(409, "ใช้ได้กับรายการที่รับกล้องไปแล้ว"); return; }
    if (r.flags & RF_REFUNDED) { sendErr(409, "คืนมัดจำไปแล้ว"); return; }
    r.flags |= RF_FORFEIT | RF_REFUNDED;
    addLog("⚠ ยึดมัดจำ #" + String(r.id) + " (กล้องชำรุด/เสียหาย)");
  } else if (act == "lost") {         // ไม่คืนกล้อง: ยึดมัดจำ
    if (r.status != RS_ACTIVE) { sendErr(409, "ใช้ได้กับรายการที่กำลังเช่า"); return; }
    r.flags |= RF_LOST;
    addLog("⚠ ไม่คืนกล้อง #" + String(r.id) + " ยึดมัดจำ");
  } else if (act == "delete") {
    if (r.status == RS_ACTIVE || r.status == RS_READY) { sendErr(409, "ลบไม่ได้ รายการยังไม่จบ"); return; }
    if (r.status == RS_AWAIT || r.status == RS_VERIFY) freeCam = true;
    uint16_t id = r.id; uint8_t cam = r.cam;
    deleteEvidence(id);
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
  if (hasArg("imgUrl")) {
    String u = arg("imgUrl"); u.trim();
    if (u.length() && (!(u.startsWith("https://") || u.startsWith("http://")) || u.length() > 400)) { sendErr(400, "ลิงก์รูปต้องขึ้นต้นด้วย https:// และยาวไม่เกิน 400 ตัวอักษร"); return; }
    camUrl[id] = u;
    prefs.begin("camlock", false); prefs.putString(("cu" + String(id)).c_str(), u); prefs.end();
  }
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
  if (fsOk) LittleFS.remove(camPhotoPath(id)); camPh[id] = 0; savePhotoVersions();
  camUrl[id] = ""; prefs.begin("camlock", false); prefs.remove(("cu" + String(id)).c_str()); prefs.end();
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
  else if (act == "approve") { u.verify = VF_OK; deletePhotos(idx); }
  else if (act == "reject")  { u.verify = VF_REJECTED; deletePhotos(idx); }
  else if (act == "credit") {        // เติม (+) หรือหัก (−) เงินในกระเป๋าโดยผู้ดูแล
    long amt = arg("amount").toInt();
    if (amt == 0 || amt > 100000 || amt < -100000) { sendErr(400, "จำนวนเงินไม่ถูกต้อง"); return; }
    walletAdd(idx, amt, amt > 0 ? "ผู้ดูแลเติมเงิน" : "ผู้ดูแลหักเงิน");
    sendOk("บันทึกแล้ว ยอดคงเหลือ " + String(wallet[idx]) + " บาท"); return;
  }
  else if (act == "password") {
    String p = arg("pass");
    if (p.length() < 4) { sendErr(400, "รหัสผ่านใหม่สั้นเกินไป"); return; }
    setPassword(u, p); dropSessionsOf(idx);
  } else if (act == "delete") {
    if (openRentalOf(idx) >= 0) { sendErr(409, "สมาชิกมีรายการเช่าค้างอยู่"); return; }
    dropSessionsOf(idx); deletePhotos(idx); addLog(String("ลบสมาชิก: ") + u.sid);
    wallet[idx] = 0; saveWallet();
    if (fsOk) LittleFS.remove(avatarPath(idx)); avPh[idx] = 0; savePhotoVersions();
    memset(&u, 0, sizeof(User)); saveUsers(); sendOk("ลบแล้ว"); return;
  } else { sendErr(400, "คำสั่งไม่ถูกต้อง"); return; }
  saveUsers();
  addLog("ผู้ดูแล " + act + " สมาชิก " + String(u.sid));
  sendOk("บันทึกแล้ว");
}

// ---------- ยืนยันตัวตน ----------
// ส่งรูปทีละรูป: kind = "id" (บัตร) หรือ "selfie" · img = data URL JPEG ที่แอปย่อขนาดแล้ว
void handleVerifyUpload() {
  int u = requireUser(); if (u < 0) return;
  if (!fsOk) { sendErr(500, "หน่วยความจำรูปของตู้ใช้ไม่ได้"); return; }
  if (users[u].verify == VF_OK) { sendOk("ยืนยันตัวตนแล้ว"); return; }
  String kind = arg("kind"), img = arg("img");
  if (kind != "id" && kind != "selfie") { sendErr(400, "ชนิดรูปไม่ถูกต้อง"); return; }
  if (!img.startsWith("data:image/jpeg;base64,") || img.length() > 20000) { sendErr(400, "รูปใหญ่เกินไปหรือไม่ใช่ JPEG"); return; }
  File f = LittleFS.open(photoPath(u, kind), "w");
  if (!f) { sendErr(500, "บันทึกรูปไม่ได้"); return; }
  f.print(img); f.close();
  sendOk("อัปโหลดแล้ว");
}

void handleVerifySubmit() {
  int u = requireUser(); if (u < 0) return;
  if (users[u].verify == VF_OK) { sendOk("ยืนยันตัวตนแล้ว"); return; }
  if (!fsOk || !LittleFS.exists(photoPath(u, "id")) || !LittleFS.exists(photoPath(u, "selfie"))) { sendErr(400, "กรุณาถ่ายรูปบัตรและรูปหน้าให้ครบ"); return; }
  String stu = arg("studentId"); stu.trim();
  if (stu.length() && stu.length() <= 15) copyStr(users[u].studentId, sizeof(users[u].studentId), stu);
  users[u].verify = VF_PENDING;
  saveUsers();
  addLog(String("ส่งยืนยันตัวตน: ") + users[u].sid);
  sendJson(200, "{\"ok\":true,\"user\":" + userJson(u) + "}");
}

void handleAdminUserPhoto() {
  if (requireAdmin() < 0) return;
  long idx = arg("idx").toInt();
  if (idx < 0 || idx >= MAX_USERS || !(users[idx].flags & UF_USED)) { sendErr(404, "ไม่พบสมาชิก"); return; }
  String path = photoPath(idx, arg("kind"));
  if (!fsOk || !LittleFS.exists(path)) { sendErr(404, "ไม่มีรูป"); return; }
  File f = LittleFS.open(path, "r");
  String img = f.readString(); f.close();
  sendJson(200, "{\"ok\":true,\"img\":\"" + img + "\"}");
}

// ผู้ใช้ส่งรูปหลักฐาน: kind = "pickup" (ตอนรับ) หรือ "return" (ตอนคืน)
void handleEvidence() {
  int u = requireUser(); if (u < 0) return;
  int i = ownRental(u); if (i < 0) return;
  Rental &r = rentals[i];
  String kind = arg("kind"), img = arg("img");
  if (kind != "pickup" && kind != "return") { sendErr(400, "ชนิดรูปไม่ถูกต้อง"); return; }
  if (r.status != RS_ACTIVE && r.status != RS_RETURNED) { sendErr(409, "ส่งหลักฐานได้หลังเปิดตู้เท่านั้น"); return; }
  if (!img.startsWith("data:image/jpeg;base64,") || img.length() > 20000) { sendErr(400, "รูปใหญ่เกินไปหรือไม่ใช่ JPEG"); return; }
  if (!fsOk) { sendErr(500, "หน่วยความจำรูปของตู้ใช้ไม่ได้"); return; }
  File f = LittleFS.open(evidencePath(r.id, kind), "w");
  if (!f) { sendErr(500, "บันทึกรูปไม่ได้ (หน่วยความจำเต็ม?)"); return; }
  size_t n = f.print(img); f.close();
  if (n != img.length()) { LittleFS.remove(evidencePath(r.id, kind)); sendErr(500, "หน่วยความจำรูปเต็ม แจ้งผู้ดูแลให้ลบรายการเก่า"); return; }
  r.flags |= (kind == "return") ? RF_PHOTO_RET : RF_PHOTO_PICK;
  saveRentals();
  addLog("รูปหลักฐาน" + String(kind == "return" ? "คืน" : "รับ") + " #" + String(r.id));
  sendJson(200, "{\"ok\":true,\"rental\":" + rentalJson(i) + "}");
}

void handleAdminEvidence() {
  if (requireAdmin() < 0) return;
  int i = findRental(arg("rid").toInt());
  if (i < 0) { sendErr(404, "ไม่พบรายการ"); return; }
  String path = evidencePath(rentals[i].id, arg("kind"));
  if (!fsOk || !LittleFS.exists(path)) { sendErr(404, "ไม่มีรูป"); return; }
  File f = LittleFS.open(path, "r");
  String img = f.readString(); f.close();
  sendJson(200, "{\"ok\":true,\"img\":\"" + img + "\"}");
}

// ---------- รูปกล้อง (ผู้ดูแลอัปโหลด ทุกคนดูได้) และรูปโปรไฟล์ ----------
String camPhotoPath(int id) { return "/cp" + String(id) + ".txt"; }
String avatarPath(int idx)  { return "/av" + String(idx) + ".txt"; }
uint32_t photoVersion() { uint32_t e = nowEpoch(); return e ? e : (millis() | 1); }
bool savePhotoFile(const String &path, const String &img, size_t limit) {
  if (!fsOk || !img.startsWith("data:image/jpeg;base64,") || img.length() > limit) return false;
  File f = LittleFS.open(path, "w"); if (!f) return false;
  size_t n = f.print(img); f.close();
  if (n != img.length()) { LittleFS.remove(path); return false; }
  return true;
}
void sendPhotoFile(const String &path) {
  if (!fsOk || !LittleFS.exists(path)) { sendErr(404, "ไม่มีรูป"); return; }
  File f = LittleFS.open(path, "r"); String img = f.readString(); f.close();
  sendJson(200, "{\"ok\":true,\"img\":\"" + img + "\"}");
}
void handleCamPhoto() {
  long id = arg("id").toInt();
  if (id < 0 || id >= MAX_CAMS || !cams[id].used || !camPh[id]) { sendErr(404, "ไม่มีรูป"); return; }
  sendPhotoFile(camPhotoPath(id));
}
void handleAdminCamPhoto() {
  if (requireAdmin() < 0) return;
  long id = arg("id").toInt();
  if (id < 0 || id >= MAX_CAMS || !cams[id].used) { sendErr(404, "ไม่พบกล้อง"); return; }
  String img = arg("img");
  if (!img.length()) { if (fsOk) LittleFS.remove(camPhotoPath(id)); camPh[id] = 0; savePhotoVersions(); sendOk("ลบรูปแล้ว"); return; }
  if (!savePhotoFile(camPhotoPath(id), img, 24000)) { sendErr(400, "บันทึกรูปไม่ได้ (ต้องเป็น JPEG ไม่เกิน 24KB)"); return; }
  camPh[id] = photoVersion(); savePhotoVersions();
  addLog(String("อัปโหลดรูปกล้อง: ") + cams[id].name);
  sendJson(200, "{\"ok\":true,\"msg\":\"บันทึกรูปแล้ว\",\"ph\":" + String(camPh[id]) + "}");
}
void handleAvatarUpload() {
  int u = requireUser(); if (u < 0) return;
  String img = arg("img");
  if (!img.length()) { if (fsOk) LittleFS.remove(avatarPath(u)); avPh[u] = 0; savePhotoVersions(); sendJson(200, "{\"ok\":true,\"user\":" + userJson(u) + "}"); return; }
  if (!savePhotoFile(avatarPath(u), img, 16000)) { sendErr(400, "บันทึกรูปไม่ได้ (ต้องเป็น JPEG ไม่เกิน 16KB)"); return; }
  avPh[u] = photoVersion(); savePhotoVersions();
  sendJson(200, "{\"ok\":true,\"user\":" + userJson(u) + "}");
}
void handleAvatar() {
  int u = requireUser(); if (u < 0) return;
  long idx = hasArg("idx") ? arg("idx").toInt() : u;
  if (idx != u && !(users[u].flags & UF_ADMIN)) { sendErr(403, "ดูได้เฉพาะรูปของตัวเอง"); return; }
  if (idx < 0 || idx >= MAX_USERS || !avPh[idx]) { sendErr(404, "ไม่มีรูป"); return; }
  sendPhotoFile(avatarPath(idx));
}

void handleAdminSettings() {
  if (requireAdmin() < 0) return;
  sendJson(200, "{\"ok\":true,\"lockerName\":" + jstr(lockerName) + ",\"lockerLoc\":" + jstr(lockerLoc) +
                ",\"lockerNote\":" + jstr(lockerNote) + ",\"promptpay\":" + jstr(promptpay) + ",\"ppName\":" + jstr(ppName) +
                ",\"approve\":" + jbool(requireApprove) + ",\"unlockMs\":" + String(unlockMs) +
                ",\"alarmSec\":" + String(alarmSec) + ",\"ssid\":" + jstr(WIFI_SSID) +
                ",\"slipKeySet\":" + jbool(easySlipKey().length() > 0) + ",\"slipKeyFromCode\":" + jbool(!slipKey.length() && strlen(EASYSLIP_KEY) > 0) +
                ",\"lateFeeMin\":" + String(lateFeeMin) + ",\"lateGraceMin\":" + String(lateGraceMin) + ",\"lostHours\":" + String(lostHours) + "}");
}

void handleAdminSettingsSave() {
  if (requireAdmin() < 0) return;
  if (hasArg("unlockMs")) {
    long v = arg("unlockMs").toInt();
    if (v < 3000 || v > MAX_UNLOCK_MS) { sendErr(400, "เวลาเปิดค้างต้องอยู่ระหว่าง 3–60 วินาที"); return; }
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
  if (hasArg("ppName")) { ppName = arg("ppName"); ppName.trim(); }
  if (hasArg("lateFeeMin")) { long v = arg("lateFeeMin").toInt(); if (v < 0 || v > 10000) { sendErr(400, "ค่าปรับต่อนาทีต้องอยู่ระหว่าง 0–10000 บาท"); return; } lateFeeMin = v; }
  if (hasArg("lateGraceMin")) { long v = arg("lateGraceMin").toInt(); if (v < 0 || v > 120) { sendErr(400, "เวลาผ่อนผันต้องอยู่ระหว่าง 0–120 นาที"); return; } lateGraceMin = v; }
  if (hasArg("lostHours")) { long v = arg("lostHours").toInt(); if (v < 0 || v > 720) { sendErr(400, "ชั่วโมงที่ถือว่าไม่คืนต้องอยู่ระหว่าง 0–720"); return; } lostHours = v; }
  if (hasArg("lockerName")) { String s = arg("lockerName"); s.trim(); if (s.length()) lockerName = s; }
  if (hasArg("lockerLoc"))  { lockerLoc = arg("lockerLoc"); lockerLoc.trim(); }
  if (hasArg("lockerNote")) { lockerNote = arg("lockerNote"); lockerNote.trim(); }
  if (hasArg("approve")) requireApprove = arg("approve") == "1";
  if (hasArg("slipKey")) { String k = arg("slipKey"); k.trim(); if (k == "-") slipKey = ""; else if (k.length()) slipKey = k; }
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


// lockNow: ผู้ใช้ที่เข้าสู่ระบบกดล็อกตู้หลังหยิบ/ใส่กล้องเสร็จ
void handleLockNow() {
  int u = requireUser(); if (u < 0) return;
  lockNow(users[u].sid);
  sendOk("ล็อกตู้แล้ว");
}

// getStatus: คงไว้ให้วิดเจ็ตบนแดชบอร์ด ThingsBoard เดิมใช้ได้
void handleGetStatus() {
  sendJson(200, String("{\"ok\":true,\"doorOpen\":") + jbool(!doorClosed) + ",\"lockActive\":" + jbool(relayActive) + "}");
}

typedef void (*Handler)();
struct Route { const char *method; Handler fn; };
const Route ROUTES[] = {
  {"status", handleStatus},           {"getStatus", handleGetStatus},     {"lockNow", handleLockNow},
  {"evidence", handleEvidence},       {"adminEvidence", handleAdminEvidence},
  {"verifyUpload", handleVerifyUpload}, {"verifySubmit", handleVerifySubmit}, {"adminUserPhoto", handleAdminUserPhoto},           {"register", handleRegister},       {"login", handleLogin},
  {"logout", handleLogout},           {"me", handleMe},                   {"rent", handleRent},
  {"pay", handlePayNotify},           {"paySlip", handlePaySlip},
  {"payWallet", handlePayWallet},     {"topupSlip", handleTopupSlip},     {"camPhoto", handleCamPhoto},
  {"adminCamPhoto", handleAdminCamPhoto}, {"avatarUpload", handleAvatarUpload}, {"avatar", handleAvatar},           {"cancel", handleCancel},           {"open", handleOpen},
  {"extend", handleExtend},           {"password", handlePassword},
  {"adminOverview", handleAdminOverview}, {"adminUnlock", handleAdminUnlock}, {"adminRental", handleAdminRental},
  {"adminCamera", handleAdminCamSave},    {"adminCameraDelete", handleAdminCamDelete}, {"adminUser", handleAdminUser},
  {"adminSettings", handleAdminSettings}, {"adminSettingsSave", handleAdminSettingsSave}, {"adminLog", handleAdminLog},
};

uint32_t lastHeartbeat = 0;
void publishTelemetry() {
  int avail = 0, total = 0, active = 0, overdue = 0, ready = 0, pendPay = 0, refund = 0, members = 0, pendId = 0;
  uint32_t ep = nowEpoch();
  for (auto &c : cams) if (c.used) { total++; if (c.status == CAM_AVAILABLE) avail++; }
  for (auto &r : rentals) {
    if (r.status == RS_ACTIVE) { active++; if (ep && r.dueAt && ep > r.dueAt) overdue++; }
    if (r.status == RS_READY) ready++;
    if (r.status == RS_VERIFY || ((r.flags & RF_PAY_NOTIFIED) && r.paid < r.amount)) pendPay++;
    if (r.status == RS_RETURNED && !(r.flags & RF_REFUNDED) && r.deposit) refund++;
  }
  for (auto &u : users) if (u.flags & UF_USED) { members++; if (u.verify == VF_PENDING) pendId++; }
  String j = "{\"activeCount\":" + String(active) + ",\"overdueCount\":" + String(overdue) + ",\"readyCount\":" + String(ready) +
             ",\"pendingPay\":" + String(pendPay) + ",\"pendingVerify\":" + String(pendId) + ",\"refundPending\":" + String(refund) +
             ",\"members\":" + String(members) + ",\"doorOpen\":" + String(jbool(!doorClosed)) + ",\"lockActive\":" + jbool(relayActive) +
             ",\"alarm\":" + jbool(doorAlarm) + ",\"returning\":" + jbool(returningIdx >= 0) +
             ",\"available\":" + String(avail) + ",\"camCount\":" + String(total) +
             ",\"unlockCount\":" + String(totalUnlocks);
  // ส่งเฉพาะเมื่อค่าเปลี่ยน หรือครบรอบ heartbeat (ลดจำนวนข้อความ ไม่ให้ ThingsBoard ตัดเพราะส่งถี่เกิน)
  static String lastState;
  bool heartbeat = millis() - lastHeartbeat >= TELEMETRY_MS;
  if (j == lastState && !heartbeat) { telemetryDirty = false; return; }
  lastState = j;
  if (heartbeat) lastHeartbeat = millis();
  j += ",\"rssi\":" + String(WiFi.RSSI()) + ",\"uptime\":" + String(millis() / 1000) + ",\"heap\":" + String(ESP.getFreeHeap()) + "}";
  mqtt.publish("v1/devices/me/telemetry", j.c_str());
  telemetryDirty = false;
}

// ส่งเหตุการณ์ในคิวขึ้น ThingsBoard เป็น telemetry "event" (แต่ละอันคนละเวลา ไม่ทับกัน)
void publishEvents() {
  static uint32_t lastEv = 0;
  if (!evCount || !mqtt.connected() || millis() - lastEv < 1000) return;   // รวมส่งเป็นชุด อย่างมากวินาทีละครั้ง
  lastEv = millis();
  String j = "[";
  bool first = true;
  while (evCount) {
    LogEntry &e = logs[evQueue[evHead]];
    char ms[4]; snprintf(ms, sizeof(ms), "%03u", (unsigned)(evSeq++ % 1000));   // ms ต่างกันเพื่อไม่ให้ทับกัน
    if (e.epoch) {
      if (!first) j += ",";
      first = false;
      j += "{\"ts\":" + String(e.epoch) + ms + ",\"values\":{\"event\":" + jstr(e.msg) + "}}";
    }
    evHead = (evHead + 1) % EV_QUEUE; evCount--;
  }
  j += "]";
  if (!first) mqtt.publish("v1/devices/me/telemetry", j.c_str());
}

void publishAttributes() {
  String j = "{\"lockerId\":\"" LOCKER_ID "\",\"firmware\":\"" FW_VERSION "\",\"ip\":" + jstr(WiFi.localIP().toString()) +
             ",\"ssid\":" + jstr(WIFI_SSID) + ",\"lockerName\":" + jstr(lockerName) + ",\"lockerLoc\":" + jstr(lockerLoc) +
             ",\"lockerNote\":" + jstr(lockerNote) + ",\"promptpay\":" + jstr(promptpay) + ",\"ppName\":" + jstr(ppName) +
             ",\"approve\":" + jbool(requireApprove) + ",\"unlockMs\":" + String(unlockMs) +
             ",\"reedSwitch\":" + jbool(HAS_REED_SWITCH) + ",\"autoCloseSec\":" + String(unlockMs / 1000) +
             ",\"easyslip\":" + jbool(easySlipKey().length() > 0) +
             ",\"lateFeeMin\":" + String(lateFeeMin) + ",\"lateGraceMin\":" + String(lateGraceMin) + ",\"lostHours\":" + String(lostHours) + "}";
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

// เหตุผลที่ WiFi หลุด (จาก event ของ ESP32) ใช้บอกใน Serial ว่าต้องแก้ตรงไหน
volatile uint16_t wifiReason = 0;
uint32_t wifiDownSince = 0, mqttDownSince = 0;
uint8_t  wifiTries = 0;

const char *wifiReasonText(uint16_t r) {
  switch (r) {
    case 201: return "หา WiFi ไม่เจอ (ชื่อผิด, อยู่ไกล หรือเป็น 5GHz — ESP32 ใช้ได้แค่ 2.4GHz)";
    case 202: case 15: case 204: return "รหัสผ่าน WiFi ไม่ถูกต้อง";
    case 203: return "เชื่อมต่อไม่สำเร็จ (เราเตอร์ปฏิเสธ)";
    case 8:   return "ออกจาก WiFi";
    case 200: return "สัญญาณหาย (beacon timeout)";
    default:  return "หลุดการเชื่อมต่อ";
  }
}

void onWifiDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  wifiReason = info.wifi_sta_disconnected.reason;
}

void startWifi(bool hardReset) {
  if (hardReset) { WiFi.disconnect(true); delay(200); WiFi.mode(WIFI_STA); WiFi.setSleep(false); }
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("กำลังต่อ WiFi \"%s\" (ครั้งที่ %u)...\n", WIFI_SSID, (unsigned)wifiTries + 1);
  wifiTries++;
}

// ไล่ดูว่ามองเห็น WiFi ที่ตั้งไว้ไหม ช่วยบอกสาเหตุใน Serial Monitor
void diagnoseWifi() {
  esp_task_wdt_reset();
  int n = WiFi.scanNetworks();
  esp_task_wdt_reset();
  bool found = false;
  for (int i = 0; i < n; i++) if (WiFi.SSID(i) == WIFI_SSID) {
    found = true;
    Serial.printf("  เห็น \"%s\" สัญญาณ %d dBm ช่อง %d\n", WIFI_SSID, WiFi.RSSI(i), WiFi.channel(i));
  }
  if (!found) Serial.printf("  ไม่เห็น WiFi \"%s\" เลย (เปิดฮอตสปอตหรือยัง? ต้องเป็น 2.4GHz / iPhone เปิด Maximize Compatibility)\n", WIFI_SSID);
  WiFi.scanDelete();
}

void maintainNetwork() {
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasUp) {
      wifiWasUp = false; wifiDownSince = now;
      addLog(String("WiFi หลุด: ") + wifiReasonText(wifiReason) + " กำลังเชื่อมใหม่");
      telemetryDirty = true;
    }
    if (!wifiDownSince) wifiDownSince = now;
    // ครั้งแรกเริ่มต่อเลย จากนั้นให้เวลาแต่ละครั้ง 20 วินาที (ESP32 ต่อใหม่เองด้วย autoReconnect)
    if (lastWifiTry == 0 || now - lastWifiTry > 20000) {
      if (lastWifiTry) {
        Serial.printf("WiFi ยังไม่ติด: %s (reason %u)\n", wifiReasonText(wifiReason), (unsigned)wifiReason);
        if (wifiTries % 3 == 0) diagnoseWifi();
      }
      lastWifiTry = now;
      startWifi(wifiTries > 0 && wifiTries % 3 == 0);   // ทุก 3 รอบ รีเซ็ต WiFi ใหม่หมด
    }
    // หลุดนานเกิน 10 นาที รีสตาร์ทบอร์ด (ยกเว้นตอนกลอนเปิดอยู่)
    if (now - wifiDownSince > 600000UL && !relayActive && returningIdx < 0) {
      Serial.println("WiFi หลุดนานเกิน 10 นาที รีสตาร์ทบอร์ด");
      delay(200); ESP.restart();
    }
    return;
  }
  wifiDownSince = 0; wifiTries = 0;
  if (!wifiWasUp) {
    wifiWasUp = true;
    addLog("เชื่อม WiFi " WIFI_SSID " IP " + WiFi.localIP().toString());
    if (!ntpStarted) { configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.navy.mi.th"); ntpStarted = true; }
  }
  if (!mqtt.connected()) {
    if (!mqttDownSince) mqttDownSince = now;
    if (now - mqttDownSince > 600000UL && !relayActive && returningIdx < 0) {   // ต่อ ThingsBoard ไม่ได้ 10 นาที รีสตาร์ท
      Serial.println("ต่อ ThingsBoard ไม่ได้นานเกิน 10 นาที รีสตาร์ทบอร์ด");
      delay(200); ESP.restart();
    }
    if (now - lastMqttTry < 5000 && lastMqttTry != 0) return;
    lastMqttTry = now;
    String clientId = "camlock-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.printf("กำลังต่อ ThingsBoard %s ...\n", TB_HOST);
    esp_task_wdt_reset();
    if (mqtt.connect(clientId.c_str(), TB_TOKEN, NULL)) {      // username = access token
      mqtt.subscribe("v1/devices/me/rpc/request/+");
      addLog("เชื่อม ThingsBoard แล้ว");
      mqttDownSince = 0;
      publishAttributes();
      publishTelemetry();
    } else {
      int st = mqtt.state();
      Serial.printf("ต่อ ThingsBoard ไม่ได้ (state %d) %s\n", st,
        st == 4 || st == 5 ? "— TB_TOKEN ไม่ถูกต้อง" : st == -2 ? "— ออกเน็ตไม่ได้ (WiFi ไม่มีอินเทอร์เน็ต?)" : "");
    }
    return;
  }
  mqtt.loop();
  if (attributesDirty) publishAttributes();
  if ((telemetryDirty && now - lastTelemetry >= 300) || now - lastTelemetry >= TELEMETRY_MS) { lastTelemetry = now; publishTelemetry(); }
  publishEvents();
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
  fsOk = LittleFS.begin(true);   // true = ฟอร์แมตให้ถ้ายังไม่เคยใช้
  loadAll();
  if (!fsOk) addLog("⚠ LittleFS ใช้ไม่ได้ (เลือก Partition Scheme ที่มี SPIFFS)");

  doorClosed = doorRawLast = readDoorClosed();
  doorOpenedAt = millis();
  addLog(String("เปิดเครื่อง ประตู") + (doorClosed ? "ปิดอยู่" : "เปิดอยู่"));

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("camlock");
  WiFi.setSleep(false);                 // ปิดโหมดประหยัดไฟ ลดการหลุด
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWifiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
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
  processSlipJob();
  if (millis() - lastHousekeep > 10000) { lastHousekeep = millis(); housekeeping(); }
  delay(2);
}
