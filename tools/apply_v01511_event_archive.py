from pathlib import Path

fw = Path('firmware/ANENJI_ESP32_Controller/ANENJI_ESP32_Controller.ino')
s = fw.read_text(encoding='utf-8')

def must_replace(old, new, count=1):
    global s
    n = s.count(old)
    if n < count:
        raise SystemExit(f'patch marker not found enough times ({n} < {count}): {old[:100]!r}')
    s = s.replace(old, new, count)

# Version + LittleFS.
must_replace('#include <Wire.h>\n#include <time.h>', '#include <Wire.h>\n#include <LittleFS.h>\n#include <time.h>')
must_replace('const char* FW_VERSION = "0.15.10";\nconst char* FW_VERSION_PREVIOUS = "0.15.9";',
             'const char* FW_VERSION = "0.15.11";\nconst char* FW_VERSION_PREVIOUS = "0.15.10";')

# Expand live RAM ring and add bounded persistent archive state.
old = '''const uint8_t INVERTER_EVENT_CAPACITY = 48;
InverterEvent inverterEvents[INVERTER_EVENT_CAPACITY] = {};
uint8_t inverterEventHead = 0;
uint8_t inverterEventCount = 0;
uint32_t inverterEventNextId = 1;
portMUX_TYPE inverterEventMux = portMUX_INITIALIZER_UNLOCKED;
'''
new = '''const uint8_t INVERTER_EVENT_CAPACITY = 64;
InverterEvent inverterEvents[INVERTER_EVENT_CAPACITY] = {};
uint8_t inverterEventHead = 0;
uint8_t inverterEventCount = 0;
uint32_t inverterEventNextId = 1;
portMUX_TYPE inverterEventMux = portMUX_INITIALIZER_UNLOCKED;

// Persistent archive: only events evicted from the live RAM ring are queued here.
// The Web overview stays read-only/uncluttered; archive controls live in Service.
struct EventArchiveRecord {
  uint32_t id;
  uint32_t firstUptimeSec;
  uint32_t lastUptimeSec;
  char firstTimestamp[20];
  char lastTimestamp[20];
  uint16_t repeatCount;
  uint8_t type;
  uint8_t bit;
  uint8_t code;
  uint8_t active;
  char severity[10];
  char message[88];
  uint16_t crc;
};

const char* EVENT_ARCHIVE_PATH = "/event_archive.bin";
const char* EVENT_ARCHIVE_TMP_PATH = "/event_archive.tmp";
const size_t EVENT_ARCHIVE_MAX_BYTES = 64U * 1024U;
const size_t EVENT_ARCHIVE_TARGET_BYTES = 48U * 1024U;
const uint16_t EVENT_ARCHIVE_RETENTION_DAYS = 30;
const uint8_t EVENT_ARCHIVE_PENDING_CAPACITY = 16;
const uint8_t EVENT_ARCHIVE_BATCH = 8;
const uint32_t EVENT_ARCHIVE_FLUSH_MS = 60000UL;
const uint32_t EVENT_ARCHIVE_PRUNE_MS = 6UL * 60UL * 60UL * 1000UL;
EventArchiveRecord eventArchivePending[EVENT_ARCHIVE_PENDING_CAPACITY] = {};
uint8_t eventArchivePendingCount = 0;
portMUX_TYPE eventArchiveMux = portMUX_INITIALIZER_UNLOCKED;
bool eventArchiveFsReady = false;
uint32_t eventArchiveLastFlushMs = 0;
uint32_t eventArchiveLastPruneMs = 0;
uint32_t eventArchiveDropped = 0;
'''
must_replace(old, new)

# Forward declarations.
must_replace('void handleEvents();\nbool i2cProbe(uint8_t addr);', '''void handleEvents();
void eventArchiveBegin();
void eventArchiveQueue(const InverterEvent& e);
void serviceEventArchive();
void handleEventArchive();
void handleEventArchiveClear();
void handleEventArchiveCsv();
bool i2cProbe(uint8_t addr);''')

# Archive the record that is about to be overwritten by the live ring.
old = '''  portENTER_CRITICAL(&inverterEventMux);
  inverterEvents[inverterEventHead] = e;
  inverterEventHead = (uint8_t)((inverterEventHead + 1) % INVERTER_EVENT_CAPACITY);
  if (inverterEventCount < INVERTER_EVENT_CAPACITY) inverterEventCount++;
  portEXIT_CRITICAL(&inverterEventMux);
}'''
new = '''  InverterEvent evicted = {};
  bool haveEvicted = false;
  portENTER_CRITICAL(&inverterEventMux);
  if (inverterEventCount >= INVERTER_EVENT_CAPACITY) {
    evicted = inverterEvents[inverterEventHead];
    haveEvicted = true;
  }
  inverterEvents[inverterEventHead] = e;
  inverterEventHead = (uint8_t)((inverterEventHead + 1) % INVERTER_EVENT_CAPACITY);
  if (inverterEventCount < INVERTER_EVENT_CAPACITY) inverterEventCount++;
  portEXIT_CRITICAL(&inverterEventMux);
  if (haveEvicted) eventArchiveQueue(evicted);
}'''
must_replace(old, new)

# Persistent archive implementation inserted immediately before current /api/events handler.
marker = 'void handleEvents() {'
archive_impl = r'''// ---------------- persistent inverter event archive ----------------
uint16_t eventArchiveCrc(const uint8_t* data, size_t len) {
  uint16_t crc=0xFFFF;
  while(len--){ crc^=*data++; for(uint8_t i=0;i<8;i++) crc=(crc&1)?(uint16_t)((crc>>1)^0xA001):(uint16_t)(crc>>1); }
  return crc;
}

bool eventArchiveValid(const EventArchiveRecord& r) {
  return r.repeatCount>0 && r.crc==eventArchiveCrc((const uint8_t*)&r,sizeof(r)-sizeof(r.crc));
}

int32_t eventArchiveCivilDay(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe;
}

bool eventArchiveTimestampDay(const char* ts, int32_t& day) {
  if(!ts || strlen(ts)<10 || ts[4]!='-' || ts[7]!='-') return false;
  int y=(ts[0]-'0')*1000+(ts[1]-'0')*100+(ts[2]-'0')*10+(ts[3]-'0');
  int m=(ts[5]-'0')*10+(ts[6]-'0');
  int d=(ts[8]-'0')*10+(ts[9]-'0');
  if(y<2024 || m<1 || m>12 || d<1 || d>31) return false;
  day=eventArchiveCivilDay(y,(unsigned)m,(unsigned)d); return true;
}

bool eventArchiveSame(const EventArchiveRecord& a, const InverterEvent& e) {
  return a.type==e.type && a.bit==e.bit && a.code==e.code && a.active==(uint8_t)(e.active?1:0) &&
         strcmp(a.severity,e.severity)==0 && strcmp(a.message,e.message)==0;
}

void eventArchiveQueue(const InverterEvent& e) {
  portENTER_CRITICAL(&eventArchiveMux);
  if(eventArchivePendingCount){
    EventArchiveRecord& last=eventArchivePending[eventArchivePendingCount-1];
    if(eventArchiveSame(last,e)){
      if(last.repeatCount<65535) last.repeatCount++;
      last.lastUptimeSec=e.uptimeSec;
      snprintf(last.lastTimestamp,sizeof(last.lastTimestamp),"%s",e.timestamp);
      last.crc=eventArchiveCrc((const uint8_t*)&last,sizeof(last)-sizeof(last.crc));
      portEXIT_CRITICAL(&eventArchiveMux); return;
    }
  }
  if(eventArchivePendingCount>=EVENT_ARCHIVE_PENDING_CAPACITY){ eventArchiveDropped++; portEXIT_CRITICAL(&eventArchiveMux); return; }
  EventArchiveRecord& r=eventArchivePending[eventArchivePendingCount++]; r={};
  r.id=e.id; r.firstUptimeSec=e.uptimeSec; r.lastUptimeSec=e.uptimeSec; r.repeatCount=1;
  r.type=e.type; r.bit=e.bit; r.code=e.code; r.active=e.active?1:0;
  snprintf(r.firstTimestamp,sizeof(r.firstTimestamp),"%s",e.timestamp);
  snprintf(r.lastTimestamp,sizeof(r.lastTimestamp),"%s",e.timestamp);
  snprintf(r.severity,sizeof(r.severity),"%s",e.severity);
  snprintf(r.message,sizeof(r.message),"%s",e.message);
  r.crc=eventArchiveCrc((const uint8_t*)&r,sizeof(r)-sizeof(r.crc));
  portEXIT_CRITICAL(&eventArchiveMux);
}

bool eventArchiveCurrentDay(int32_t& day) {
  RtcDateTime dt={}; if(!clockNow(dt)) return false;
  day=eventArchiveCivilDay(dt.year,dt.month,dt.day); return true;
}

void eventArchiveCompact(bool ageCheck) {
  if(!eventArchiveFsReady || !LittleFS.exists(EVENT_ARCHIVE_PATH)) return;
  File in=LittleFS.open(EVENT_ARCHIVE_PATH,FILE_READ); if(!in) return;
  const size_t recSize=sizeof(EventArchiveRecord); size_t total=in.size()/recSize;
  if(!total){ in.close(); LittleFS.remove(EVENT_ARCHIVE_PATH); return; }
  size_t maxKeep=EVENT_ARCHIVE_TARGET_BYTES/recSize;
  size_t start=(total>maxKeep)?(total-maxKeep):0;
  int32_t nowDay=0; bool haveDay=ageCheck && eventArchiveCurrentDay(nowDay);
  bool needsRewrite=(in.size()>EVENT_ARCHIVE_MAX_BYTES);
  if(haveDay && !needsRewrite){
    EventArchiveRecord r={};
    for(size_t i=0;i<total;i++){
      if(in.read((uint8_t*)&r,recSize)!=recSize) break;
      int32_t d=0; if(eventArchiveValid(r) && eventArchiveTimestampDay(r.lastTimestamp,d) && nowDay-d>(int32_t)EVENT_ARCHIVE_RETENTION_DAYS){ needsRewrite=true; break; }
    }
  }
  if(!needsRewrite){ in.close(); return; }
  in.seek(start*recSize,SeekSet);
  LittleFS.remove(EVENT_ARCHIVE_TMP_PATH);
  File out=LittleFS.open(EVENT_ARCHIVE_TMP_PATH,FILE_WRITE); if(!out){ in.close(); return; }
  EventArchiveRecord r={};
  while(in.read((uint8_t*)&r,recSize)==recSize){
    if(!eventArchiveValid(r)) continue;
    if(haveDay){ int32_t d=0; if(eventArchiveTimestampDay(r.lastTimestamp,d) && nowDay-d>(int32_t)EVENT_ARCHIVE_RETENTION_DAYS) continue; }
    out.write((const uint8_t*)&r,recSize);
  }
  out.close(); in.close();
  LittleFS.remove(EVENT_ARCHIVE_PATH); LittleFS.rename(EVENT_ARCHIVE_TMP_PATH,EVENT_ARCHIVE_PATH);
}

void eventArchiveBegin() {
  eventArchiveFsReady=LittleFS.begin(true);
  if(!eventArchiveFsReady){ Serial.println(F("[EVENT ARCHIVE] LittleFS mount failed")); return; }
  if(LittleFS.exists(EVENT_ARCHIVE_PATH)){
    File f=LittleFS.open(EVENT_ARCHIVE_PATH,FILE_READ);
    if(f && (f.size()%sizeof(EventArchiveRecord))!=0){ f.close(); LittleFS.remove(EVENT_ARCHIVE_PATH); }
    else if(f) f.close();
  }
  eventArchiveCompact(true);
}

void serviceEventArchive() {
  if(!eventArchiveFsReady) return;
  uint32_t now=millis(); uint8_t count=0;
  portENTER_CRITICAL(&eventArchiveMux); count=eventArchivePendingCount; portEXIT_CRITICAL(&eventArchiveMux);
  if(count && (count>=EVENT_ARCHIVE_BATCH || !eventArchiveLastFlushMs || (uint32_t)(now-eventArchiveLastFlushMs)>=EVENT_ARCHIVE_FLUSH_MS)){
    EventArchiveRecord batch[EVENT_ARCHIVE_PENDING_CAPACITY]; uint8_t n=0;
    portENTER_CRITICAL(&eventArchiveMux);
    n=eventArchivePendingCount; if(n>EVENT_ARCHIVE_PENDING_CAPACITY)n=EVENT_ARCHIVE_PENDING_CAPACITY;
    for(uint8_t i=0;i<n;i++) batch[i]=eventArchivePending[i]; eventArchivePendingCount=0;
    portEXIT_CRITICAL(&eventArchiveMux);
    File f=LittleFS.open(EVENT_ARCHIVE_PATH,FILE_APPEND);
    if(f){ for(uint8_t i=0;i<n;i++) f.write((const uint8_t*)&batch[i],sizeof(EventArchiveRecord)); f.close(); eventArchiveLastFlushMs=now; }
    else { eventArchiveDropped+=n; }
    eventArchiveCompact(false);
  }
  if(!eventArchiveLastPruneMs || (uint32_t)(now-eventArchiveLastPruneMs)>=EVENT_ARCHIVE_PRUNE_MS){ eventArchiveLastPruneMs=now; eventArchiveCompact(true); }
}

const char* eventArchiveTypeName(uint8_t type){ return type==1?"fault":(type==2?"warning":(type==3?"mode":"event")); }

void handleEventArchive() {
  String j; j.reserve(22000); size_t bytes=0,total=0; uint8_t pending=0;
  portENTER_CRITICAL(&eventArchiveMux); pending=eventArchivePendingCount; portEXIT_CRITICAL(&eventArchiveMux);
  if(eventArchiveFsReady && LittleFS.exists(EVENT_ARCHIVE_PATH)){ File f=LittleFS.open(EVENT_ARCHIVE_PATH,FILE_READ); if(f){ bytes=f.size(); total=bytes/sizeof(EventArchiveRecord); f.close(); } }
  j=F("{\"ok\":true,\"fs_ready\":"); j+=eventArchiveFsReady?F("true"):F("false");
  j+=F(",\"count\":"); j+=(uint32_t)total; j+=F(",\"bytes\":"); j+=(uint32_t)bytes;
  j+=F(",\"max_bytes\":"); j+=(uint32_t)EVENT_ARCHIVE_MAX_BYTES; j+=F(",\"pending\":"); j+=pending;
  j+=F(",\"dropped\":"); j+=eventArchiveDropped; j+=F(",\"retention_days\":"); j+=EVENT_ARCHIVE_RETENTION_DAYS;
  j+=F(",\"events\":[");
  if(eventArchiveFsReady && total){
    File f=LittleFS.open(EVENT_ARCHIVE_PATH,FILE_READ); if(f){ size_t shown=min((size_t)100,total); bool first=true;
      for(size_t n=0;n<shown;n++){ size_t idx=total-1-n; EventArchiveRecord r={}; if(!f.seek(idx*sizeof(r),SeekSet) || f.read((uint8_t*)&r,sizeof(r))!=sizeof(r) || !eventArchiveValid(r)) continue;
        if(!first)j+=','; first=false; j+=F("{\"id\":");j+=r.id; j+=F(",\"first_timestamp\":"); if(r.firstTimestamp[0]){j+='\"';j+=r.firstTimestamp;j+='\"';}else j+=F("null");
        j+=F(",\"last_timestamp\":"); if(r.lastTimestamp[0]){j+='\"';j+=r.lastTimestamp;j+='\"';}else j+=F("null");
        j+=F(",\"first_uptime_s\":");j+=r.firstUptimeSec;j+=F(",\"last_uptime_s\":");j+=r.lastUptimeSec;j+=F(",\"repeats\":");j+=r.repeatCount;
        j+=F(",\"type\":\"");j+=eventArchiveTypeName(r.type);j+=F("\",\"active\":");j+=r.active?F("true"):F("false");j+=F(",\"severity\":\"");j+=jsonEscape(String(r.severity));
        j+=F("\",\"message\":\"");j+=jsonEscape(String(r.message));j+=F("\"}");
      } f.close();
    }
  }
  j+=F("]}"); sendJson(200,j);
}

String eventArchiveCsvField(const char* s){ String v=s?s:""; v.replace("\"","\"\""); return String("\"")+v+"\""; }
void handleEventArchiveCsv() {
  if(!eventArchiveFsReady){ sendJson(503,F("{\"ok\":false,\"error\":\"Event archive filesystem unavailable\"}")); return; }
  web.sendHeader("Content-Disposition","attachment; filename=anenji-event-archive.csv"); web.setContentLength(CONTENT_LENGTH_UNKNOWN); web.send(200,"text/csv; charset=utf-8","");
  web.sendContent("first_timestamp,last_timestamp,repeats,type,state,severity,message,first_uptime_s,last_uptime_s\r\n");
  if(LittleFS.exists(EVENT_ARCHIVE_PATH)){ File f=LittleFS.open(EVENT_ARCHIVE_PATH,FILE_READ); EventArchiveRecord r={};
    if(f){ while(f.read((uint8_t*)&r,sizeof(r))==sizeof(r)){ if(!eventArchiveValid(r))continue; String line;
      line.reserve(240); line+=eventArchiveCsvField(r.firstTimestamp);line+=',';line+=eventArchiveCsvField(r.lastTimestamp);line+=',';line+=r.repeatCount;line+=',';line+=eventArchiveTypeName(r.type);line+=',';line+=(r.active?"raised":"cleared");line+=',';line+=eventArchiveCsvField(r.severity);line+=',';line+=eventArchiveCsvField(r.message);line+=',';line+=r.firstUptimeSec;line+=',';line+=r.lastUptimeSec;line+="\r\n"; web.sendContent(line); } f.close(); }
  }
  web.sendContent("");
}

void handleEventArchiveClear() {
  if(!adminAuthorized()){ sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}")); return; }
  if(eventArchiveFsReady){ LittleFS.remove(EVENT_ARCHIVE_PATH); LittleFS.remove(EVENT_ARCHIVE_TMP_PATH); }
  portENTER_CRITICAL(&eventArchiveMux); eventArchivePendingCount=0; portEXIT_CRITICAL(&eventArchiveMux);
  eventArchiveDropped=0; sendJson(200,F("{\"ok\":true}"));
}

'''
if marker not in s:
    raise SystemExit('handleEvents marker missing')
s = s.replace(marker, archive_impl + marker, 1)

# API routes.
must_replace('  web.on("/api/events", HTTP_GET, handleEvents);', '''  web.on("/api/events", HTTP_GET, handleEvents);
  web.on("/api/events/archive", HTTP_GET, handleEventArchive);
  web.on("/api/events/archive.csv", HTTP_GET, handleEventArchiveCsv);
  web.on("/api/events/archive/clear", HTTP_POST, handleEventArchiveClear);''')

# Initialize archive after I2C setup; service writes from normal loop (never from RTU event producer).
must_replace('  Wire.setTimeOut(50);  // prevent a stuck RTC/I2C bus from blocking the web loop', '''  Wire.setTimeOut(50);  // prevent a stuck RTC/I2C bus from blocking the web loop
  eventArchiveBegin();''')
must_replace('  serviceHallCurrent();\n  serviceHeapWatchdog();', '  serviceHallCurrent();\n  serviceEventArchive();\n  serviceHeapWatchdog();')

# Service-only UI. Nothing is added to the overview event list.
service_marker = '<div class="card"><h2>PZCT-DC63 · DC ток / ADS1115</h2>'
archive_card = '''<div class="card"><h2>Архив событий</h2>
<div class="small">Старые события автоматически переносятся сюда из оперативного журнала. Главный экран остаётся без кнопок управления архивом. Хранение ограничено 30 днями и 64 КБ, старые записи ротируются автоматически; одинаковые подряд события объединяются со счётчиком повторов.</div>
<div class="grid" style="margin-top:10px">
 <div class="metric"><small>События в архиве</small><b id="eventArchiveCount">—</b></div>
 <div class="metric"><small>Размер архива</small><b id="eventArchiveSize">—</b></div>
 <div class="metric"><small>В очереди на запись</small><b id="eventArchivePending">—</b></div>
</div>
<div class="toolbar" style="margin-top:10px"><button onclick="eventArchiveLoad()">Обновить архив</button><button onclick="eventArchiveDownload()">Скачать CSV</button><button class="danger" onclick="eventArchiveClear()">Очистить архив</button><span id="eventArchiveMsg" class="small"></span></div>
<div id="eventArchiveList" style="margin-top:10px;max-height:420px;overflow:auto"><div class="small">Архив ещё не загружен.</div></div>
</div>

'''
if service_marker not in s:
    raise SystemExit('service marker missing')
s = s.replace(service_marker, archive_card + service_marker, 1)

# Localized archive UI behavior, inserted after current live-events loader.
js_marker = 'let autonomyAvgW=null,autonomyLastMs=0;'
archive_js = r'''async function eventArchiveLoad(){
 try{
  const x=await api('/api/events/archive',{timeoutMs:3500}),ev=Array.isArray(x.events)?x.events:[];
  if($('eventArchiveCount'))$('eventArchiveCount').textContent=String(x.count||0);
  if($('eventArchiveSize'))$('eventArchiveSize').textContent=((Number(x.bytes||0)/1024).toFixed(1))+' KB / '+((Number(x.max_bytes||65536)/1024).toFixed(0))+' KB';
  if($('eventArchivePending'))$('eventArchivePending').textContent=String(x.pending||0);
  if($('eventArchiveMsg'))$('eventArchiveMsg').textContent=x.fs_ready?(x.dropped?((currentLang==='ru'?'Потеряно из очереди: ':'Dropped from queue: ')+x.dropped):''):(currentLang==='ru'?'LittleFS недоступен':'LittleFS unavailable');
  if($('eventArchiveList'))$('eventArchiveList').innerHTML=ev.length?ev.map(e=>{const cls=e.severity==='CRITICAL'?'critical':(e.severity==='WARNING'?'warning':'');const state=e.type==='mode'?'':(e.active?'RAISED':'CLEARED');const first=e.first_timestamp||('uptime '+e.first_uptime_s+'s');const last=e.last_timestamp||('uptime '+e.last_uptime_s+'s');const times=e.repeats>1?`${first} → ${last} · ×${e.repeats}`:first;return `<div class="alert-row ${cls} ${e.active?'':'clear'}"><b>${times} · ${e.severity} · ${String(e.type||'').toUpperCase()} ${state}</b><small>${e.message||''}</small></div>`}).join(''):`<div class="small">${currentLang==='ru'?'Архив пока пуст.':'Archive is empty.'}</div>`;
 }catch(e){if($('eventArchiveMsg'))$('eventArchiveMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}
function eventArchiveDownload(){ window.location.href='/api/events/archive.csv'; }
async function eventArchiveClear(){
 const q=currentLang==='ru'?'Удалить весь архив событий? Текущий журнал на главном экране не будет очищен.':'Delete the whole event archive? The live log on the overview will not be cleared.';
 if(!confirm(q))return;
 try{await api('/api/events/archive/clear',{method:'POST',headers:adminHeaders(),body:'{}'});if($('eventArchiveMsg'))$('eventArchiveMsg').textContent=currentLang==='ru'?'Архив очищен':'Archive cleared';await eventArchiveLoad();}catch(e){if($('eventArchiveMsg'))$('eventArchiveMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}

'''
if js_marker not in s:
    raise SystemExit('JS insertion marker missing')
s = s.replace(js_marker, archive_js + js_marker, 1)

# Load archive only when Service is opened, never from the overview refresh loop.
s = s.replace(" window.scrollTo({top:0,behavior:'instant'});", " if(id==='service')eventArchiveLoad();\n window.scrollTo({top:0,behavior:'instant'});", 1)

# Static translations for the new Service card.
trans_marker = " ['Состояние / аварии','Status / alarms'],['Аварии','Faults'],['Предупреждения','Warnings'],['События','Events'],"
trans_new = trans_marker + "\n ['Архив событий','Event archive'],['События в архиве','Archived events'],['Размер архива','Archive size'],['В очереди на запись','Pending writes'],['Обновить архив','Refresh archive'],['Скачать CSV','Download CSV'],['Очистить архив','Clear archive'],['Архив ещё не загружен.','Archive has not been loaded yet.'],"
if trans_marker not in s:
    raise SystemExit('translation marker missing')
s = s.replace(trans_marker, trans_new, 1)

fw.write_text(s, encoding='utf-8')

# README version/capability note.
readme = Path('README.md')
r = readme.read_text(encoding='utf-8')
r = r.replace('Текущая версия прошивки: **0.15.9**.', 'Текущая версия прошивки: **0.15.11**.', 1)
r = r.replace('- журнал событий с локальной датой и временем после синхронизации RTC/NTP/браузером;', '- журнал событий с локальной датой и временем после синхронизации RTC/NTP/браузером;\n- автоматический архив старых событий во flash: 30 дней / 64 КБ, ротация, объединение повторов, просмотр/очистка/CSV только в разделе «Сервис»;', 1)
readme.write_text(r, encoding='utf-8')

print('v0.15.11 event archive patch applied')
