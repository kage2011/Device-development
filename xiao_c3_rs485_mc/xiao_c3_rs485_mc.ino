#include <Arduino.h>

// MAX485 control pins
static const int PIN_RE = 3;   // Receiver Enable (LOW = receive)
static const int PIN_DE = 4;   // Driver Enable   (HIGH = transmit)

// UART pins to MAX485
static const int PIN_RX = 20;  // RO -> RX
static const int PIN_TX = 21;  // DI -> TX

// FX5U MC protocol target
static const uint8_t STATION_NO = 0x00;

void rs485RxMode() {
  digitalWrite(PIN_DE, LOW);
  digitalWrite(PIN_RE, LOW);
}

void rs485TxMode() {
  digitalWrite(PIN_RE, HIGH);
  digitalWrite(PIN_DE, HIGH);
}

void hexDump(const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (buf[i] < 0x10) Serial.print('0');
    Serial.print(buf[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}

// MC 3E binary frame for Batch Read D2000, 2 words (D2000-D2001)
size_t buildReadD2000Frame(uint8_t *out) {
  // 3E binary over serial
  // Subheader(2), Network(1), PC(1), I/O(2), Station(1), Length(2), Timer(2), Cmd(2), Subcmd(2), HeadDev(3), DevCode(1), Points(2)
  uint8_t frame[] = {
    0x50, 0x00,
    0x00,
    0xFF,
    0xFF, 0x03,
    STATION_NO,
    0x0C, 0x00,
    0x10, 0x00,
    0x01, 0x04,
    0x00, 0x00,
    0xD0, 0x07, 0x00, // D2000 = 0x07D0
    0xA8,
    0x02, 0x00        // 2 words
  };
  memcpy(out, frame, sizeof(frame));
  return sizeof(frame);
}

bool readOnce() {
  uint8_t req[32];
  size_t reqLen = buildReadD2000Frame(req);

  while (Serial1.available()) Serial1.read();

  rs485TxMode();
  delayMicroseconds(100);
  Serial1.write(req, reqLen);
  Serial1.flush();
  delayMicroseconds(150);
  rs485RxMode();

  uint8_t res[128];
  size_t n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 500) {
    while (Serial1.available() && n < sizeof(res)) {
      res[n++] = (uint8_t)Serial1.read();
    }
    if (n >= 11) {
      uint16_t dataLen = (uint16_t)res[7] | ((uint16_t)res[8] << 8);
      size_t total = 9 + dataLen;
      if (n >= total) break;
    }
  }

  Serial.print("RX("), Serial.print(n), Serial.println(")");
  hexDump(res, n);

  if (n < 15) {
    Serial.println("ERR short response");
    return false;
  }

  uint16_t endCode = (uint16_t)res[9] | ((uint16_t)res[10] << 8);
  Serial.print("endCode="), Serial.println(endCode);
  if (endCode != 0) {
    Serial.println("ERR MC endCode != 0");
    return false;
  }

  uint16_t lo = (uint16_t)res[11] | ((uint16_t)res[12] << 8);
  uint16_t hi = (uint16_t)res[13] | ((uint16_t)res[14] << 8);
  uint32_t u32 = ((uint32_t)hi << 16) | lo;
  int32_t s32 = (u32 < 0x80000000UL) ? (int32_t)u32 : (int32_t)(u32 - 0x100000000ULL);

  Serial.print("D2000_u16="), Serial.println(lo);
  Serial.print("D2000_2001_u32="), Serial.println(u32);
  Serial.print("D2000_2001_s32="), Serial.println(s32);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_RE, OUTPUT);
  pinMode(PIN_DE, OUTPUT);
  rs485RxMode();

  // User specified: 7-bit, odd parity, 1 stop, 9600
  Serial1.begin(9600, SERIAL_7O1, PIN_RX, PIN_TX);

  Serial.println("xiao_c3_rs485_mc ready");
  Serial.println("type: read3e / read1e / readasc / read1c");
}

bool readOnce1C() {
  // 1C / no-procedure style: ENQ + "00FF" + body + CR (checksum OFF)
  const uint8_t ENQ = 0x05;
  const uint8_t CR  = 0x0D;

  auto clearRxQuiet = [&]() {
    unsigned long tEnd = millis() + 30;
    while (millis() < tEnd) {
      while (Serial1.available()) {
        Serial1.read();
        tEnd = millis() + 20;
      }
    }
  };

  auto sendBody = [&](const char *body) {
    clearRxQuiet();

    rs485TxMode();
    delayMicroseconds(120);
    Serial1.write(ENQ);
    Serial1.print("00");  // station
    Serial1.print("FF");  // pc no
    Serial1.print(body);   // e.g. WR0D200002
    Serial1.write(CR);
    Serial1.flush();

    delayMicroseconds(220);
    rs485RxMode();
    delayMicroseconds(120);

    uint8_t raw[196];
    size_t n = 0;
    char buf[96];
    size_t bi = 0;
    bool hasSTX = false, hasETX = false, hasACK = false, hasNAK = false, hasCR = false;
    unsigned long t0 = millis();
    unsigned long lastRx = t0;

    while (millis() - t0 < 2000 && n < sizeof(raw)) {
      if (Serial1.available()) {
        uint8_t c = (uint8_t)Serial1.read();
        raw[n++] = c;
        lastRx = millis();

        bool shouldBuffer = true;
        if (c == 0x02) { hasSTX = true; shouldBuffer = false; }
        if (c == 0x03) { hasETX = true; shouldBuffer = false; }
        if (c == 0x06) { hasACK = true; shouldBuffer = false; }
        if (c == 0x15) { hasNAK = true; shouldBuffer = false; }
        if (c == 0x0D) { hasCR  = true; shouldBuffer = false; }

        if (shouldBuffer && bi < sizeof(buf) - 1) buf[bi++] = (char)c;
      }

      // complete frame captured; wait a short quiet window for trailing bytes
      if ((hasSTX && hasETX) || (hasCR && (hasACK || hasNAK))) {
        if (millis() - lastRx > 50) break;
      }
    }
    buf[bi] = '\0';

    Serial.print("1C body="), Serial.println(body);
    Serial.print("1C RX("), Serial.print(n), Serial.println(")");
    Serial.print("1C parsed="), Serial.println(buf);
    if (n == 0) return false;

    Serial.println("--hex--");
    hexDump(raw, n);

    // prefer stable 14-byte frame: STX + "00FF" + 8 hex chars + ETX
    if (n == 14 && raw[0] == 0x02 && raw[13] == 0x03 &&
        raw[1] == '0' && raw[2] == '0' && raw[3] == 'F' && raw[4] == 'F') {
      char h1[5] = {(char)raw[5], (char)raw[6], (char)raw[7], (char)raw[8], 0};
      char h2[5] = {(char)raw[9], (char)raw[10], (char)raw[11], (char)raw[12], 0};
      uint16_t w1 = (uint16_t)strtoul(h1, nullptr, 16);
      uint16_t w2 = (uint16_t)strtoul(h2, nullptr, 16);
      uint32_t u32_lohi = ((uint32_t)w2 << 16) | w1;
      uint32_t u32_hilo = ((uint32_t)w1 << 16) | w2;
      int32_t s32_lohi = (u32_lohi < 0x80000000UL) ? (int32_t)u32_lohi : (int32_t)(u32_lohi - 0x100000000ULL);
      int32_t s32_hilo = (u32_hilo < 0x80000000UL) ? (int32_t)u32_hilo : (int32_t)(u32_hilo - 0x100000000ULL);
      Serial.print("1C decode14 w1="); Serial.print(h1);
      Serial.print(" w2="); Serial.println(h2);
      Serial.print("1C decode14 lohi_u32="); Serial.print(u32_lohi);
      Serial.print(" lohi_s32="); Serial.println(s32_lohi);
      Serial.print("1C decode14 hilo_u32="); Serial.print(u32_hilo);
      Serial.print(" hilo_s32="); Serial.println(s32_hilo);
    }
    return true;
  };

  // Alternate known points: D0 and D2000
  bool any = false;
  any |= sendBody("WR0D200002");   // D2000-D2001
  return any;
}

bool readOnceAscii() {
  // Mitsubishi MC protocol ASCII, no-procedure, station 0
  // Try multiple candidate frames because serial ASCII layouts differ by config.
  const char *frames[] = {
    // 3E ASCII candidate (device 2000, points 2)
    "500000FF03FF00000C001004010000D*0020000002",
    // 3E ASCII candidate (head device in hex-like style)
    "500000FF03FF00000C0010040100000007D0D*0002",
    // 1E/Computer-link style trial
    "00FFBRD0D2000002"
  };

  for (size_t k = 0; k < sizeof(frames)/sizeof(frames[0]); k++) {
    const char *frm = frames[k];
    while (Serial1.available()) Serial1.read();

    rs485TxMode();
    delayMicroseconds(100);
    Serial1.write((const uint8_t*)frm, strlen(frm));
    Serial1.flush();
    delayMicroseconds(200);
    rs485RxMode();

    uint8_t res[196];
    size_t n = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 900) {
      while (Serial1.available() && n < sizeof(res)) res[n++] = (uint8_t)Serial1.read();
    }

    Serial.print("ASC tx#"), Serial.print(k), Serial.print(" RX("), Serial.print(n), Serial.println(")");
    if (n > 0) {
      // print as text + hex
      Serial.println("--text--");
      for (size_t i = 0; i < n; i++) Serial.write(res[i]);
      Serial.println();
      Serial.println("--hex--");
      hexDump(res, n);
      return true;
    }
  }
  return false;
}

bool readOnce1E() {
  // Trial frame for Mitsubishi serial MC/Computer-link style (7O1)
  // This is a probe frame to identify whether PLC expects 1E/Computer-link family.
  const char *trial = "\x0500FFBRD0D2000002"; // ENQ + station/command/body (trial)

  while (Serial1.available()) Serial1.read();
  rs485TxMode();
  delayMicroseconds(100);
  Serial1.write((const uint8_t*)trial, strlen(trial));
  Serial1.flush();
  delayMicroseconds(150);
  rs485RxMode();

  uint8_t res[128];
  size_t n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 700) {
    while (Serial1.available() && n < sizeof(res)) res[n++] = (uint8_t)Serial1.read();
  }
  Serial.print("RX1E("), Serial.print(n), Serial.println(")");
  hexDump(res, n);
  return n > 0;
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("read") || cmd.equalsIgnoreCase("read3e")) {
      Serial.println("ACK read3e");
      readOnce();
    } else if (cmd.equalsIgnoreCase("read1e")) {
      Serial.println("ACK read1e");
      readOnce1E();
    } else if (cmd.equalsIgnoreCase("readasc")) {
      Serial.println("ACK readasc");
      readOnceAscii();
    } else if (cmd.equalsIgnoreCase("read1c")) {
      Serial.println("ACK read1c");
      readOnce1C();
    }
  }
  delay(5);
}
