# REST API

API обслуживается встроенным HTTP server на порту 80. Ответы JSON. Изменяющие/чувствительные операции требуют admin header там, где это проверяется handler-ом.

## Status / telemetry

| Method | Endpoint | Назначение |
|---|---|---|
| GET | `/api/status` | состояние системы, telemetry/cache/diagnostics |
| GET | `/api/refresh` | запрос обновления данных |
| GET | `/api/settings` | текущие settings из кэша |
| GET | `/api/profiles` | доступные battery profiles и compatibility |

## Raw Modbus

| Method | Endpoint | Назначение |
|---|---|---|
| GET | `/api/raw` | enqueue raw register read |
| GET | `/api/raw/status` | статус raw job |

Raw-операция ограничена внутренней реализацией; RTU job содержит буфер максимум 20 значений.

## Writes

| Method | Endpoint | Назначение |
|---|---|---|
| POST | `/api/write` | enqueue записи allow-listed регистра |
| GET | `/api/write/status` | результат записи/verify |
| POST | `/api/profile/apply` | применение профиля |
| GET | `/api/profile/status` | результат profile job, включая partial failures |

`ALLOW_WEB_WRITES` в исходнике установлен в `true`.

## Network / auth

| Method | Endpoint |
|---|---|
| GET | `/api/network` |
| POST | `/api/network` |
| POST | `/api/auth/check` |
| GET | `/api/wifi/scan` |
| POST | `/api/setup/enter` |
| POST | `/api/wifi/forget` |
| GET | `/api/modbus/config` |
| POST | `/api/modbus/config` |

## Battery statistics / calibration

- `GET /api/battery/stats`
- `POST /api/battery/calibration/start`
- `POST /api/battery/calibration/cancel`
- `POST /api/battery/calibration/finish`
- `POST /api/battery/calibration/manual`
- `POST /api/battery/stats/reset`

## RTC / NTP / schedule

- `GET /api/rtc`
- `POST /api/rtc/set`
- `POST /api/rtc/ntp`
- `POST /api/rtc/ntp/config`
- `GET /api/schedule`
- `POST /api/schedule`

## PZEM

- `POST /api/pzem/config`
- `POST /api/pzem/tariff`
- `POST /api/pzem/energy/reset`

PZEM energy reset требует защитное confirmation value `RESET_PZEM`. Команда `0x42` сбрасывает внутренний accumulated-energy counter PZEM; виртуальные T1/T2 затем переякориваются и сохраняются.

## OTA

- `GET /api/ota/status`
- `POST /api/ota/unlock`
- `POST /api/ota/update`

OTA использует ESP32 `Update` API и после успешной записи планирует reboot.

## Пример curl

```bash
curl http://DEVICE_IP/api/status
```

Для admin endpoint:

```bash
curl -X POST \
  -H 'Content-Type: application/json' \
  -H 'X-ANENJI-Admin: YOUR_PASSWORD' \
  http://DEVICE_IP/api/auth/check \
  -d '{}'
```

> Точные JSON поля запросов следует сверять с соответствующими handler-функциями в `.ino`; этот документ намеренно не придумывает контракт для полей, которые не были отдельно вынесены из исходника.
