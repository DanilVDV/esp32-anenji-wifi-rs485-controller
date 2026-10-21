from pathlib import Path
import re, sys

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')
p = root / 'firmware/ANENJI_ESP32_Controller/ANENJI_ESP32_Controller.ino'
s = p.read_text(encoding='utf-8')

def sub_once(pattern, repl, label):
    global s
    s2,n=re.subn(pattern,repl,s,count=1,flags=re.S)
    if n!=1: raise SystemExit(f'pattern failed ({n}): {label}')
    s=s2

sub_once(r'async function rtcLoad\(\)\{.*?(?=\nasync function rtcSyncBrowser\(\)\{)', r'''async function rtcLoad(){
 try{
  const r=await api('/api/rtc');
  const srcRu={rtc:'RTC',ntp:'NTP',browser:'Браузер',none:'Нет времени'};
  const srcEn={rtc:'RTC',ntp:'NTP',browser:'Browser',none:'No time'};
  const src=(currentLang==='ru'?srcRu:srcEn)[r.source]||r.source||(currentLang==='ru'?'Нет времени':'No time');
  $('rtcState').textContent=r.valid?`${src}${r.present?(currentLang==='ru'?' · RTC есть':' · RTC present'):(currentLang==='ru'?' · программные часы':' · software clock')}`:(currentLang==='ru'?'НЕТ ВРЕМЕНИ':'NO TIME');
  $('rtcTime').textContent=r.time||'—';
  $('rtcRuns').textContent=`${r.runs} / ${r.errors}`;
  if($('rtcMsg')) $('rtcMsg').textContent=`RTC: ${r.present?(currentLang==='ru'?'есть':'present'):(currentLang==='ru'?'нет':'absent')} · EEPROM: ${r.eeprom_present?(currentLang==='ru'?'есть':'present'):(currentLang==='ru'?'нет':'absent')}${r.last?' · '+r.last:''}`;
  if($('ntpServerSelect')){
   const known=['pool.ntp.org','time.google.com','time.cloudflare.com'];
   const v=known.includes(r.ntp_server)?r.ntp_server:'custom'; $('ntpServerSelect').value=v;
   $('ntpServerCustom').disabled=v!=='custom'; $('ntpServerCustom').value=v==='custom'?(r.ntp_server||''):'';
   $('ntpUtcOffset').value=((r.ntp_utc_offset_min??180)/60).toFixed(2).replace(/\.00$/,'');
  }
  const sched=await api('/api/schedule'); renderSchedule(sched.tasks);
 }catch(e){$('rtcMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}''', 'rtcLoad')

sub_once(r'async function rtcSyncBrowser\(\)\{.*?(?=\nasync function rtcSyncNtp\(\)\{)', r'''async function rtcSyncBrowser(){
 const d=new Date(),body={year:d.getFullYear(),month:d.getMonth()+1,day:d.getDate(),hour:d.getHours(),minute:d.getMinutes(),second:d.getSeconds()};
 try{
  const h=await verifiedAdminHeaders('rtcMsg'); if(!h)return;
  const r=await api('/api/rtc/set',{method:'POST',headers:h,body:JSON.stringify(body)});
  $('rtcMsg').innerHTML='<span class=ok>'+(currentLang==='ru'?'Время установлено из браузера':'Time set from browser')+(r.rtc_persisted?(currentLang==='ru'?' и записано в RTC':' and persisted to RTC'):(currentLang==='ru'?' · программный ход':' · software clock'))+'.</span>';
  await rtcLoad();
 }catch(e){$('rtcMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}''', 'rtcSyncBrowser')

sub_once(r'async function rtcSyncNtp\(\)\{.*?(?=\nfunction ntpServerChoice\(\)\{)', r'''async function rtcSyncNtp(){
 try{
  const h=await verifiedAdminHeaders('rtcMsg'); if(!h)return;
  const r=await api('/api/rtc/ntp',{method:'POST',headers:h,body:'{}',timeoutMs:6000});
  $('rtcMsg').innerHTML='<span class=ok>'+(currentLang==='ru'?'Время синхронизировано по NTP':'Time synchronized by NTP')+(r.rtc_persisted?(currentLang==='ru'?' и записано в RTC':' and persisted to RTC'):(currentLang==='ru'?' · программный ход':' · software clock'))+'.</span>';
  await rtcLoad();
 }catch(e){$('rtcMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}''', 'rtcSyncNtp')

p.write_text(s,encoding='utf-8')
print('fixed UI',p,p.stat().st_size)
