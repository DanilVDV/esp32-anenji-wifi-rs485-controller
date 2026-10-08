#!/usr/bin/env python3
# ANENJI v0.15.8 event timestamps + single README screenshot
from pathlib import Path
import re, sys

FW=Path('firmware/ANENJI_ESP32_Controller/ANENJI_ESP32_Controller.ino')
README=Path('README.md')

def one(s,a,b,label):
    n=s.count(a)
    if n!=1: raise SystemExit(f'{label}: expected 1, got {n}')
    return s.replace(a,b,1)

def patch():
    s=FW.read_text(encoding='utf-8')
    s=one(s,'const char* FW_VERSION = "0.15.7";\nconst char* FW_VERSION_PREVIOUS = "0.15.6";',
          'const char* FW_VERSION = "0.15.8";\nconst char* FW_VERSION_PREVIOUS = "0.15.7";','version')
    s=one(s,'  uint32_t uptimeSec;\n  uint8_t type;',
          '  uint32_t uptimeSec;\n  char timestamp[20]; // YYYY-MM-DD HH:MM:SS, empty until logical clock is valid\n  uint8_t type;','event timestamp field')
    s=one(s,'  e.id = inverterEventNextId++;\n  e.uptimeSec = millis()/1000UL;\n  e.type = type;',
          '  e.id = inverterEventNextId++;\n  e.uptimeSec = millis()/1000UL;\n  RtcDateTime eventDt = {};\n  if (clockNow(eventDt)) {\n    snprintf(e.timestamp, sizeof(e.timestamp), "%04u-%02u-%02u %02u:%02u:%02u",\n             eventDt.year, eventDt.month, eventDt.day, eventDt.hour, eventDt.minute, eventDt.second);\n  }\n  e.type = type;','capture event time')
    s=one(s,'    if(n)j+=\',\'; j+=F("{\\"id\\":");j+=e.id; j+=F(",\\"uptime_s\\":");j+=e.uptimeSec;\n    j+=F(",\\"type\\":\\"");',
          '    if(n)j+=\',\'; j+=F("{\\"id\\":");j+=e.id; j+=F(",\\"uptime_s\\":");j+=e.uptimeSec;\n    j+=F(",\\"timestamp\\":"); if(e.timestamp[0]){j+=\'"\';j+=e.timestamp;j+=\'"\';}else j+=F("null");\n    j+=F(",\\"type\\":\\"");','events api timestamp')
    old="return `<div class=\"alert-row ${cls} ${e.active?'':'clear'}\"><b>${e.severity} · ${e.type.toUpperCase()} ${state}</b><small>${e.message} · uptime ${e.uptime_s}s</small></div>`"
    new="const when=e.timestamp?e.timestamp:(`uptime ${e.uptime_s}s`);return `<div class=\"alert-row ${cls} ${e.active?'':'clear'}\"><b>${when} · ${e.severity} · ${e.type.toUpperCase()} ${state}</b><small>${e.message}</small></div>`"
    s=one(s,old,new,'event ui timestamp')
    FW.write_text(s,encoding='utf-8')

def readme():
    s=README.read_text(encoding='utf-8')
    s=s.replace('Текущая версия прошивки: **0.15.7**.','Текущая версия прошивки: **0.15.8**.')
    start=s.index('## Демонстрация Web UI')
    end=s.index('\n## Возможности',start)
    block='''## Демонстрация Web UI\n\nОдин актуальный снимок рабочего интерфейса:\n\n<p align="center"><img src="docs/media/current-overview.webp" alt="ANENJI ESP32 Web UI" width="900"></p>\n\n> Скриншот снят с реального интерфейса контроллера; старые preview/GIF и автоматически сгенерированные галереи README больше не использует.\n'''
    s=s[:start]+block+s[end:]
    if '- журнал событий с датой и временем' not in s:
        marker='- полный warning map bit0..20, включая lithium communication и превышение тока разряда;'
        s=s.replace(marker,marker+'\n- журнал событий с датой и временем после синхронизации RTC/NTP/браузером;',1)
    README.write_text(s,encoding='utf-8')

def validate():
    s=FW.read_text(encoding='utf-8')
    for x in ['FW_VERSION = "0.15.8"','char timestamp[20]','clockNow(eventDt)','\\"timestamp\\"','const when=e.timestamp?e.timestamp']:
        if x not in s: raise SystemExit('missing '+x)
    m=re.search(r'R"HTML\((.*?)\)HTML"',s,re.S)
    if not m: raise SystemExit('html missing')
    js='\n'.join(re.findall(r'<script>(.*?)</script>',m.group(1),re.S))
    Path('/tmp/embedded.js').write_text(js,encoding='utf-8')
    print('v0.15.8 validation OK')

if __name__=='__main__':
    cmd=sys.argv[1]
    {'patch':patch,'readme':readme,'validate':validate}[cmd]()
