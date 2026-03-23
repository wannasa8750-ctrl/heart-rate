#define BLYNK_TEMPLATE_ID "TMPL6Zbm4Q3_J"
#define BLYNK_TEMPLATE_NAME "Heart Rate Monitor"
#define BLYNK_AUTH_TOKEN "F5qkG105qMpLBrbqtE9U6M0gXbgQzjGM"
#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <time.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ── WiFi ──────────────────────────────────────────────
char ssid[] = "Banyapon";
char pass[] = "12345678";

// ── Firebase ──────────────────────────────────────────
#define FIREBASE_API_KEY      "AIzaSyABM0WLTw0PXm72H54QzQKTY3p5prW4I18"
#define FIREBASE_DATABASE_URL "https://heartrate-db-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_USER_EMAIL   "jadoo@gmail.com"
#define FIREBASE_USER_PASS    "jadoo2569"

FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;
bool firebaseReady = false;

// ── Sensor ────────────────────────────────────────────
MAX30105 particleSensor;
const byte RATE_SIZE = 4;
byte  rates[RATE_SIZE];
byte  rateSpot       = 0;
long  lastBeat       = 0;
float beatsPerMinute = 0;
int   beatAvg        = 0;
bool  fingerOn       = false;

BlynkTimer timer;

// ── แปลง timestamp เป็น วัน/เดือน/ปี เวลา ─────────────
String getTimestamp() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[30];
  // รูปแบบ: 21/03/2026 14:35:00
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", t);
  return String(buf);
}

// ── ส่งข้อมูล ─────────────────────────────────────────
void sendData() {
  int valueToSend = (fingerOn && beatAvg > 0) ? beatAvg : 0;

  // --- Blynk ---
  Blynk.virtualWrite(V0, valueToSend);
  Serial.printf(">>> Blynk V0 = %d BPM\n", valueToSend);

  if (!firebaseReady || !Firebase.ready()) return;

  String ts = getTimestamp();

  // --- Firebase: current (เขียนทับ ดูค่าล่าสุด) ---
  FirebaseJson current;
  current.set("bpm",       valueToSend);
  current.set("fingerOn",  fingerOn);
  current.set("timestamp", ts);

  bool ok = Firebase.RTDB.setJSON(&fbdo, "/heartrate/current", &current);
  if (!ok && fbdo.errorReason().indexOf("timed out") >= 0) {
    delay(500);
    ok = Firebase.RTDB.setJSON(&fbdo, "/heartrate/current", &current);
  }
  Serial.println(ok ? ">>> Firebase current ✅" :
    ">>> Firebase current ❌ " + fbdo.errorReason());

  // --- Firebase: history (push เก็บทุกค่า > 0) ---
  if (valueToSend > 0) {
    FirebaseJson history;
    history.set("bpm",       valueToSend);
    history.set("timestamp", ts);

    ok = Firebase.RTDB.pushJSON(&fbdo, "/heartrate/history", &history);
    if (!ok && fbdo.errorReason().indexOf("timed out") >= 0) {
      delay(500);
      ok = Firebase.RTDB.pushJSON(&fbdo, "/heartrate/history", &history);
    }
    if (ok) {
      Serial.printf(">>> Firebase history ✅  [%s] %d BPM\n",
                    ts.c_str(), valueToSend);
    } else {
      Serial.println(">>> Firebase history ❌ " + fbdo.errorReason());
    }
  }
}

// ── Setup ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(100);

  // WiFi
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, pass);
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++timeout > 30) {
      Serial.println("\n❌ WiFi Failed!");
      ESP.restart();
    }
  }
  Serial.print("\n✅ WiFi Connected! IP: ");
  Serial.println(WiFi.localIP());

  // NTP — ดึงเวลาจริง GMT+7 ไทย
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing time");
  while (time(nullptr) < 100000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Time synced: " + getTimestamp());

  // Blynk
  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect(5000);
  Serial.println(Blynk.connected() ? "✅ Blynk Connected!" : "⚠️ Blynk offline");

  // Firebase
  config.api_key                = FIREBASE_API_KEY;
  config.database_url           = FIREBASE_DATABASE_URL;
  config.token_status_callback  = tokenStatusCallback;
  config.timeout.serverResponse = 10 * 1000;
  fbdo.setResponseSize(4096);
  Firebase.setDoubleDigits(5);

  auth.user.email    = FIREBASE_USER_EMAIL;
  auth.user.password = FIREBASE_USER_PASS;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.print("Waiting for Firebase token");
  unsigned long t = millis();
  while (!Firebase.ready() && millis() - t < 10000) {
    delay(300);
    Serial.print(".");
  }
  firebaseReady = Firebase.ready();
  Serial.println(firebaseReady ? "\n✅ Firebase Ready!" : "\n⚠️ Firebase offline");

  // Sensor
  Wire.begin(21, 22);
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("❌ MAX30102 not found!");
    while (1);
  }
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x1F);
  particleSensor.setPulseAmplitudeGreen(0);
  particleSensor.setPulseAmplitudeIR(0x1F);

  timer.setInterval(2000L, sendData);
  Serial.println("✅ Sensor Ready! Place your finger.");
}

// ── Loop ──────────────────────────────────────────────
void loop() {
  if (Blynk.connected()) Blynk.run();
  timer.run();

  long irValue = particleSensor.getIR();

  if (irValue < 50000) {
    if (fingerOn) {
      fingerOn       = false;
      beatsPerMinute = 0;
      beatAvg        = 0;
      for (byte x = 0; x < RATE_SIZE; x++) rates[x] = 0;
      rateSpot = 0;
      Serial.println("Finger removed.");
    }
    delay(500);
    return;
  }

  fingerOn = true;

  if (checkForBeat(irValue)) {
    long delta     = millis() - lastBeat;
    lastBeat       = millis();
    beatsPerMinute = 60 / (delta / 1000.0);

    if (beatsPerMinute > 40 && beatsPerMinute < 180) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;
      beatAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++) beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
    }
    Serial.printf("IR=%ld | BPM=%.1f | Avg=%d\n", irValue, beatsPerMinute, beatAvg);
  }
}
