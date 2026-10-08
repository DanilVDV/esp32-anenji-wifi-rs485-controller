from pathlib import Path
import re, sys

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')
p = root / 'firmware/ANENJI_ESP32_Controller/ANENJI_ESP32_Controller.ino'
s = p.read_text(encoding='utf-8')

def once(old, new, label):
    global s
    if old not in s:
        raise SystemExit('anchor missing: ' + label)
    s = s.replace(old, new, 1)

def sub_once(pattern, repl, label, flags=0):
    global s
    s2, n = re.subn(pattern, repl, s, count=1, flags=flags)
    if n != 1:
        raise SystemExit(f'pattern failed ({n}): {label}')
    s = s2

# Version.
once('const char* FW_VERSION = "0.15.4";\nconst char* FW_VERSION_PREVIOUS = "0.15.3";',
     'const char* FW_VERSION = "0.15.5";\nconst char* FW_VERSION_PREVIOUS = "0.15.4";', 'version')

# Remove the duplicate dedicated Power Flow/Energy tab introduced in v0.15.4.
sub_once(r'\n\n/\* EyeBond-inspired dedicated Power Flow tab\. Browser-side only\. \*/.*?(?=\n</style>)', '', 'power flow CSS', re.S)
once(' <button class="app-tab" data-page="power" onclick="showTab(\'power\')">Энергия</button>\n', '', 'power tab button')
sub_once(r'\n<div class="card pf-card">.*?(?=\n<div class="card"><h2>Полная телеметрия</h2>)', '', 'power flow card', re.S)
once(" ['Состояние / аварии','overview'],['Энергопотоки','overview'],['Power Flow','power'],['Полная телеметрия','overview'],",
     " ['Состояние / аварии','overview'],['Энергопотоки','overview'],['Полная телеметрия','overview'],", 'power tab rule')
once(" for(const id of ['overview','power','battery','settings','scheduler','network','service']){",
     " for(const id of ['overview','battery','settings','scheduler','network','service']){", 'power page host')
sub_once(r'\nfunction renderPowerFlowCard\(t\)\{.*?(?=\nfunction renderEnergy\(t\)\{)', '', 'power flow renderer', re.S)
s = s.replace(' renderPowerFlowCard(t);', '', 1)

# The physical RTC is optional. rtcCached becomes the common software clock cache,
# with RTC, NTP or browser time as possible sources. esp_timer_get_time() keeps the
# software clock monotonic beyond the 49-day millis() wrap.
once('#include <esp_task_wdt.h>\n#include <Wire.h>', '#include <esp_task_wdt.h>\n#include <esp_timer.h>\n#include <Wire.h>', 'esp timer include')
once('bool rtcPresent = false;\n\n// NTP -> DS1307 synchronization.',
'''bool rtcPresent = false;
enum ClockSource : uint8_t { CLOCK_NONE=0, CLOCK_RTC=1, CLOCK_NTP=2, CLOCK_BROWSER=3 };
ClockSource clockSource = CLOCK_NONE;
uint64_t clockCachedAtUs = 0;
bool clockNow(RtcDateTime& out);
const char* clockSourceName();

// NTP -> local software clock, with optional DS1307 persistence.''', 'clock globals')
once('const uint32_t NTP_RETRY_MS = 15UL*60UL*1000UL;\nconst uint32_t NTP_RESYNC_MS = 6UL*60UL*60UL*1000UL;',
     'const uint32_t NTP_INITIAL_RETRY_MS = 5000UL;\nconst uint32_t NTP_RETRY_MS = 15UL*60UL*1000UL;\nconst uint32_t NTP_RESYNC_MS = 6UL*60UL*60UL*1000UL;', 'initial NTP retry')

# rtcDateKey must use logical time, not physical RTC presence.
once('uint32_t rtcDateKey() {\n  if(!rtcPresent || !rtcCached.valid) return 0;\n  return (uint32_t)rtcCached.year*10000UL+(uint32_t)rtcCached.month*100UL+rtcCached.day;\n}',
'''uint32_t rtcDateKey() {
  RtcDateTime now;
  if(!clockNow(now)) return 0;
  return (uint32_t)now.year*10000UL+(uint32_t)now.month*100UL+now.day;
}''', 'date key logical clock')

# Tariff switching likewise follows the logical clock.
once('uint8_t currentTariff(){\n  // T1 07:00..22:59, T2 23:00..06:59. RTC is kept in local civil time.\n  if(!rtcPresent || !rtcCached.valid) return 0;\n  return (rtcCached.hour>=7 && rtcCached.hour<23) ? 1 : 2;\n}',
'''uint8_t currentTariff(){
  // T1 07:00..22:59, T2 23:00..06:59. Clock is local civil time.
  RtcDateTime now;
  if(!clockNow(now)) return 0;
  return (now.hour>=7 && now.hour<23) ? 1 : 2;
}''', 'tariff logical clock')

# Add common clock helpers after civil-second conversion.
once('int64_t rtcCivilSeconds(const RtcDateTime& d) {\n  return civilDaysFromEpoch(d.year,d.month,d.day)*86400LL + (int64_t)d.hour*3600LL + (int64_t)d.minute*60LL + d.second;\n}\n\nbool ntpLocalDateTime(RtcDateTime& out) {',
'''int64_t rtcCivilSeconds(const RtcDateTime& d) {
  return civilDaysFromEpoch(d.year,d.month,d.day)*86400LL + (int64_t)d.hour*3600LL + (int64_t)d.minute*60LL + d.second;
}

bool clockDateTimeValid(const RtcDateTime& d) {
  return d.valid && d.year>=2024 && d.year<=2099 && d.month>=1 && d.month<=12 &&
         d.day>=1 && d.day<=31 && d.hour<=23 && d.minute<=59 && d.second<=59;
}

const char* clockSourceName() {
  switch(clockSource){
    case CLOCK_RTC: return "rtc";
    case CLOCK_NTP: return "ntp";
    case CLOCK_BROWSER: return "browser";
    default: return "none";
  }
}

void clockCache(const RtcDateTime& dt, ClockSource source) {
  if(!clockDateTimeValid(dt)) return;
  rtcCached=dt;
  rtcCachedAtMs=millis();
  clockCachedAtUs=(uint64_t)esp_timer_get_time();
  clockSource=source;
}

bool clockNow(RtcDateTime& out) {
  if(!clockDateTimeValid(rtcCached) || !clockCachedAtUs) { out.valid=false; return false; }
  uint64_t elapsed=(uint64_t)(esp_timer_get_time()-clockCachedAtUs)/1000000ULL;
  uint64_t tod=(uint64_t)rtcCached.hour*3600ULL+(uint64_t)rtcCached.minute*60ULL+rtcCached.second+elapsed;
  uint32_t days=(uint32_t)(tod/86400ULL);
  uint32_t rem=(uint32_t)(tod%86400ULL);
  out=rtcCached;
  out.hour=(uint8_t)(rem/3600U); rem%=3600U;
  out.minute=(uint8_t)(rem/60U); out.second=(uint8_t)(rem%60U);
  while(days--){
    static const uint8_t mdays[]={31,28,31,30,31,30,31,31,30,31,30,31};
    uint8_t dim=mdays[out.month-1];
    bool leap=(out.year%4==0 && (out.year%100!=0 || out.year%400==0));
    if(out.month==2 && leap) dim=29;
    if(++out.day>dim){out.day=1;if(++out.month>12){out.month=1;out.year++;}}
    if(out.year>2099){out.valid=false;return false;}
  }
  out.valid=true;
  return true;
}

bool ntpLocalDateTime(RtcDateTime& out) {''', 'clock helpers')

# NTP synchronization must succeed even when there is no RTC. If a physical RTC
# exists, write the same time to it opportunistically; a failed RTC write does not
# invalidate the NTP result.
sub_once(r'void serviceNtpRtcSync\(\) \{.*?\n\}\n\nvoid serviceScheduler\(\) \{', r'''void serviceNtpRtcSync() {
  if (setupMode || WiFi.status()!=WL_CONNECTED) return;
  uint32_t now=millis();
  if (!ntpConfigured) {
    // Fixed UTC offset only; no DST/seasonal conversion. The logical clock uses local civil time.
    configTime((long)ntpUtcOffsetMin * 60L, 0, ntpServer.c_str(), NTP_FALLBACK_SERVER);
    ntpConfigured=true;
    ntpLastAttemptMs=0; // allow the first sync immediately after Wi-Fi/NTP setup
    Serial.println(F("[NTP] client configured"));
  }
  bool due=!ntpEverSynced || (uint32_t)(now-ntpLastSyncMs)>=NTP_RESYNC_MS;
  if (!due) return;
  uint32_t retryMs=ntpEverSynced?NTP_RETRY_MS:NTP_INITIAL_RETRY_MS;
  if (ntpLastAttemptMs && (uint32_t)(now-ntpLastAttemptMs)<retryMs) return;
  ntpLastAttemptMs=now;
  RtcDateTime ndt;
  if (!ntpLocalDateTime(ndt)) return;
  RtcDateTime old;
  if (clockNow(old)) ntpLastCorrectionSec=(int32_t)(rtcCivilSeconds(ndt)-rtcCivilSeconds(old)); else ntpLastCorrectionSec=0;
  clockCache(ndt,CLOCK_NTP);
  bool rtcStored=false;
  if(rtcPresent){
    rtcStored=rtcWrite(ndt);
    if(!rtcStored) rtcPresent=false;
  }
  schedulerLastMinuteKey=0xFFFFFFFFUL;
  ntpEverSynced=true; ntpLastSyncMs=now;
  Serial.print(F("[NTP] clock synchronized; correction s=")); Serial.print(ntpLastCorrectionSec);
  Serial.print(F("; RTC persisted=")); Serial.println(rtcStored?F("yes"):F("no"));
}

void serviceScheduler() {''', 'NTP service', re.S)

# Scheduler works from the logical clock. Hardware RTC is refreshed while present;
# if absent it is probed only once a minute, avoiding a blocking I2C request every second.
sub_once(r'void serviceScheduler\(\) \{.*?\n\}\n\nvoid handleRtcGet\(\) \{', r'''void serviceScheduler() {
  static uint32_t lastCheckMs=0;
  static uint32_t lastRtcProbeMs=0;
  if (setupMode || (uint32_t)(millis()-lastCheckMs) < 1000) return;
  lastCheckMs=millis();

  bool probeRtc=rtcPresent || !lastRtcProbeMs || (uint32_t)(millis()-lastRtcProbeMs)>=60000UL;
  if(probeRtc){
    lastRtcProbeMs=millis();
    RtcDateTime hw={0,0,0,0,0,0,false};
    bool busOk=rtcRead(hw);
    rtcPresent=busOk;
    if(busOk && hw.valid) clockCache(hw,CLOCK_RTC);
  }

  RtcDateTime dt;
  if (!clockNow(dt)) return;

  uint32_t dayKey=(uint32_t)dt.year*10000UL+(uint32_t)dt.month*100UL+dt.day;
  uint32_t minuteKey=(dayKey*1440UL)+(uint32_t)dt.hour*60UL+dt.minute;
  if (minuteKey == schedulerLastMinuteKey) return;
  schedulerLastMinuteKey = minuteKey;

  uint8_t wd=weekdayMon0(dt.year,dt.month,dt.day);
  uint8_t bit=(uint8_t)(1U<<wd);
  for (uint8_t i=0;i<SCHEDULE_TASK_COUNT;++i) {
    const ScheduleTask &t=scheduleTasks[i];
    if (!t.enabled || !(t.daysMask & bit)) continue;
    if (t.hour==dt.hour && t.minute==dt.minute) executeScheduleTask(i);
  }
}

void handleRtcGet() {''', 'scheduler logical clock', re.S)

# RTC/time API reports physical RTC separately from logical time validity and source.
sub_once(r'void handleRtcGet\(\) \{.*?\n\}\n\nvoid handleRtcSet\(\) \{', r'''void handleRtcGet() {
  RtcDateTime dt={0,0,0,0,0,0,false};
  bool timeOk=clockNow(dt);
  String j=F("{\"ok\":true,\"present\":");
  j+=rtcPresent?F("true"):F("false");
  j+=F(",\"valid\":"); j+=timeOk?F("true"):F("false");
  j+=F(",\"type\":\""); j+=rtcPresent?F("DS1307/compatible"):F("software"); j+=F("\"");
  j+=F(",\"source\":\""); j+=clockSourceName(); j+=F("\"");
  j+=F(",\"persistent_clock\":"); j+=rtcPresent?F("true"):F("false");
  j+=F(",\"eeprom_present\":"); j+=eepromPresent?F("true"):F("false");
  j+=F(",\"persistent_stats\":"); j+=eepromPresent?F("true"):F("false");
  j+=F(",\"eeprom_addr\":"); if(eepromPresent) j+=eepromI2cAddr; else j+=F("null");
  j+=F(",\"time\":\""); if(timeOk)j+=rtcIso(dt); j+=F("\"");
  j+=F(",\"cache_age_ms\":"); if(clockCachedAtUs)j+=(uint32_t)((esp_timer_get_time()-clockCachedAtUs)/1000ULL);else j+=F("null");
  j+=F(",\"ntp_server\":\""); j+=jsonEscape(ntpServer); j+=F("\"");
  j+=F(",\"ntp_utc_offset_min\":"); j+=ntpUtcOffsetMin;
  j+=F(",\"ntp_synced\":"); j+=ntpEverSynced?F("true"):F("false");
  j+=F(",\"ntp_last_correction_s\":"); j+=ntpLastCorrectionSec;
  j+=F(",\"ntp_sync_age_s\":"); if(ntpEverSynced) j+=(millis()-ntpLastSyncMs)/1000UL; else j+=F("null");
  j+=F(",\"runs\":");j+=schedulerRuns;
  j+=F(",\"errors\":");j+=schedulerErrors;
  j+=F(",\"last\":\"");j+=jsonEscape(schedulerLast);j+=F("\"}");
  sendJson(200,j);
}

void handleRtcSet() {''', 'RTC get API', re.S)

# Manual browser time always seeds the logical clock. RTC write is best-effort.
sub_once(r'void handleRtcSet\(\) \{.*?\n\}\n\nvoid handleNtpConfigSet\(\) \{', r'''void handleRtcSet() {
  if (!adminAuthorized()) { sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}")); return; }
  String body=web.arg("plain");
  double y,mo,d,h,mi,se;
  if (!jsonFindNumber(body,"year",y)||!jsonFindNumber(body,"month",mo)||!jsonFindNumber(body,"day",d)||
      !jsonFindNumber(body,"hour",h)||!jsonFindNumber(body,"minute",mi)||!jsonFindNumber(body,"second",se)) {
    sendJson(400,F("{\"ok\":false,\"error\":\"year/month/day/hour/minute/second required\"}")); return;
  }
  RtcDateTime dt={(uint16_t)y,(uint8_t)mo,(uint8_t)d,(uint8_t)h,(uint8_t)mi,(uint8_t)se,true};
  if(!clockDateTimeValid(dt)){sendJson(400,F("{\"ok\":false,\"error\":\"invalid date/time\"}"));return;}
  clockCache(dt,CLOCK_BROWSER);
  bool rtcStored=false;
  if(rtcPresent){rtcStored=rtcWrite(dt);if(!rtcStored)rtcPresent=false;}
  schedulerLastMinuteKey=0xFFFFFFFFUL;
  String j=F("{\"ok\":true,\"saved\":true,\"source\":\"browser\",\"rtc_persisted\":");j+=rtcStored?F("true"):F("false");j+='}';sendJson(200,j);
}

void handleNtpConfigSet() {''', 'browser clock set API', re.S)

# Manual NTP sync also succeeds without hardware RTC.
sub_once(r'void handleRtcNtpSync\(\) \{.*?\n\}\n\nvoid handleScheduleGet\(\) \{', r'''void handleRtcNtpSync() {
  if (!adminAuthorized()) { sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}")); return; }
  if (WiFi.status()!=WL_CONNECTED) { sendJson(503,F("{\"ok\":false,\"error\":\"Wi-Fi offline\"}")); return; }
  if (!ntpConfigured) { configTime((long)ntpUtcOffsetMin * 60L, 0, ntpServer.c_str(), NTP_FALLBACK_SERVER); ntpConfigured=true; ntpLastAttemptMs=0; }
  RtcDateTime ndt={0,0,0,0,0,0,false}; uint32_t deadline=millis()+3500;
  while(!ntpLocalDateTime(ndt) && (int32_t)(deadline-millis())>0) { delay(50); feedTaskWatchdog(); }
  if (!ndt.valid) { sendJson(504,F("{\"ok\":false,\"error\":\"NTP time unavailable\"}")); return; }
  RtcDateTime old;
  if(clockNow(old)) ntpLastCorrectionSec=(int32_t)(rtcCivilSeconds(ndt)-rtcCivilSeconds(old)); else ntpLastCorrectionSec=0;
  clockCache(ndt,CLOCK_NTP);
  bool rtcStored=false;
  if(rtcPresent){rtcStored=rtcWrite(ndt);if(!rtcStored)rtcPresent=false;}
  ntpEverSynced=true;ntpLastSyncMs=millis();ntpLastAttemptMs=millis();schedulerLastMinuteKey=0xFFFFFFFFUL;
  String j=F("{\"ok\":true,\"synced\":true,\"source\":\"ntp\",\"rtc_persisted\":");j+=rtcStored?F("true"):F("false");j+='}';sendJson(200,j);
}

void handleScheduleGet() {''', 'manual NTP API', re.S)

# Seed common clock from RTC at boot only when RTC time itself is valid.
once('  rtcPresent = rtcRead(bootRtc);\n  if (rtcPresent) { rtcCached=bootRtc; rtcCachedAtMs=millis(); }',
     '  rtcPresent = rtcRead(bootRtc);\n  if (rtcPresent && bootRtc.valid) clockCache(bootRtc,CLOCK_RTC);', 'boot clock')

# Explain optional hardware in the UI.
once('<div class="card"><h2>RTC / Планировщик</h2>', '<div class="card"><h2>Время / Планировщик</h2>', 'time card title')
once('<div class="metric"><small>RTC DS1307</small><b id="rtcState">—</b></div>\n <div class="metric"><small>Время RTC</small><b id="rtcTime">—</b></div>',
     '<div class="metric"><small>Источник времени</small><b id="rtcState">—</b></div>\n <div class="metric"><small>Текущее время</small><b id="rtcTime">—</b></div>', 'time metrics')
once('<button onclick="rtcLoad()">Обновить RTC</button>', '<button onclick="rtcLoad()">Обновить время</button>', 'time refresh label')
once('<div class="small" style="margin-top:8px">DS1307/совместимый RTC: SDA GPIO21, SCL GPIO22. AT24C32 определяется автоматически на 0x50…0x57. Планировщик работает автономно без Wi‑Fi.</div>',
'''<div class="small" style="margin-top:8px"><b>DS1307 и AT24C32 опциональны.</b> Без RTC после загрузки время автоматически берётся по NTP; если сеть недоступна — его можно задать кнопкой «Синхронизировать с браузером». После получения времени программные часы и планировщик продолжают работать без Wi‑Fi до следующей перезагрузки. Задания планировщика хранятся в NVS. Без AT24C32 накопительная статистика батареи и T1/T2 работает в RAM, но не сохраняется после перезагрузки.</div>''', 'optional hardware help')

# Replace RTC UI loader to show logical clock even when physical RTC is absent.
sub_once(r'async function rtcLoad\(\)\{.*?(?=\nasync function rtcSyncBrowser\(\)\{)', r'''async function rtcLoad(){
 try{
  const r=await api('/api/rtc');
  const src={rtc:'RTC',ntp:'NTP',browser:'Браузер',none:'Нет времени'}[r.source]||r.source||'Нет времени';
  $('rtcState').textContent=r.valid?`${src}${r.present?' · RTC есть':' · программные часы'}`:'НЕТ ВРЕМЕНИ';
  $('rtcTime').textContent=r.time||'—';
  $('rtcRuns').textContent=`${r.runs} / ${r.errors}`;
  if($('rtcMsg')) $('rtcMsg').textContent=`RTC: ${r.present?'есть':'нет'} · EEPROM: ${r.eeprom_present?'есть':'нет'}${r.last?' · '+r.last:''}`;
  if($('ntpServerSelect')){
   const known=['pool.ntp.org','time.google.com','time.cloudflare.com'];
   const v=known.includes(r.ntp_server)?r.ntp_server:'custom'; $('ntpServerSelect').value=v;
   $('ntpServerCustom').disabled=v!=='custom'; $('ntpServerCustom').value=v==='custom'?(r.ntp_server||''):'';
   $('ntpUtcOffset').value=Number(r.ntp_utc_offset_min||0)/60;
  }
 }catch(e){$('rtcMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}''', 'RTC UI loader', re.S)

# Update synchronization success wording; use API rtc_persisted flag.
sub_once(r'async function rtcSyncBrowser\(\)\{.*?(?=\nasync function rtcSyncNtp\(\)\{)', r'''async function rtcSyncBrowser(){
 const d=new Date();
 try{
  const r=await api('/api/rtc/set',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({year:d.getFullYear(),month:d.getMonth()+1,day:d.getDate(),hour:d.getHours(),minute:d.getMinutes(),second:d.getSeconds()})});
  $('rtcMsg').innerHTML='<span class=ok>Время установлено из браузера'+(r.rtc_persisted?' и записано в RTC':' · используется программный ход')+'.</span>'; await rtcLoad();
 }catch(e){$('rtcMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}''', 'browser UI sync', re.S)
sub_once(r'async function rtcSyncNtp\(\)\{.*?(?=\nasync function ntpSave\(\)\{)', r'''async function rtcSyncNtp(){
 try{
  const r=await api('/api/ntp/sync',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}',timeoutMs:6000});
  $('rtcMsg').innerHTML='<span class=ok>Время синхронизировано по NTP'+(r.rtc_persisted?' и записано в RTC':' · используется программный ход')+'.</span>'; await rtcLoad();
 }catch(e){$('rtcMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}''', 'NTP UI sync', re.S)

p.write_text(s, encoding='utf-8')
print('patched', p, p.stat().st_size)
