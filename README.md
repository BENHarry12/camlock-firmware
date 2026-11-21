# CamLock Firmware — ตู้เช่ากล้องอัตโนมัติ (ESP32 + ThingsBoard)

เฟิร์มแวร์ ESP32 ของตู้ล็อกเกอร์ให้เช่ากล้องสำหรับนักศึกษา ไม่ต้องมีคนเฝ้า
วิชา CPE23367 · มหาวิทยาลัยศรีปทุม

- **แอป (GitHub Pages):** https://benharry12.github.io/camlock-app/
- **Repo แอป:** https://github.com/BENHarry12/camlock-app

## ปัญหาที่แก้

ห้องแล็บให้ยืมกล้องด้วยสมุดจดและกุญแจ ทำให้เกิดปัญหาสามข้อ

1. ยืมได้เฉพาะเวลาที่มีเจ้าหน้าที่อยู่
2. ไม่รู้ว่าใครถือกุญแจอยู่ตอนนี้
3. กล้องหายแล้วย้อนหาคนรับผิดชอบไม่ได้

CamLock เปลี่ยนให้เป็นตู้ที่นักศึกษาสมัคร จอง จ่ายเงิน และเปิดตู้เองจากแอป
ทุกการเปิดผูกกับบัญชีผู้ใช้และบันทึกไว้ ผู้ดูแลเห็นสถานะประตูแบบเรียลไทม์

## ภาพรวมระบบ

```
มือถือนักศึกษา ── HTTPS ──> ThingsBoard Cloud ── MQTT ──> ESP32 (ตู้)
  (GitHub Pages)   RPC / อ่าน telemetry        RPC / telemetry      กลอน + reed switch
```

ESP32 ทำหน้าที่เป็น "หลังบ้าน" ของตู้ทั้งหมด: เก็บบัญชีสมาชิก รายการกล้อง ประวัติการเช่า
สถานะการชำระเงิน และสั่งกลอน ข้อมูลเก็บใน flash (NVS) ไฟดับก็ไม่หาย
ThingsBoard เป็นตัวกลางส่งคำสั่ง (RPC) จากแอปมาที่บอร์ด และเก็บสถานะตู้ (telemetry)

## ฮาร์ดแวร์ (แผง CAMERA LOCKER v1.0)

บอร์ดออกแบบเอง 100 × 80 มม. สองชั้น ผลิตที่ JLCPCB — ดูผังอุปกรณ์ที่ [`docs/CAMLOCK_board_guide.png`](docs/CAMLOCK_board_guide.png),
ภาพ 3 มิติ [`docs/CAMLOCK_board_3D.png`](docs/CAMLOCK_board_3D.png), ผังการวาง [`docs/CAMLOCK_floorplan.png`](docs/CAMLOCK_floorplan.png)

| ส่วน | อุปกรณ์ |
|---|---|
| ตัวประมวลผล | ESP32-WROOM-32E (Flash 4MB) |
| ภาคไฟ | DC 12V → K7805-1000R3 → 5V → AMS1117-3.3 → 3.3V |
| ขับกลอน | S8050 → รีเลย์ SRD-05VDC-SL-C → กลอนโซลินอยด์ 12V |
| อ่านประตู | รีดสวิตช์แม่เหล็ก แบบ NO |
| โปรแกรม | USB-C + CH340C |
| ป้องกัน | ฟิวส์ PTC 1.85A · ไดโอดกันขั้วกลับ · ไดโอดฟลายแบ็ก 2 จุด |

| คอนเนกเตอร์ | ต่อกับ |
|---|---|
| J1 / J2 | ไฟเลี้ยง 12V (แจ็ค DC 5.5×2.1 มม. หรือเทอร์มินัล) |
| J3 | กลอนไฟฟ้าโซลินอยด์ 12V |
| J4 | รีดสวิตช์แม่เหล็กที่ขอบประตู |
| USBC1 | สาย USB สำหรับอัปโหลดโค้ด |

| ขา | ชื่อบนแผง | ทิศ | หน้าที่ |
|---|---|---|---|
| GPIO26 | RELAY_DRV | ออก | สั่งรีเลย์ HIGH = กลอนเปิด |
| GPIO27 | DOOR_GPIO | เข้า | reed switch LOW = ประตูปิด (R5 10k ดึงขึ้น) |
| GPIO25 | STATUS | ออก | ไฟสถานะ |

ไฟสถานะ: ติดค้าง = ออนไลน์กับ ThingsBoard · กะพริบช้า = มี WiFi แต่ยังต่อ ThingsBoard ไม่ได้ · กะพริบเร็ว = ประตูเปิดค้าง · แวบ ๆ = ไม่มี WiFi

## ติดตั้ง

1. Arduino IDE → Boards Manager → **esp32 by Espressif** (core 2.x หรือ 3.x) → บอร์ด **ESP32 Dev Module**
2. Library Manager → ติดตั้ง **PubSubClient** (Nick O'Leary) และ **ArduinoJson** (Benoit Blanchon รุ่น 6 หรือ 7)
3. เปิด `src/camlock/camlock.ino` แก้ส่วน "ตั้งค่าตรงนี้"

```cpp
#define WIFI_SSID  "ESP_CAMLOCK"
#define WIFI_PASS  "0649682424"
#define TB_HOST    "thingsboard.cloud"
#define TB_TOKEN   "<Access token ของอุปกรณ์>"
#define LOCKER_ID  "A"
```

   ยังไม่ได้ติด reed switch? ปล่อย `#define HAS_REED_SWITCH 0` ไว้ ระบบจะถือว่าประตูปิดและล็อกเอง
   หลังปลดล็อก 10 วินาที (`AUTO_CLOSE_MS`) ผู้ใช้ต้องปิดประตูภายในเวลานี้ ติด reed switch ที่ J4 แล้วเปลี่ยนเป็น `1`
4. Upload แล้วเปิด Serial Monitor (115200) ต้องเห็น `เชื่อม ThingsBoard แล้ว`
5. เปิดแอป เข้าระบบ **admin / 1234** → ตั้งค่า → เปลี่ยนรหัสผ่านทันที ใส่เลขพร้อมเพย์ และแก้รายการกล้อง

## คำสั่ง RPC (ThingsBoard two-way RPC)

แอปเรียก `POST /api/plugins/rpc/twoway/{deviceId}` ด้วย `{"method": "...", "params": {...}}`
ทุกคำสั่งตอบเป็น JSON ที่มี `ok` เสมอ ถ้าผิดพลาดจะมี `code` และ `msg` (ภาษาไทย)
คำสั่งที่ต้องเข้าสู่ระบบ ให้ส่ง `t` (token ที่ได้จาก `login`) มาใน params

| method | สิทธิ์ | params | หน้าที่ |
|---|---|---|---|
| `status` | ทุกคน | – | สถานะตู้ทั้งหมด |
| `getStatus` | ทุกคน | – | `{doorOpen, lockActive}` (รูปแบบเดิม ใช้กับวิดเจ็ตแดชบอร์ด) |
| `register` | ทุกคน | sid, name, phone, pass | สมัครสมาชิก (ได้ token กลับ) |
| `login` | ทุกคน | sid, pass | เข้าสู่ระบบ (ได้ token กลับ) |
| `logout` | ผู้ใช้ | t | ออกจากระบบ |
| `me` | ผู้ใช้ | t | ข้อมูลตัวเอง รายการเช่า และกล้อง |
| `rent` | ผู้ใช้ | t, cam, hours | จองกล้อง |
| `pay` | ผู้ใช้ | t, rid | แจ้งว่าโอนเงินแล้ว |
| `cancel` | ผู้ใช้ | t, rid | ยกเลิกก่อนรับกล้อง |
| `open` | ผู้ใช้ | t, rid | ปลดล็อกเพื่อรับ (ถ้าชำระแล้ว) หรือคืนกล้อง |
| `extend` | ผู้ใช้ | t, rid, hours | ต่อเวลา 1–24 ชม. |
| `password` | ผู้ใช้ | t, old, new | เปลี่ยนรหัสผ่าน |
| `adminOverview` | ผู้ดูแล | t | สมาชิก กล้อง การเช่าทั้งหมด |
| `adminUnlock` | ผู้ดูแล | t | ปลดล็อกทันที |
| `adminRental` | ผู้ดูแล | t, rid, action | confirm / reject / cancel / returned / refund / delete |
| `adminCamera` | ผู้ดูแล | t, id?, name, lens, specs, price, deposit, status | เพิ่ม/แก้กล้อง |
| `adminCameraDelete` | ผู้ดูแล | t, id | ลบกล้อง |
| `adminUser` | ผู้ดูแล | t, idx, action, pass? | disable / enable / admin / unadmin / password / delete |
| `adminSettings` / `adminSettingsSave` | ผู้ดูแล | t, lockerName, lockerLoc, lockerNote, promptpay, approve, unlockMs, alarmSec | อ่าน/บันทึกการตั้งค่า |
| `adminLog` | ผู้ดูแล | t | บันทึกการทำงาน 50 รายการล่าสุด |

> เวอร์ชันนี้ **เลิกใช้คำสั่ง `openLocker`** ของ v1 เพราะใครที่ส่ง RPC ได้ก็เปิดตู้ได้ทันที
> การเปิดตู้ต้องผ่าน `open` (นักศึกษาที่ชำระเงินแล้ว) หรือ `adminUnlock` (ผู้ดูแลที่เข้าสู่ระบบ) เท่านั้น

## Telemetry และ Attributes

ส่งทุก 10 วินาที และทันทีที่ประตู/กลอนเปลี่ยนสถานะ

- **Telemetry:** `doorOpen`, `lockActive`, `alarm`, `returning`, `available`, `camCount`, `unlockCount`, `rssi`, `uptime`, `heap`
- **Client attributes:** `lockerId`, `firmware`, `ip`, `ssid`, `lockerName`, `lockerLoc`, `lockerNote`, `promptpay`, `approve`, `unlockMs`

## ขั้นตอนการเช่า (สถานะในบอร์ด)

`await` รอชำระ → `verify` แจ้งโอนแล้ว รอผู้ดูแลยืนยัน → `ready` พร้อมรับ → `active` กำลังเช่า (เริ่มนับเวลาตอนเปิดตู้) → `returned` คืนแล้ว (บันทึกเองเมื่อ reed switch บอกว่าปิดประตู)

- ไม่ชำระภายใน 30 นาที ยกเลิกเอง
- คืนช้าเกิน 10 นาที คิดค่าปรับชั่วโมงละเท่าค่าเช่า หักจากมัดจำ
- ปิดการยืนยันโดยผู้ดูแลได้ในตั้งค่า (ไม่แนะนำ เพราะบอร์ดเช็คยอดเงินเข้าบัญชีเองไม่ได้)

## ความปลอดภัย

- รีเลย์ตั้งเป็น OFF ก่อนอย่างอื่นตอนบูต กันกลอนเปิดเอง
- จ่ายไฟกลอนได้ 0.5–8 วินาที และพักอย่างน้อย 2 วินาทีระหว่างครั้ง กันคอยล์ร้อน
- การต่อ WiFi/MQTT ไม่บล็อก loop กลอนจึงปิดตรงเวลาแม้เน็ตหลุด · Watchdog 15 วินาที
- รหัสผ่านเก็บเป็น SHA-256 + salt ทำซ้ำ 1000 รอบ · ใส่รหัสผิด 5 ครั้ง ล็อก 60 วินาที
- บันทึกเตือนเมื่อประตูถูกเปิดโดยไม่มีคำสั่ง และเมื่อประตูเปิดค้าง

## ข้อจำกัด

- เน็ตหรือ ThingsBoard ล่ม = เปิดตู้จากแอปไม่ได้ ต้องมีกุญแจสำรองเชิงกล
- `TB_TOKEN` อยู่ในโค้ด ใครได้ token ไปก็ส่งข้อมูลแทนตู้ได้ (แต่สั่งเปิดตู้ไม่ได้) ถ้า token หลุดให้กดสร้างใหม่ใน ThingsBoard
- ตู้มีช่องเดียว ไม่มีเซ็นเซอร์ตรวจว่ามีกล้องอยู่ในช่อง ควรมีกล้องวงจรปิดจ่อตู้
- โค้ดผ่านการตรวจคอมไพล์ (ESP32 core 2.x/3.x, ArduinoJson 6/7) แต่ยังไม่เคยรันบนบอร์ดจริง เพราะแผงยังไม่ได้สั่งผลิต

## โครงสร้างไฟล์

```
camlock-firmware/
├── src/camlock/camlock.ino   เฟิร์มแวร์ทั้งหมด
└── docs/                     ผังบอร์ด ภาพ 3 มิติ ผังการวาง
```

## คณะผู้จัดทำ

อาจารย์ที่ปรึกษา: นิมิตร ทักษวิทยาพงศ์

| รหัสนักศึกษา | ชื่อ |
|---|---|
| 68019652 | ภานุวัฒน์ ลิ้มประยูร |
| 68092262 | กฤตยชญ์ มีหล้า |
| 68001021 | กาณุพงศ์ แก้วอนันต์ |
| 68062253 | ณัฐภัทร ศรีพุฒ |
