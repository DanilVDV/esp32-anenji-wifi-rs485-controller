# Security notes

## Authentication model

Прошивка использует admin password. Для защищённых endpoints браузер/API передаёт:

```http
X-ANENJI-Admin: <password>
```

`adminAuthorized()` выполняет простое сравнение значения заголовка с паролем из NVS.

## Setup AP

- SSID: `ANENJI-SETUP-<MAC suffix>`;
- IP: `192.168.4.1`;
- AP password генерируется один раз и сохраняется в namespace `device_cfg`;
- если admin password ещё не существует, он инициализируется значением Setup AP password.

## Ограничения безопасности

В этой версии нет HTTPS/TLS. Следовательно, HTTP header с admin password не шифруется на уровне приложения.

Рекомендуется:

1. не пробрасывать Web UI/OTA наружу через router/NAT;
2. использовать отдельный IoT VLAN или доверенную LAN;
3. не подключаться к Web UI через публичный/чужой Wi‑Fi;
4. ограничить доступ firewall-ом;
5. выполнять OTA только из доверенного сегмента;
6. перед публикацией репозитория проверить, что в истории git отсутствуют дампы NVS, реальные Wi‑Fi credentials и firmware images с секретами.

## Fail-safe поведения

Если сохранённая домашняя сеть недоступна, прошивка намеренно **не открывает Setup AP автоматически**. Это уменьшает риск неожиданно доступной точки конфигурации. Setup AP запускается при отсутствии сохранённой сети или по явному BOOT action/setup flag.
