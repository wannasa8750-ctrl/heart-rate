#define BLYNK_TEMPLATE_ID   "TMPL6Zbm4Q3_J"
#define BLYNK_TEMPLATE_NAME "Heart Rate Monitor"
#define BLYNK_AUTH_TOKEN    "F5qkG105qMpLBrbqtE9U6M0gXbgQzjGM"
#define BLYNK_PRINT Serial

// ✅ ต้อง include ตามลำดับนี้เท่านั้น
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"

char ssid[] = "Banyapon";
char pass[] = "12345678";

MAX30105 particleSensor;

const byte RATE_SIZE = 4;
byte  rates[RATE_SIZE];
byte  rateSpot       = 0;
long  lastBeat       = 0;
float beatsPerMinute = 0;
int   beatAvg        = 0;
bool  fingerOn       = false;

BlynkTimer timer;

void sendToBlynk() {
  if (fingerOn && beatAvg > 0) {
    Blynk.virtualWrite(V0, beatAvg);   // ส่งแค่ค่าเฉลี่ย BPM เป็น Integer
    Serial.printf(">>> Sent to Blynk V0 = %d BPM\n", beatAvg);
  } else {
    Blynk.virtualWrite(V0, 0);
    Serial.println(">>> Sent to Blynk V0 = 0 (no finger)");
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // ✅ เชื่อม WiFi แยกก่อน แล้วค่อย Blynk
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, pass);

  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++timeout > 30) {           // รอสูงสุด 15 วินาที
      Serial.println("\n❌ WiFi Failed! Check SSID/Password.");
      ESP.restart();
    }
  }

  Serial.print("\n✅ WiFi Connected! IP: ");
  Serial.println(WiFi.localIP());

  // ✅ เชื่อม Blynk หลัง WiFi พร้อมแล้ว
  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect(5000);             // timeout 5 วินาที

  if (Blynk.connected()) {
    Serial.println("✅ Blynk Connected!");
  } else {
    Serial.println("⚠️ Blynk offline — รันต่อแบบ local");
  }

  // เริ่ม Sensor
  Wire.begin(21, 22);
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("❌ MAX30102 not found!");
    while (1);
  }

  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x1F);
  particleSensor.setPulseAmplitudeGreen(0);
  particleSensor.setPulseAmplitudeIR(0x1F);

  timer.setInterval(1000L, sendToBlynk);
  Serial.println("✅ Sensor Ready! Place your finger.");
}

void loop() {
  if (Blynk.connected()) Blynk.run();
  timer.run();

  long irValue = particleSensor.getIR();

  if (irValue < 50000) {
    if (fingerOn) {
      fingerOn = false;
      beatsPerMinute = 0;
      beatAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++) rates[x] = 0;
      rateSpot = 0;
      Serial.println("Finger removed.");
    }
    delay(500);
    return;
  }

  fingerOn = true;

  if (checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();
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