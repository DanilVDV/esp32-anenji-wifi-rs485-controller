# Changelog

## 0.14.31

Текущая версия подготовленного проекта. По banner/comment в исходнике включает partial profile apply и совокупность ранее добавленных функций Web OTA, PZEM, EEPROM tariffs, NTP/RTC, battery statistics и RTU watchdog.

## Из явно задокументированных комментариев исходника

- **0.14.24** — OTA stabilization: RTU/system watchdog services остаются paused в post-upload reboot window.
- **0.14.15** — compile fix: forward declaration для `pzemTariffSave()`.
- **0.14.14** — protected PZEM-016 accumulated-energy reset (`0x42`) через RTU worker; T1/T2 сохраняются.

Полной истории версий в предоставленном исходнике нет, поэтому более ранние изменения здесь не реконструируются.
