#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

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

struct SerialProfile {
  long baud;
  String fmt;
};

SerialProfile g_plcProfile = {9600, "7O1"};
SerialProfile g_invProfile = {19200, "8E2"};

WebServer g_web(80);
const char *AP_SSID = "RS485COM";

struct PlcItem {
  uint16_t addr;
  String view;   // word|bit
  uint8_t width; // 16|32
  bool sign;
};

PlcItem g_plcItems[5] = {
  {0, "word", 16, false},
  {10, "word", 16, false},
  {20, "word", 16, false},
  {30, "word", 16, false},
  {40, "word", 32, false},
};

uint32_t toSerialConfig(const String &fmt) {
  if (fmt == "7O1") return SERIAL_7O1;
  if (fmt == "7E1") return SERIAL_7E1;
  if (fmt == "8N1") return SERIAL_8N1;
  if (fmt == "8E1") return SERIAL_8E1;
  if (fmt == "8O1") return SERIAL_8O1;
  if (fmt == "8E2") return SERIAL_8E2;
  return SERIAL_8N1;
}

void applySerialProfile(ProtoMode mode) {
  Serial1.end();
  if (mode == MODE_PLC_FX5_1C) {
    Serial1.begin(g_plcProfile.baud, toSerialConfig(g_plcProfile.fmt), PIN_RX, PIN_TX);
    Serial.print("profile=plc (");
    Serial.print(g_plcProfile.baud);
    Serial.print(" ");
    Serial.print(g_plcProfile.fmt);
    Serial.println(")");
  } else {
    Serial1.begin(g_invProfile.baud, toSerialConfig(g_invProfile.fmt), PIN_RX, PIN_TX);
    Serial.print("profile=inv (");
    Serial.print(g_invProfile.baud);
    Serial.print(" ");
    Serial.print(g_invProfile.fmt);
    Serial.println(")");
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

bool plcReadWords1C(uint16_t dAddr, uint8_t words, uint32_t &u32out) {
  const uint8_t ENQ = 0x05;
  const uint8_t CR  = 0x0D;

  char body[32];
  snprintf(body, sizeof(body), "00FFWR0D%04u%02X", dAddr, words);

  while (Serial1.available()) Serial1.read();
  rs485TxMode();
  delayMicroseconds(120);
  Serial1.write(ENQ);
  Serial1.print(body);
  Serial1.write(CR);
  Serial1.flush();
  rs485RxMode();

  uint8_t raw[96]; size_t n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 1200 && n < sizeof(raw)) {
    if (Serial1.available()) raw[n++] = (uint8_t)Serial1.read();
  }

  if (n >= 10 && raw[0] == 0x02 && raw[1] == '0' && raw[2] == '0' && raw[3] == 'F' && raw[4] == 'F') {
    // STX 00FF [data...] ETX
    int etx = -1;
    for (size_t i = 5; i < n; i++) if (raw[i] == 0x03) { etx = (int)i; break; }
    if (etx > 8) {
      String data;
      for (int i = 5; i < etx; i++) data += (char)raw[i];
      if (words == 1 && data.length() >= 4) {
        String h = data.substring(0,4);
        u32out = (uint32_t)strtoul(h.c_str(), nullptr, 16);
        return true;
      }
      if (words >= 2 && data.length() >= 8) {
        String h1 = data.substring(0,4);
        String h2 = data.substring(4,8);
        uint16_t w1 = (uint16_t)strtoul(h1.c_str(), nullptr, 16);
        uint16_t w2 = (uint16_t)strtoul(h2.c_str(), nullptr, 16);
        u32out = ((uint32_t)w2 << 16) | w1;
        return true;
      }
    }
  }
  return false;
}

void setupWebUi() {
  g_web.on("/", HTTP_GET, []() {
    String html = R"HTML(
<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>RS485COM</title><style>body{font-family:sans-serif;padding:12px}label{display:block;margin-top:8px}input,select,button{font-size:16px;padding:6px;margin-top:4px}</style></head>
<body><h3>RS485COM</h3>
<label>Mode<select id='mode'><option value='plc'>PLC</option><option value='inv'>INV</option></select></label>
<label>PLC Baud<input id='plcBaud' type='number'></label>
<label>PLC Format<select id='plcFmt'><option>7O1</option><option>7E1</option><option>8N1</option><option>8E1</option><option>8O1</option><option>8E2</option></select></label>
<label>INV Baud<input id='invBaud' type='number'></label>
<label>INV Format<select id='invFmt'><option>8E2</option><option>8N1</option><option>8E1</option><option>8O1</option><option>7O1</option><option>7E1</option></select></label>
<button onclick='save()'>Save & Apply</button>
<h4>PLC Items (5)</h4>
<div id='plcItems'></div>
<button onclick='savePlc()'>Save PLC Items</button>
<button onclick='readNow()'>Read Now</button>
<pre id='st'></pre>
<script>
function row(i,it){return `<div style="border:1px solid #ddd;padding:6px;margin:6px 0">#${i+1} Addr:<input id='a${i}' type='number' value='${it.addr}' style='width:90px'> View:<select id='v${i}'><option ${it.view==='word'?'selected':''}>word</option><option ${it.view==='bit'?'selected':''}>bit</option></select> Width:<select id='w${i}'><option ${it.width==16?'selected':''}>16</option><option ${it.width==32?'selected':''}>32</option></select> Signed:<select id='s${i}'><option value='0' ${!it.sign?'selected':''}>no</option><option value='1' ${it.sign?'selected':''}>yes</option></select></div>`}
async function load(){let r=await fetch('/cfg');let j=await r.json();
mode.value=j.mode;plcBaud.value=j.plcBaud;plcFmt.value=j.plcFmt;invBaud.value=j.invBaud;invFmt.value=j.invFmt;
let pr=await fetch('/plccfg'); let pj=await pr.json(); plcItems.innerHTML=pj.items.map((it,i)=>row(i,it)).join('');
st.textContent=JSON.stringify(j,null,2)}
async function save(){let p=new URLSearchParams({mode:mode.value,plcBaud:plcBaud.value,plcFmt:plcFmt.value,invBaud:invBaud.value,invFmt:invFmt.value});
let r=await fetch('/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});st.textContent=await r.text();setTimeout(load,300)}
async function savePlc(){let p=new URLSearchParams();
for(let i=0;i<5;i++){p.append('addr'+i,document.getElementById('a'+i).value);p.append('view'+i,document.getElementById('v'+i).value);p.append('width'+i,document.getElementById('w'+i).value);p.append('sign'+i,document.getElementById('s'+i).value)}
let r=await fetch('/plcset',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});st.textContent=await r.text();}
async function readNow(){let r=await fetch('/plcread');st.textContent=await r.text();}
load();
</script></body></html>)HTML";
    g_web.send(200, "text/html", html);
  });

  g_web.on("/cfg", HTTP_GET, []() {
    String j = "{";
    j += "\"mode\":\"" + String(g_mode == MODE_PLC_FX5_1C ? "plc" : "inv") + "\",";
    j += "\"plcBaud\":" + String(g_plcProfile.baud) + ",";
    j += "\"plcFmt\":\"" + g_plcProfile.fmt + "\",";
    j += "\"invBaud\":" + String(g_invProfile.baud) + ",";
    j += "\"invFmt\":\"" + g_invProfile.fmt + "\"}";
    g_web.send(200, "application/json", j);
  });

  g_web.on("/set", HTTP_POST, []() {
    if (g_web.hasArg("plcBaud")) g_plcProfile.baud = g_web.arg("plcBaud").toInt();
    if (g_web.hasArg("plcFmt")) g_plcProfile.fmt = g_web.arg("plcFmt");
    if (g_web.hasArg("invBaud")) g_invProfile.baud = g_web.arg("invBaud").toInt();
    if (g_web.hasArg("invFmt")) g_invProfile.fmt = g_web.arg("invFmt");
    if (g_web.hasArg("mode") && g_web.arg("mode") == "inv") applySerialProfile(MODE_INV_FRD820);
    else applySerialProfile(MODE_PLC_FX5_1C);
    g_web.send(200, "text/plain", "OK");
  });

  g_web.on("/plccfg", HTTP_GET, []() {
    String j = "{\"items\":[";
    for (int i = 0; i < 5; i++) {
      if (i) j += ",";
      j += "{\"addr\":" + String(g_plcItems[i].addr)
        + ",\"view\":\"" + g_plcItems[i].view + "\""
        + ",\"width\":" + String(g_plcItems[i].width)
        + ",\"sign\":" + String(g_plcItems[i].sign ? "true" : "false")
        + "}";
    }
    j += "]}";
    g_web.send(200, "application/json", j);
  });

  g_web.on("/plcset", HTTP_POST, []() {
    for (int i = 0; i < 5; i++) {
      String kA = "addr" + String(i), kV = "view" + String(i), kW = "width" + String(i), kS = "sign" + String(i);
      if (g_web.hasArg(kA)) g_plcItems[i].addr = (uint16_t)g_web.arg(kA).toInt();
      if (g_web.hasArg(kV)) g_plcItems[i].view = g_web.arg(kV);
      if (g_web.hasArg(kW)) g_plcItems[i].width = (uint8_t)g_web.arg(kW).toInt();
      if (g_web.hasArg(kS)) g_plcItems[i].sign = (g_web.arg(kS) == "1");
    }
    g_web.send(200, "text/plain", "OK");
  });

  g_web.on("/plcread", HTTP_GET, []() {
    applySerialProfile(MODE_PLC_FX5_1C);
    String j = "{\"items\":[";
    for (int i = 0; i < 5; i++) {
      uint32_t u = 0;
      uint8_t words = (g_plcItems[i].width == 32) ? 2 : 1;
      bool ok = plcReadWords1C(g_plcItems[i].addr, words, u);
      if (i) j += ",";
      j += "{\"idx\":" + String(i)
        + ",\"addr\":" + String(g_plcItems[i].addr)
        + ",\"ok\":" + String(ok ? "true" : "false")
        + ",\"u32\":" + String(u)
        + ",\"s32\":" + String((int32_t)u)
        + "}";
    }
    j += "]}";
    g_web.send(200, "application/json", j);
  });

  g_web.begin();
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

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID); // open AP (no password)
  setupWebUi();
  Serial.print("AP up: "); Serial.println(AP_SSID);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  Serial.println("xiao_c3_rs485_mc ready");
  Serial.println("type: read3e / read1e / readasc / read1c / readinv");
  Serial.println("type: setproto plc / setproto inv / proto");
}

bool readInverterOnce(const char *cmd2, uint16_t &valueOut) {
  const uint8_t ENQ = 0x05;
  const uint8_t CR = 0x0D;
  const uint8_t LF = 0x0A;

  auto checksum2 = [&](const String &s) {
    uint8_t sum = 0;
    for (size_t i = 0; i < s.length(); i++) sum += (uint8_t)s[i];
    char c[3];
    snprintf(c, sizeof(c), "%02X", sum);
    return String(c);
  };

  auto trySend = [&](const String &body, bool addLF) -> bool {
    while (Serial1.available()) Serial1.read();

    String cks = checksum2(body);

    rs485TxMode();
    delayMicroseconds(120);
    Serial1.write(ENQ);
    Serial1.print(body);
    Serial1.print(cks);   // checksum required by inverter
    Serial1.write(CR);    // CR only by default
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
  // keep CR-only per user setting
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
  g_web.handleClient();
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
