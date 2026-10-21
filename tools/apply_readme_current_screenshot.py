from pathlib import Path
p=Path('README.md')
s=p.read_text(encoding='utf-8')
s=s.replace('Текущая версия прошивки: **0.15.7**.','Текущая версия прошивки: **0.15.8**.')
start=s.index('## Демонстрация Web UI')
end=s.index('\n## Возможности', start)
block='''## Демонстрация Web UI

Актуальный рабочий интерфейс контроллера:

<p align="center">
  <img src="docs/media/current-overview.webp" alt="ANENJI ESP32 Web UI" width="700">
</p>

> В README оставлен только этот актуальный снимок реального интерфейса. Старые preview/GIF и автоматически сгенерированные галереи больше не используются.
'''
s=s[:start]+block+s[end:]
marker='- полный warning map bit0..20, включая lithium communication и превышение тока разряда;'
extra='- журнал событий с локальной датой и временем после синхронизации RTC/NTP/браузером;'
if extra not in s and marker in s:
    s=s.replace(marker, marker+'\n'+extra, 1)
p.write_text(s,encoding='utf-8')
