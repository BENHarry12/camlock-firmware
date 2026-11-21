/* ------------------------------------------------------------------
   CamLock — ตู้เช่ากล้องอัตโนมัติ
   ESP32 DevKit V1  +  ThingsBoard (MQTT)

   ขาที่ใช้ (ตรงกับบอร์ด CAMERA LOCKER v1.0)
     GPIO26  RELAY_DRV   ออก   สั่งรีเลย์ (HIGH = กลอนเปิด)
     GPIO27  DOOR_GPIO   เข้า  reed switch  (LOW = ประตูปิด)
     GPIO25  STATUS      ออก   ไฟแสดงสถานะ

   ไลบรารีที่ต้องติดตั้งใน Arduino IDE
     - PubSubClient   (Nick O'Leary)
     - ArduinoJson    (Benoit Blanchon)

   คำเตือน: โค้ดนี้เขียนตามสเปกของบอร์ดและเอกสาร ThingsBoard
   ยังไม่เคยรันบนบอร์ดจริง เพราะบอร์ดยังไม่ได้สั่งผลิต
   ------------------------------------------------------------------ */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

/* ---------- ตั้งค่าตรงนี้ ---------- */
const char* WIFI_SSID = "ชื่อ Wi-Fi";
const char* WIFI_PASS = "รหัส Wi-Fi";

const char* TB_HOST   = "thingsboard.cloud";   // หรือ IP ของเซิร์ฟเวอร์ที่ติดตั้งเอง
const int   TB_PORT   = 1883;
const char* TB_TOKEN  = "ใส่ Access Token ของ device";

const char* LOCKER_ID = "A";
const char* FIRMWARE  = "1.0.0";
/* ---------------------------------- */

#define RELAY_PIN   26
#define DOOR_PIN    27
#define STATUS_LED  25

const unsigned long TELEMETRY_MS = 10000;   // ส่งค่าทุก 10 วินาที
const unsigned long DEFAULT_OPEN_MS = 3000; // กลอนเปิดค้าง 3 วินาที

WiFiClient   net;
PubSubClient mqtt(net);

bool          doorOpen     = false;
bool          lastDoorOpen = false;
bool          lockActive   = false;
uint32_t      unlockCount  = 0;
unsigned long lastTelemetry = 0;
unsigned long lockOffAt     = 0;   // 0 = กลอนไม่ได้ทำงานอยู่

/* ---------- ส่งข้อมูลขึ้น ThingsBoard ---------- */
void publishTelemetry() {
  StaticJsonDocument<192> doc;
  doc["doorOpen"]    = doorOpen;
  doc["lockActive"]  = lockActive;
  doc["rssi"]        = WiFi.RSSI();
  doc["uptime"]      = (uint32_t)(millis() / 1000);
  doc["unlockCount"] = unlockCount;

  char buf[192];
  size_t n = serializeJson(doc, buf);
  mqtt.publish("v1/devices/me/telemetry", buf, n);
}

void publishAttributes() {
  StaticJsonDocument<192> doc;
  doc["lockerId"] = LOCKER_ID;
  doc["firmware"] = FIRMWARE;
  doc["ip"]       = WiFi.localIP().toString();

  char buf[192];
  size_t n = serializeJson(doc, buf);
  mqtt.publish("v1/devices/me/attributes", buf, n);
}

/* ---------- สั่งเปิดกลอน ---------- */
void openLocker(unsigned long ms) {
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(STATUS_LED, HIGH);
  lockActive = true;
  lockOffAt  = millis() + ms;
  unlockCount++;
  publishTelemetry();
}

/* ---------- รับคำสั่ง RPC จากแดชบอร์ด ---------- */
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  String t(topic);
  if (t.indexOf("v1/devices/me/rpc/request/") != 0) return;

  String requestId = t.substring(strlen("v1/devices/me/rpc/request/"));

  StaticJsonDocument<256> req;
  if (deserializeJson(req, payload, len)) return;      // JSON เสีย ไม่ต้องทำอะไร

  const char* method = req["method"];
  if (!method) return;

  StaticJsonDocument<128> res;

  if (strcmp(method, "openLocker") == 0) {
    unsigned long ms = req["params"]["ms"] | DEFAULT_OPEN_MS;
    if (ms < 500)   ms = 500;         // กันค่าที่สั้นจนกลอนไม่ทัน
    if (ms > 10000) ms = 10000;       // กันค่าที่ยาวจนคอยล์ร้อน
    openLocker(ms);
    res["ok"] = true;
    res["ms"] = (uint32_t)ms;

  } else if (strcmp(method, "getStatus") == 0) {
    res["doorOpen"]   = doorOpen;
    res["lockActive"] = lockActive;

  } else {
    res["ok"]    = false;
    res["error"] = "unknown method";
  }

  char buf[128];
  size_t n = serializeJson(res, buf);
  String topicOut = "v1/devices/me/rpc/response/" + requestId;
  mqtt.publish(topicOut.c_str(), buf, n);
}

/* ---------- เชื่อมต่อ ---------- */
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));   // กะพริบระหว่างรอ
    delay(300);
  }
  digitalWrite(STATUS_LED, LOW);
}

void connectMqtt() {
  while (!mqtt.connected()) {
    String clientId = "camlock-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    // username = access token, password เว้นว่าง
    if (mqtt.connect(clientId.c_str(), TB_TOKEN, NULL)) {
      mqtt.subscribe("v1/devices/me/rpc/request/+");
      publishAttributes();
      publishTelemetry();
    } else {
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);          // ตั้งเป็น LOW ก่อนเสมอ กันกลอนเปิดตอนบูต
  pinMode(STATUS_LED, OUTPUT);
  pinMode(DOOR_PIN, INPUT_PULLUP);       // บนบอร์ดมี R5 10k ดึงขึ้นอยู่แล้ว

  connectWifi();
  mqtt.setServer(TB_HOST, TB_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  connectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWifi();
  if (!mqtt.connected())             connectMqtt();
  mqtt.loop();

  // ปิดกลอนเมื่อครบเวลา
  if (lockOffAt && (long)(millis() - lockOffAt) >= 0) {
    digitalWrite(RELAY_PIN, LOW);
    digitalWrite(STATUS_LED, LOW);
    lockActive = false;
    lockOffAt  = 0;
    publishTelemetry();
  }

  // reed switch: LOW = ประตูปิด, HIGH = ประตูเปิด
  doorOpen = (digitalRead(DOOR_PIN) == HIGH);
  if (doorOpen != lastDoorOpen) {
    lastDoorOpen = doorOpen;
    publishTelemetry();                 // ส่งทันทีเมื่อสถานะประตูเปลี่ยน
  }

  if (millis() - lastTelemetry >= TELEMETRY_MS) {
    lastTelemetry = millis();
    publishTelemetry();
  }
}
