// ═══════════════════════════════════════════════════════════
//  Heart Rate Monitor — ESP32 + MAX30102 + OLED + Blynk + Firebase
//  Sensor + OLED : SDA=21, SCL=22  (Wire / I2C Bus 0 ร่วมกัน)
// ═══════════════════════════════════════════════════════════

#define BLYNK_TEMPLATE_ID   "TMPL6Zbm4Q3_J"
#define BLYNK_TEMPLATE_NAME "Heart Rate Monitor"
#define BLYNK_AUTH_TOKEN    "F5qkG105qMpLBrbqtE9U6M0gXbgQzjGM"
#define BLYNK_PRINT         Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <time.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ─── WiFi ────────────────────────────────────────────────
char ssid[] = "Banyapon";
char pass[] = "12345678";

// ─── Firebase ────────────────────────────────────────────
#define FIREBASE_API_KEY      "AIzaSyABM0WLTw0PXm72H54QzQKTY3p5prW4I18"
#define FIREBASE_DATABASE_URL "https://heartrate-db-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_USER_EMAIL   "jadoo@gmail.com"
#define FIREBASE_USER_PASS    "jadoo2569"

FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;
bool firebaseReady = false;

// ─── I2C ร่วมกัน (SDA=21, SCL=22) ───────────────────────
#define OLED_ADDR 0x3C

// ─── Sensor ──────────────────────────────────────────────
MAX30105 particleSensor;
bool sensorReady = false;

const byte RATE_SIZE = 4;
byte  rates[RATE_SIZE];
byte  rateSpot       = 0;
long  lastBeat       = 0;
float beatsPerMinute = 0;
int   beatAvg        = 0;
bool  fingerOn       = false;

BlynkTimer timer;

// ─── OLED ────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool oledReady = false;

// ─── Layout หน้าจอ ────────────────────────────────────────
//
//  ┌──────────────────────────────┐  Y=0
//  │ ♥ 75 bpm      [ NORMAL ]    │  Header 15px
//  ├──────────────────────────────┤  Y=15
//  │100 ┤·····················    │  }
//  │    │          ╭─╮            │  } Graph
//  │ 60 ┤- - - - -╯  ╰- - - -    │  } 49px
//  │    │                    ●    │  } (dot = ค่าปัจจุบัน)
//  └────┴────────────────────────┘  Y=63
//       X=22                X=127
//
#define HEADER_H   15          // ความสูง header
#define GRAPH_X    22          // เริ่มกราฟ (เว้นซ้ายให้ label Y)
#define GRAPH_Y    HEADER_H    // กราฟเริ่มต่อจาก header
#define GRAPH_W   (128 - GRAPH_X)  // = 106 px
#define GRAPH_H   (64  - GRAPH_Y)  // = 49 px
#define BPM_MIN    40
#define BPM_MAX   180

// ─── กราฟ BPM ────────────────────────────────────────────
int  bpmGraph[GRAPH_W];
int  graphIndex  = 0;
bool graphFilled = false;

// ─── Zone ────────────────────────────────────────────────
// LOW  : BPM < 60
// NORMAL: 60 <= BPM <= 100
// HIGH : BPM > 100
enum Zone { ZONE_NONE, ZONE_LOW, ZONE_NORMAL, ZONE_HIGH };

Zone getZone(int bpm) {
  if (bpm <= 0)    return ZONE_NONE;
  if (bpm < 60)    return ZONE_LOW;
  if (bpm <= 100)  return ZONE_NORMAL;
  return ZONE_HIGH;
}

// ═══════════════════════════════════════════════════════════
//  Timestamp (NTP +7)
// ═══════════════════════════════════════════════════════════
String getTimestamp() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[30];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", t);
  return String(buf);
}

// ═══════════════════════════════════════════════════════════
//  วาดกราฟบน OLED
// ═══════════════════════════════════════════════════════════
void drawGraph(int bpm) {
  if (!oledReady) return;

  // เก็บค่าลง ring buffer
  bpmGraph[graphIndex] = bpm;
  graphIndex = (graphIndex + 1) % GRAPH_W;
  if (graphIndex == 0) graphFilled = true;

  display.clearDisplay();

  // ══════════════════════════════════════════════
  //  HEADER — แถวบนสุด
  // ══════════════════════════════════════════════
  display.setTextColor(SSD1306_WHITE);

  if (!fingerOn) {
    // ── ยังไม่วางนิ้ว ─────────────────────────
    display.setTextSize(1);
    display.setCursor(0, 4);
    display.print("Place finger...");
  } else {
    // ── แสดง BPM ─────────────────────────────
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.printf("%d", bpm);

    // ── แสดง "bpm" ขนาดเล็ก ──────────────────
    display.setTextSize(1);
    int bpmLabelX = (bpm >= 100) ? 38 : 26;
    display.setCursor(bpmLabelX, 6);
    display.print("bpm");

    // ── Zone Badge ────────────────────────────
    Zone z = getZone(bpm);
    const char* zoneStr = "";
    int badgeX = 72;
    int badgeW = 54;

    if      (z == ZONE_LOW)    zoneStr = " LOW  ";
    else if (z == ZONE_NORMAL) zoneStr = "NORMAL";
    else if (z == ZONE_HIGH)   zoneStr = " HIGH ";

    if (z != ZONE_NONE) {
      // กรอบ badge
      display.drawRoundRect(badgeX, 1, badgeW, 12, 3, SSD1306_WHITE);

      if (z == ZONE_HIGH) {
        // HIGH → พื้นขาว ตัวอักษรดำ (เน้นเตือน)
        display.fillRoundRect(badgeX, 1, badgeW, 12, 3, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.setTextColor(SSD1306_WHITE);
      }

      display.setTextSize(1);
      display.setCursor(badgeX + 3, 3);
      display.print(zoneStr);
      display.setTextColor(SSD1306_WHITE); // reset
    }
  }

  // เส้นคั่น header / กราฟ
  display.drawLine(0, HEADER_H - 1, 127, HEADER_H - 1, SSD1306_WHITE);

  // ══════════════════════════════════════════════
  //  Y-AXIS LABELS (ซ้ายกราฟ)
  // ══════════════════════════════════════════════
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // คำนวณตำแหน่ง Y สำหรับ label แต่ละค่า
  auto yPos = [](int val) -> int {
    return map(val, BPM_MIN, BPM_MAX, GRAPH_Y + GRAPH_H - 1, GRAPH_Y);
  };

  // label 100
  int y100 = yPos(100);
  display.setCursor(0, y100 - 3);
  display.print("100");

  // label 60
  int y60 = yPos(60);
  display.setCursor(4, y60 - 3);
  display.print("60");

  // เส้น Y-axis
  display.drawLine(GRAPH_X - 2, GRAPH_Y,
                   GRAPH_X - 2, GRAPH_Y + GRAPH_H - 1, SSD1306_WHITE);

  // เส้น X-axis (ล่างสุด)
  display.drawLine(GRAPH_X - 2, GRAPH_Y + GRAPH_H - 1,
                   127,         GRAPH_Y + GRAPH_H - 1, SSD1306_WHITE);

  // ══════════════════════════════════════════════
  //  เส้น REFERENCE (เส้นประ)
  // ══════════════════════════════════════════════
  // เส้น 100 BPM (เส้นประถี่)
  for (int x = GRAPH_X; x < 128; x += 3)
    display.drawPixel(x, y100, SSD1306_WHITE);

  // เส้น 60 BPM (เส้นประห่าง)
  for (int x = GRAPH_X; x < 128; x += 6)
    display.drawPixel(x, y60, SSD1306_WHITE);

  // ══════════════════════════════════════════════
  //  ZONE SHADING — แรเงาโซน NORMAL (60-100)
  //  ทุก 4 pixel แนวตั้ง เพื่อให้ดูเป็น shade อ่อนๆ
  // ══════════════════════════════════════════════
  for (int y = y100 + 1; y < y60; y += 3)
    display.drawLine(GRAPH_X, y, 127, y, SSD1306_WHITE);

  // ลบ pixel ที่ชนกับ reference line ออก (ทำให้เส้นประชัด)
  for (int x = GRAPH_X; x < 128; x++) {
    display.drawPixel(x, y100, SSD1306_WHITE);
    display.drawPixel(x, y60,  SSD1306_WHITE);
  }

  // ══════════════════════════════════════════════
  //  วาดเส้นกราฟ BPM
  // ══════════════════════════════════════════════
  int totalPoints = graphFilled ? GRAPH_W : graphIndex;
  if (totalPoints >= 2) {
    for (int i = 0; i < GRAPH_W - 1; i++) {
      int idxA = (graphIndex + i)     % GRAPH_W;
      int idxB = (graphIndex + i + 1) % GRAPH_W;

      if (!graphFilled) {
        int age = (graphIndex - i - 1 + GRAPH_W) % GRAPH_W;
        if (age >= totalPoints) continue;
      }

      int valA = bpmGraph[idxA];
      int valB = bpmGraph[idxB];
      if (valA == 0 || valB == 0) continue; // ข้ามช่วงที่ยังไม่มีข้อมูล

      int y1 = yPos(constrain(valA, BPM_MIN, BPM_MAX));
      int y2 = yPos(constrain(valB, BPM_MIN, BPM_MAX));

      display.drawLine(GRAPH_X + i, y1, GRAPH_X + i + 1, y2, SSD1306_WHITE);
    }
  }

  // ══════════════════════════════════════════════
  //  จุดค่าปัจจุบัน (ขวาสุดของกราฟ)
  // ══════════════════════════════════════════════
  if (fingerOn && bpm > 0) {
    int yCurrent = yPos(constrain(bpm, BPM_MIN, BPM_MAX));
    // วงกลมเล็กๆ ขนาด r=2
    display.fillCircle(127, yCurrent, 2, SSD1306_WHITE);
  }

  display.display();
}

// ═══════════════════════════════════════════════════════════
//  ส่งข้อมูล Blynk + Firebase (ทุก 2 วินาที)
// ═══════════════════════════════════════════════════════════
void sendData() {
  int valueToSend = (fingerOn && beatAvg > 0) ? beatAvg : 0;

  if (Blynk.connected()) {
    Blynk.virtualWrite(V0, valueToSend);
    Serial.printf(">>> Blynk V0 = %d BPM\n", valueToSend);
  }

  if (!firebaseReady || !Firebase.ready()) return;

  String ts = getTimestamp();

  FirebaseJson current;
  current.set("bpm",       valueToSend);
  current.set("fingerOn",  fingerOn);
  current.set("timestamp", ts);
  Firebase.RTDB.setJSON(&fbdo, "/heartrate/current", &current);

  if (valueToSend > 0) {
    FirebaseJson history;
    history.set("bpm",       valueToSend);
    history.set("timestamp", ts);
    Firebase.RTDB.pushJSON(&fbdo, "/heartrate/history", &history);
  }
}

// ═══════════════════════════════════════════════════════════
//  Setup
// ═══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);

  // ── 1. I2C Bus เดียว (SDA=21, SCL=22, 100kHz) ────────────
  Wire.begin(21, 22, 100000);
  delay(100);

  // ── 2. I2C Scanner ───────────────────────────────────────
  Serial.println("Scanning I2C...");
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0)
      Serial.printf("  Found: 0x%02X\n", addr);
  }
  Serial.println("Scan done.");

  // ── 3. MAX30102 (addr 0x57) ──────────────────────────────
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("❌ MAX30102 not found!");
  } else {
    particleSensor.setup();
    particleSensor.setPulseAmplitudeRed(0x1F);
    particleSensor.setPulseAmplitudeIR(0x1F);
    sensorReady = true;
    Serial.println("✅ MAX30102 Ready");
  }

  // ── 4. OLED (addr 0x3C) ──────────────────────────────────
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.printf("❌ OLED not found at 0x%02X\n", OLED_ADDR);
  } else {
    oledReady = true;
    Serial.println("✅ OLED Ready");

    memset(bpmGraph, 0, sizeof(bpmGraph));

    // Splash screen
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(15, 20);
    display.println("Heart Rate Monitor");
    display.setCursor(35, 40);
    display.println("Initializing...");
    display.display();
  }

  // ── 5. WiFi ──────────────────────────────────────────────
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi Connected");

  // ── 6. NTP (UTC+7) ───────────────────────────────────────
  configTime(7 * 3600, 0, "pool.ntp.org");

  // ── 7. Blynk ─────────────────────────────────────────────
  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect();

  // ── 8. Firebase ──────────────────────────────────────────
  config.api_key      = FIREBASE_API_KEY;
  config.database_url = FIREBASE_DATABASE_URL;
  auth.user.email     = FIREBASE_USER_EMAIL;
  auth.user.password  = FIREBASE_USER_PASS;
  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  firebaseReady = true;
  Serial.println("✅ Firebase Initialized");

  // ── 9. Timer ─────────────────────────────────────────────
  timer.setInterval(2000L, sendData);

  delay(1500);
}

// ═══════════════════════════════════════════════════════════
//  Loop
// ═══════════════════════════════════════════════════════════
void loop() {
  if (Blynk.connected()) Blynk.run();
  timer.run();

  if (!sensorReady) {
    drawGraph(0);
    delay(200);
    return;
  }

  long irValue = particleSensor.getIR();

  if (irValue < 50000) {
    fingerOn = false;
    beatAvg  = 0;
    drawGraph(0);
    delay(200);
    return;
  }

  fingerOn = true;

  if (checkForBeat(irValue)) {
    long delta     = millis() - lastBeat;
    lastBeat       = millis();
    beatsPerMinute = 60.0 / (delta / 1000.0);

    if (beatsPerMinute > 40 && beatsPerMinute < 180) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;

      beatAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++) beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
    }

    Serial.printf("IR=%ld  BPM=%.1f  Avg=%d  Zone=%s\n",
      irValue, beatsPerMinute, beatAvg,
      beatAvg < 60 ? "LOW" : beatAvg <= 100 ? "NORMAL" : "HIGH");
  }

  drawGraph(beatAvg);
}
