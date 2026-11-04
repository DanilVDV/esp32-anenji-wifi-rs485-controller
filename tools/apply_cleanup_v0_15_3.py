from pathlib import Path
import re, sys

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')
p = root / 'firmware/ANENJI_ESP32_Controller/ANENJI_ESP32_Controller.ino'
s = p.read_text(encoding='utf-8')

def once(old, new, label):
    global s
    if old not in s:
        raise SystemExit('anchor missing: ' + label)
    s = s.replace(old, new, 1)

def sub_once(pattern, repl, label, flags=0):
    global s
    s2, n = re.subn(pattern, repl, s, count=1, flags=flags)
    if n != 1:
        raise SystemExit(f'pattern failed ({n}): {label}')
    s = s2

# Version.
once('const char* FW_VERSION = "0.15.2";\nconst char* FW_VERSION_PREVIOUS = "0.15.1";',
     'const char* FW_VERSION = "0.15.3";\nconst char* FW_VERSION_PREVIOUS = "0.15.2";', 'version')

# Remove datalogger-only dependency and declarations.
once('#include <WiFiUdp.h>\n', '', 'WiFiUdp include')
for decl in [
    'void handleDataloggerStatus();\n','void handleDataloggerServiceApStart();\n','void handleDataloggerConnect();\n',
    'void handleDataloggerRestore();\n','void handleDataloggerInfo();\n','void handleDataloggerPing();\n','void handleDataloggerSetParam();\n']:
    if decl not in s: raise SystemExit('missing datalogger declaration: '+decl.strip())
    s=s.replace(decl,'',1)
sub_once(r'\nconst uint16_t DATALOGGER_UDP_PORT = 58899;.*?uint32_t dataloggerLastOkMs = 0;\n', '\n', 'datalogger globals', re.S)

# Revert watchdog/reconnect special cases that only existed for datalogger service mode.
once('  // Never run UART recovery/reboot logic while OTA or datalogger service owns networking.\n  // Datalogger service must keep the temporary AP stable even if inverter RTU is offline.\n  if (otaInProgress || otaRestartPending || dataloggerServiceMode) return;',
     '  // Never run UART recovery/reboot logic while the synchronous OTA HTTP request owns loopTask.\n  if (otaInProgress || otaRestartPending) return;', 'RTU watchdog cleanup')
once('  // A deliberate datalogger AP session is not a home-Wi-Fi outage.\n  // Do not reboot or advance the Wi-Fi outage timer while service mode is active.\n  if (setupMode || dataloggerServiceMode || wifiSsid.length() == 0) {',
     '  if (setupMode || wifiSsid.length() == 0) {', 'WiFi watchdog cleanup')
once('  } else if (!dataloggerServiceMode && WiFi.status() != WL_CONNECTED && wifiSsid.length()) {',
     '  } else if (WiFi.status() != WL_CONNECTED && wifiSsid.length()) {', 'loop reconnect cleanup')

# Remove the complete datalogger implementation.
sub_once(r'\n// ---------------- EyeBond / SmartESS datalogger service ----------------\n.*?\n// ---------------- Network / provisioning API ----------------',
         '\n// ---------------- Network / provisioning API ----------------', 'datalogger implementation', re.S)

# Remove API routes.
sub_once(r'  web\.on\("/api/datalogger/status".*?  web\.on\("/api/datalogger/set", HTTP_POST, handleDataloggerSetParam\);\n',
         '', 'datalogger routes', re.S)

# Remove UI card and JavaScript.
sub_once(r'\n<div class="card"><h2>Datalogger</h2>.*?<pre id="dlOut".*?</pre></div>\n', '\n', 'datalogger card', re.S)
sub_once(r'\nasync function dlReq\(path,body=\{\},admin=false\).*?setTimeout\(dlStatus,1200\);\n', '\n', 'datalogger JavaScript', re.S)
once(" ['Связь','service'],['Datalogger','service'],\n", " ['Связь','service'],\n", 'datalogger tab rule')

# PZEM is still configurable, but it must not occupy the Overview dashboard when it is absent/offline.
once('<svg id="pzemHouseSvg" aria-hidden="true">', '<svg id="pzemHouseSvg" data-pzem-overview aria-hidden="true">', 'pzem svg marker')
once('<div class="flow-node flow-input"><div class="ico">▤</div><small>Ввод · PZEM</small>', '<div id="pzemFlowNode" class="flow-node flow-input" data-pzem-overview><div class="ico">▤</div><small>Ввод · PZEM</small>', 'pzem flow node')
once('<div id="inputDown" class="flow-v"><span></span></div>', '<div id="inputDown" data-pzem-overview class="flow-v"><span></span></div>', 'pzem flow connector')
once('<div class="small" style="margin:-5px 0 12px">Направление видно по движущейся точке и стрелке. Пунктир PZEM → Нагрузка показывает общий сетевой ввод дома; это не мощность выхода ANENJI reg213.</div>',
     '<div id="pzemFlowNote" data-pzem-overview class="small" style="margin:-5px 0 12px">Направление видно по движущейся точке и стрелке. Пунктир PZEM → Нагрузка показывает общий сетевой ввод дома; это не мощность выхода ANENJI reg213.</div>', 'pzem flow note')
once('<div class="info-box"><h3>PZEM-016 · общий ввод</h3><div id="infoPzem">—</div></div>',
     '<div id="infoPzemBox" data-pzem-overview class="info-box"><h3>PZEM-016 · общий ввод</h3><div id="infoPzem">—</div></div>', 'pzem info box')

# Toggle every PZEM element on the main Overview from the actual online state.
old = "  const pz=x.pzem||{}; window.lastPzem=pz; if($('pzV')){"
new = "  const pz=x.pzem||{}; window.lastPzem=pz; const pzemOverviewVisible=!!pz.online; document.querySelectorAll('[data-pzem-overview]').forEach(el=>el.style.display=pzemOverviewVisible?'':'none'); if(!pzemOverviewVisible)hidePzemHouseFlow(); if($('pzV')){"
once(old, new, 'pzem overview visibility')

p.write_text(s, encoding='utf-8')
print('patched', p, p.stat().st_size)
