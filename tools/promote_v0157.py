#!/usr/bin/env python3
from pathlib import Path
import re, sys

ROOT = Path('.')
FW = ROOT / 'firmware/ANENJI_ESP32_Controller/ANENJI_ESP32_Controller.ino'
README = ROOT / 'README.md'


def replace_one(s, old, new, label):
    n = s.count(old)
    if n != 1:
        raise SystemExit(f'{label}: expected one occurrence, got {n}')
    return s.replace(old, new, 1)


def patch_firmware():
    s = FW.read_text(encoding='utf-8')
    s = replace_one(s,
        'const char* FW_VERSION = "0.15.6";\nconst char* FW_VERSION_PREVIOUS = "0.15.5";',
        'const char* FW_VERSION = "0.15.7";\nconst char* FW_VERSION_PREVIOUS = "0.15.6";', 'version')

    m = re.search(r'static const char\* const INVERTER_WARNINGS\[\] = \{.*?\n\};', s, re.S)
    if not m:
        raise SystemExit('warning map not found')
    warnings = '''static const char* const INVERTER_WARNINGS[] = {
  "Mains supply zero-crossing loss", "Mains waveform abnormal", "Mains over voltage", "Mains low voltage",
  "Mains over frequency", "Mains low frequency", "PV low voltage", "Over temperature",
  "Battery low voltage", "Battery not connected", "Overload", "Battery equalization charging",
  "Battery undervoltage / recovery point not reached", "Output power derating", "Fan blocked", "PV energy too low to use",
  "Parallel communication interrupted", "Single/parallel output mode inconsistent",
  "Parallel battery voltage difference too large", "Lithium battery communication abnormal",
  "Battery discharge current exceeds configured limit"
};'''
    s = s[:m.start()] + warnings + s[m.end():]

    s = replace_one(s,
        'uint32_t inverterWarningMask = 0;\nuint32_t inverterPrevFaultMask = 0;',
        'uint32_t inverterWarningMask = 0;\nuint32_t inverterWarningUnmaskedMask = 0;\nuint32_t inverterPrevFaultMask = 0;', 'unmasked warning global')
    s = replace_one(s,
        'inverterFaultMask=((uint32_t)d[0]<<16)|d[1];\n    inverterWarningMask=((uint32_t)d[8]<<16)|d[9];',
        'inverterFaultMask=((uint32_t)d[0]<<16)|d[1];\n    inverterWarningUnmaskedMask=((uint32_t)d[4]<<16)|d[5];\n    inverterWarningMask=((uint32_t)d[8]<<16)|d[9];', 'diag masks')
    s = replace_one(s,
        'j += F(",\\"warning_raw\\":"); j += inverterWarningMask;',
        'j += F(",\\"warning_raw\\":"); j += inverterWarningMask;\n  j += F(",\\"warning_unmasked_raw\\":"); j += inverterWarningUnmaskedMask;', 'status warning raw')
    s = replace_one(s,
        'j += F(",\\"warnings\\":"); appendMaskNamesJson(j,inverterWarningMask,INVERTER_WARNINGS,INVERTER_WARNING_COUNT);',
        'j += F(",\\"warnings\\":"); appendMaskNamesJson(j,inverterWarningMask,INVERTER_WARNINGS,INVERTER_WARNING_COUNT);\n  j += F(",\\"warnings_unmasked\\":"); appendMaskNamesJson(j,inverterWarningUnmaskedMask,INVERTER_WARNINGS,INVERTER_WARNING_COUNT);', 'status warning list')

    s = replace_one(s,
        '  {313,"АКБ","Выравнивание АКБ",SK_SELECT,1,0,1,true},\n  {320,"Основные","Выходное напряжение",SK_SELECT,1,2200,2400,true},',
        '  {313,"АКБ","Выравнивание АКБ",SK_SELECT,1,0,1,true},\n  {316,"Прочие","Dry contact / Grounding box",SK_SELECT,1,0,1,true},\n  {320,"Основные","Выходное напряжение",SK_SELECT,1,2200,2400,true},', 'reg316')
    s = replace_one(s, '{330,"АКБ","CV -> Float, время",SK_NUMBER,1,0,600,true}', '{330,"АКБ","CV -> Float, время",SK_NUMBER,1,0,900,true}', 'reg330')
    s = replace_one(s, '{332,"АКБ","Макс. ток зарядки",SK_NUMBER,10,0,6553.5,true}', '{332,"АКБ","Макс. ток зарядки",SK_NUMBER,10,10,80,true}', 'reg332')
    s = replace_one(s, '{333,"АКБ","Макс. ток зарядки от сети",SK_NUMBER,10,0,6553.5,true}', '{333,"АКБ","Макс. ток зарядки от сети",SK_NUMBER,10,2,80,true}', 'reg333')
    s = replace_one(s, '{335,"АКБ","Equalization time",SK_NUMBER,1,0,180,true}', '{335,"АКБ","Equalization time",SK_NUMBER,1,0,900,true}', 'reg335')
    s = replace_one(s, '{336,"АКБ","Equalization timeout",SK_NUMBER,1,0,300,true}', '{336,"АКБ","Equalization timeout",SK_NUMBER,1,0,900,true}', 'reg336')
    s = replace_one(s,
        '  {337,"АКБ","Equalization interval",SK_NUMBER,1,1,90,true},\n  {341,"SOC","Low DC SOC в режиме сети",SK_NUMBER,1,0,100,true},',
        '  {337,"АКБ","Equalization interval",SK_NUMBER,1,1,90,true},\n  {338,"Прочие","Automatic mains output",SK_SELECT,1,0,1,true},\n  {341,"SOC","Low DC SOC в режиме сети",SK_NUMBER,1,20,50,true},', 'reg338/341')
    s = replace_one(s,
        '  {342,"SOC","Recovery SOC",SK_NUMBER,1,0,100,true},\n  {343,"SOC","Off-grid cut-off SOC",SK_NUMBER,1,0,100,true},\n  {344,"Основные","Макс. отдача в сеть",SK_NUMBER,1,0,6200,true},',
        '  {342,"SOC","Recovery SOC",SK_NUMBER,1,60,100,true},\n  {343,"SOC","Off-grid cut-off SOC",SK_NUMBER,1,3,30,true},\n  {344,"Основные","Reserved 344 — запись заблокирована",SK_NUMBER,1,0,65535,false},', 'soc/344')
    s = replace_one(s,
        '    case 305: case 306: case 307: case 308: case 309: case 310: case 313:\n      return raw <= 1;',
        '    case 305: case 306: case 307: case 308: case 309: case 310: case 313: case 316: case 338:\n      return raw <= 1;', 'select316/338')

    old = '''  float userVal = ((float)raw) / s->scale;
  if (userVal < s->minVal || userVal > s->maxVal) {
    err = F("Value outside allowed range");
    return false;
  }
  return true;
}'''
    new = '''  float userVal = ((float)raw) / s->scale;
  if (userVal < s->minVal || userVal > s->maxVal) {
    err = F("Value outside allowed range");
    return false;
  }

  uint16_t other=0;
  if (reg==332 && getCachedSettingRaw(333,other) && raw < other) {
    err = F("Maximum charge current must be >= mains charge current"); return false;
  }
  if (reg==333 && getCachedSettingRaw(332,other) && raw > other) {
    err = F("Mains charge current must be <= maximum charge current"); return false;
  }
  if (reg==343 && getCachedSettingRaw(341,other) && raw > other) {
    err = F("Off-grid cut-off SOC must be <= mains SOC protection"); return false;
  }

  BatterySystemDetect bd=detectBatterySystem();
  uint16_t J=(bd.systemV==24?2:(bd.systemV==48?4:0));
  if(J){
    uint16_t A=0,B=0,C=0,D=0,E=0,Fv=0;
    getCachedSettingRaw(323,A); getCachedSettingRaw(324,B); getCachedSettingRaw(325,C);
    getCachedSettingRaw(326,D); getCachedSettingRaw(327,E); getCachedSettingRaw(329,Fv);
    if(reg==323 && B && (raw < B+10*J || raw > 165*J)){err=F("Battery OVP violates B/J limits");return false;}
    if(reg==324 && C && A && (raw < C || raw > A-10*J)){err=F("Bulk/CV violates C/A/J limits");return false;}
    if(reg==325 && B && (raw < 120*J || raw > B)){err=F("Float violates J/B limits");return false;}
    if(reg==326 && raw!=0 && B && E && (raw < max((uint16_t)(120*J),E) || raw > B-5*J)){err=F("Mains recovery voltage violates B/E/J limits");return false;}
    if(reg==327 && Fv){
      uint16_t lo=max((uint16_t)(110*J),Fv), hi=(uint16_t)(143*J); if(D) hi=min(hi,D);
      if(raw<lo || raw>hi){err=F("Mains low-voltage point violates D/F/J limits");return false;}
    }
    if(reg==329 && E && (raw < 100*J || raw > min((uint16_t)(135*J),E))){err=F("Off-grid low-voltage point violates E/J limits");return false;}
    if(reg==334 && C && A && (raw < C || raw > A-5*J)){err=F("Equalization voltage violates C/A/J limits");return false;}
  }
  return true;
}'''
    s = replace_one(s, old, new, 'dynamic validation')

    old = '''  uint16_t reg=(uint16_t)regD,raw=(uint16_t)rawD;
  String verr;
  if (!validateRaw(reg,raw,verr)) {'''
    new = '''  uint16_t reg=(uint16_t)regD,raw=(uint16_t)rawD;
  String verr;
  if (reg==316 || reg==338 || reg>=400 || reg==344) {
    sendJson(400,F("{\\"ok\\":false,\\"error\\":\\"This register is blocked in scheduler\\"}")); return;
  }
  if (!validateRaw(reg,raw,verr)) {'''
    s = replace_one(s, old, new, 'scheduler safety')

    FW.write_text(s, encoding='utf-8')


def extract_ui():
    s = FW.read_text(encoding='utf-8')
    m = re.search(r'R"HTML\((.*?)\)HTML"', s, re.S)
    if not m:
        raise SystemExit('embedded HTML not found')
    html = m.group(1)
    scripts = re.findall(r'<script>(.*?)</script>', html, re.S)
    Path('/tmp/ui.html').write_text(html, encoding='utf-8')
    Path('/tmp/embedded.js').write_text('\n'.join(scripts), encoding='utf-8')


def validate_firmware():
    s = FW.read_text(encoding='utf-8')
    required = [
        'const char* FW_VERSION = "0.15.7";',
        'const char* FW_VERSION_PREVIOUS = "0.15.6";',
        'Lithium battery communication abnormal',
        'Battery discharge current exceeds configured limit',
        'inverterWarningUnmaskedMask',
        '{316,"Прочие","Dry contact / Grounding box"',
        '{338,"Прочие","Automatic mains output"',
        'Reserved 344 — запись заблокирована',
        'This register is blocked in scheduler',
    ]
    missing = [x for x in required if x not in s]
    if missing:
        raise SystemExit('missing markers: ' + repr(missing))
    if '{344,"Основные","Макс. отдача в сеть"' in s:
        raise SystemExit('stale writable reg344 remains')
    extract_ui()
    print('firmware validation OK; bytes', FW.stat().st_size)


def patch_readme():
    s = README.read_text(encoding='utf-8')
    s = s.replace('Текущая версия прошивки: **0.15.6**.', 'Текущая версия прошивки: **0.15.7**.')
    s = s.replace('Версия исходной прошивки: **0.14.31**.', 'Текущая версия прошивки: **0.15.7**.')
    start = s.index('## Демонстрация Web UI')
    end = s.index('\n## Возможности', start)
    block = '''## Демонстрация Web UI

Короткое превью актуального интерфейса **v0.15.7**:

<p align="center"><img src="docs/media/ui-v0157-preview.gif" alt="ANENJI ESP32 Web UI v0.15.7 preview" width="900"></p>

Скриншоты сгруппированы по страницам интерфейса. Для каждой страницы оставлены только 2–3 характерных кадра.

### Обзор
<p align="center"><img src="docs/media/ui-v0157-overview-1.png" width="46%"> <img src="docs/media/ui-v0157-overview-2.png" width="46%"></p>

### Батарея
<p align="center"><img src="docs/media/ui-v0157-battery-1.png" width="31%"> <img src="docs/media/ui-v0157-battery-2.png" width="31%"> <img src="docs/media/ui-v0157-battery-3.png" width="31%"></p>

### Настройки
<p align="center"><img src="docs/media/ui-v0157-settings-1.png" width="31%"> <img src="docs/media/ui-v0157-settings-2.png" width="31%"> <img src="docs/media/ui-v0157-settings-3.png" width="31%"></p>

### Планировщик
<p align="center"><img src="docs/media/ui-v0157-scheduler-1.png" width="46%"> <img src="docs/media/ui-v0157-scheduler-2.png" width="46%"></p>

### Сеть
<p align="center"><img src="docs/media/ui-v0157-network-1.png" width="46%"> <img src="docs/media/ui-v0157-network-2.png" width="46%"></p>

### Сервис
<p align="center"><img src="docs/media/ui-v0157-service-1.png" width="46%"> <img src="docs/media/ui-v0157-service-2.png" width="46%"></p>

> Медиа генерируются из Web UI той же версии прошивки. Старые `demo.gif`, `demo.mp4` и прежние разрозненные скриншоты README больше не использует.
'''
    s = s[:start] + block + s[end:]
    marker = '- чтение и изменение разрешённого набора Modbus-регистров;'
    replacement = '''- чтение и изменение разрешённого набора Modbus-регистров с проверкой диапазонов и взаимосвязей;
- полный warning map bit0..20, включая lithium communication и превышение тока разряда;
- отображение masked и unmasked warning-кодов;
- подтверждённые R/W `316` (dry contact) и `338` (automatic mains output);
- регистр `344` помечен reserved и заблокирован для записи;
- опасные `420/425/426/460/461` не доступны через обычную запись и планировщик;'''
    if marker in s:
        s = s.replace(marker, replacement, 1)
    README.write_text(s, encoding='utf-8')


def validate_readme():
    s = README.read_text(encoding='utf-8')
    required = ['Текущая версия прошивки: **0.15.7**.', 'ui-v0157-preview.gif', 'ui-v0157-scheduler-2.png', 'опасные `420/425/426/460/461`']
    missing = [x for x in required if x not in s]
    if missing:
        raise SystemExit('README missing: ' + repr(missing))
    if 'docs/media/demo.gif' in s or 'docs/media/demo.mp4' in s:
        raise SystemExit('stale demo media still referenced')
    pngs = list((ROOT / 'docs/media').glob('ui-v0157-*.png'))
    if len(pngs) != 14:
        raise SystemExit(f'expected 14 screenshots, found {len(pngs)}')
    preview = ROOT / 'docs/media/ui-v0157-preview.gif'
    if not preview.exists() or preview.stat().st_size < 1000:
        raise SystemExit('preview gif missing/empty')
    print('README/media validation OK')


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'patch': patch_firmware()
    elif cmd == 'validate': validate_firmware()
    elif cmd == 'readme': patch_readme()
    elif cmd == 'validate_readme': validate_readme()
    elif cmd == 'extract_ui': extract_ui()
    else: raise SystemExit('usage: promote_v0157.py patch|validate|readme|validate_readme|extract_ui')
