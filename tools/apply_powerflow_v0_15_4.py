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


# Firmware version.
once('const char* FW_VERSION = "0.15.3";\nconst char* FW_VERSION_PREVIOUS = "0.15.2";',
     'const char* FW_VERSION = "0.15.4";\nconst char* FW_VERSION_PREVIOUS = "0.15.3";', 'version')

# Dedicated EyeBond-style power-flow tab. This is browser-side only: it reuses
# /api/status data and does not add another HTTP server or another RTU poller.
pf_css = r'''

/* EyeBond-inspired dedicated Power Flow tab. Browser-side only. */
.pf-card{overflow:hidden;background:radial-gradient(circle at 50% 43%,#12324a 0,#0b1728 34%,#0b1220 72%);border-color:#1e3a5f}
.pf-head{display:flex;justify-content:space-between;align-items:center;gap:10px;margin-bottom:6px}.pf-head h2{margin:0}.pf-live{font-size:12px;color:#93c5fd}
.pf-board{--pf-line:#38bdf8;display:grid;grid-template-columns:minmax(105px,1fr) 72px minmax(120px,1.1fr) 72px minmax(105px,1fr);grid-template-rows:minmax(108px,auto) 62px minmax(108px,auto);align-items:center;justify-items:center;gap:5px;min-height:390px;padding:16px 8px 8px;position:relative}
.pf-node{width:min(150px,100%);min-height:92px;border:1px solid #334155;border-radius:22px;background:linear-gradient(145deg,#111c2e,#0b1322);box-shadow:0 12px 30px #0006,inset 0 0 28px #0ea5e915;display:flex;flex-direction:column;align-items:center;justify-content:center;text-align:center;padding:9px;position:relative;z-index:2}
.pf-node .pf-ico{font-size:29px;line-height:1;margin-bottom:5px;filter:drop-shadow(0 0 8px #38bdf888)}.pf-node small{color:#94a3b8}.pf-node b{font-size:20px;margin:3px 0}.pf-node .pf-sub{font-size:11px;color:#94a3b8;line-height:1.25}
.pf-node.pf-core{border-color:#0ea5e9;box-shadow:0 0 28px #0284c744,inset 0 0 32px #0ea5e91c}.pf-node.pf-home{border-color:#a855f7}.pf-node.pf-battery{border-color:#22c55e}.pf-node.pf-grid{border-color:#f59e0b}.pf-node.pf-pv{border-color:#06b6d4}
.pf-pv{grid-column:3;grid-row:1}.pf-v-top{grid-column:3;grid-row:2}.pf-grid{grid-column:1;grid-row:3}.pf-h-left{grid-column:2;grid-row:3}.pf-core{grid-column:3;grid-row:3}.pf-h-right{grid-column:4;grid-row:3}.pf-home{grid-column:5;grid-row:3}.pf-v-bottom{grid-column:3;grid-row:4}.pf-battery{grid-column:3;grid-row:5}
.pf-board{grid-template-rows:minmax(108px,auto) 58px minmax(108px,auto) 58px minmax(108px,auto)}
.pf-line{position:relative;opacity:.28;transition:opacity .25s,filter .25s}.pf-line::before{content:"";position:absolute;border-radius:99px;background:#334155}.pf-line::after{content:"";position:absolute;opacity:0}
.pf-line.pf-h{width:100%;height:20px}.pf-line.pf-h::before{left:0;right:0;top:9px;height:3px}.pf-line.pf-v{width:20px;height:100%}.pf-line.pf-v::before{top:0;bottom:0;left:9px;width:3px}
.pf-line.active{opacity:1;filter:drop-shadow(0 0 7px var(--pf-line))}.pf-line.active::before{background:var(--pf-line)}
.pf-line.active::after{opacity:1;width:9px;height:9px;border-radius:50%;background:#fff;box-shadow:0 0 12px 3px var(--pf-line)}
.pf-line.pf-h.active::after{top:6px;animation:pfMoveH 1.5s linear infinite}.pf-line.pf-v.active::after{left:6px;animation:pfMoveV 1.5s linear infinite}
.pf-line.reverse.pf-h.active::after{animation-direction:reverse}.pf-line.reverse.pf-v.active::after{animation-direction:reverse}
@keyframes pfMoveH{from{left:0}to{left:calc(100% - 9px)}}@keyframes pfMoveV{from{top:0}to{top:calc(100% - 9px)}}
.pf-summary{display:grid;grid-template-columns:repeat(4,minmax(120px,1fr));gap:8px;margin-top:10px}.pf-stat{background:#0b1322;border:1px solid #263548;border-radius:12px;padding:10px}.pf-stat small{display:block;color:#94a3b8}.pf-stat b{display:block;margin-top:4px;font-size:16px}
.pf-hidden{display:none!important}.pf-offline .pf-node{opacity:.48}.pf-offline .pf-line{opacity:.12}
@media(max-width:720px){.pf-board{grid-template-columns:minmax(92px,1fr) 42px minmax(108px,1.15fr) 42px minmax(92px,1fr);padding-left:0;padding-right:0}.pf-node{min-height:82px;border-radius:18px;padding:7px}.pf-node b{font-size:16px}.pf-node .pf-ico{font-size:24px}.pf-summary{grid-template-columns:repeat(2,1fr)}}
@media(max-width:500px){.pf-board{grid-template-columns:1fr 30px 1.08fr 30px 1fr;min-height:350px}.pf-node{min-height:76px}.pf-node .pf-sub{display:none}.pf-node small{font-size:10px}.pf-node b{font-size:14px}}
'''
once('</style></head><body><div class="wrap">', pf_css + '\n</style></head><body><div class="wrap">', 'powerflow CSS')

# Add a first-class application tab.
once(' <button class="app-tab active" data-page="overview" onclick="showTab(\'overview\')">Обзор</button>\n <button class="app-tab" data-page="battery"',
     ' <button class="app-tab active" data-page="overview" onclick="showTab(\'overview\')">Обзор</button>\n <button class="app-tab" data-page="power" onclick="showTab(\'power\')">Энергия</button>\n <button class="app-tab" data-page="battery"', 'power tab button')

pf_card = r'''

<div class="card pf-card"><div class="pf-head"><h2>Power Flow</h2><span id="pfLive" class="pf-live">—</span></div>
 <div id="pfBoard" class="pf-board pf-offline">
  <div id="pfPvNode" class="pf-node pf-pv"><div class="pf-ico">☀️</div><small>PV</small><b id="pfPv">— W</b><div id="pfPvSub" class="pf-sub">—</div></div>
  <div id="pfPvLine" class="pf-line pf-v pf-v-top"></div>
  <div id="pfGridNode" class="pf-node pf-grid"><div class="pf-ico">⚡</div><small>Сеть</small><b id="pfGrid">— W</b><div id="pfGridSub" class="pf-sub">—</div></div>
  <div id="pfGridLine" class="pf-line pf-h pf-h-left"></div>
  <div class="pf-node pf-core"><div class="pf-ico">▣</div><small>ANENJI</small><b id="pfInv">— W</b><div id="pfInvSub" class="pf-sub">—</div></div>
  <div id="pfHomeLine" class="pf-line pf-h pf-h-right"></div>
  <div class="pf-node pf-home"><div class="pf-ico">🏠</div><small>Дом / нагрузка</small><b id="pfHome">— W</b><div id="pfHomeSub" class="pf-sub">—</div></div>
  <div id="pfBattLine" class="pf-line pf-v pf-v-bottom"></div>
  <div class="pf-node pf-battery"><div class="pf-ico">🔋</div><small>Батарея</small><b id="pfBatt">— %</b><div id="pfBattSub" class="pf-sub">—</div></div>
 </div>
 <div class="pf-summary">
  <div class="pf-stat"><small>PV сейчас</small><b id="pfStatPv">—</b></div>
  <div class="pf-stat"><small>Нагрузка</small><b id="pfStatLoad">—</b></div>
  <div class="pf-stat"><small>Батарея</small><b id="pfStatBatt">—</b></div>
  <div id="pfStatGridBox" class="pf-stat"><small>Общий ввод · PZEM</small><b id="pfStatGrid">—</b></div>
 </div>
 <div class="small" style="margin-top:10px">Анимация показывает направление потока. Узел сети автоматически скрывается, если PZEM отсутствует или не отвечает.</div>
</div>
'''
once('\n<div class="card"><h2>Полная телеметрия</h2>', pf_card + '\n<div class="card"><h2>Полная телеметрия</h2>', 'powerflow card')

# Route the card to its dedicated page and create that page.
once(" ['Состояние / аварии','overview'],['Энергопотоки','overview'],['Полная телеметрия','overview'],",
     " ['Состояние / аварии','overview'],['Энергопотоки','overview'],['Power Flow','power'],['Полная телеметрия','overview'],", 'powerflow tab rule')
once(" for(const id of ['overview','battery','settings','scheduler','network','service']){",
     " for(const id of ['overview','power','battery','settings','scheduler','network','service']){", 'power page host')

# EyeBond-style view updater. Grid visibility is based on actual PZEM online state.
pf_js = r'''

function renderPowerFlowCard(t){
 const board=$('pfBoard'); if(!board)return;
 const online=!!(window.lastStatusPacket&&window.lastStatusPacket.inverter_online);
 board.classList.toggle('pf-offline',!online);
 const pv=Math.max(0,Number(t.pv_power||0));
 const load=Math.max(0,Number(t.output_active_power||0));
 const inv=Number(t.inverter_power||0);
 const bv=Number(t.battery_voltage||0),bc=Number(t.net_battery_current??t.battery_current??0),soc=Number(t.battery_soc||0);
 const battRaw=Number(t.battery_power),battW=Number.isFinite(battRaw)&&Math.abs(battRaw)>0.5?Math.abs(battRaw):Math.abs(bv*bc);
 const pz=window.lastPzem||{},gridOnline=!!pz.online&&Number.isFinite(Number(pz.power)),gridW=gridOnline?Number(pz.power):0;
 const gridNode=$('pfGridNode'),gridLine=$('pfGridLine'),gridStat=$('pfStatGridBox');
 [gridNode,gridLine,gridStat].forEach(el=>{if(el)el.classList.toggle('pf-hidden',!gridOnline)});
 $('pfPv').textContent=f(pv,0)+' W'; $('pfPvSub').textContent=`${f(t.pv_voltage,1)} V · ${f(t.pv_current,1)} A`;
 $('pfGrid').textContent=gridOnline?f(Math.abs(gridW),0)+' W':'— W'; $('pfGridSub').textContent=gridOnline?`${f(pz.voltage,1)} V · ${f(pz.current,2)} A`:'—';
 $('pfInv').textContent=f(inv,0)+' W'; $('pfInvSub').textContent=`${f(t.inverter_voltage,1)} V · ${f(t.inverter_frequency,2)} Hz`;
 $('pfHome').textContent=f(load,0)+' W'; $('pfHomeSub').textContent=`${f(t.load_percent,0)} % · ${f(t.output_current,1)} A`;
 $('pfBatt').textContent=f(soc,0)+' %'; $('pfBattSub').textContent=`${f(bv,1)} V · ${f(battW,0)} W · ${bc>0.2?'заряд':(bc<-.2?'разряд':'ожидание')}`;
 const setLine=(id,on,reverse=false)=>{const el=$(id);if(!el)return;el.classList.toggle('active',!!on);el.classList.toggle('reverse',!!reverse)};
 setLine('pfPvLine',pv>5,false);
 setLine('pfHomeLine',load>5,false);
 setLine('pfBattLine',Math.abs(bc)>0.2,bc<-.2);
 setLine('pfGridLine',gridOnline&&Math.abs(gridW)>5,gridW<0);
 $('pfStatPv').textContent=f(pv,0)+' W'; $('pfStatLoad').textContent=f(load,0)+' W'; $('pfStatBatt').textContent=`${f(soc,0)} % · ${f(battW,0)} W`;
 if($('pfStatGrid'))$('pfStatGrid').textContent=gridOnline?f(gridW,0)+' W':'—';
 if($('pfLive'))$('pfLive').textContent=online?(currentLang==='ru'?'● LIVE · '+(window.lastStatusPacket.age_ms||0)+' ms':'● LIVE · '+(window.lastStatusPacket.age_ms||0)+' ms'):(currentLang==='ru'?'● инвертор offline':'● inverter offline');
}
'''
once('\nfunction renderEnergy(t){', pf_js + '\nfunction renderEnergy(t){', 'powerflow renderer')
once(" $('infoInv').innerHTML=kv(L?'Выход в нагрузку':'Load output',f(loadOut,0)+' W')+kv(L?'Мощность инвертора · reg208':'Inverter power · reg208',f(ip,0)+' W')+kv(L?'Ток силового тракта':'Power-stage current',f(t.inverter_current)+' A')+kv(L?'Напряжение выхода':'Output voltage',f(t.output_voltage)+' V')+kv(L?'Частота выхода':'Output frequency',f(t.output_frequency,2)+' Hz')+kv(L?'Полная мощность нагрузки':'Load apparent power',f(t.output_apparent_power,0)+' VA')+kv('DCDC temp',f(t.dcdc_temperature,0)+' °C')+kv('Inverter temp',f(t.inverter_temperature,0)+' °C');\n}",
     " $('infoInv').innerHTML=kv(L?'Выход в нагрузку':'Load output',f(loadOut,0)+' W')+kv(L?'Мощность инвертора · reg208':'Inverter power · reg208',f(ip,0)+' W')+kv(L?'Ток силового тракта':'Power-stage current',f(t.inverter_current)+' A')+kv(L?'Напряжение выхода':'Output voltage',f(t.output_voltage)+' V')+kv(L?'Частота выхода':'Output frequency',f(t.output_frequency,2)+' Hz')+kv(L?'Полная мощность нагрузки':'Load apparent power',f(t.output_apparent_power,0)+' VA')+kv('DCDC temp',f(t.dcdc_temperature,0)+' °C')+kv('Inverter temp',f(t.inverter_temperature,0)+' °C');\n renderPowerFlowCard(t);\n}", 'powerflow render hook')

p.write_text(s, encoding='utf-8')
print('patched', p, p.stat().st_size)
