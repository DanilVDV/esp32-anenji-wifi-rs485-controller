const { chromium } = require('playwright-core');
const { execFileSync } = require('child_process');
const fs = require('fs');

function chromePath() {
  for (const p of ['/usr/bin/google-chrome','/usr/bin/chromium','/usr/bin/chromium-browser']) {
    if (fs.existsSync(p)) return p;
  }
  throw new Error('Chrome/Chromium executable not found');
}

async function switchPage(page, id) {
  await page.evaluate(id => {
    const b = document.querySelector(`.app-tab[data-page="${id}"]`);
    if (!b) throw new Error(`tab not found: ${id}`);
    b.click();
    window.scrollTo(0,0);
  }, id);
  await page.waitForTimeout(350);
}

(async () => {
  const browser = await chromium.launch({headless:true, executablePath:chromePath(), args:['--no-sandbox']});
  const page = await browser.newPage({viewport:{width:1440,height:900}, deviceScaleFactor:1});

  await page.route('**/api/**', async route => {
    const url = route.request().url();
    let body = {ok:true};
    if (url.includes('/api/status')) {
      body = {
        ok:true, firmware_version:'0.15.7', wifi_online:true, inverter_online:true,
        rssi:-58, uptime_s:123456, modbus_slave:38, write_enabled:true,
        alerts:{valid:true,fault_raw:0,warning_raw:0,warning_unmasked_raw:0,operation_mode:3,operation_mode_text:'Off-Grid',faults:[],warnings:[],warnings_unmasked:[],event_count:0},
        pzem:{enabled:true,online:true,address:1,voltage:231.4,current:0.21,power:24.8,energy_kwh:190.994,frequency:49.9,pf:0.50,t1_kwh:2905.181,t2_kwh:1104.418,meter_total_kwh:4009.599,tariff:1,tariff_saved:true,poll_ok:100,poll_errors:0},
        telemetry:{mode:3,grid_voltage:0,grid_frequency:0,grid_power:0,inverter_voltage:230.2,inverter_current:2.4,inverter_frequency:50,inverter_power:552,inverter_charging_power:0,output_voltage:230.2,output_current:2.4,output_frequency:50,output_active_power:552,output_apparent_power:580,battery_voltage:26.3,battery_current:-25.8,battery_power:-678,pv_voltage:59.8,pv_current:9.3,pv_power:558,pv_charging_power:552,load_percent:17,dcdc_temperature:29,inverter_temperature:33,battery_soc:100,net_battery_current:-25.8,inverter_charge_current:0,pv_charge_current:9.3,power_flow_status:85}
      };
    } else if (url.includes('/api/settings')) {
      body = {ok:true, values:[]};
    } else if (url.includes('/api/schedule')) {
      body = {ok:true, tasks:[]};
    } else if (url.includes('/api/rtc')) {
      body = {ok:true,present:false,valid:true,type:'software',source:'ntp',time:'2026-10-08 12:34:56',ntp_server:'pool.ntp.org',utc_offset_min:180,scheduler_runs:12,scheduler_errors:0};
    } else if (url.includes('/api/battery/stats')) {
      body = {ok:true,persistent:true,charged_kwh:28.45,discharged_kwh:26.91,charged_ah:1088,discharged_ah:1031,efc:84.2,estimated_capacity_ah:280,days:31};
    } else if (url.includes('/api/network')) {
      body = {ok:true,ssid:'Home-WiFi',ip:'192.168.88.158',gateway:'192.168.88.1',mask:'255.255.255.0',dns:'192.168.88.1',static:true};
    } else if (url.includes('/api/modbus')) {
      body = {ok:true,slave:38,baud:9600,format:'8N1'};
    }
    await route.fulfill({status:200,contentType:'application/json',body:JSON.stringify(body)});
  });

  await page.goto('file:///tmp/ui.html');
  await page.waitForTimeout(1400);

  const pageDefs = [
    ['overview', [[1440,900],[430,900]]],
    ['battery', [[1440,900],[768,900],[430,900]]],
    ['settings', [[1440,900],[768,900],[430,900]]],
    ['scheduler', [[1440,900],[430,900]]],
    ['network', [[1440,900],[430,900]]],
    ['service', [[1440,900],[430,900]]]
  ];

  for (const [id, views] of pageDefs) {
    for (let i=0;i<views.length;i++) {
      const [width,height] = views[i];
      await page.setViewportSize({width,height});
      await switchPage(page,id);
      await page.screenshot({path:`docs/media/ui-v0157-${id}-${i+1}.png`, fullPage:false});
    }
  }
  await browser.close();

  const shots = [];
  for (const id of ['overview','battery','settings','scheduler','network','service']) {
    shots.push(`docs/media/ui-v0157-${id}-1.png`);
  }
  execFileSync('convert', ['-delay','85','-loop','0', ...shots, '-resize','900x', 'docs/media/ui-v0157-preview.gif'], {stdio:'inherit'});

  // Fail if a page gallery accidentally contains byte-identical screenshots.
  const crypto = require('crypto');
  for (const [id, views] of pageDefs) {
    const hashes = views.map((_,i)=>crypto.createHash('sha256').update(fs.readFileSync(`docs/media/ui-v0157-${id}-${i+1}.png`)).digest('hex'));
    if (new Set(hashes).size !== hashes.length) throw new Error(`duplicate screenshots for ${id}`);
  }
  console.log('Rendered 14 distinct responsive screenshots and short preview');
})().catch(e => { console.error(e); process.exit(1); });
