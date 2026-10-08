from pathlib import Path
import sys

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')
p = root / 'firmware/ANENJI_ESP32_Controller/ANENJI_ESP32_Controller.ino'
s = p.read_text(encoding='utf-8')

def once(old, new, label):
    global s
    if old not in s:
        raise SystemExit('anchor missing: ' + label)
    s = s.replace(old, new, 1)

once('const char* FW_VERSION = "0.15.1";\nconst char* FW_VERSION_PREVIOUS = "0.15.0";',
     'const char* FW_VERSION = "0.15.2";\nconst char* FW_VERSION_PREVIOUS = "0.15.1";',
     'version')

once('void serviceRtuWatchdog() {\n  // Never run UART recovery/reboot logic while the synchronous OTA HTTP request owns loopTask.\n  if (otaInProgress || otaRestartPending) return;',
     'void serviceRtuWatchdog() {\n  // Never run UART recovery/reboot logic while OTA or datalogger service owns networking.\n  // Datalogger service must keep the temporary AP stable even if inverter RTU is offline.\n  if (otaInProgress || otaRestartPending || dataloggerServiceMode) return;',
     'rtu watchdog service-mode guard')

once('void serviceWifiWatchdog() {\n  if (setupMode || wifiSsid.length() == 0) {',
     'void serviceWifiWatchdog() {\n  // A deliberate datalogger AP session is not a home-Wi-Fi outage.\n  // Do not reboot or advance the Wi-Fi outage timer while service mode is active.\n  if (setupMode || dataloggerServiceMode || wifiSsid.length() == 0) {',
     'wifi watchdog service-mode guard')

once('  } else if (WiFi.status() != WL_CONNECTED && wifiSsid.length()) {',
     '  } else if (!dataloggerServiceMode && WiFi.status() != WL_CONNECTED && wifiSsid.length()) {',
     'loop stored wifi reconnect guard')

once('void handleDataloggerServiceApStart(){\n  if(!adminAuthorized()){sendJson(401,F("{\\"ok\\":false,\\"error\\":\\"Admin authorization required\\"}"));return;}\n',
     'void handleDataloggerServiceApStart(){\n',
     'service ap auth')

once('void handleDataloggerConnect(){\n  if(!adminAuthorized()){sendJson(401,F("{\\"ok\\":false,\\"error\\":\\"Admin authorization required\\"}"));return;}\n',
     'void handleDataloggerConnect(){\n',
     'connect auth')

once('void handleDataloggerRestore(){\n  if(!adminAuthorized()){sendJson(401,F("{\\"ok\\":false,\\"error\\":\\"Admin authorization required\\"}"));return;}\n',
     'void handleDataloggerRestore(){\n',
     'restore auth')

once("async function dlServiceAp(){try{const x=await dlReq('/api/datalogger/service-ap/start',{},true);",
     "async function dlServiceAp(){try{const x=await dlReq('/api/datalogger/service-ap/start');",
     'service ap js auth')

once("async function dlConnectAp(){try{const x=await dlReq('/api/datalogger/connect',{ssid:$('dlApSsid').value,password:$('dlApPass').value},true);",
     "async function dlConnectAp(){try{const x=await dlReq('/api/datalogger/connect',{ssid:$('dlApSsid').value,password:$('dlApPass').value});",
     'connect js auth')

once("async function dlRestore(){try{dlShow(await dlReq('/api/datalogger/restore',{},true));",
     "async function dlRestore(){try{dlShow(await dlReq('/api/datalogger/restore'));",
     'restore js auth')

# Preserve the service AP and pre-arm service mode before deliberately dropping home STA.
once('  WiFi.mode(WIFI_AP_STA);WiFi.disconnect(false,false);delay(100);WiFi.begin(ssid.c_str(),pass.c_str());',
     '  dataloggerServiceMode=true;wifiDisconnectedSinceMs=0;inverterOfflineSinceMs=0;\n  WiFi.mode(WIFI_AP_STA);WiFi.setAutoReconnect(false);WiFi.disconnect(false,false);delay(100);WiFi.begin(ssid.c_str(),pass.c_str());',
     'connect service mode pre-arm')

once('  if(WiFi.status()!=WL_CONNECTED){sendJson(504,F("{\\"ok\\":false,\\"error\\":\\"Could not connect to datalogger AP; service AP remains active\\"}"));return;}\n  dataloggerServiceMode=true;String j=',
     '  if(WiFi.status()!=WL_CONNECTED){dataloggerServiceMode=false;sendJson(504,F("{\\"ok\\":false,\\"error\\":\\"Could not connect to datalogger AP; service AP remains active\\"}"));return;}\n  dataloggerServiceMode=true;String j=',
     'connect failure clear service mode')

once('  dataloggerServiceMode=false;WiFi.disconnect(false,false);delay(100);if(wifiSsid.length())WiFi.begin(wifiSsid.c_str(),wifiPass.c_str());',
     '  dataloggerServiceMode=false;wifiDisconnectedSinceMs=0;inverterOfflineSinceMs=0;WiFi.mode(WIFI_AP_STA);WiFi.setAutoReconnect(false);WiFi.disconnect(false,false);delay(100);if(wifiSsid.length())WiFi.begin(wifiSsid.c_str(),wifiPass.c_str());',
     'restore stability')

p.write_text(s, encoding='utf-8')
print('patched', p, p.stat().st_size)
