#include <Arduino.h>

// MAX485 control pins
static const int PIN_RE = 3;   // Receiver Enable (LOW = receive)
static const int PIN_DE = 4;   // Driver Enable   (HIGH = transmit)

// UART pins to MAX485
static const int PIN_RX = 20;  // RO -> RX
static const int PIN_TX = 21;  // DI -> TX

// FX5U MC protocol target
static const uint8_t STATION_NO = 0x00;

enum ProtoMode {
  MODE_PLC_FX5_1C,
  MODE_INV_FRD820,
};

ProtoMode g_mode = MODE_PLC_FX5_1C;

void applySerialProfile(ProtoMode mode) {
  Serial1.end();
  if (mode == MODE_PLC_FX5_1C) {
    // PLC profile: MC 1C no-procedure
    Serial1.begin(9600, SERIAL_7O1, PIN_RX, PIN_TX);
    Serial.println("profile=plc (9600 7O1)");
  } else {
    // Inverter profile: FR-D820 no-procedure
    Serial1.begin(19200, SERIAL_8E2, PIN_RX, PIN_TX);
    Serial.println("profile=inv (19200 8E2)");
  }
  g_mode = mode;
}

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

  applySerialProfile(MODE_PLC_FX5_1C);

  Serial.println("xiao_c3_rs485_mc ready");
  Serial.println("type: read3e / read1e / readasc / read1c / readinv");
  Serial.println("type: setproto plc / setproto inv / proto");
}

bool readInverterOnce(const char *cmd2, uint16_t &valueOut) {
  const uint8_t ENQ = 0x05;
  const uint8_t CR = 0x0D;
  const uint8_t LF = 0x0A;

  auto trySend = [&](const String &body, bool addLF) -> bool {
    while (Serial1.available()) Serial1.read();

    rs485TxMode();
    delayMicroseconds(120);
    Serial1.write(ENQ);
    Serial1.print(body);
    Serial1.write(CR);
    if (addLF) Serial1.write(LF);
    Serial1.flush();
    rs485RxMode();

    uint8_t raw[96]; size_t n = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 900 && n < sizeof(raw)) {
      if (Serial1.available()) raw[n++] = (uint8_t)Serial1.read();
    }

    Serial.print("INV tx body="); Serial.print(body);
    Serial.print(addLF ? " +LF" : "");
    Serial.print(" RX("); Serial.print(n); Serial.println(")");
    if (n == 0) return false;
    hexDump(raw, n);

    // expected: STX + station(2) + data(4hex) + ETX (+optional checksum/crlf)
    int stx = -1, etx = -1;
    for (size_t i = 0; i < n; i++) if (raw[i] == 0x02) { stx = (int)i; break; }
    for (size_t i = stx + 1; i < (int)n; i++) if (raw[i] == 0x03) { etx = (int)i; break; }
    if (stx >= 0 && etx > stx + 2) {
      String payload;
      for (int i = stx + 1; i < etx; i++) payload += (char)raw[i];
      // payload usually: "00" + "hhhh"
      if (payload.length() >= 6) {
        String h = payload.substring(payload.length() - 4);
        valueOut = (uint16_t)strtoul(h.c_str(), nullptr, 16);
        Serial.print("INV parsed="); Serial.println(h);
        return true;
      }
    }
    return false;
  };

  // command read patterns: station + cmd (+ optional wait)
  String b1 = String("00") + cmd2;
  String b2 = String("00") + cmd2 + "0";

  if (trySend(b1, false)) return true;
  if (trySend(b2, false)) return true;
  if (trySend(b1, true)) return true;
  if (trySend(b2, true)) return true;
  return false;
}

bool readInverterMonitors() {
  uint16_t f=0,i=0,v=0;
  bool okF = readInverterOnce("6F", f); // output frequency (0.01Hz)
  bool okI = readInverterOnce("70", i); // output current (0.01A)
  bool okV = readInverterOnce("71", v); // output voltage (0.1V)

  if (okF) { Serial.print("INV freq_Hz="); Serial.println(f / 100.0f, 2); }
  else Serial.println("INV freq read NG");
  if (okI) { Serial.print("INV current_A="); Serial.println(i / 100.0f, 2); }
  else Serial.println("INV current read NG");
  if (okV) { Serial.print("INV voltage_V="); Serial.println(v / 10.0f, 1); }
  else Serial.println("INV voltage read NG");

  return okF || okI || okV;
}

bool readOnce1C() {
  // Reuse the proven communication style from user's PlatformIO sample
  const uint8_t ENQ = 0x05;
  const uint8_t CR  = 0x0D;

  auto sendBody = [&](const char *body) {
    // clear stale RX bytes
    while (Serial1.available()) Serial1.read();

    rs485TxMode();
    delayMicroseconds(120);

    // ENQ + station + pc + body + CR (checksum OFF)
    Serial1.write(ENQ);
    Serial1.print("00");
    Serial1.print("FF");
    Serial1.print(body);
    Serial1.write(CR);
    Serial1.flush();

    rs485RxMode();

    uint8_t raw[196];
    size_t n = 0;
    char parsed[96];
    size_t pi = 0;

    bool hasSTX = false, hasETX = false, hasACK = false, hasNAK = false, hasCR = false;
    unsigned long t0 = millis();

    while (millis() - t0 < 1600 && n < sizeof(raw)) {
      if (!Serial1.available()) continue;
      uint8_t c = (uint8_t)Serial1.read();
      raw[n++] = c;

      bool keep = true;
      if (c == 0x02) { hasSTX = true; keep = false; }
      else if (c == 0x03) { hasETX = true; keep = false; }
      else if (c == 0x06) { hasACK = true; keep = false; }
      else if (c == 0x15) { hasNAK = true; keep = false; }
      else if (c == 0x0D) { hasCR  = true; keep = false; }

      if (keep && pi < sizeof(parsed)-1) parsed[pi++] = (char)c;

      if ((hasSTX && hasETX) || (hasCR && (hasACK || hasNAK))) break;
    }
    parsed[pi] = '\0';

    Serial.print("1C body="); Serial.println(body);
    Serial.print("1C RX("); Serial.print(n); Serial.println(")");
    Serial.print("1C parsed="); Serial.println(parsed);
    Serial.println("--hex--");
    hexDump(raw, n);

    if (n == 14 && raw[0] == 0x02 && raw[13] == 0x03 &&
        raw[1] == '0' && raw[2] == '0' && raw[3] == 'F' && raw[4] == 'F') {
      char h1[5] = {(char)raw[5], (char)raw[6], (char)raw[7], (char)raw[8], 0};
      char h2[5] = {(char)raw[9], (char)raw[10], (char)raw[11], (char)raw[12], 0};
      uint16_t w1 = (uint16_t)strtoul(h1, nullptr, 16);
      uint16_t w2 = (uint16_t)strtoul(h2, nullptr, 16);
      uint32_t u32 = ((uint32_t)w2 << 16) | w1;
      int32_t s32 = (u32 < 0x80000000UL) ? (int32_t)u32 : (int32_t)(u32 - 0x100000000ULL);
      Serial.print("decode14 w1="); Serial.print(h1);
      Serial.print(" w2="); Serial.println(h2);
      Serial.print("decode14 u32="); Serial.print(u32);
      Serial.print(" s32="); Serial.println(s32);
      return true;
    }

    return false;
  };

  for (int i = 0; i < 3; i++) {
    if (sendBody("WR0D200002")) return true;
    delay(900);
  }
  return false;
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
    } else if (cmd.equalsIgnoreCase("readinv")) {
      Serial.println("ACK readinv");
      readInverterMonitors();
    } else if (cmd.equalsIgnoreCase("setproto plc")) {
      applySerialProfile(MODE_PLC_FX5_1C);
    } else if (cmd.equalsIgnoreCase("setproto inv")) {
      applySerialProfile(MODE_INV_FRD820);
    } else if (cmd.equalsIgnoreCase("proto")) {
      if (g_mode == MODE_PLC_FX5_1C) Serial.println("proto=plc");
      else Serial.println("proto=inv");
    }
  }
  delay(5);
}
