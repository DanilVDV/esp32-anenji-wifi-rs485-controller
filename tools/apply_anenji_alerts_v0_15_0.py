from pathlib import Path
import sys

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else '.')
ino = ROOT / 'firmware/ANENJI_ESP32_Controller/ANENJI_ESP32_Controller.ino'
if not ino.exists():
    raise SystemExit(f'Not found: {ino}')

s = ino.read_text(encoding='utf-8')

def repl(old, new, label):
    global s
    if old not in s:
        raise SystemExit(f'Anchor not found: {label}')
    s = s.replace(old, new, 1)

repl('''struct ProfileApplyResult {
  uint32_t id;
  bool done;
  bool ok;
  bool partial;
  uint16_t attempted;
  uint16_t applied;
  uint16_t failed;
  char profile[32];
  char error[200];
  ProfileFailure failures[16];
};
''', '''struct ProfileApplyResult {
  uint32_t id;
  bool done;
  bool ok;
  bool partial;
  uint16_t attempted;
  uint16_t applied;
  uint16_t failed;
  char profile[32];
  char error[200];
  ProfileFailure failures[16];
};

// ---------------- inverter faults / warnings / event log ----------------
struct InverterEvent {
  uint32_t id;
  uint32_t uptimeSec;
  uint8_t type;      // 1=fault, 2=warning, 3=mode
  uint8_t bit;       // 0..31 for masks, 255 for mode
  uint8_t code;      // mode value for type=3
  bool active;
  char severity[10];
  char message[88];
};

const uint8_t INVERTER_EVENT_CAPACITY = 48;
InverterEvent inverterEvents[INVERTER_EVENT_CAPACITY] = {};
uint8_t inverterEventHead = 0;
uint8_t inverterEventCount = 0;
uint32_t inverterEventNextId = 1;
portMUX_TYPE inverterEventMux = portMUX_INITIALIZER_UNLOCKED;

static const char* const INVERTER_FAULTS[] = {
  "Inverter module over temperature", "DCDC module over temperature", "Battery over voltage",
  "PV module over temperature", "Output short circuit", "Inverter over voltage", "Output overload",
  "Bus over voltage", "Bus soft start timed out", "PV over current", "PV over voltage",
  "Battery over current", "Inverter over current", "Bus low voltage", "Reserved fault bit 14",
  "Inverter DC component too high", "Reserved fault bit 16", "Output current zero bias too large",
  "Inverter current zero bias too large", "Battery current zero bias too large",
  "PV current zero bias too large", "Inverter low voltage", "Inverter negative power protection",
  "Parallel host lost", "Parallel synchronization signal abnormal", "Battery type incompatible",
  "Parallel versions incompatible"
};
static const char* const INVERTER_WARNINGS[] = {
  "Reserved warning bit 0", "Mains waveform abnormal", "Reserved warning bit 2", "Mains low voltage",
  "Mains over frequency", "Mains low frequency", "PV low voltage", "Over temperature",
  "Battery low voltage", "Battery not connected", "Overload", "Battery equalization charging",
  "Battery undervoltage", "Output power derating", "Fan blocked", "PV energy too low to use",
  "Parallel communication interrupted", "Single/parallel output mode inconsistent",
  "Parallel battery voltage difference too large"
};
const uint8_t INVERTER_FAULT_COUNT = sizeof(INVERTER_FAULTS)/sizeof(INVERTER_FAULTS[0]);
const uint8_t INVERTER_WARNING_COUNT = sizeof(INVERTER_WARNINGS)/sizeof(INVERTER_WARNINGS[0]);
''', 'event structs')

repl('''void batteryStatsSave(bool force=false);
bool adminAuthorized();
''', '''void batteryStatsSave(bool force=false);
bool adminAuthorized();
bool haveReg(uint16_t reg);
void handleEvents();
''', 'forward declarations')

repl('''const char* FW_VERSION = "0.14.31";
const char* FW_VERSION_PREVIOUS = "0.14.28";
''', '''const char* FW_VERSION = "0.15.0";
const char* FW_VERSION_PREVIOUS = "0.14.31";
''', 'version')

repl('''uint16_t telemetryRaw[40] = {0};   // 200..239
bool telemetryValid[40] = {false};
uint32_t telemetryUpdatedMs = 0;
uint32_t lastPollMs = 0;
''', '''uint16_t telemetryRaw[40] = {0};   // 200..239
bool telemetryValid[40] = {false};
uint32_t telemetryUpdatedMs = 0;
uint32_t lastPollMs = 0;

uint16_t inverterDiagRaw[10] = {0}; // 100..109
bool inverterDiagValid = false;
uint32_t inverterDiagUpdatedMs = 0;
uint32_t inverterFaultMask = 0;
uint32_t inverterWarningMask = 0;
uint32_t inverterPrevFaultMask = 0;
uint32_t inverterPrevWarningMask = 0;
bool inverterMasksInitialized = false;
uint8_t inverterPrevMode = 0;
bool inverterPrevModeValid = false;
''', 'diagnostic cache')

repl('''// ---------------- telemetry ----------------
void pollTelemetry() {
''', '''// ---------------- telemetry ----------------
const char* inverterModeText(uint8_t mode) {
  switch (mode) {
    case 0: return "Power On"; case 1: return "Standby"; case 2: return "Mains";
    case 3: return "Off-Grid"; case 4: return "Bypass"; case 5: return "Charging";
    case 6: return "Fault"; default: return "Unknown";
  }
}

void appendInverterEvent(uint8_t type, uint8_t bit, uint8_t code, bool active,
                         const char* severity, const char* message) {
  InverterEvent e = {};
  e.id = inverterEventNextId++;
  e.uptimeSec = millis()/1000UL;
  e.type = type; e.bit = bit; e.code = code; e.active = active;
  snprintf(e.severity, sizeof(e.severity), "%s", severity ? severity : "INFO");
  snprintf(e.message, sizeof(e.message), "%s", message ? message : "");
  portENTER_CRITICAL(&inverterEventMux);
  inverterEvents[inverterEventHead] = e;
  inverterEventHead = (uint8_t)((inverterEventHead + 1) % INVERTER_EVENT_CAPACITY);
  if (inverterEventCount < INVERTER_EVENT_CAPACITY) inverterEventCount++;
  portEXIT_CRITICAL(&inverterEventMux);
}

void processInverterMasks(uint32_t faultMask, uint32_t warningMask) {
  if (!inverterMasksInitialized) {
    for (uint8_t bit=0; bit<INVERTER_FAULT_COUNT; ++bit)
      if (faultMask & (1UL<<bit)) appendInverterEvent(1,bit,0,true,"CRITICAL",INVERTER_FAULTS[bit]);
    for (uint8_t bit=0; bit<INVERTER_WARNING_COUNT; ++bit)
      if (warningMask & (1UL<<bit)) appendInverterEvent(2,bit,0,true,"WARNING",INVERTER_WARNINGS[bit]);
    inverterPrevFaultMask=faultMask; inverterPrevWarningMask=warningMask; inverterMasksInitialized=true; return;
  }
  uint32_t fc=faultMask^inverterPrevFaultMask, wc=warningMask^inverterPrevWarningMask;
  for (uint8_t bit=0; bit<INVERTER_FAULT_COUNT; ++bit) { uint32_t m=1UL<<bit; if(fc&m) appendInverterEvent(1,bit,0,(faultMask&m)!=0,"CRITICAL",INVERTER_FAULTS[bit]); }
  for (uint8_t bit=0; bit<INVERTER_WARNING_COUNT; ++bit) { uint32_t m=1UL<<bit; if(wc&m) appendInverterEvent(2,bit,0,(warningMask&m)!=0,"WARNING",INVERTER_WARNINGS[bit]); }
  inverterPrevFaultMask=faultMask; inverterPrevWarningMask=warningMask;
}

void processInverterMode() {
  if (!haveReg(201)) return;
  uint8_t mode=(uint8_t)telemetryRaw[1];
  if (!inverterPrevModeValid) { inverterPrevMode=mode; inverterPrevModeValid=true; return; }
  if (mode==inverterPrevMode) return;
  char msg[88]; snprintf(msg,sizeof(msg),"Mode: %s -> %s",inverterModeText(inverterPrevMode),inverterModeText(mode));
  appendInverterEvent(3,255,mode,true,mode==6?"CRITICAL":"INFO",msg);
  inverterPrevMode=mode;
}

void pollTelemetry() {
''', 'telemetry helpers')

repl('''  uint16_t a[20], b[20];
''', '''  uint16_t a[20], b[20], d[10];
''', 'poll buffers')

repl('''  if (okB) {
    for (int i = 0; i < 20; ++i) {
      telemetryRaw[20 + i] = b[i];
      telemetryValid[20 + i] = true;
    }
  }

  if (okA || okB) {
    telemetryUpdatedMs = millis();
    rtuConsecutivePollFailures = 0;
''', '''  if (okB) {
    for (int i = 0; i < 20; ++i) {
      telemetryRaw[20 + i] = b[i];
      telemetryValid[20 + i] = true;
    }
  }

  bool okDiag = false;
  if (okA) {
    delay(15);
    okDiag = readHolding(100, 10, d, RTU_POLL_TIMEOUT_MS);
  }
  if (okDiag) {
    for (int i=0;i<10;++i) inverterDiagRaw[i]=d[i];
    inverterDiagValid=true; inverterDiagUpdatedMs=millis();
    inverterFaultMask=((uint32_t)d[0]<<16)|d[1];
    inverterWarningMask=((uint32_t)d[8]<<16)|d[9];
    processInverterMasks(inverterFaultMask,inverterWarningMask);
  }

  if (okA || okB) {
    telemetryUpdatedMs = millis();
    rtuConsecutivePollFailures = 0;
    processInverterMode();
''', 'diagnostic poll')

repl('''// ---------------- HTTP API ----------------
void sendJsonStatus() {
''', '''void appendMaskNamesJson(String& j, uint32_t mask, const char* const* names, uint8_t count) {
  j+='['; bool first=true;
  for(uint8_t bit=0;bit<count;++bit){
    if(!(mask&(1UL<<bit))) continue;
    if(!first) j+=','; first=false;
    j+=F("{\\\"bit\\\":"); j+=bit; j+=F(",\\\"text\\\":\\\""); j+=jsonEscape(String(names[bit])); j+=F("\\\"}");
  }
  j+=']';
}

void handleEvents() {
  String j; j.reserve(6000); j=F("{\\\"ok\\\":true,\\\"events\\\":[");
  uint8_t count,head;
  portENTER_CRITICAL(&inverterEventMux); count=inverterEventCount; head=inverterEventHead; portEXIT_CRITICAL(&inverterEventMux);
  for(uint8_t n=0;n<count;++n){
    uint8_t idx=(uint8_t)((head+INVERTER_EVENT_CAPACITY-1-n)%INVERTER_EVENT_CAPACITY); InverterEvent e;
    portENTER_CRITICAL(&inverterEventMux); e=inverterEvents[idx]; portEXIT_CRITICAL(&inverterEventMux);
    if(n)j+=','; j+=F("{\\\"id\\\":");j+=e.id; j+=F(",\\\"uptime_s\\\":");j+=e.uptimeSec;
    j+=F(",\\\"type\\\":\\\"");j+=e.type==1?F("fault"):(e.type==2?F("warning"):F("mode"));j+='"';
    j+=F(",\\\"severity\\\":\\\"");j+=e.severity;j+='"'; j+=F(",\\\"bit\\\":");if(e.bit==255)j+=F("null");else j+=e.bit;
    j+=F(",\\\"code\\\":");j+=e.code; j+=F(",\\\"active\\\":");j+=e.active?F("true"):F("false");
    j+=F(",\\\"message\\\":\\\"");j+=jsonEscape(String(e.message));j+=F("\\\"}");
  }
  j+=F("]}"); sendJson(200,j);
}

// ---------------- HTTP API ----------------
void sendJsonStatus() {
''', 'events API helpers')

repl('''  j.reserve(4200);
''', '''  j.reserve(6500);
''', 'status reserve')

repl('''  j += F(",\\\"active_profile\\\":\\\""); j += jsonEscape(activeProfile); j += '"';
  j += F(",\\\"pzem\\\":{\\\"enabled\\\":"); j += pzemEnabled?F("true"):F("false");
''', '''  j += F(",\\\"active_profile\\\":\\\""); j += jsonEscape(activeProfile); j += '"';
  j += F(",\\\"alerts\\\":{\\\"valid\\\":"); j += inverterDiagValid?F("true"):F("false");
  j += F(",\\\"age_ms\\\":"); if(inverterDiagUpdatedMs) j+=(uint32_t)(millis()-inverterDiagUpdatedMs); else j+=F("null");
  j += F(",\\\"fault_raw\\\":"); j += inverterFaultMask;
  j += F(",\\\"warning_raw\\\":"); j += inverterWarningMask;
  j += F(",\\\"operation_mode\\\":"); if(haveReg(201)) j+=telemetryRaw[1]; else j+=F("null");
  j += F(",\\\"operation_mode_text\\\":\\\""); j += haveReg(201)?inverterModeText((uint8_t)telemetryRaw[1]):"Unknown"; j += '"';
  j += F(",\\\"faults\\\":"); appendMaskNamesJson(j,inverterFaultMask,INVERTER_FAULTS,INVERTER_FAULT_COUNT);
  j += F(",\\\"warnings\\\":"); appendMaskNamesJson(j,inverterWarningMask,INVERTER_WARNINGS,INVERTER_WARNING_COUNT);
  j += F(",\\\"event_count\\\":"); j += inverterEventCount; j += '}';
  j += F(",\\\"pzem\\\":{\\\"enabled\\\":"); j += pzemEnabled?F("true"):F("false");
''', 'status alerts')

repl('''.badge.online{color:#86efac;background:#14532d55}.badge.offline{color:#fca5a5;background:#7f1d1d55}.badge.setup{color:#fde68a;background:#78350f55}
''', '''.badge.online{color:#86efac;background:#14532d55}.badge.offline{color:#fca5a5;background:#7f1d1d55}.badge.setup{color:#fde68a;background:#78350f55}
.alert-summary{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:9px;margin-bottom:10px}
.alert-box{background:#111827;border:1px solid #374151;border-radius:10px;padding:10px}.alert-box small{display:block;color:#9ca3af}.alert-box b{font-size:20px}
.alert-box.warning{border-color:#a16207;background:#42200655}.alert-box.critical{border-color:#b91c1c;background:#450a0a66}
.alert-list{display:grid;gap:7px}.alert-row{padding:8px 10px;border-radius:8px;background:#111827;border-left:4px solid #6b7280}
.alert-row.warning{border-left-color:#f59e0b}.alert-row.critical{border-left-color:#ef4444}.alert-row.clear{opacity:.62}.alert-row small{display:block;color:#9ca3af;margin-top:2px}
''', 'alert css')

repl('''</div></div>
<div class="card"><h2>Полная телеметрия</h2><div id="tele" class="grid"></div><div class="toolbar" style="margin-top:12px"><button onclick="statusLoad(true)">Обновить сейчас</button><span id="age" class="small"></span></div></div>
''', '''</div></div>
<div class="card"><h2>Состояние / аварии</h2>
 <div class="alert-summary"><div class="alert-box"><small>Режим</small><b id="alertMode">—</b></div><div class="alert-box critical"><small>Активные аварии</small><b id="alertFaultCount">0</b></div><div class="alert-box warning"><small>Активные предупреждения</small><b id="alertWarningCount">0</b></div></div>
 <div id="activeAlerts" class="alert-list"><div class="small">Нет активных аварий.</div></div>
 <details style="margin-top:10px"><summary>Журнал событий</summary><div id="eventLog" class="alert-list" style="margin-top:8px"><div class="small">—</div></div></details>
</div>
<div class="card"><h2>Полная телеметрия</h2><div id="tele" class="grid"></div><div class="toolbar" style="margin-top:12px"><button onclick="statusLoad(true)">Обновить сейчас</button><span id="age" class="small"></span></div></div>
''', 'alert html')

repl('''];
let autonomyAvgW=null,autonomyLastMs=0;
''', '''];
let lastEventLoadMs=0;
function renderAlerts(x){
 const a=x.alerts||{},faults=Array.isArray(a.faults)?a.faults:[],warnings=Array.isArray(a.warnings)?a.warnings:[];
 if($(\'alertMode\'))$(\'alertMode\').textContent=a.operation_mode_text||\'—\';
 if($(\'alertFaultCount\'))$(\'alertFaultCount\').textContent=String(faults.length);
 if($(\'alertWarningCount\'))$(\'alertWarningCount\').textContent=String(warnings.length);
 if($(\'activeAlerts\')){const rows=[];faults.forEach(v=>rows.push(`<div class="alert-row critical"><b>FAULT · bit ${v.bit}</b><small>${v.text}</small></div>`));warnings.forEach(v=>rows.push(`<div class="alert-row warning"><b>WARNING · bit ${v.bit}</b><small>${v.text}</small></div>`));$(\'activeAlerts\').innerHTML=rows.length?rows.join(\'\'):\'<div class="small ok">Активных аварий и предупреждений нет.</div>\';}
}
async function eventsLoad(force=false){
 const now=Date.now();if(!force&&now-lastEventLoadMs<9000)return;lastEventLoadMs=now;
 try{const x=await api(\'/api/events\',{timeoutMs:2200}),ev=Array.isArray(x.events)?x.events:[];if(!$(\'eventLog\'))return;$(\'eventLog\').innerHTML=ev.length?ev.slice(0,24).map(e=>{const cls=e.severity===\'CRITICAL\'?\'critical\':(e.severity===\'WARNING\'?\'warning\':\'\');const state=e.type===\'mode\'?\'\':(e.active?\'RAISED\':\'CLEARED\');return `<div class="alert-row ${cls} ${e.active?\'\':\'clear\'}"><b>${e.severity} · ${e.type.toUpperCase()} ${state}</b><small>${e.message} · uptime ${e.uptime_s}s</small></div>`}).join(\'\'):\'<div class="small">Событий пока нет.</div>\';}catch(e){}
}
let autonomyAvgW=null,autonomyLastMs=0;
''', 'alert javascript')

repl('''   $(\'tele\').innerHTML=teleDefs.map(d=>`<div class=metric><small>${teleLabel(d[0],d[1])} · ${d[2]}</small><b>${f(t[d[0]],d[4])} ${d[3]}</b></div>`).join(\'\');
''', '''   $(\'tele\').innerHTML=teleDefs.map(d=>`<div class=metric><small>${teleLabel(d[0],d[1])} · ${d[2]}</small><b>${f(t[d[0]],d[4])} ${d[3]}</b></div>`).join(\'\');
   renderAlerts(x); eventsLoad(force);
''', 'status render')

repl('''setTimeout(()=>otaVersionLoad(),950);
''', '''setTimeout(()=>otaVersionLoad(),950);
setTimeout(()=>eventsLoad(true),1150);
''', 'event startup')

repl('''  web.on("/api/status", HTTP_GET, sendJsonStatus);
''', '''  web.on("/api/status", HTTP_GET, sendJsonStatus);
  web.on("/api/events", HTTP_GET, handleEvents);
''', 'events route')

ino.write_text(s, encoding='utf-8')
print('Patched:', ino)
print('Firmware version: 0.15.0')
