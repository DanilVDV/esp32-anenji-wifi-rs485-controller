#!/usr/bin/env python3
from pathlib import Path
import re, sys

FW=Path('firmware/ANENJI_ESP32_Controller/ANENJI_ESP32_Controller.ino')
README=Path('README.md')

def one(s,a,b,label):
    n=s.count(a)
    if n!=1:
        raise SystemExit(f'{label}: expected 1 occurrence, got {n}')
    return s.replace(a,b,1)

def patch_fw():
    s=FW.read_text(encoding='utf-8')
    s=one(s,
        'const char* FW_VERSION = "0.15.8";\nconst char* FW_VERSION_PREVIOUS = "0.15.7";',
        'const char* FW_VERSION = "0.15.9";\nconst char* FW_VERSION_PREVIOUS = "0.15.8";',
        'version')

    pzem_anchor='''bool pzemAlarm=false;\nuint32_t pzemUpdatedMs=0, pzemLastPollMs=0, pzemPollOk=0, pzemPollErrors=0;'''
    hall_globals='''bool pzemAlarm=false;\nuint32_t pzemUpdatedMs=0, pzemLastPollMs=0, pzemPollOk=0, pzemPollErrors=0;\n\n// Optional PZCT-DC63 Hall DC current sensor via ADS1115 on the shared I2C bus.\n// Address 0 means auto-detect 0x48..0x4B. Default calibration matches the\n// common 100 A version: about 2.5 V at zero and about 20 mV/A.\nconst uint8_t ADS1115_ADDR_FIRST=0x48;\nconst uint8_t ADS1115_ADDR_LAST=0x4B;\nbool hallEnabled=false;\nbool hallOnline=false;\nuint8_t hallAdsAddress=0;\nuint8_t hallResolvedAddress=0;\nuint8_t hallChannel=0;\nfloat hallZeroMv=2500.0f;\nfloat hallSensitivityMvPerA=20.0f;\nbool hallInvert=false;\nfloat hallInputMv=NAN;\nfloat hallCurrentA=NAN;\nuint32_t hallUpdatedMs=0;\nuint32_t hallLastPollMs=0;\nconst uint32_t HALL_POLL_INTERVAL_MS=1000;'''
    s=one(s,pzem_anchor,hall_globals,'hall globals')

    s=one(s,'void handleEvents();',
          'void handleEvents();\nbool i2cProbe(uint8_t addr);\nvoid serviceHallCurrent();\nvoid handleHallGet();\nvoid handleHallConfigSet();\nvoid handleHallZeroCal();\nvoid handleHallSpanCal();',
          'hall forward declarations')

    pref_anchor='''  pzemEnabled = appPrefs.getBool("pzem_en", true);\n  uint32_t ps=appPrefs.getUInt("pzem_slave", PZEM_DEFAULT_SLAVE);\n  pzemSlave=(ps>=1 && ps<=247)?(uint8_t)ps:PZEM_DEFAULT_SLAVE;'''
    pref_new=pref_anchor+'''\n  hallEnabled = appPrefs.getBool("hall_en", false);\n  uint32_t ha=appPrefs.getUInt("hall_addr", 0);\n  hallAdsAddress=(ha==0 || (ha>=ADS1115_ADDR_FIRST && ha<=ADS1115_ADDR_LAST))?(uint8_t)ha:0;\n  uint32_t hc=appPrefs.getUInt("hall_ch", 0); hallChannel=(hc<=3)?(uint8_t)hc:0;\n  hallZeroMv=appPrefs.getFloat("hall_zero",2500.0f);\n  if(hallZeroMv<0.0f || hallZeroMv>5500.0f) hallZeroMv=2500.0f;\n  hallSensitivityMvPerA=appPrefs.getFloat("hall_sens",20.0f);\n  if(hallSensitivityMvPerA<0.05f || hallSensitivityMvPerA>1000.0f) hallSensitivityMvPerA=20.0f;\n  hallInvert=appPrefs.getBool("hall_inv",false);'''
    s=one(s,pref_anchor,pref_new,'hall prefs load')

    rtc_marker='// ---------------- DS1307/DS3231-common RTC + scheduler ----------------'
    hall_code=r'''// ---------------- PZCT-DC63 Hall current sensor / ADS1115 ----------------
uint8_t hallResolveAddress() {
  if(hallAdsAddress>=ADS1115_ADDR_FIRST && hallAdsAddress<=ADS1115_ADDR_LAST) {
    if(i2cProbe(hallAdsAddress)) { hallResolvedAddress=hallAdsAddress; return hallResolvedAddress; }
    hallResolvedAddress=0; return 0;
  }
  for(uint8_t a=ADS1115_ADDR_FIRST;a<=ADS1115_ADDR_LAST;a++) {
    if(i2cProbe(a)) { hallResolvedAddress=a; return a; }
  }
  hallResolvedAddress=0; return 0;
}

bool hallReadMv(float& mv) {
  uint8_t addr=hallResolveAddress();
  if(!addr || hallChannel>3) return false;
  // ADS1115: single-shot, AINx vs GND, PGA +/-6.144 V, 128 SPS, comparator disabled.
  uint16_t cfg=(uint16_t)(0x8000 | ((uint16_t)(4+hallChannel)<<12) | 0x0100 | (4u<<5) | 0x0003);
  Wire.beginTransmission(addr); Wire.write((uint8_t)0x01); Wire.write((uint8_t)(cfg>>8)); Wire.write((uint8_t)cfg);
  if(Wire.endTransmission()!=0) return false;
  delay(10);
  Wire.beginTransmission(addr); Wire.write((uint8_t)0x00);
  if(Wire.endTransmission(false)!=0) return false;
  if(Wire.requestFrom((int)addr,2)!=2) return false;
  int16_t raw=(int16_t)(((uint16_t)Wire.read()<<8)|Wire.read());
  mv=(float)raw*0.1875f; // +/-6.144 V range -> 187.5 uV/LSB
  return true;
}

bool hallAverageMv(float& mv, uint8_t samples=6) {
  if(samples<1) samples=1; if(samples>16) samples=16;
  float sum=0.0f; uint8_t ok=0;
  for(uint8_t i=0;i<samples;i++) { float v=0; if(hallReadMv(v)){sum+=v;ok++;} delay(2); }
  if(ok<((samples+1)/2)) return false;
  mv=sum/(float)ok; return true;
}

void serviceHallCurrent() {
  if(!hallEnabled) { hallOnline=false; hallCurrentA=NAN; hallInputMv=NAN; return; }
  uint32_t now=millis(); if(hallLastPollMs && now-hallLastPollMs<HALL_POLL_INTERVAL_MS) return; hallLastPollMs=now;
  float mv=0; if(!hallAverageMv(mv,3)) { hallOnline=false; return; }
  hallInputMv=mv; hallOnline=true; hallUpdatedMs=now;
  float a=(mv-hallZeroMv)/hallSensitivityMvPerA; if(hallInvert) a=-a;
  if(fabsf(a)<0.03f) a=0.0f; hallCurrentA=a;
}

'''
    s=one(s,rtc_marker,hall_code+rtc_marker,'hall service block')

    handler_marker='void handlePzemConfigSet() {'
    handlers=r'''void handleHallGet() {
  String j=F("{\"ok\":true");
  j+=F(",\"enabled\":"); j+=hallEnabled?F("true"):F("false");
  j+=F(",\"online\":"); j+=hallOnline?F("true"):F("false");
  j+=F(",\"address\":"); j+=hallAdsAddress;
  j+=F(",\"resolved_address\":"); if(hallResolvedAddress)j+=hallResolvedAddress;else j+=F("null");
  j+=F(",\"channel\":"); j+=hallChannel;
  j+=F(",\"zero_mv\":"); j+=String(hallZeroMv,3);
  j+=F(",\"sensitivity_mv_per_a\":"); j+=String(hallSensitivityMvPerA,5);
  j+=F(",\"invert\":"); j+=hallInvert?F("true"):F("false");
  j+=F(",\"input_mv\":"); if(hallOnline&&!isnan(hallInputMv))j+=String(hallInputMv,3);else j+=F("null");
  j+=F(",\"current_a\":"); if(hallOnline&&!isnan(hallCurrentA))j+=String(hallCurrentA,3);else j+=F("null");
  j+=F(",\"age_ms\":"); if(hallUpdatedMs)j+=(millis()-hallUpdatedMs);else j+=F("null"); j+='}'; sendJson(200,j);
}

void handleHallConfigSet() {
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  String body=web.arg("plain");
  double en=hallEnabled?1:0,addr=hallAdsAddress,ch=hallChannel,zero=hallZeroMv,sens=hallSensitivityMvPerA,inv=hallInvert?1:0;
  jsonFindNumber(body,"enabled",en); jsonFindNumber(body,"address",addr); jsonFindNumber(body,"channel",ch);
  jsonFindNumber(body,"zero_mv",zero); jsonFindNumber(body,"sensitivity_mv_per_a",sens); jsonFindNumber(body,"invert",inv);
  if(!((int)addr==0 || ((int)addr>=ADS1115_ADDR_FIRST && (int)addr<=ADS1115_ADDR_LAST)) || ch<0 || ch>3 || zero<0 || zero>5500 || sens<0.05 || sens>1000) {
    sendJson(400,F("{\"ok\":false,\"error\":\"Invalid ADS1115/Hall configuration\"}")); return;
  }
  hallEnabled=(en!=0); hallAdsAddress=(uint8_t)addr; hallChannel=(uint8_t)ch; hallZeroMv=(float)zero;
  hallSensitivityMvPerA=(float)sens; hallInvert=(inv!=0); hallResolvedAddress=0; hallLastPollMs=0;
  appPrefs.putBool("hall_en",hallEnabled); appPrefs.putUInt("hall_addr",hallAdsAddress); appPrefs.putUInt("hall_ch",hallChannel);
  appPrefs.putFloat("hall_zero",hallZeroMv); appPrefs.putFloat("hall_sens",hallSensitivityMvPerA); appPrefs.putBool("hall_inv",hallInvert);
  serviceHallCurrent(); sendJson(200,F("{\"ok\":true}"));
}

void handleHallZeroCal() {
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  float mv=0; if(!hallAverageMv(mv,10)){sendJson(503,F("{\"ok\":false,\"error\":\"ADS1115 not responding\"}"));return;}
  hallZeroMv=mv; appPrefs.putFloat("hall_zero",hallZeroMv); hallLastPollMs=0; serviceHallCurrent();
  String j=String("{\"ok\":true,\"zero_mv\":")+String(hallZeroMv,3)+"}"; sendJson(200,j);
}

void handleHallSpanCal() {
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  String body=web.arg("plain"); double known=0; if(!jsonFindNumber(body,"current_a",known) || fabs(known)<0.5 || fabs(known)>1000) {
    sendJson(400,F("{\"ok\":false,\"error\":\"Known current must be between 0.5 and 1000 A (signed)\"}"));return;
  }
  float mv=0; if(!hallAverageMv(mv,10)){sendJson(503,F("{\"ok\":false,\"error\":\"ADS1115 not responding\"}"));return;}
  float signedK=(mv-hallZeroMv)/(float)known; float sens=fabsf(signedK);
  if(sens<0.05f || sens>1000.0f){sendJson(400,F("{\"ok\":false,\"error\":\"Calibration span is too small or invalid\"}"));return;}
  hallSensitivityMvPerA=sens; hallInvert=(signedK<0.0f); appPrefs.putFloat("hall_sens",hallSensitivityMvPerA); appPrefs.putBool("hall_inv",hallInvert);
  hallLastPollMs=0; serviceHallCurrent();
  String j=String("{\"ok\":true,\"sensitivity_mv_per_a\":")+String(hallSensitivityMvPerA,5)+",\"invert\":"+(hallInvert?"true":"false")+"}"; sendJson(200,j);
}

'''
    s=one(s,handler_marker,handlers+handler_marker,'hall API handlers')

    routes='''  web.on("/api/pzem/config", HTTP_POST, handlePzemConfigSet);\n  web.on("/api/pzem/tariff", HTTP_POST, handlePzemTariffSet);\n  web.on("/api/pzem/energy/reset", HTTP_POST, handlePzemEnergyReset);'''
    routes_new=routes+'''\n  web.on("/api/hall", HTTP_GET, handleHallGet);\n  web.on("/api/hall/config", HTTP_POST, handleHallConfigSet);\n  web.on("/api/hall/cal/zero", HTTP_POST, handleHallZeroCal);\n  web.on("/api/hall/cal/span", HTTP_POST, handleHallSpanCal);'''
    s=one(s,routes,routes_new,'hall routes')

    card_anchor='<div class="card"><h2>Modbus / RS‑485</h2>'
    card=r'''<div class="card"><h2>PZCT-DC63 · DC ток / ADS1115</h2>
<div class="grid">
 <div class="metric"><small>Ток Холла</small><b id="hallA">— A</b></div>
 <div class="metric"><small>Вход ADS1115</small><b id="hallMv">— mV</b></div>
 <div class="metric"><small>ADS1115</small><b id="hallAdsState">—</b></div>
</div>
<div class="netgrid" style="margin-top:12px">
 <label><span><input id="hallEnabled" type="checkbox"> включить датчик</span></label>
 <label>Адрес ADS1115<select id="hallAddr"><option value="0">Авто 0x48…0x4B</option><option value="72">0x48</option><option value="73">0x49</option><option value="74">0x4A</option><option value="75">0x4B</option></select></label>
 <label>Канал ADS1115<select id="hallChannel"><option value="0">A0</option><option value="1">A1</option><option value="2">A2</option><option value="3">A3</option></select></label>
 <label>Ноль, mV<input id="hallZeroMv" type="number" step="0.1" min="0" max="5500"></label>
 <label>Чувствительность, mV/A<input id="hallSens" type="number" step="0.0001" min="0.05" max="1000"></label>
 <label><span><input id="hallInvert" type="checkbox"> инвертировать направление</span></label>
</div>
<div class="toolbar" style="margin-top:10px"><button onclick="hallSave()">Сохранить</button><button onclick="hallNominal(100)">100 A номинал</button><button onclick="hallNominal(300)">300 A</button><button onclick="hallNominal(500)">500 A</button><span id="hallMsg" class="small"></span></div>
<details style="margin-top:10px"><summary>Калибровка датчика тока</summary>
 <div class="toolbar" style="margin-top:8px"><button onclick="hallCalZero()">Калибровать ноль</button><label>Эталонный ток, A <input id="hallKnownA" type="number" step="0.1" style="width:110px"></label><button onclick="hallCalSpan()">Калибровать по току</button></div>
 <div class="small">Сначала отключите ток через проводник и выполните «Калибровать ноль». Затем пропустите известный ток, введите его со знаком и выполните калибровку по току. Параметры сохраняются в NVS ESP32.</div>
</details>
<div class="small" style="margin-top:8px">PZCT-DC63 обычно имеет середину около 2.5 В и выход примерно 0.5…4.5 В. Для ADS1115, питаемого от 5 В, используйте двунаправленный преобразователь уровней I²C между ADS1115 и ESP32. Схемы: <code>docs/hall-current-ads1115.md</code>.</div>
</div>

'''
    s=one(s,card_anchor,card+card_anchor,'hall UI card')

    js_anchor='async function pzemSave(){'
    js=r'''let hallLoadBusy=false;
async function hallLoad(force=false){
 if(hallLoadBusy)return;hallLoadBusy=true;
 try{const x=await api('/api/hall',{timeoutMs:1800});
  if($('hallEnabled'))$('hallEnabled').checked=!!x.enabled;
  if($('hallAddr'))$('hallAddr').value=String(x.address??0);
  if($('hallChannel'))$('hallChannel').value=String(x.channel??0);
  if($('hallZeroMv'))$('hallZeroMv').value=Number(x.zero_mv??2500).toFixed(2);
  if($('hallSens'))$('hallSens').value=Number(x.sensitivity_mv_per_a??20).toFixed(5);
  if($('hallInvert'))$('hallInvert').checked=!!x.invert;
  if($('hallA'))$('hallA').textContent=x.current_a==null?'— A':Number(x.current_a).toFixed(2)+' A';
  if($('hallMv'))$('hallMv').textContent=x.input_mv==null?'— mV':Number(x.input_mv).toFixed(1)+' mV';
  if($('hallAdsState'))$('hallAdsState').textContent=x.online?('ONLINE · 0x'+Number(x.resolved_address).toString(16).toUpperCase()+' / A'+x.channel):'OFFLINE';
 }catch(e){if($('hallAdsState'))$('hallAdsState').textContent='OFFLINE'}finally{hallLoadBusy=false}
}
function hallNominal(a){if($('hallSens'))$('hallSens').value=(2000/Number(a)).toFixed(5);if($('hallMsg'))$('hallMsg').textContent='Номинал '+a+' A; сохраните или выполните двухточечную калибровку.';}
async function hallSave(){
 const body={enabled:$('hallEnabled').checked?1:0,address:Number($('hallAddr').value),channel:Number($('hallChannel').value),zero_mv:Number($('hallZeroMv').value),sensitivity_mv_per_a:Number($('hallSens').value),invert:$('hallInvert').checked?1:0};
 try{const h=await verifiedAdminHeaders('hallMsg');if(!h)return;await api('/api/hall/config',{method:'POST',headers:h,body:JSON.stringify(body)});$('hallMsg').innerHTML='<span class=ok>Сохранено</span>';setTimeout(()=>hallLoad(true),300);}catch(e){$('hallMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}
async function hallCalZero(){
 if(!confirm('Убедитесь, что через датчик сейчас не течёт ток. Записать текущее напряжение как ноль?'))return;
 try{const h=await verifiedAdminHeaders('hallMsg');if(!h)return;const r=await api('/api/hall/cal/zero',{method:'POST',headers:h,body:'{}'});$('hallMsg').innerHTML='<span class=ok>Ноль: '+Number(r.zero_mv).toFixed(2)+' mV</span>';setTimeout(()=>hallLoad(true),250);}catch(e){$('hallMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}
async function hallCalSpan(){
 const current_a=Number($('hallKnownA').value);if(!Number.isFinite(current_a)||Math.abs(current_a)<0.5){$('hallMsg').innerHTML='<span class=bad>Введите эталонный ток не менее 0.5 A</span>';return;}
 if(!confirm('Использовать '+current_a+' A как эталон для калибровки?'))return;
 try{const h=await verifiedAdminHeaders('hallMsg');if(!h)return;const r=await api('/api/hall/cal/span',{method:'POST',headers:h,body:JSON.stringify({current_a})});$('hallMsg').innerHTML='<span class=ok>Калибровка: '+Number(r.sensitivity_mv_per_a).toFixed(5)+' mV/A</span>';setTimeout(()=>hallLoad(true),250);}catch(e){$('hallMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}
setTimeout(()=>hallLoad(true),1200);setInterval(()=>hallLoad(false),3000);

'''
    s=one(s,js_anchor,js+js_anchor,'hall UI javascript')

    loop_anchor='''  serviceBootButton();\n  serviceWifiWatchdog();'''
    s=one(s,loop_anchor,loop_anchor+'\n  serviceHallCurrent();','hall loop service')

    FW.write_text(s,encoding='utf-8')


def patch_readme():
    s=README.read_text(encoding='utf-8')
    s=one(s,'Текущая версия прошивки: **0.15.8**.','Текущая версия прошивки: **0.15.9**.','README version')
    marker='- PZEM‑016 на той же RS‑485-шине;'
    s=one(s,marker,marker+'\n- PZCT-DC63 через ADS1115: локальное измерение DC-тока, автодетект ADS1115 и двухточечная калибровка;','README feature')
    link='Подробности: [docs/wiring.md](docs/wiring.md).'
    extra='''Подробности: [docs/wiring.md](docs/wiring.md).\n\n### Датчик DC-тока PZCT-DC63 + ADS1115\n\nПрошивка поддерживает опциональный датчик Холла **PZCT-DC63** через **ADS1115** на общей I²C-шине (`GPIO21/22`). В Web UI отображаются ток и напряжение входа АЦП; доступны калибровка нуля и калибровка по известному току.\n\nДва безопасных варианта схемы подключения и порядок калибровки: **[docs/hall-current-ads1115.md](docs/hall-current-ads1115.md)**.\n'''
    s=one(s,link,extra,'README Hall section')
    README.write_text(s,encoding='utf-8')


def validate():
    s=FW.read_text(encoding='utf-8')
    must=['FW_VERSION = "0.15.9"','hallReadMv','handleHallSpanCal','/api/hall/cal/zero','PZCT-DC63 · DC ток / ADS1115','serviceHallCurrent();']
    for x in must:
        if x not in s: raise SystemExit('missing '+x)
    m=re.search(r'R"HTML\((.*?)\)HTML"',s,re.S)
    if not m: raise SystemExit('embedded HTML not found')
    scripts=re.findall(r'<script>(.*?)</script>',m.group(1),re.S)
    if not scripts: raise SystemExit('embedded JS not found')
    Path('/tmp/embedded.js').write_text('\n'.join(scripts),encoding='utf-8')
    print('v0.15.9 Hall patch validation OK')

if __name__=='__main__':
    cmd=sys.argv[1]
    {'patch':patch_fw,'readme':patch_readme,'validate':validate}[cmd]()
