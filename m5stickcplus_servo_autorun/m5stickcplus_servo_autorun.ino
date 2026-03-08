#include <ESP32Servo.h>

// Wiring (as requested)
static const int PIN_SG90 = 26;      // SG90 servo
static const int PIN_SERVO180 = 25;  // M5STACK 180° servo

Servo servoA;
Servo servoB;

// Motion profile
int aPos = 90;
int aDir = 1;
unsigned long lastStepA = 0;
unsigned long lastStepB = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("M5StickCPlus autonomous servo start");

  // 50Hz servo period
  servoA.setPeriodHertz(50);
  servoB.setPeriodHertz(50);

  // Pulse ranges can be tuned per-servo
  // SG90 typical 500-2400us
  servoA.attach(PIN_SG90, 500, 2400);
  // 180 servo often similar but slightly narrower can reduce jitter
  servoB.attach(PIN_SERVO180, 500, 2400);

  servoA.write(90);
  servoB.write(90);
}

void loop() {
  unsigned long now = millis();

  // Servo A (G26 / SG90): responsive sweep 20..160
  if (now - lastStepA >= 20) {
    lastStepA = now;
    aPos += aDir;
    if (aPos >= 160) { aPos = 160; aDir = -1; }
    if (aPos <= 20)  { aPos = 20;  aDir = +1; }
    servoA.write(aPos);
  }

  // Servo B (G25 / 180deg): slower sinusoid-like motion
  if (now - lastStepB >= 30) {
    lastStepB = now;
    float t = now / 1000.0f;
    // 90 +/- 70
    int bPos = (int)(90 + 70 * sinf(t * 0.9f));
    if (bPos < 20) bPos = 20;
    if (bPos > 160) bPos = 160;
    servoB.write(bPos);
  }

  // Telemetry every ~1s
  static unsigned long lastPrint = 0;
  if (now - lastPrint > 1000) {
    lastPrint = now;
    Serial.printf("A=%d\n", aPos);
  }
}
