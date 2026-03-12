#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <RTClib.h>
#include <SD.h>
#include <SPIFFS.h>
#include <ModbusMaster.h>

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
String g_invProto = "clink"; // clink | modbus
uint16_t g_invUnit = 0;
uint8_t g_invDataBits = 8;
char g_invParity = 'E'; // N/E/O
uint8_t g_invStopBits = 2;
ModbusMaster g_mb;

WebServer g_web(80);
const char *AP_SSID = "RS485COM_X92";
RTC_DS3231 rtc;
const int SD_CS_PIN = 5;

struct PlcItem {
  String dev;    // D|X|Y
  uint16_t addr;
  String view;   // word|bit
  uint8_t width; // 16|32
  bool sign;
};

PlcItem g_plcItems[5] = {
  {"D", 0, "word", 16, false},
  {"D", 10, "word", 16, false},
  {"D", 20, "word", 16, false},
  {"D", 30, "word", 16, false},
  {"D", 40, "word", 32, false},
};

struct LogConfig {
  bool enabled;
  uint32_t intervalMs;
  String filename;
  String target;
  unsigned long lastWriteMs;
};

LogConfig g_logCfg = {false, 1000, "", "inv", 0};
String g_logPath = "";
uint32_t g_plcLastU32[5] = {0,0,0,0,0};
bool g_plcLastOk[5] = {false,false,false,false,false};
uint8_t g_plcScanIdx = 0;

void saveRuntimeConfig() {
  File f = SPIFFS.open("/runtime_cfg.txt", FILE_WRITE);
  if (!f) {
    Serial.println("CFG save NG");
    return;
  }
  f.println(String("mode=") + (g_mode == MODE_INV_FRD820 ? "inv" : "plc"));
  f.println(String("plcBaud=") + g_plcProfile.baud);
  f.println(String("plcFmt=") + g_plcProfile.fmt);
  f.println(String("invBaud=") + g_invProfile.baud);
  f.println(String("invFmt=") + g_invProfile.fmt);
  f.println(String("invProto=") + g_invProto);
  f.println(String("invUnit=") + g_invUnit);
  f.println(String("invDataBits=") + g_invDataBits);
  f.println(String("invParity=") + String(g_invParity));
  f.println(String("invStopBits=") + g_invStopBits);
  for (int i=0;i<5;i++) {
    f.println(String("plc")+i+"_dev="+g_plcItems[i].dev);
    f.println(String("plc")+i+"_addr="+g_plcItems[i].addr);
    f.println(String("plc")+i+"_view="+g_plcItems[i].view);
    f.println(String("plc")+i+"_width="+g_plcItems[i].width);
    f.println(String("plc")+i+"_sign="+(g_plcItems[i].sign?1:0));
  }
  f.println(String("logEnabled=") + (g_logCfg.enabled?1:0));
  f.println(String("logIntervalMs=") + g_logCfg.intervalMs);
  f.println(String("logFilename=") + g_logCfg.filename);
  f.println(String("logTarget=") + g_logCfg.target);
  f.flush();
  size_t sz = f.size();
  f.close();
  Serial.print("CFG saved /runtime_cfg.txt size="); Serial.println((int)sz);
}

void loadRuntimeConfig() {
  if (!SPIFFS.exists("/runtime_cfg.txt")) {
    Serial.println("CFG not found");
    return;
  }
  File f = SPIFFS.open("/runtime_cfg.txt", FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int p = line.indexOf('=');
    if (p <= 0) continue;
    String k = line.substring(0,p);
    String v = line.substring(p+1);
    if (k == "mode") g_mode = (v == "inv") ? MODE_INV_FRD820 : MODE_PLC_FX5_1C;
    else if (k == "plcBaud") g_plcProfile.baud = v.toInt();
    else if (k == "plcFmt") g_plcProfile.fmt = v;
    else if (k == "invBaud") g_invProfile.baud = v.toInt();
    else if (k == "invFmt") g_invProfile.fmt = v;
    else if (k == "invProto") g_invProto = v;
    else if (k == "invUnit") g_invUnit = (uint16_t)v.toInt();
    else if (k == "invDataBits") g_invDataBits = (uint8_t)v.toInt();
    else if (k == "invParity") g_invParity = v.length() ? v[0] : 'N';
    else if (k == "invStopBits") g_invStopBits = (uint8_t)v.toInt();
    else if (k == "logEnabled") g_logCfg.enabled = (v.toInt() == 1);
    else if (k == "logIntervalMs") g_logCfg.intervalMs = (uint32_t) v.toInt();
    else if (k == "logFilename") g_logCfg.filename = v;
    else if (k == "logTarget") g_logCfg.target = v;
    else {
      for (int i=0;i<5;i++) {
        if (k == String("plc")+i+"_dev") g_plcItems[i].dev = v;
        else if (k == String("plc")+i+"_addr") g_plcItems[i].addr = (uint16_t)v.toInt();
        else if (k == String("plc")+i+"_view") g_plcItems[i].view = v;
        else if (k == String("plc")+i+"_width") g_plcItems[i].width = (uint8_t)v.toInt();
        else if (k == String("plc")+i+"_sign") g_plcItems[i].sign = (v.toInt() == 1);
      }
    }
  }
  f.close();
  Serial.println("CFG loaded /runtime_cfg.txt");
}

String makeInvFmt(uint8_t bits, char parity, uint8_t stop) {
  if (bits != 7 && bits != 8) bits = 8;
  if (!(parity == 'N' || parity == 'E' || parity == 'O')) parity = 'N';
  if (stop != 1 && stop != 2) stop = 1;
  String s = String(bits) + String(parity) + String(stop);
  return s;
}

uint32_t toSerialConfig(const String &fmt) {
  if (fmt == "7O1") return SERIAL_7O1;
  if (fmt == "7E1") return SERIAL_7E1;
  if (fmt == "8N1") return SERIAL_8N1;
  if (fmt == "8E1") return SERIAL_8E1;
  if (fmt == "8O1") return SERIAL_8O1;
  if (fmt == "8E2") return SERIAL_8E2;
  return SERIAL_8N1;
}

String nowStampCompact() {
  DateTime dt = rtc.now();
  char b[24];
  snprintf(b, sizeof(b), "%04d%02d%02d_%02d%02d%02d",
           dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
  return String(b);
}

void ensureLogFile() {
  if (!g_logCfg.enabled) return;
  if (g_logPath.length() == 0) {
    String base = g_logCfg.filename;
    if (base.length() == 0) base = g_logCfg.target + "_" + nowStampCompact();
    if (!base.endsWith(".csv")) base += ".csv";
    g_logPath = "/" + base;
    File f = SD.open(g_logPath, FILE_WRITE);
    if (f) {
      if (g_logCfg.target == "inv") {
        f.println("ts,freq_hz,current_a,voltage_v,status_hex");
      } else {
        f.print("ts");
        for (int i=0;i<5;i++) { f.print(","); f.print(g_plcItems[i].dev); f.print(g_plcItems[i].addr); }
        f.println();
      }
      f.close();
      Serial.print("LOG create: "); Serial.println(g_logPath);
    } else {
      Serial.print("LOG create NG: "); Serial.println(g_logPath);
    }
  }
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
    // NOTE: runtime read path is currently clink logic; modbus selection keeps transport configurable.
    String invFmt = makeInvFmt(g_invDataBits, g_invParity, g_invStopBits);
    if (g_invProto == "modbus") {
      // keep 8-bit fixed for modbus UI rule
      invFmt = makeInvFmt(8, g_invParity, g_invStopBits);
      Serial1.begin(g_invProfile.baud, toSerialConfig(invFmt), PIN_RX, PIN_TX);
      Serial.print("profile=inv (modbus selected, transport ");
      Serial.print(g_invProfile.baud); Serial.print(" "); Serial.print(invFmt); Serial.println(")");
    } else {
      Serial1.begin(g_invProfile.baud, toSerialConfig(invFmt), PIN_RX, PIN_TX);
      Serial.print("profile=inv (");
      Serial.print(g_invProfile.baud);
      Serial.print(" ");
      Serial.print(invFmt);
      Serial.println(")");
    }
    g_invProfile.fmt = invFmt;
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

bool plcReadWords1C(const String &dev, uint16_t addr, uint8_t words, uint32_t &u32out) {
  const uint8_t ENQ = 0x05;
  const uint8_t CR  = 0x0D;

  String d = dev;
  d.toUpperCase();
  if (!(d=="D"||d=="X"||d=="Y"||d=="M"||d=="L"||d=="F"||d=="V"||d=="B"||d=="W"||d=="R"||d=="ZR"||d=="S"||d=="T"||d=="C"||d=="TN"||d=="CN")) d = "D";

  char a[8];
  if (d == "X" || d == "Y") snprintf(a, sizeof(a), "%04o", addr); // format-1 X/Y are octal
  else snprintf(a, sizeof(a), "%04u", addr);

  char body[40];
  snprintf(body, sizeof(body), "00FFWR0%s%s%02X", d.c_str(), a, words);

  while (Serial1.available()) Serial1.read();
  rs485TxMode();
  delay(1);
  Serial1.write(ENQ);
  Serial1.print(body);
  Serial1.write(CR);
  Serial1.flush();
  rs485RxMode();

  uint8_t raw[96]; size_t n = 0;
  unsigned long t0 = millis();
  bool hasStx = false;
  while (millis() - t0 < 40 && n < sizeof(raw)) {
    if (!Serial1.available()) continue;
    uint8_t c = (uint8_t)Serial1.read();
    raw[n++] = c;
    if (c == 0x02) hasStx = true;              // STX
    if (hasStx && c == 0x03) break;            // ETX
    if (!hasStx && c == 0x15) break;           // NAK
    if (c == 0x06) break;                      // ACK only response
    if (c == 0x0D) break;                      // CR terminator
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

void maybeWriteCsvLog();

void setupWebUi() {
  g_web.on("/", HTTP_GET, []() {
    String html = R"HTML(
<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>RS485COM</title>
<style>
body{font-family:sans-serif;padding:12px;background:linear-gradient(160deg,#0f172a,#111827 45%,#0b1020);color:#e5ecff}
label{display:block;margin-top:8px}input,select,button{font-size:15px;padding:8px 10px;margin-top:4px;border-radius:10px;border:1px solid #334155;background:#0f172a;color:#e5ecff}
.card{background:rgba(15,23,42,.85);border:1px solid #334155;border-radius:14px;padding:12px;margin:10px 0;box-shadow:0 6px 24px rgba(0,0,0,.25)}
button{background:linear-gradient(135deg,#2563eb,#1d4ed8);border:none;color:#fff}
#fabWrap{position:fixed;right:10px;top:10px;display:flex;flex-direction:row;gap:6px;z-index:9999;align-items:flex-start}
.fab{border:none;border-radius:999px;padding:6px 10px;font-size:12px;background:linear-gradient(135deg,#06b6d4,#2563eb);color:#fff;box-shadow:0 2px 8px rgba(0,0,0,.35)}
.kpi{display:inline-block;min-width:110px;background:#1e293b;border:1px solid #334155;border-radius:10px;padding:8px;margin:4px;color:#e2e8f0}
.badge{display:inline-block;padding:3px 8px;border-radius:999px;background:#1e293b;margin-right:6px}
.ok{background:#14532d}.ng{background:#3f3f46}
.alarm{cursor:pointer;padding:8px;border:1px solid #334155;border-radius:10px;margin:6px 0;background:#0f172a}
.small{font-size:12px;color:#94a3b8}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(72px,1fr));gap:8px;margin-top:8px;max-width:100%}
.cell{border:1px solid #334155;border-radius:8px;padding:8px;text-align:center;background:#0b1220;min-width:0}
.cell .n{font-size:12px;color:#93c5fd;display:block;margin-bottom:4px}
.cell .v{font-weight:800;font-size:15px;letter-spacing:.3px}
.cell.on{background:#14532d;border-color:#22c55e;color:#dcfce7}
.cell.on .v{color:#86efac}
.cell.off{background:#1f2937;border-color:#6b7280;color:#e5e7eb}
.cell.off .v{color:#f3f4f6}
canvas{width:100%;max-width:100%;background:#fff;border:1px solid #d7dbea;border-radius:8px;margin:6px 0}
</style></head>
<body><h3>RS485COM <span class='small'>v20260312c</span></h3>
<div id='mainPage'>
<div class='card'>
<label>Mode<select id='mode'><option value='plc'>PLC</option><option value='inv'>INV</option></select></label>
<div id='plcCfg'>
<label>PLC Baud<select id='plcBaud'><option>1200</option><option>2400</option><option>4800</option><option selected>9600</option><option>19200</option><option>38400</option><option>57600</option><option>115200</option></select></label>
<label>PLC Format<select id='plcFmt'><option>7O1</option><option>7E1</option><option>8N1</option><option>8E1</option><option>8O1</option><option>8E2</option></select></label>
</div>
<div id='invCfg'>
<label>通信方式<select id='invProto' onchange='updateInvParamView()'><option value='clink'>計算機リンク</option><option value='modbus'>Modbus RTU</option></select></label>
<label>インバータ号機番号<input id='invUnit' type='number' min='0' max='31' value='0'></label>
<label>INV Baud<select id='invBaud'><option>1200</option><option>2400</option><option>4800</option><option>9600</option><option selected>19200</option><option>38400</option><option>57600</option><option>115200</option></select></label>
<label>データ長<select id='invDataBits'><option value='7'>7</option><option value='8' selected>8</option></select></label>
<label>パリティ<select id='invParity'><option value='N'>None</option><option value='E' selected>Even</option><option value='O'>Odd</option></select></label>
<label>ストップビット<select id='invStopBits'><option value='1'>1</option><option value='2' selected>2</option></select></label>
</div>
<div id='topActionWrap'>
<button onclick='save()'>Save & Apply</button>
<button id='topReadBtn' onclick='readNowByMode()'>Read INV</button>
</div>
</div>

<div class='card' id='plcCard'>
<h4>PLC Monitor</h4>
<div id='plcItems'></div>

</div>

<div class='card' id='plcBottomActions'>
<button onclick='save()'>Save & Apply</button>
<button onclick='openPlcPage()'>Read PLC</button>
</div>

</div>

<div class='card' id='plcPage' style='display:none'>
<h4>PLC Dashboard</h4>
<button onclick='backToMain()'>← Back</button>
<div id='plcStatus' class='small'></div>
<div id='plcOut' class='small'></div>
</div>

<div class='card' id='invCard' style='display:none'>
<h4>INV Dashboard</h4>
<button onclick='backToMain()'>← Back</button>
<div id='invDash' style='display:none'>
<div id='invKpi'></div>
<div id='invStatus'></div>
<div class='card'>
  <h5>Trend</h5>
  <div class='small'>Hz(左軸) + V(右軸)</div>
  <canvas id='hzvChart' width='640' height='180'></canvas>
  <div class='small'>I</div>
  <canvas id='curChart' width='640' height='160'></canvas>
</div>
<h5>Alarm History (名称のみ / クリックで詳細)</h5>
<button onclick='readInvAlarms()'>Read Alarms</button>
<div id='alarms'></div>
</div>
</div>

<div id='fabWrap'>
  <div class='card small'>デバイス時刻: <span id='devTime'>-</span></div>
  <button class='fab' onclick='syncTime()'>時刻同期</button>
  <button class='fab' onclick='openSaveSettings()'>保存設定</button>
  <div id='savePanel' class='card' style='display:none;min-width:240px'>
    <label><input type='checkbox' id='logEnable'> CSV保存有効</label>
    <label>保存間隔
      <select id='logInterval'>
        <option value='100'>0.1秒</option>
        <option value='1000' selected>1秒</option>
        <option value='10000'>10秒</option>
        <option value='30000'>30秒</option>
        <option value='60000'>1分</option>
        <option value='600000'>10分</option>
        <option value='1800000'>30分</option>
        <option value='3600000'>1時間</option>
      </select>
    </label>
    <label>ファイル名(任意)<input id='logFile' placeholder='空欄で自動'></label>
    <button onclick='saveLogSettings()'>保存</button>
    <button onclick='closeSaveSettings()'>閉じる</button>
  </div>
</div>

<script>
let timer=null;
let pollBusy=false;
let invActive=false;
let plcActive=false;
let plcCfgItems=[];
let plcDashInited=false;
let hzHist=[], vHist=[], aHist=[];
let lastAlarms=[];
let expandedAlarm={};
const $ = (id)=>document.getElementById(id);
function row(i,it){const d=(it.dev||'D');return `<div style="border:1px solid #ddd;padding:6px;margin:6px 0">#${i+1} Dev:<select id='d${i}'><option ${d==='D'?'selected':''}>D</option><option ${d==='X'?'selected':''}>X</option><option ${d==='Y'?'selected':''}>Y</option><option ${d==='M'?'selected':''}>M</option><option ${d==='L'?'selected':''}>L</option><option ${d==='F'?'selected':''}>F</option><option ${d==='V'?'selected':''}>V</option><option ${d==='B'?'selected':''}>B</option><option ${d==='W'?'selected':''}>W</option><option ${d==='R'?'selected':''}>R</option><option ${d==='ZR'?'selected':''}>ZR</option><option ${d==='S'?'selected':''}>S</option><option ${d==='T'?'selected':''}>T</option><option ${d==='C'?'selected':''}>C</option><option ${d==='TN'?'selected':''}>TN</option><option ${d==='CN'?'selected':''}>CN</option></select> Addr:<input id='a${i}' type='number' value='${it.addr}' style='width:90px'> View:<select id='v${i}'><option ${it.view==='word'?'selected':''}>word</option><option ${it.view==='bit'?'selected':''}>bit</option></select> Width:<select id='w${i}'><option ${it.width==16?'selected':''}>16</option><option ${it.width==32?'selected':''}>32</option></select> Signed:<select id='s${i}'><option value='0' ${!it.sign?'selected':''}>no</option><option value='1' ${it.sign?'selected':''}>yes</option></select></div>`}
function updateInvParamView(){
  const isMb = $('invProto').value === 'modbus';
  $('invUnit').min = isMb ? '1' : '0';
  $('invUnit').max = isMb ? '247' : '31';
  if (Number($('invUnit').value) < Number($('invUnit').min)) $('invUnit').value = $('invUnit').min;
  if (Number($('invUnit').value) > Number($('invUnit').max)) $('invUnit').value = $('invUnit').max;
  // modbus: data bits fixed 8
  $('invDataBits').value = isMb ? '8' : $('invDataBits').value;
  $('invDataBits').disabled = isMb;
}
function updateModePanels(){

  const isInv = $('mode').value==='inv';
  $('plcCard').style.display = isInv ? 'none' : 'block';
  $('plcCfg').style.display = isInv ? 'none' : 'block';
  $('invCfg').style.display = isInv ? 'block' : 'none';
  $('plcBottomActions').style.display = isInv ? 'none' : 'block';
  $('topActionWrap').style.display = isInv ? 'block' : 'none';
  $('topReadBtn').textContent = isInv ? 'Read INV' : 'Read PLC';
  if(!isInv && !plcActive){ invActive=false; $('invDash').style.display='none'; $('invCard').style.display='none'; $('mainPage').style.display='block'; }
}
function readNowByMode(){
  if($('mode').value==='inv') readInvNow();
  else openPlcPage();
}
function openPlcPage(){
  plcActive=true;
  invActive=false;
  plcDashInited=false;
  $('mainPage').style.display='none';
  $('invCard').style.display='none';
  $('plcPage').style.display='block';
  readPlcNow();
}
function backToMain(){
  invActive=false;
  plcActive=false;
  $('invCard').style.display='none';
  $('plcPage').style.display='none';
  $('mainPage').style.display='block';
}

async function load(){
  let r=await fetch('/cfg');let j=await r.json();
  $('mode').value=j.mode; $('plcBaud').value=String(j.plcBaud); $('plcFmt').value=j.plcFmt; $('invBaud').value=String(j.invBaud);
  $('invProto').value=j.invProto || 'clink';
  $('invUnit').value=String((j.invUnit ?? 0));
  $('invDataBits').value=String(j.invDataBits || 8);
  $('invParity').value=String(j.invParity || 'E');
  $('invStopBits').value=String(j.invStopBits || 2);
  updateInvParamView();
  $('logEnable').checked = !!j.logEnabled;
  $('logInterval').value = String(j.logIntervalMs || 1000);
  $('logFile').value = j.logFilename || '';
  let pr=await fetch('/plccfg'); let pj=await pr.json(); plcCfgItems=pj.items||[]; $('plcItems').innerHTML=plcCfgItems.map((it,i)=>row(i,it)).join('');
  updateModePanels();
  startPolling();
}

function openInvPage(){
  invActive=true;
  $('mainPage').style.display='none';
  $('invCard').style.display='block';
  $('invDash').style.display='block';
}

async function save(){
  // PLC item settings are saved together via Save & Apply.
  await savePlc();
  const m = $('mode').value;
  let p=new URLSearchParams({mode:m,plcBaud:$('plcBaud').value,plcFmt:$('plcFmt').value,invBaud:$('invBaud').value,invProto:$('invProto').value,invUnit:$('invUnit').value,invDataBits:$('invDataBits').value,invParity:$('invParity').value,invStopBits:$('invStopBits').value});
  await fetch('/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
  updateModePanels();
  alert('保存されました');
}

async function savePlc(){
  let p=new URLSearchParams();
  for(let i=0;i<5;i++){
    p.append('dev'+i,document.getElementById('d'+i).value);
    p.append('addr'+i,document.getElementById('a'+i).value);
    p.append('view'+i,document.getElementById('v'+i).value);
    p.append('width'+i,document.getElementById('w'+i).value);
    p.append('sign'+i,document.getElementById('s'+i).value);
  }
  await fetch('/plcset',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
}

function plcBitGrid16ById(id,v){
  let h = `<div class='grid' style='grid-template-columns:repeat(8,minmax(38px,1fr));'>`;
  for(let b=15;b>=0;b--){
    const on = ((v>>>b)&1)===1;
    h += `<div id='bit_${id}_${b}' class='cell ${on?'on':'off'}' style='padding:6px'><span class='n'>b${b}</span><span class='v'>${on?'1':'0'}</span></div>`;
  }
  h += `</div>`;
  return h;
}
function renderPlcSkeleton(items){
  $('plcOut').innerHTML = items.map(it=>{
    const isBit = String(it.view||'word')==='bit';
    const signLabel = it.sign ? 'sign' : 'unsign';
    const head = `#${it.idx+1} ${it.dev}${it.addr} (${it.view}/${it.width}/${signLabel}) --`;
    if(isBit){
      return `<div class='card'><div id='head_${it.idx}' class='small'>${head}</div>${plcBitGrid16ById(it.idx,0)}</div>`;
    }
    return `<div class='card'><div id='head_${it.idx}' class='small'>${head}</div><div id='val_${it.idx}' class='kpi'>-</div></div>`;
  }).join('');
  plcDashInited = true;
}
function applyPlcItemUpdate(it){
  const isBit = String(it.view||'word')==='bit';
  const signLabel = it.sign ? 'sign' : 'unsign';
  const h = document.getElementById('head_'+it.idx);
  if(h) h.textContent = `#${it.idx+1} ${it.dev}${it.addr} (${it.view}/${it.width}/${signLabel}) ${it.ok?'OK':'NG'}`;
  if(!it.ok) return;
  if(isBit){
    const v = Number(it.u32)||0;
    for(let b=15;b>=0;b--){
      const el = document.getElementById(`bit_${it.idx}_${b}`);
      if(!el) continue;
      const on = ((v>>>b)&1)===1;
      el.className = `cell ${on?'on':'off'}`;
      const vv = el.querySelector('.v'); if(vv) vv.textContent = on?'1':'0';
    }
  }else{
    const val = it.sign ? it.s32 : it.u32;
    const el = document.getElementById('val_'+it.idx); if(el) el.textContent = String(val);
  }
}
async function readPlcNow(){
  try{
    const ac = new AbortController();
    const t = setTimeout(()=>ac.abort(), 600);
    let r=await fetch('/plcread_fast',{signal:ac.signal});
    clearTimeout(t);
    let j=await r.json();
    if(!plcDashInited) renderPlcSkeleton(j.items||[]);
    (j.updated||[]).forEach(applyPlcItemUpdate);
    $('plcStatus').innerHTML = "<div class='card small'>更新: " + new Date().toLocaleTimeString() + "</div>";
  }catch(e){
    $('plcStatus').innerHTML = "<div class='card small'>Read PLC失敗: " + e + "</div>";
  }
}

function bitCell(name,v){return `<div class='cell ${v?'on':'off'}'><span class='n'>${name}</span><span class='v'>${v?'ON':'OFF'}</span></div>`}
function pushHist(arr,v){ if(v>=0){arr.push(v); if(arr.length>120) arr.shift();} }
function drawLines(cv, series, colors, minY, maxY, padL=34, padR=12){
  const ctx=cv.getContext('2d'); const w=cv.width,h=cv.height;
  ctx.clearRect(0,0,w,h); ctx.fillStyle='#fff'; ctx.fillRect(0,0,w,h);
  const plotW=w-padL-padR, plotH=h-24;
  ctx.strokeStyle='#e9edf7';
  for(let i=0;i<=4;i++){const y=8+i*plotH/4; ctx.beginPath(); ctx.moveTo(padL,y); ctx.lineTo(w-padR,y); ctx.stroke();}
  series.forEach((arr,idx)=>{
    if(arr.length<2) return;
    ctx.strokeStyle=colors[idx]; ctx.lineWidth=2; ctx.beginPath();
    for(let i=0;i<arr.length;i++){
      const x=padL+i*plotW/(Math.max(1,arr.length-1));
      const y=8+plotH-( (arr[i]-minY)/(Math.max(0.0001,maxY-minY)) )*plotH;
      if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
    }
    ctx.stroke();
  });
}
function drawLegend(ctx, items, x, y){
  ctx.font='12px sans-serif';
  items.forEach((it,idx)=>{
    const yy=y+idx*14;
    ctx.fillStyle=it.c; ctx.fillRect(x,yy-8,10,10);
    ctx.fillStyle='#334'; ctx.fillText(it.t,x+14,yy);
  });
}
function drawInvCharts(){
  const hzv = document.getElementById('hzvChart');
  const cur = document.getElementById('curChart');
  if(!hzv || !cur) return;
  const hzMin=Math.min(...hzHist,0), hzMax=Math.max(...hzHist,1);
  const vMin=Math.min(...vHist,0), vMax=Math.max(...vHist,1);
  const cMin=Math.min(...aHist,0), cMax=Math.max(...aHist,1);

  const hzN=hzHist.map(v=>(v-hzMin)/Math.max(0.0001,hzMax-hzMin));
  const vN=vHist.map(v=>(v-vMin)/Math.max(0.0001,vMax-vMin));
  drawLines(hzv,[hzN,vN],['#2e7dff','#ff6d00'],0,1,40,40);
  let c1=hzv.getContext('2d');
  c1.fillStyle='#2e7dff'; c1.font='12px sans-serif';
  c1.fillText(`${hzMax.toFixed(1)} Hz`,4,16); c1.fillText(`${hzMin.toFixed(1)} Hz`,4,hzv.height-6);
  c1.fillStyle='#ff6d00';
  c1.fillText(`${vMax.toFixed(1)} V`,hzv.width-38,16); c1.fillText(`${vMin.toFixed(1)} V`,hzv.width-38,hzv.height-6);
  drawLegend(c1,[{t:'Freq(Hz)',c:'#2e7dff'},{t:'Volt(V)',c:'#ff6d00'}],hzv.width-120,34);

  drawLines(cur,[aHist],['#26a69a'],cMin,cMax,40,12);
  let c2=cur.getContext('2d');
  c2.fillStyle='#26a69a'; c2.font='12px sans-serif';
  c2.fillText(`${cMax.toFixed(2)} A`,4,16); c2.fillText(`${cMin.toFixed(2)} A`,4,cur.height-6);
  drawLegend(c2,[{t:'Current(I)',c:'#26a69a'}],cur.width-120,20);
}
function renderAlarms(){
  alarms.innerHTML = lastAlarms.map((a,i)=>`<div class='alarm' onclick="toggleDetail(${i})"><b>${a.code} ${a.name}</b><div id='d${i}' class='small' style='display:${expandedAlarm[i]?'block':'none'};margin-top:4px'>${a.detail}</div></div>`).join('');
}
function renderInv(j, updateAlarms){
  invKpi.innerHTML = `<div class='kpi'>Hz: ${j.freqHz}</div><div class='kpi'>I: ${j.currentA}</div><div class='kpi'>V: ${j.voltageV}</div><div class='kpi'>St: ${j.statusHex}</div>`;
  invStatus.innerHTML = `<div class='grid'>`
    + bitCell('RUN',j.status.run)
    + bitCell('FWD',j.status.fwd)
    + bitCell('REV',j.status.rev)
    + bitCell('SU',j.status.su)
    + bitCell('OL',j.status.ol)
    + bitCell('FU',j.status.fu)
    + bitCell('ABC',j.status.abc)
    + bitCell('ALM',j.status.alm)
    + `</div>`;
  pushHist(hzHist, Number(j.freqHz));
  pushHist(aHist, Number(j.currentA));
  pushHist(vHist, Number(j.voltageV));
  drawInvCharts();
  if(updateAlarms || lastAlarms.length===0){ lastAlarms = j.alarms || []; renderAlarms(); }
}
function toggleDetail(i){ expandedAlarm[i]=!expandedAlarm[i]; renderAlarms(); }

async function readInvNow(){
  openInvPage();
  let r=await fetch('/invread'); let j=await r.json(); renderInv(j,false);
}
async function readInvAlarms(){
  invActive=true;
  invDash.style.display='block';
  let r=await fetch('/invalarms'); let j=await r.json(); renderInv(j,true);
}

async function syncTime(){
  const epoch = Math.floor(Date.now()/1000);
  const tzMin = new Date().getTimezoneOffset();
  let p = new URLSearchParams({epoch:String(epoch), tzMin:String(tzMin)});
  let r = await fetch('/timesync',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
  let j = await r.json();
  alert(j && j.rtc ? '時刻同期完了' : '時刻同期失敗');
  await refreshDeviceTime();
}

async function refreshDeviceTime(){
  try { let r=await fetch('/time'); let j=await r.json(); $('devTime').textContent = j.now || '-'; } catch(e){}
}

function openSaveSettings(){
  $('savePanel').style.display='block';
}
function closeSaveSettings(){
  $('savePanel').style.display='none';
}
async function saveLogSettings(){
  let p = new URLSearchParams({
    enabled: $('logEnable').checked?'1':'0',
    intervalMs: $('logInterval').value,
    filename: $('logFile').value || '',
    target: $('mode').value
  });
  let r = await fetch('/logcfg',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
  let j = await r.json();
  alert(`保存設定: enabled=${j.enabled} interval=${j.intervalMs} path=${j.path||'(auto)'}`);
  closeSaveSettings();
}

function startPolling(){
  if(timer){ clearTimeout(timer); timer=null; }
  const tick = async ()=>{
    if(pollBusy){ timer=setTimeout(tick,1); return; }
    pollBusy = true;
    try{
      if($('mode').value==='plc' && plcActive) await readPlcNow();
      else if(invActive) await readInvNow();
    }catch(e){}
    finally{ pollBusy = false; timer=setTimeout(tick,1); }
  };
  timer=setTimeout(tick,1);
}

$('mode').addEventListener('change',()=>{ if($('mode').value==='inv') plcActive=false; updateModePanels(); startPolling(); });
setInterval(refreshDeviceTime, 2000); refreshDeviceTime();
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
    j += "\"invFmt\":\"" + g_invProfile.fmt + "\",";
    j += "\"invProto\":\"" + g_invProto + "\",";
    j += "\"invUnit\":" + String(g_invUnit) + ",";
    j += "\"invDataBits\":" + String(g_invDataBits) + ",";
    j += "\"invParity\":\"" + String(g_invParity) + "\",";
    j += "\"invStopBits\":" + String(g_invStopBits) + ",";
    j += "\"logEnabled\":" + String(g_logCfg.enabled?"true":"false") + ",";
    j += "\"logIntervalMs\":" + String(g_logCfg.intervalMs) + ",";
    j += "\"logTarget\":\"" + g_logCfg.target + "\",";
    j += "\"logFilename\":\"" + g_logCfg.filename + "\"}";
    g_web.send(200, "application/json", j);
  });

  g_web.on("/cfgfile", HTTP_GET, []() {
    if (!SPIFFS.exists("/runtime_cfg.txt")) { g_web.send(200, "text/plain", "CFG_MISSING"); return; }
    File f = SPIFFS.open("/runtime_cfg.txt", FILE_READ);
    if (!f) { g_web.send(500, "text/plain", "CFG_OPEN_NG"); return; }
    String txt = f.readString();
    f.close();
    g_web.send(200, "text/plain", txt);
  });

  g_web.on("/set", HTTP_POST, []() {
    if (g_web.hasArg("plcBaud")) g_plcProfile.baud = g_web.arg("plcBaud").toInt();
    if (g_web.hasArg("plcFmt")) g_plcProfile.fmt = g_web.arg("plcFmt");
    if (g_web.hasArg("invBaud")) g_invProfile.baud = g_web.arg("invBaud").toInt();
    if (g_web.hasArg("invProto")) g_invProto = g_web.arg("invProto");
    if (g_web.hasArg("invDataBits")) g_invDataBits = (uint8_t)g_web.arg("invDataBits").toInt();
    if (g_web.hasArg("invParity")) { String pv = g_web.arg("invParity"); g_invParity = pv.length()?pv[0]:'N'; }
    if (g_web.hasArg("invStopBits")) g_invStopBits = (uint8_t)g_web.arg("invStopBits").toInt();
    if (g_invProto == "modbus") g_invDataBits = 8;
    if (g_web.hasArg("invUnit")) {
      int u = g_web.arg("invUnit").toInt();
      if (g_invProto == "modbus") { if (u < 1) u = 1; if (u > 247) u = 247; }
      else { if (u < 0) u = 0; if (u > 31) u = 31; }
      g_invUnit = (uint16_t)u;
    }
    g_invProfile.fmt = makeInvFmt(g_invDataBits, g_invParity, g_invStopBits);
    if (g_web.hasArg("mode") && g_web.arg("mode") == "inv") applySerialProfile(MODE_INV_FRD820);
    else applySerialProfile(MODE_PLC_FX5_1C);
    saveRuntimeConfig();
    g_web.send(200, "text/plain", "OK");
  });

  g_web.on("/timesync", HTTP_POST, []() {
    if (!g_web.hasArg("epoch")) { g_web.send(400, "text/plain", "epoch required"); return; }
    long ep = g_web.arg("epoch").toInt();
    long tzMin = g_web.hasArg("tzMin") ? g_web.arg("tzMin").toInt() : 0;
    long epLocal = ep - (tzMin * 60L);
    bool rtcOk = rtc.begin();
    if (rtcOk) rtc.adjust(DateTime((uint32_t)epLocal));
    g_web.send(200, "application/json", String("{\"ok\":true,\"rtc\":") + (rtcOk ? "true" : "false") + "}");
  });

  g_web.on("/time", HTTP_GET, []() {
    DateTime dt = rtc.now();
    char b[32];
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
    g_web.send(200, "application/json", String("{\"now\":\"") + b + "\"}");
  });

  g_web.on("/logcfg", HTTP_POST, []() {
    if (g_web.hasArg("enabled")) g_logCfg.enabled = (g_web.arg("enabled") == "1");
    if (g_web.hasArg("intervalMs")) {
      long iv = g_web.arg("intervalMs").toInt();
      if (iv < 50) iv = 50;
      g_logCfg.intervalMs = (uint32_t)iv;
    }
    if (g_web.hasArg("filename")) g_logCfg.filename = g_web.arg("filename");
    if (g_web.hasArg("target")) g_logCfg.target = g_web.arg("target");
    if (!g_logCfg.enabled) {
      g_logPath = "";
      g_logCfg.lastWriteMs = millis();
    } else {
      g_logPath = ""; // force re-create with start timestamp/custom name
      g_logCfg.lastWriteMs = millis() - g_logCfg.intervalMs; // start immediately
      maybeWriteCsvLog();
    }
    saveRuntimeConfig();
    String j = String("{\"ok\":true,\"enabled\":") + (g_logCfg.enabled?"true":"false")
             + ",\"intervalMs\":" + String(g_logCfg.intervalMs)
             + ",\"target\":\"" + g_logCfg.target + "\""
             + ",\"path\":\"" + g_logPath + "\"}";
    g_web.send(200, "application/json", j);
  });

  g_web.on("/logcfg", HTTP_GET, []() {
    String j = String("{\"enabled\":") + (g_logCfg.enabled?"true":"false")
             + ",\"intervalMs\":" + String(g_logCfg.intervalMs)
             + ",\"target\":\"" + g_logCfg.target + "\""
             + ",\"path\":\"" + g_logPath + "\"}";
    g_web.send(200, "application/json", j);
  });

  g_web.on("/plccfg", HTTP_GET, []() {
    String j = "{\"items\":[";
    for (int i = 0; i < 5; i++) {
      if (i) j += ",";
      j += "{\"dev\":\"" + g_plcItems[i].dev + "\""
        + ",\"addr\":" + String(g_plcItems[i].addr)
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
      String kD = "dev" + String(i), kA = "addr" + String(i), kV = "view" + String(i), kW = "width" + String(i), kS = "sign" + String(i);
      if (g_web.hasArg(kD)) g_plcItems[i].dev = g_web.arg(kD);
      if (g_web.hasArg(kA)) g_plcItems[i].addr = (uint16_t)g_web.arg(kA).toInt();
      if (g_web.hasArg(kV)) g_plcItems[i].view = g_web.arg(kV);
      if (g_web.hasArg(kW)) g_plcItems[i].width = (uint8_t)g_web.arg(kW).toInt();
      if (g_web.hasArg(kS)) g_plcItems[i].sign = (g_web.arg(kS) == "1");
    }
    saveRuntimeConfig();
    g_web.send(200, "text/plain", "OK");
  });

  g_web.on("/plcread", HTTP_GET, []() {
    if (g_mode != MODE_PLC_FX5_1C) applySerialProfile(MODE_PLC_FX5_1C);

    // Faster scan: read two items per request (round-robin), return cached snapshot for all.
    uint8_t i = g_plcScanIdx % 5;
    for (int step=0; step<2; step++) {
      uint8_t k = (i + step) % 5;
      uint32_t u = 0;
      uint8_t words = (g_plcItems[k].width == 32) ? 2 : 1;
      bool ok = plcReadWords1C(g_plcItems[k].dev, g_plcItems[k].addr, words, u);
      g_plcLastOk[k] = ok;
      if (ok) g_plcLastU32[k] = u;
    }
    g_plcScanIdx = (g_plcScanIdx + 2) % 5;

    String j = "{\"scanIdx\":" + String(i) + ",\"items\":[";
    for (int k = 0; k < 5; k++) {
      if (k) j += ",";
      uint32_t cu = g_plcLastU32[k];
      bool cok = g_plcLastOk[k];
      j += "{\"idx\":" + String(k)
        + ",\"dev\":\"" + g_plcItems[k].dev + "\""
        + ",\"addr\":" + String(g_plcItems[k].addr)
        + ",\"view\":\"" + g_plcItems[k].view + "\""
        + ",\"width\":" + String(g_plcItems[k].width)
        + ",\"sign\":" + String(g_plcItems[k].sign ? "true" : "false")
        + ",\"ok\":" + String(cok ? "true" : "false")
        + ",\"u32\":" + String(cu)
        + ",\"s32\":" + String((int32_t)cu)
        + "}";
    }
    j += "]}";
    g_web.send(200, "application/json", j);
  });

  g_web.on("/plcread_fast", HTTP_GET, []() {
    if (g_mode != MODE_PLC_FX5_1C) applySerialProfile(MODE_PLC_FX5_1C);

    uint8_t i = g_plcScanIdx % 5;
    uint8_t ks[2] = { (uint8_t)(i % 5), (uint8_t)((i + 1) % 5) };
    for (int step=0; step<2; step++) {
      uint8_t k = ks[step];
      uint32_t u = 0;
      uint8_t words = (g_plcItems[k].width == 32) ? 2 : 1;
      bool ok = plcReadWords1C(g_plcItems[k].dev, g_plcItems[k].addr, words, u);
      g_plcLastOk[k] = ok;
      if (ok) g_plcLastU32[k] = u;
    }
    g_plcScanIdx = (g_plcScanIdx + 2) % 5;

    String j = "{\"items\":[";
    for (int k = 0; k < 5; k++) {
      if (k) j += ",";
      j += "{\"idx\":" + String(k)
        + ",\"dev\":\"" + g_plcItems[k].dev + "\""
        + ",\"addr\":" + String(g_plcItems[k].addr)
        + ",\"view\":\"" + g_plcItems[k].view + "\""
        + ",\"width\":" + String(g_plcItems[k].width)
        + ",\"sign\":" + String(g_plcItems[k].sign ? "true" : "false")
        + "}";
    }
    j += "],\"updated\":[";
    for (int n = 0; n < 2; n++) {
      int k = ks[n];
      if (n) j += ",";
      uint32_t cu = g_plcLastU32[k];
      bool cok = g_plcLastOk[k];
      j += "{\"idx\":" + String(k)
        + ",\"dev\":\"" + g_plcItems[k].dev + "\""
        + ",\"addr\":" + String(g_plcItems[k].addr)
        + ",\"view\":\"" + g_plcItems[k].view + "\""
        + ",\"width\":" + String(g_plcItems[k].width)
        + ",\"sign\":" + String(g_plcItems[k].sign ? "true" : "false")
        + ",\"ok\":" + String(cok ? "true" : "false")
        + ",\"u32\":" + String(cu)
        + ",\"s32\":" + String((int32_t)cu)
        + "}";
    }
    j += "]}";
    g_web.send(200, "application/json", j);
  });

  g_web.on("/invread", HTTP_GET, []() {
    if (g_mode != MODE_INV_FRD820) applySerialProfile(MODE_INV_FRD820);
    String j = buildInvReadJson();
    g_web.send(200, "application/json", j);
  });

  g_web.on("/invalarms", HTTP_GET, []() {
    if (g_mode != MODE_INV_FRD820) applySerialProfile(MODE_INV_FRD820);
    refreshInvAlarms();
    String j = buildInvReadJson();
    g_web.send(200, "application/json", j);
  });

  g_web.begin();
}

bool readOnce() {
  uint8_t req[32];
  size_t reqLen = buildReadD2000Frame(req);

  while (Serial1.available()) Serial1.read();

  rs485TxMode();
  delay(10);
  Serial1.write(req, reqLen);
  Serial1.flush();
  delay(10);
  rs485RxMode();

  uint8_t res[128];
  size_t n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 10) {
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

  bool fsok = SPIFFS.begin(false);
  if (!fsok) {
    Serial.println("SPIFFS mount NG -> format");
    fsok = SPIFFS.begin(true);
  }
  if (fsok) loadRuntimeConfig();
  else Serial.println("SPIFFS unavailable");
  applySerialProfile(g_mode);
  Wire.begin();
  if (!rtc.begin()) {
    Serial.println("RTC NG");
  } else {
    if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  if (!SD.begin(SD_CS_PIN, SPI, 24000000, "/sd")) {
    Serial.println("SD NG");
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID); // open AP (no password)
  setupWebUi();
  Serial.print("AP up: "); Serial.println(AP_SSID);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  Serial.println("xiao_c3_rs485_mc ready");
  Serial.println("BUILD_TAG=X92");
  Serial.println("type: read3e / read1e / readasc / read1c / readinv");
  Serial.println("type: setproto plc / setproto inv / proto");
}

static uint16_t g_f=0, g_i=0, g_v=0, g_st=0;
static bool g_fok=false, g_iok=false, g_vok=false, g_stok=false;
static uint8_t g_fastIdx=0;
static uint16_t g_h74=0, g_h75=0, g_h76=0, g_h77=0;
static bool g_h74ok=false, g_h75ok=false, g_h76ok=false, g_h77ok=false;
static bool g_alarmInitialized=false;

void mbPreTx() { rs485TxMode(); delayMicroseconds(120); }
void mbPostTx() { Serial1.flush(); delayMicroseconds(120); rs485RxMode(); }

bool readInverterOnceClink(const char *cmd2, uint16_t &valueOut) {
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

  auto trySend = [&](const String &body, bool addLF, uint16_t timeoutMs) -> bool {
    // clear stale RX before each transaction
    while (Serial1.available()) Serial1.read();

    String cks = checksum2(body);

    rs485TxMode();
    delay(10);
    Serial1.write(ENQ);
    Serial1.print(body);
    Serial1.print(cks);   // checksum required by inverter
    Serial1.write(CR);    // CR only by default
    if (addLF) Serial1.write(LF);
    Serial1.flush();
    delay(10);
    rs485RxMode();

    uint8_t raw[96]; size_t n = 0;
    unsigned long t0 = millis();
    bool hasStx = false;
    while (millis() - t0 < timeoutMs && n < sizeof(raw)) {
      if (Serial1.available()) {
        uint8_t c = (uint8_t)Serial1.read();
        raw[n++] = c;
        if (c == 0x02) hasStx = true;
        if (hasStx && c == 0x03) break;
      }
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
  uint8_t st = (uint8_t)g_invUnit;
  if (st > 0x1F) st = 0; // clink practical range guard
  char stHex[3]; snprintf(stHex, sizeof(stHex), "%02X", st);
  String b2 = String(stHex) + cmd2 + "0";

  for (int r = 0; r < 3; r++) {
    if (trySend(b2, false, 250)) return true;
    delay(5);
  }
  // keep CR-only per user setting
  return false;
}

bool readInverterOnceModbus(const char *cmd2, uint16_t &valueOut) {
  // FR-D Modbus register map (manual):
  // 6F->40201(freq), 70->40202(current), 71->40203(voltage), 79->40009(status), 74..77->40501..40504(alarm hist)
  uint16_t reg4x = 0;
  String c = String(cmd2);
  if (c == "6F") reg4x = 40201;
  else if (c == "70") reg4x = 40202;
  else if (c == "71") reg4x = 40203;
  else if (c == "79") reg4x = 40009;
  else if (c == "74") reg4x = 40501;
  else if (c == "75") reg4x = 40502;
  else if (c == "76") reg4x = 40503;
  else if (c == "77") reg4x = 40504;
  else return false;

  // Modbus start address = register number - 40001
  uint16_t addr = reg4x - 40001;

  if (g_invUnit < 1 || g_invUnit > 247) g_invUnit = 1;
  g_mb.begin((uint8_t)g_invUnit, Serial1);
  g_mb.preTransmission(mbPreTx);
  g_mb.postTransmission(mbPostTx);
  while (Serial1.available()) Serial1.read();
  uint8_t rc = g_mb.readHoldingRegisters(addr, 1);
  if (rc == g_mb.ku8MBSuccess) {
    valueOut = g_mb.getResponseBuffer(0);
    Serial.print("MB reg="); Serial.print(reg4x);
    Serial.print(" addr="); Serial.print(addr);
    Serial.print(" val=0x"); Serial.println(valueOut, HEX);
    return true;
  }
  Serial.print("MB NG reg="); Serial.print(reg4x);
  Serial.print(" rc="); Serial.println(rc);
  return false;
}

bool readInverterOnce(const char *cmd2, uint16_t &valueOut) {
  if (g_invProto == "modbus") return readInverterOnceModbus(cmd2, valueOut);
  return readInverterOnceClink(cmd2, valueOut);
}

String invAlarmCodeName(uint8_t c) {
  switch (c) {
    case 0x00: return "No alarm";
    case 0x10: return "E.OC1";
    case 0x11: return "E.OC2";
    case 0x12: return "E.OC3";
    case 0x22: return "E.OV3";
    case 0x30: return "E.THT";
    case 0x31: return "E.THM";
    case 0x40: return "E.FIN";
    case 0x52: return "E.ILF";
    case 0x60: return "E.OLT";
    case 0x70: return "E.BE";
    case 0x80: return "E.GF";
    case 0x81: return "E.LF";
    case 0xB0: return "E.PE";
    case 0xB1: return "E.PUE";
    case 0xB2: return "E.RET";
    case 0xC0: return "E.CPU";
    case 0xC4: return "E.CDO";
    case 0xC5: return "E.IOH";
    case 0xC7: return "E.AIE";
    case 0xC9: return "E.SAF";
    case 0xF5: return "E.5";
    default: {
      char b[16]; snprintf(b, sizeof(b), "Unknown(0x%02X)", c); return String(b);
    }
  }
}

String invAlarmDetail(uint8_t c) {
  switch (c) {
    case 0x00: return "異常なし";
    case 0xB1: return "PU抜け/通信異常（通信途絶・受信異常連続）";
    case 0xB0: return "パラメータ記憶素子異常";
    case 0xC4: return "出力電流検出値オーバー";
    case 0xC7: return "アナログ入力異常";
    case 0x10: case 0x11: case 0x12: return "過電流系異常";
    case 0x22: return "過電圧異常";
    case 0x30: case 0x31: return "過熱系異常";
    default: return "詳細はマニュアル異常一覧を参照";
  }
}

void refreshInvAlarms() {
  g_h74ok = readInverterOnce("74", g_h74);
  g_h75ok = readInverterOnce("75", g_h75);
  g_h76ok = readInverterOnce("76", g_h76);
  g_h77ok = readInverterOnce("77", g_h77);
  g_alarmInitialized = true;
}

void maybeWriteCsvLog() {
  if (!g_logCfg.enabled) return;
  if (millis() - g_logCfg.lastWriteMs < g_logCfg.intervalMs) return;
  g_logCfg.lastWriteMs = millis();
  ensureLogFile();
  if (g_logPath.length() == 0) return;
  File f = SD.open(g_logPath, FILE_APPEND);
  if (!f) return;
  DateTime dt = rtc.now();
  char ts[24];
  snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d", dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
  if (g_logCfg.target == "inv") {
    uint16_t stView = g_st & ((1u<<0)|(1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<6)|(1u<<7)|(1u<<15));
    char hx[12]; snprintf(hx, sizeof(hx), "0x%X", stView);
    f.print(ts); f.print(",");
    f.print(g_fok ? (g_f / 100.0f) : -1); f.print(",");
    f.print(g_iok ? (g_i / 100.0f) : -1); f.print(",");
    f.print(g_vok ? (g_v / 10.0f) : -1); f.print(",");
    f.println(hx);
  } else {
    // PLC: save currently acquired configured addresses
    f.print(ts);
    for (int i=0;i<5;i++) {
      f.print(","); f.print(g_plcItems[i].dev); f.print(g_plcItems[i].addr); f.print("=");
      if (g_plcLastOk[i]) f.print(g_plcLastU32[i]); else f.print("NA");
    }
    f.println();
  }
  f.close();
}

String buildInvReadJson() {
  // Prioritize frequency every cycle to keep main KPI responsive
  g_fok = readInverterOnce("6F", g_f);

  // round-robin for the rest
  if (g_fastIdx == 0) g_iok  = readInverterOnce("70", g_i);
  else if (g_fastIdx == 1) g_vok  = readInverterOnce("71", g_v);
  else g_stok = readInverterOnce("79", g_st);
  g_fastIdx = (g_fastIdx + 1) % 3;

  // Alarm/history: read only once at first, then on-demand button
  if (!g_alarmInitialized) refreshInvAlarms();

  uint8_t a0 = (uint8_t)(g_h74 & 0x00FF);
  uint8_t a1 = (uint8_t)((g_h74 >> 8) & 0x00FF);
  uint8_t a2 = (uint8_t)(g_h75 & 0x00FF);
  uint8_t a3 = (uint8_t)((g_h75 >> 8) & 0x00FF);

  String j = "{";
  j += "\"ok\":" + String((g_fok||g_iok||g_vok||g_stok||g_h74ok||g_h75ok||g_h76ok||g_h77ok) ? "true" : "false") + ",";
  uint16_t stView = g_st & ((1u<<0)|(1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<6)|(1u<<7)|(1u<<15));
  j += "\"freqHz\":" + String(g_fok ? (g_f / 100.0f) : -1, 2) + ",";
  j += "\"currentA\":" + String(g_iok ? (g_i / 100.0f) : -1, 2) + ",";
  j += "\"voltageV\":" + String(g_vok ? (g_v / 10.0f) : -1, 1) + ",";
  j += "\"statusHex\":\"0x" + String(stView, HEX) + "\",";
  j += "\"status\":{";
  j += "\"run\":" + String((stView & (1u<<0)) ? "true":"false") + ",";
  j += "\"fwd\":" + String((stView & (1u<<1)) ? "true":"false") + ",";
  j += "\"rev\":" + String((stView & (1u<<2)) ? "true":"false") + ",";
  j += "\"su\":"  + String((stView & (1u<<3)) ? "true":"false") + ",";
  j += "\"ol\":"  + String((stView & (1u<<4)) ? "true":"false") + ",";
  j += "\"fu\":"  + String((stView & (1u<<6)) ? "true":"false") + ",";
  j += "\"abc\":" + String((stView & (1u<<7)) ? "true":"false") + ",";
  j += "\"alm\":" + String((stView & (1u<<15)) ? "true":"false");
  j += "},";
  j += "\"alarms\":[";
  uint8_t aa[4] = {a0,a1,a2,a3};
  for (int k=0;k<4;k++) {
    if (k) j += ",";
    char hc[8]; snprintf(hc, sizeof(hc), "0x%02X", aa[k]);
    j += "{\"code\":\"" + String(hc) + "\",\"name\":\"" + invAlarmCodeName(aa[k]) + "\",\"detail\":\"" + invAlarmDetail(aa[k]) + "\"}";
  }
  j += "]}";
  return j;
}

bool readInverterMonitors() {
  uint16_t f=0,i=0,v=0,st=0,h74=0,h75=0,h76=0,h77=0;
  bool okF  = readInverterOnce("6F", f);   // output frequency (0.01Hz)
  bool okI  = readInverterOnce("70", i);   // output current (0.01A)
  bool okV  = readInverterOnce("71", v);   // output voltage (0.1V)
  bool okSt = readInverterOnce("79", st);  // inverter status
  bool ok74 = readInverterOnce("74", h74); // alarm history latest/prev
  bool ok75 = readInverterOnce("75", h75); // alarm history
  bool ok76 = readInverterOnce("76", h76); // alarm history
  bool ok77 = readInverterOnce("77", h77); // alarm history

  if (okF) { Serial.print("INV freq_Hz="); Serial.println(f / 100.0f, 2); }
  else Serial.println("INV freq read NG");
  if (okI) { Serial.print("INV current_A="); Serial.println(i / 100.0f, 2); }
  else Serial.println("INV current read NG");
  if (okV) { Serial.print("INV voltage_V="); Serial.println(v / 10.0f, 1); }
  else Serial.println("INV voltage read NG");

  if (okSt) {
    Serial.print("INV status_hex=0x"); Serial.println(st, HEX);
    bool b0_run   = st & (1u << 0);
    bool b1_fwd   = st & (1u << 1);
    bool b2_rev   = st & (1u << 2);
    bool b3_su    = st & (1u << 3);   // frequency reached
    bool b4_ol    = st & (1u << 4);   // overload
    bool b6_fu    = st & (1u << 6);   // frequency detection
    bool b7_abc   = st & (1u << 7);   // alarm bit
    bool b15_trip = st & (1u << 15);  // alarm occurrence

    Serial.print("INV ST RUN="); Serial.println(b0_run ? "ON" : "OFF");
    Serial.print("INV ST FWD="); Serial.println(b1_fwd ? "ON" : "OFF");
    Serial.print("INV ST REV="); Serial.println(b2_rev ? "ON" : "OFF");
    Serial.print("INV ST SU ="); Serial.println(b3_su ? "ON" : "OFF");
    Serial.print("INV ST OL ="); Serial.println(b4_ol ? "ON" : "OFF");
    Serial.print("INV ST FU ="); Serial.println(b6_fu ? "ON" : "OFF");
    Serial.print("INV ST ABC="); Serial.println(b7_abc ? "ON" : "OFF");
    Serial.print("INV ST ALM="); Serial.println(b15_trip ? "ON" : "OFF");

    bool alarmNow = b7_abc || b15_trip;
    Serial.print("INV alarm_now="); Serial.println(alarmNow ? "YES" : "NO");
  } else {
    Serial.println("INV status read NG");
  }

  if (ok74 || ok75 || ok76 || ok77) {
    Serial.print("INV alarm_hist_74=0x"); Serial.println(h74, HEX);
    Serial.print("INV alarm_hist_75=0x"); Serial.println(h75, HEX);
    Serial.print("INV alarm_hist_76=0x"); Serial.println(h76, HEX);
    Serial.print("INV alarm_hist_77=0x"); Serial.println(h77, HEX);

    uint8_t latest = (uint8_t)(h74 & 0x00FF);
    uint8_t prev1  = (uint8_t)((h74 >> 8) & 0x00FF);
    uint8_t prev2  = (uint8_t)(h75 & 0x00FF);
    uint8_t prev3  = (uint8_t)((h75 >> 8) & 0x00FF);
    uint8_t prev4  = (uint8_t)(h76 & 0x00FF);
    uint8_t prev5  = (uint8_t)((h76 >> 8) & 0x00FF);
    uint8_t prev6  = (uint8_t)(h77 & 0x00FF);
    uint8_t prev7  = (uint8_t)((h77 >> 8) & 0x00FF);

    Serial.print("INV alarm_latest=0x"); Serial.print(latest, HEX);
    Serial.print(" "); Serial.println(invAlarmCodeName(latest));

    Serial.print("INV alarm_prev1=0x"); Serial.print(prev1, HEX);
    Serial.print(" "); Serial.println(invAlarmCodeName(prev1));
    Serial.print("INV alarm_prev2=0x"); Serial.print(prev2, HEX);
    Serial.print(" "); Serial.println(invAlarmCodeName(prev2));
    Serial.print("INV alarm_prev3=0x"); Serial.print(prev3, HEX);
    Serial.print(" "); Serial.println(invAlarmCodeName(prev3));
    Serial.print("INV alarm_prev4=0x"); Serial.print(prev4, HEX);
    Serial.print(" "); Serial.println(invAlarmCodeName(prev4));
    Serial.print("INV alarm_prev5=0x"); Serial.print(prev5, HEX);
    Serial.print(" "); Serial.println(invAlarmCodeName(prev5));
    Serial.print("INV alarm_prev6=0x"); Serial.print(prev6, HEX);
    Serial.print(" "); Serial.println(invAlarmCodeName(prev6));
    Serial.print("INV alarm_prev7=0x"); Serial.print(prev7, HEX);
    Serial.print(" "); Serial.println(invAlarmCodeName(prev7));
  } else {
    Serial.println("INV alarm history read NG");
  }

  return okF || okI || okV || okSt || ok74 || ok75 || ok76 || ok77;
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
  maybeWriteCsvLog();
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
