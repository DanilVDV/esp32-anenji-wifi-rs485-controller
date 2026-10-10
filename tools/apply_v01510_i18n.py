from pathlib import Path

p=Path('firmware/ANENJI_ESP32_Controller/ANENJI_ESP32_Controller.ino')
s=p.read_text(encoding='utf-8')
s=s.replace('const char* FW_VERSION = "0.15.9";\nconst char* FW_VERSION_PREVIOUS = "0.15.8";', 'const char* FW_VERSION = "0.15.10";\nconst char* FW_VERSION_PREVIOUS = "0.15.9";',1)
marker="];\nconst UI_LOOKUP=new Map();"
extra=r'''];
// Supplemental translations cover dynamic cards, statuses, dialogs and service text.
UI_TEXT.push(
 ['Обзор','Overview'],['Статистика аккумулятора','Battery statistics'],['Сервис','Service'],['Планировщик','Scheduler'],
 ['Состояние / аварии','Status / alarms'],['Аварии','Faults'],['Предупреждения','Warnings'],['События','Events'],
 ['Нет активных аварий','No active faults'],['Нет активных предупреждений','No active warnings'],['Нет событий','No events'],
 ['Онлайн','Online'],['Офлайн','Offline'],['Подключено','Connected'],['Не подключено','Disconnected'],['Неизвестно','Unknown'],
 ['Активно','Active'],['Неактивно','Inactive'],['Включено','Enabled'],['Выключено','Disabled'],['Готово','Ready'],
 ['Сохранить','Save'],['Отмена','Cancel'],['Закрыть','Close'],['Удалить','Delete'],['Добавить','Add'],['Изменить','Edit'],
 ['Обновить','Refresh'],['Сбросить','Reset'],['Перезагрузить','Reboot'],['Подтвердить','Confirm'],['Ошибка','Error'],['Успешно','Success'],
 ['Текущее значение','Current value'],['Новое значение','New value'],['Значение','Value'],['Состояние','Status'],['Последнее обновление','Last update'],
 ['Приоритет выхода','Output priority'],['Приоритет зарядки','Charge priority'],['Тип батареи','Battery type'],['Ток зарядки','Charge current'],
 ['Напряжение','Voltage'],['Ток','Current'],['Мощность','Power'],['Частота','Frequency'],['Температура','Temperature'],['Уровень заряда','State of charge'],
 ['Заряд','Charge'],['Разряд','Discharge'],['Заряжается','Charging'],['Разряжается','Discharging'],['Ожидание','Idle'],
 ['Сегодня','Today'],['Всего','Total'],['Максимум','Maximum'],['Минимум','Minimum'],['Среднее','Average'],
 ['Калибровка','Calibration'],['Калибровать ноль','Calibrate zero'],['Калибровать по току','Calibrate with reference current'],
 ['Эталонный ток','Reference current'],['Чувствительность','Sensitivity'],['Инвертировать направление','Invert direction'],
 ['Адрес ADS1115','ADS1115 address'],['Канал','Channel'],['Авто','Auto'],['Датчик тока','Current sensor'],
 ['Тариф','Tariff'],['Тариф 1','Tariff 1'],['Тариф 2','Tariff 2'],['Энергия','Energy'],['Стоимость','Cost'],
 ['Синхронизация времени','Time synchronization'],['Источник времени','Time source'],['Часовой пояс','Time zone'],
 ['Понедельник','Monday'],['Вторник','Tuesday'],['Среда','Wednesday'],['Четверг','Thursday'],['Пятница','Friday'],['Суббота','Saturday'],['Воскресенье','Sunday'],
 ['Пн','Mon'],['Вт','Tue'],['Ср','Wed'],['Чт','Thu'],['Пт','Fri'],['Сб','Sat'],['Вс','Sun'],
 ['Адрес','Address'],['Количество','Count'],['Результат','Result'],['Запись','Write'],['Чтение','Read'],['Доступна запись','Write enabled'],
 ['Администратор','Administrator'],['Пароль администратора','Admin password'],['Разблокировать','Unlock'],['Заблокировано','Locked'],
 ['Обновление прошивки','Firmware update'],['Файл прошивки','Firmware file'],['Загрузить прошивку','Upload firmware'],['Версия прошивки','Firmware version'],
 ['Перезагрузка','Reboot'],['Сброс Wi‑Fi','Reset Wi‑Fi'],['Настройки сети','Network settings'],['Статический IP','Static IP'],['Шлюз','Gateway'],['Маска','Subnet mask'],['DNS сервер','DNS server'],
 ['Проверяю соединение…','Checking connection…'],['Данные на странице могут быть устаревшими. Интерфейс не заблокирован.','Page data may be stale. The interface remains available.'],
 ['⚠ НЕТ СВЯЗИ С ESP32','⚠ ESP32 CONNECTION LOST'],['Modbus RTU gateway отключён','Modbus RTU gateway disabled'],
 ['Применение профиля','Applying profile'],['Профиль применён','Profile applied'],['Частично применён','Partially applied'],
 ['Операция выполняется','Operation in progress'],['Операция завершена','Operation completed'],['Таймаут','Timeout'],['Нет данных','No data']
);
const UI_LOOKUP=new Map();'''
if marker not in s: raise SystemExit('UI marker not found')
s=s.replace(marker,extra,1)
old='''function trPair(text){const i=UI_LOOKUP.get(text);return i===undefined?text:UI_TEXT[i][currentLang==='ru'?0:1]}
function translateStatic(root=document.body){
 const w=document.createTreeWalker(root,NodeFilter.SHOW_TEXT);let n;
 while((n=w.nextNode())){const raw=n.nodeValue,trim=raw.trim();if(!trim)continue;const t=trPair(trim);if(t!==trim)n.nodeValue=raw.replace(trim,t)}
}'''
new=r'''function trPair(text){const i=UI_LOOKUP.get(text);return i===undefined?text:UI_TEXT[i][currentLang==='ru'?0:1]}
const UI_LOOSE=[
 ['Нет связи с ESP32','ESP32 connection lost'],['нет связи с ESP32','ESP32 connection lost'],['Связь потеряна','Connection lost'],['Связь восстановлена','Connection restored'],['Последний ответ','Last response'],
 ['Ошибка чтения','Read error'],['Ошибка записи','Write error'],['Ошибка Modbus','Modbus error'],['Ошибка сети','Network error'],['Не удалось','Failed to'],['Сохранено','Saved'],['Сохранение','Saving'],['Загрузка','Loading'],['Обновление','Updating'],
 ['Запись регистра','Register write'],['Чтение регистра','Register read'],['Регистр','Register'],['активных аварий','active faults'],['активных предупреждений','active warnings'],['предупреждение','warning'],['авария','fault'],
 ['Температура','Temperature'],['Напряжение','Voltage'],['Мощность','Power'],['Частота','Frequency'],['Батарея','Battery'],['Нагрузка','Load'],['Инвертор','Inverter'],['Сеть','Grid'],
 ['зарядки','charging'],['разряда','discharge'],['заряд','charge'],['разряд','discharge'],['секунд назад','seconds ago'],['сек назад','sec ago'],['минут назад','minutes ago'],['мин назад','min ago'],['часов назад','hours ago'],
 ['доступен','available'],['недоступен','unavailable'],['подключён','connected'],['подключен','connected'],['отключён','disabled'],['отключен','disabled']
];
function trLoose(text){
 const exact=trPair(text); if(exact!==text)return exact;
 const pairs=UI_LOOSE.slice().sort((a,b)=>(currentLang==='ru'?b[1].length-a[1].length:b[0].length-a[0].length));
 let out=text; for(const p of pairs)out=out.split(currentLang==='ru'?p[1]:p[0]).join(currentLang==='ru'?p[0]:p[1]); return out;
}
function translateStatic(root=document.body){
 if(!root)return;
 const translateNode=node=>{if(node.nodeType===Node.TEXT_NODE){const raw=node.nodeValue,trim=raw.trim();if(!trim)return;const t=trLoose(trim);if(t!==trim)node.nodeValue=raw.replace(trim,t)}};
 if(root.nodeType===Node.TEXT_NODE){translateNode(root);return}
 const w=document.createTreeWalker(root,NodeFilter.SHOW_TEXT);let n;while((n=w.nextNode()))translateNode(n);
 const els=(root.querySelectorAll?root.querySelectorAll('[placeholder],[title],[aria-label],input[type=button],input[type=submit]'):[]);
 els.forEach(el=>{['placeholder','title','aria-label'].forEach(a=>{const v=el.getAttribute(a);if(v){const t=trLoose(v);if(t!==v)el.setAttribute(a,t)}});if(el.tagName==='INPUT'&&el.value){const t=trLoose(el.value);if(t!==el.value)el.value=t}})
}
let i18nObserver=null;
function startI18nObserver(){
 if(i18nObserver||!document.body)return;
 i18nObserver=new MutationObserver(ms=>{for(const m of ms){if(m.type==='characterData')translateStatic(m.target);for(const n of m.addedNodes)translateStatic(n)}});
 i18nObserver.observe(document.body,{subtree:true,childList:true,characterData:true});
}'''
if old not in s: raise SystemExit('translation functions not found')
s=s.replace(old,new,1)
needle="document.documentElement.lang=currentLang;\n translateStatic();"
if needle not in s: raise SystemExit('setLang marker not found')
s=s.replace(needle,"document.documentElement.lang=currentLang;\n translateStatic();\n startI18nObserver();",1)
p.write_text(s,encoding='utf-8')
