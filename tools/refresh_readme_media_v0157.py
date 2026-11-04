#!/usr/bin/env python3
from pathlib import Path

ROOT = Path('.')
README = ROOT / 'README.md'
MEDIA = ROOT / 'docs/media'

s = README.read_text(encoding='utf-8')
start = s.index('## Демонстрация Web UI')
end = s.index('\n## Возможности', start)
block = '''## Демонстрация Web UI

Короткое превью актуального интерфейса **v0.15.7** — кадры переключаются медленнее, чтобы интерфейс можно было рассмотреть:

<p align="center"><img src="docs/media/ui-v0157-preview.gif" alt="ANENJI ESP32 Web UI v0.15.7 preview" width="900"></p>

Для каждой страницы показаны **2 полных скриншота без обрезки содержимого**: desktop и mobile. Так видна вся страница целиком, а не только случайный участок viewport.

### Обзор
<p align="center"><img src="docs/media/ui-v0157-overview-1.png" width="95%"></p>
<p align="center"><img src="docs/media/ui-v0157-overview-2.png" width="48%"></p>

### Батарея
<p align="center"><img src="docs/media/ui-v0157-battery-1.png" width="95%"></p>
<p align="center"><img src="docs/media/ui-v0157-battery-2.png" width="48%"></p>

### Настройки
<p align="center"><img src="docs/media/ui-v0157-settings-1.png" width="95%"></p>
<p align="center"><img src="docs/media/ui-v0157-settings-2.png" width="48%"></p>

### Планировщик
<p align="center"><img src="docs/media/ui-v0157-scheduler-1.png" width="95%"></p>
<p align="center"><img src="docs/media/ui-v0157-scheduler-2.png" width="48%"></p>

### Сеть
<p align="center"><img src="docs/media/ui-v0157-network-1.png" width="95%"></p>
<p align="center"><img src="docs/media/ui-v0157-network-2.png" width="48%"></p>

### Сервис
<p align="center"><img src="docs/media/ui-v0157-service-1.png" width="95%"></p>
<p align="center"><img src="docs/media/ui-v0157-service-2.png" width="48%"></p>

> Скриншоты генерируются из Web UI той же версии прошивки. README больше не использует старые `demo.gif`, `demo.mp4` и прежние обрезанные кадры.
'''
s = s[:start] + block + s[end:]
README.write_text(s, encoding='utf-8')

pngs = sorted(MEDIA.glob('ui-v0157-*.png'))
if len(pngs) != 12:
    raise SystemExit(f'expected 12 screenshots, found {len(pngs)}')
for page in ['overview','battery','settings','scheduler','network','service']:
    for i in (1,2):
        p = MEDIA / f'ui-v0157-{page}-{i}.png'
        if not p.exists() or p.stat().st_size < 10000:
            raise SystemExit(f'missing/empty screenshot: {p}')
preview = MEDIA / 'ui-v0157-preview.gif'
if not preview.exists() or preview.stat().st_size < 1000:
    raise SystemExit('preview gif missing/empty')
if 'ui-v0157-battery-3.png' in s or 'ui-v0157-settings-3.png' in s:
    raise SystemExit('README still references third screenshots')
print('README media layout OK: 2 full-page screenshots per page')
