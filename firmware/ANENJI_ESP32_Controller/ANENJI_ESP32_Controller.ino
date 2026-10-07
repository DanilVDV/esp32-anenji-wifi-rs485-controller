/*
  ESP32_ANENJI_WiFi_RS485_v0_14_12_eeprom_pzem_load_flow.ino

  Native ESP32 build for ANENJI ANJ-4000W-24V-WIFI.

  v0.14.24: OTA stabilization: keep RTU/system watchdog services paused through the post-upload reboot window.
  v0.14.15: compile fix: forward declaration for pzemTariffSave() before PZEM reset helper.
  v0.14.14: protected PZEM-016 accumulated-energy reset (0x42) via RTU worker; T1/T2 preserved.

  Features:
    - Wi-Fi STA only
    - HTTP Web UI on port 80
    - Periodic ANENJI telemetry
    - Settings read/write
    - Battery profiles 8S 1P / 8S 2P
    - Raw register explorer
    - No BMS polling
    - DS1307 RTC on I2C + persistent task scheduler
    - AT24C32 wear-levelled battery statistics, daily history and capacity calibration
    - No Ethernet

  Target: classic ESP32-WROOM-32 / "ESP32 Dev Module"

  RS-485:
    RO       -> GPIO16 (RX2)
    DI       -> GPIO17 (TX2)
    DE + /RE -> GPIO4
    GND      -> GND
    A/B      -> inverter RS-485

  NOTE:
    Web writes are enabled by ALLOW_WEB_WRITES below.
    HTTPS is intentionally not enabled in this v0.2 build: first verify
    native ESP32 reads/writes/profile/network configuration over the proven RS-485 link.
    Modbus RTU gateway is intentionally removed.
    v0.12.0: a single dedicated FreeRTOS RTU worker is the ONLY task allowed to touch UART2/RS-485.
    HTTP handlers only read RAM caches or enqueue work; telemetry, settings refresh, raw reads,
    manual writes, profiles and scheduler writes are serialized by that worker. This prevents
    slow/noisy Modbus transactions from blocking the synchronous WebServer loop.
*/

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <Wire.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ---------------- Arduino preprocessor-safe type declarations ----------------
enum SettingKind : uint8_t { SK_NUMBER, SK_SELECT };

struct SettingDef {
  uint16_t reg;
  const char* group;
  const char* label;
  SettingKind kind;
  float scale;
  float minVal;
  float maxVal;
  bool writable;
};

struct ProfileReg {
  uint16_t reg;
  uint16_t raw;
};

struct BatterySystemDetect {
  uint8_t systemV;       // 24, 48 or 0=unknown
  bool verified;         // independent clues agree
  float batteryV;
  float bulkV;
  float floatV;
  String reason;
};


struct RtcDateTime {
  uint16_t year;
  uint8_t month, day, hour, minute, second;
  bool valid;
};

struct ScheduleTask {
  bool enabled;
  uint8_t daysMask;   // bit0=Mon ... bit6=Sun
  uint8_t hour;
  uint8_t minute;
  uint16_t reg;
  uint16_t raw;
};

struct WebWriteJob {
  uint32_t id;
  uint16_t reg;
  uint16_t raw;
};

struct WebWriteResult {
  uint32_t id;
  bool done;
  bool ok;
  bool verified;
  uint16_t reg;
  uint16_t oldRaw;
  uint16_t newRaw;
  char error[120];
};

enum RtuJobType : uint8_t {
  RTU_JOB_WRITE = 1,
  RTU_JOB_RAW = 2,
  RTU_JOB_PROFILE = 3,
  RTU_JOB_SCHEDULE = 4,
  RTU_JOB_CAL_START = 5,
  RTU_JOB_CAL_RESTORE = 6,
  RTU_JOB_PZEM_RESET = 7
};

struct RtuJob {
  RtuJobType type;
  uint32_t id;
  uint16_t reg;
  uint16_t raw;
  uint16_t addr;
  uint16_t count;
  uint8_t profileId;
  uint8_t scheduleSlot;
};

struct RawReadResult {
  uint32_t id;
  bool done;
  bool ok;
  uint16_t addr;
  uint16_t count;
  uint16_t values[20];
  char error[120];
};

struct ProfileFailure {
  uint16_t reg;
  uint16_t raw;
  char error[80];
};

struct ProfileApplyResult {
  uint32_t id;
  bool done;
  bool ok;
  bool partial;
  uint16_t attempted;
  uint16_t applied;
  uint16_t failed;
  char profile[32];
  char error[200];
  ProfileFailure failures[16];
};

// ---------------- inverter faults / warnings / event log ----------------
struct InverterEvent {
  uint32_t id;
  uint32_t uptimeSec;
  uint8_t type;      // 1=fault, 2=warning, 3=mode
  uint8_t bit;       // 0..31 for masks, 255 for mode
  uint8_t code;      // mode value for type=3
  bool active;
  char severity[10];
  char message[88];
};

const uint8_t INVERTER_EVENT_CAPACITY = 48;
InverterEvent inverterEvents[INVERTER_EVENT_CAPACITY] = {};
uint8_t inverterEventHead = 0;
uint8_t inverterEventCount = 0;
uint32_t inverterEventNextId = 1;
portMUX_TYPE inverterEventMux = portMUX_INITIALIZER_UNLOCKED;

static const char* const INVERTER_FAULTS[] = {
  "Inverter module over temperature", "DCDC module over temperature", "Battery over voltage",
  "PV module over temperature", "Output short circuit", "Inverter over voltage", "Output overload",
  "Bus over voltage", "Bus soft start timed out", "PV over current", "PV over voltage",
  "Battery over current", "Inverter over current", "Bus low voltage", "Reserved fault bit 14",
  "Inverter DC component too high", "Reserved fault bit 16", "Output current zero bias too large",
  "Inverter current zero bias too large", "Battery current zero bias too large",
  "PV current zero bias too large", "Inverter low voltage", "Inverter negative power protection",
  "Parallel host lost", "Parallel synchronization signal abnormal", "Battery type incompatible",
  "Parallel versions incompatible"
};
static const char* const INVERTER_WARNINGS[] = {
  "Reserved warning bit 0", "Mains waveform abnormal", "Reserved warning bit 2", "Mains low voltage",
  "Mains over frequency", "Mains low frequency", "PV low voltage", "Over temperature",
  "Battery low voltage", "Battery not connected", "Overload", "Battery equalization charging",
  "Battery undervoltage", "Output power derating", "Fan blocked", "PV energy too low to use",
  "Parallel communication interrupted", "Single/parallel output mode inconsistent",
  "Parallel battery voltage difference too large"
};
const uint8_t INVERTER_FAULT_COUNT = sizeof(INVERTER_FAULTS)/sizeof(INVERTER_FAULTS[0]);
const uint8_t INVERTER_WARNING_COUNT = sizeof(INVERTER_WARNINGS)/sizeof(INVERTER_WARNINGS[0]);


#pragma pack(push,1)
// v2 layout is kept only for one-time migration from v0.12/v0.13.0.
struct BatteryStatsPersistV2 {
  uint32_t magic; uint16_t version; uint16_t size; uint32_t sequence;
  uint64_t charged_mWh; uint64_t discharged_mWh;
  uint32_t chargeSeconds; uint32_t dischargeSeconds;
  int32_t maxCharge_mA; int32_t maxDischarge_mA;
  uint32_t fullChargeEvents; uint32_t deepDischargeEvents;
  uint32_t calibratedEnergy_mWh; uint32_t dayDate;
  uint64_t dayStartCharged_mWh; uint64_t dayStartDischarged_mWh;
  uint32_t firstDate; uint32_t lastFullDate;
  uint8_t calibrationActive; uint8_t calibrationStartSoc; uint8_t calibrationEndSoc; uint8_t reserved0;
  int64_t calibrationNetDischarge_mWh; uint16_t crc;
};

// v3: LiFePO4 cycle count is based on discharged Ah throughput.
// Partial discharges accumulate: e.g. two 50% discharges ~= 1 EFC.
struct BatteryStatsPersist {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t sequence;
  uint64_t charged_mWh;
  uint64_t discharged_mWh;
  uint64_t charged_mAh;
  uint64_t discharged_mAh;
  uint32_t chargeSeconds;
  uint32_t dischargeSeconds;
  int32_t maxCharge_mA;
  int32_t maxDischarge_mA;
  uint32_t calibratedEnergy_mWh;
  uint32_t dayDate;
  uint64_t dayStartCharged_mWh;
  uint64_t dayStartDischarged_mWh;
  uint64_t dayStartDischarged_mAh;
  uint32_t firstDate;
  uint8_t calibrationActive;
  uint8_t calibrationStartSoc;
  uint8_t calibrationEndSoc;
  uint8_t reserved0;
  int64_t calibrationNetDischarge_mWh;
  uint16_t crc;
};

struct BatteryDailyRecord {
  uint16_t magic;
  uint32_t date;
  uint32_t charged_mWh;
  uint32_t discharged_mWh;
  uint32_t discharged_mAh;
  uint32_t efc_milli;
  uint16_t crc;
};

// PZEM two-tariff virtual utility meter. Totals are stored as integer Wh.
struct PzemTariffPersist {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t sequence;
  uint64_t t1_mWh;
  uint64_t t2_mWh;
  uint32_t pzemBaseline_Wh;
  uint8_t baselineValid;
  uint8_t reserved[17];
  uint16_t crc;
};
#pragma pack(pop)
static_assert(sizeof(BatteryStatsPersist) <= 112, "BatteryStatsPersist exceeds EEPROM slot");
static_assert(sizeof(BatteryStatsPersistV2) <= 112, "BatteryStatsPersistV2 exceeds EEPROM slot");
static_assert(sizeof(BatteryDailyRecord) <= 24, "BatteryDailyRecord exceeds EEPROM slot");
static_assert(sizeof(PzemTariffPersist) <= 64, "PzemTariffPersist exceeds EEPROM slot");

struct BatteryCalibrationState {
  bool active;
  bool targetReached;
  bool restoreQueued;
  bool outputRestored;
  uint8_t startSoc;
  uint8_t endSoc;
  int64_t netDischarge_mWh;
  uint64_t startCharged_mAh;
  uint64_t startDischarged_mAh;
  uint64_t startCharged_mWh;
  uint64_t startDischarged_mWh;
  uint16_t previousOutputPriority;
  bool previousOutputPriorityValid;
  uint32_t startedMs;
  char status[128];
};

// Explicit forward declarations for functions used before their definitions.
// This avoids Arduino auto-prototype ordering issues on ESP32 Arduino core 3.x.
void settingsCacheInvalidate();
int settingIndexByReg(uint16_t reg);
bool getCachedSettingRaw(uint16_t reg, uint16_t& raw);
void serviceBatteryStats();
void pzemTariffLoad();
void servicePzemTariff();
uint16_t crc16Data(const uint8_t* data, size_t len);
bool eepromReadBytes(uint16_t addr, uint8_t* dst, size_t len);
bool eepromWriteBytes(uint16_t addr, const uint8_t* src, size_t len);
void batteryStatsSave(bool force=false);
bool adminAuthorized();
bool haveReg(uint16_t reg);
void handleEvents();
void handleDataloggerStatus();
void handleDataloggerServiceApStart();
void handleDataloggerConnect();
void handleDataloggerRestore();
void handleDataloggerInfo();
void handleDataloggerPing();
void handleDataloggerSetParam();


// ---------------- DEVICE / NETWORK SETTINGS ----------------
// Wi-Fi credentials are NOT stored in this sketch.
// They live in a dedicated NVS namespace "wifi_cfg".
//
// Provisioning:
//   - no saved Wi-Fi -> Setup AP automatically
//   - hold BOOT (GPIO0) ~5 s during boot -> Setup AP, saved Wi-Fi preserved
//   - hold BOOT ~15 s during boot -> erase Wi-Fi config only, then Setup AP
//
// Setup AP password is generated once and stored in "device_cfg".
// Normal firmware updates preserve NVS unless "Erase All Flash" is used.

const int BOOT_BUTTON_PIN = 0;
const uint32_t BOOT_SETUP_MS = 5000;
const uint32_t BOOT_WIFI_RESET_MS = 15000;

const bool ALLOW_WEB_WRITES = true;

Preferences wifiPrefs;
Preferences devicePrefs;
Preferences appPrefs;
Preferences schedPrefs;
DNSServer dnsServer;

String wifiSsid;
String wifiPass;
bool netUseStatic = false;
String netLocalIp = "192.168.88.158";
String netGateway = "192.168.88.1";
String netMask = "255.255.255.0";
String netDns = "192.168.88.1";

String setupApSsid;
String setupApPassword;
String adminPassword;
String activeProfile = "ANJ_24V_8S_1P";

bool setupMode = false;
bool setupRequestedByButton = false;
bool setupApRunning = false;
const IPAddress SETUP_AP_IP(192,168,4,1);
const IPAddress SETUP_AP_GW(192,168,4,1);
const IPAddress SETUP_AP_MASK(255,255,255,0);

// ANENJI / Modbus RTU
const uint8_t  DEFAULT_MODBUS_SLAVE = 1;
uint8_t modbusSlave = DEFAULT_MODBUS_SLAVE;
const uint32_t RS485_BAUD = 9600;
const uint32_t RTU_TIMEOUT_MS = 800;        // manual/API operations
const uint32_t RTU_POLL_TIMEOUT_MS = 260;   // background telemetry: fail fast
const uint32_t RTU_GAP_MS = 18;
const uint32_t POLL_INTERVAL_MS = 2000;
const uint32_t POLL_OFFLINE_INTERVAL_MS = 8000;

// Web writes use a bounded RTU time budget. Some ANENJI settings briefly pause
// Modbus while the inverter commits the new value.
const uint32_t RTU_WRITE_TIMEOUT_MS = 650;
const uint32_t RTU_VERIFY_TIMEOUT_MS = 350;
const uint32_t POST_WRITE_QUIET_MS = 1800;
uint32_t rtuQuietUntilMs = 0;

// One RTU worker owns UART2. Every Modbus operation is serialized through this queue.
QueueHandle_t rtuJobQueue = nullptr;
TaskHandle_t rtuWorkerHandle = nullptr;
volatile bool rtuWorkerBusy = false;
volatile bool webWriteBusReserved = false; // true only while a write/profile/scheduler write is executing
volatile bool telemetryForceRequested = false;
volatile bool settingsRefreshRequested = false;
volatile bool settingsRefreshBusy = false;
volatile uint32_t settingsRefreshGeneration = 0;
volatile uint32_t rtuNextJobId = 1;
volatile uint32_t rtuWorkerHeartbeat = 0;
volatile uint32_t rtuWorkerLastMs = 0;

WebWriteResult webWriteResult = {0,false,false,false,0,0,0,{0}};
portMUX_TYPE webWriteResultMux = portMUX_INITIALIZER_UNLOCKED;
RawReadResult rawReadResult = {0,false,false,0,0,{0},{0}};
portMUX_TYPE rawReadResultMux = portMUX_INITIALIZER_UNLOCKED;
ProfileApplyResult profileApplyResult = {};
portMUX_TYPE profileApplyResultMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------- watchdog / self-recovery ----------------
// Hardware Task WDT catches a genuinely stuck Arduino loop.
// Other watchdogs perform controlled recovery before a hard reboot is needed.
const uint32_t TASK_WDT_TIMEOUT_MS = 12000;
const uint8_t  RTU_RECOVERY_AFTER_FAILED_POLLS = 3;
const uint32_t WIFI_WATCHDOG_REBOOT_MS = 15UL * 60UL * 1000UL;
const uint8_t  WIFI_WATCHDOG_MAX_REBOOTS = 3;
const uint32_t LOW_HEAP_LIMIT_BYTES = 20000;
const uint32_t LOW_HEAP_HOLD_MS = 30000;

// RTU self-recovery watchdogs. The Arduino loop WDT cannot detect a dead/stuck
// dedicated RTU task, so monitor its heartbeat separately.
const uint32_t RTU_WORKER_STALL_MS = 15000;
const uint32_t RTU_OFFLINE_RESTART_MS = 90000; // restart ESP only if inverter had been online before
const uint32_t RTU_UART_RECOVERY_MIN_INTERVAL_MS = 5000;

// ESP32 pins
const int RS485_RX_PIN = 16;
const int RS485_TX_PIN = 17;
const int RS485_DE_RE_PIN = 4;

// RTC + external I2C EEPROM. The photographed module uses DS1307 + AT24C32.
// DS1307 and DS3231 share the same basic time-register layout at 0x68, so the
// time driver intentionally uses the common subset. DS1307 CH (seconds bit 7)
// is handled explicitly.
const int RTC_SDA_PIN = 21;
const int RTC_SCL_PIN = 22;
const uint8_t RTC_I2C_ADDR = 0x68;
const uint8_t AT24C32_ADDR_FIRST = 0x50;
const uint8_t AT24C32_ADDR_LAST  = 0x57;
const uint16_t AT24C32_SIZE = 4096;
const uint8_t AT24C32_PAGE_SIZE = 32;
const uint8_t SCHEDULE_TASK_COUNT = 8;
ScheduleTask scheduleTasks[SCHEDULE_TASK_COUNT] = {};
bool rtcPresent = false;

// NTP -> DS1307 synchronization. NTP is UTC; local RTC is kept in local civil time.
const char* NTP_DEFAULT_SERVER = "pool.ntp.org";
const char* NTP_FALLBACK_SERVER = "time.google.com";
String ntpServer = NTP_DEFAULT_SERVER;
int16_t ntpUtcOffsetMin = 180; // fixed UTC offset, default UTC+03:00
const uint32_t NTP_RETRY_MS = 15UL*60UL*1000UL;
const uint32_t NTP_RESYNC_MS = 6UL*60UL*60UL*1000UL;
bool ntpConfigured=false;
bool ntpEverSynced=false;
uint32_t ntpLastAttemptMs=0;
uint32_t ntpLastSyncMs=0;
int32_t ntpLastCorrectionSec=0;
uint32_t schedulerRuns = 0;
uint32_t schedulerErrors = 0;
String schedulerLast;
uint32_t schedulerLastMinuteKey = 0xFFFFFFFFUL;
RtcDateTime rtcCached = {0,0,0,0,0,0,false};
uint32_t rtcCachedAtMs = 0;


// ---------------- battery statistics / AT24C32 ----------------
const uint32_t BAT_STATS_MAGIC = 0x42535431UL; // "BST1"
const uint16_t BAT_STATS_VERSION = 3;
const uint16_t BAT_DAILY_MAGIC = 0xDA18;
const uint16_t BAT_STATS_SLOT_SIZE = 112;
const uint8_t BAT_STATS_SLOT_COUNT = 24; // 2688 bytes
const uint16_t BAT_DAILY_BASE = BAT_STATS_SLOT_SIZE * BAT_STATS_SLOT_COUNT;
const uint16_t BAT_DAILY_SLOT_SIZE = 24;
const uint8_t BAT_DAILY_SLOT_COUNT = 32;
const uint32_t BAT_STATS_SAVE_INTERVAL_MS = 5UL * 60UL * 1000UL;
const uint32_t BAT_STATS_DIRTY_ENERGY_MWH = 50000UL; // 50 Wh
const float BAT_CURRENT_DEADBAND_A = 0.20f;
// ANENJI reg232 shows about -1.4 A around true battery zero current on this installation.
// Correct only the current used for NEW battery statistics; persisted totals/EFC are never rewritten.
const float BAT_STATS_CURRENT_OFFSET_A = 1.40f;
float batteryStatsRawCurrentA = NAN;
float batteryStatsCountedCurrentA = NAN;

// AT24C32 bytes 0..3455 are used by battery stats/history. Reserve 512 bytes
// at the end for 8 wear-levelled tariff slots.
const uint32_t PZEM_TARIFF_MAGIC = 0x505A5431UL; // PZT1
const uint16_t PZEM_TARIFF_VERSION = 1;
const uint16_t PZEM_TARIFF_BASE = 3456;
const uint16_t PZEM_TARIFF_SLOT_SIZE = 64;
const uint8_t PZEM_TARIFF_SLOT_COUNT = 8;
const uint32_t PZEM_TARIFF_SAVE_INTERVAL_MS = 5UL*60UL*1000UL;
const uint32_t PZEM_TARIFF_DIRTY_MWH = 10000UL; // legacy/diagnostic only; normal saves are time-limited

bool eepromPresent = false;
uint8_t eepromI2cAddr = 0;
BatteryStatsPersist batteryStats = {};
BatteryCalibrationState batteryCalibration = {};
uint8_t batteryStatsSlot = 0;
uint8_t batteryDailySlot = 0;
bool batteryStatsDirty = false;
uint64_t batteryLastSavedCharged_mWh = 0;
uint64_t batteryLastSavedDischarged_mWh = 0;
uint32_t batteryStatsLastSaveMs = 0;
uint32_t batteryStatsLastTelemetryStamp = 0;
double batteryChargeFrac_mWh = 0.0;
double batteryDischargeFrac_mWh = 0.0;
double batteryChargeFrac_mAh = 0.0;
double batteryDischargeFrac_mAh = 0.0;

HardwareSerial RS485(2);
WebServer web(80);

const uint16_t DATALOGGER_UDP_PORT = 58899;
const uint16_t DATALOGGER_TCP_PORT = 8899;
WiFiUDP dataloggerUdp;
WiFiServer dataloggerServer(DATALOGGER_TCP_PORT);
bool dataloggerServiceMode = false;
String dataloggerLastTarget;
String dataloggerLastInfo;
uint32_t dataloggerLastOkMs = 0;

const char* FW_VERSION = "0.15.1";
const char* FW_VERSION_PREVIOUS = "0.15.0";

// Web OTA state. During flash writes the RTU worker and scheduler are paused.
volatile bool otaInProgress=false;
bool otaUploadAuthorized=false;
bool otaUploadOk=false;
bool otaRestartPending=false;
uint32_t otaRestartAtMs=0;
String otaUploadError;
size_t otaBytesWritten=0;
size_t otaPartitionSize=0;
int otaUpdateError=0;
bool otaHeaderChecked=false;
volatile bool otaUploadUnlocked=false;
uint32_t otaUploadUnlockUntilMs=0;

// ---------------- diagnostics / cache ----------------
uint32_t rtuTx = 0;
uint32_t rtuRx = 0;
uint32_t rtuErrors = 0;
uint32_t rtuTimeouts = 0;
uint32_t webWrites = 0;
uint32_t webWriteErrors = 0;
String lastError; // RTU-worker private
char lastErrorPublic[128] = {0};
uint32_t lastOkMs = 0;

uint16_t telemetryRaw[40] = {0};   // 200..239
bool telemetryValid[40] = {false};
uint32_t telemetryUpdatedMs = 0;
uint32_t lastPollMs = 0;

uint16_t inverterDiagRaw[10] = {0}; // 100..109
bool inverterDiagValid = false;
uint32_t inverterDiagUpdatedMs = 0;
uint32_t inverterFaultMask = 0;
uint32_t inverterWarningMask = 0;
uint32_t inverterPrevFaultMask = 0;
uint32_t inverterPrevWarningMask = 0;
bool inverterMasksInitialized = false;
uint8_t inverterPrevMode = 0;
bool inverterPrevModeValid = false;

// PZEM-016 on the same RS-485 bus as ANENJI. Default PZEM address is 1;
// ANENJI currently uses its own configurable address (38 in the present installation).
const uint8_t PZEM_DEFAULT_SLAVE = 1;
uint8_t pzemSlave = PZEM_DEFAULT_SLAVE;
bool pzemEnabled = true;
bool pzemOnline = false;
float pzemVoltage=NAN, pzemCurrent=NAN, pzemPower=NAN, pzemEnergyKwh=NAN, pzemFrequency=NAN, pzemPf=NAN;
bool pzemAlarm=false;
uint32_t pzemUpdatedMs=0, pzemLastPollMs=0, pzemPollOk=0, pzemPollErrors=0;
// PZEM-016 is an energy meter, so a slow poll is preferable to competing with
// the inverter for the shared half-duplex RS-485 bus.
const uint32_t PZEM_POLL_INTERVAL_MS = 5000;
const uint32_t PZEM_RTU_TIMEOUT_MS = 700;
const uint32_t PZEM_BUS_GUARD_BEFORE_MS = 90;
const uint32_t PZEM_BUS_GUARD_AFTER_MS = 70;
PzemTariffPersist pzemTariff = {};
uint8_t pzemTariffSlot = 0;
bool pzemTariffDirty = false;
uint32_t pzemTariffLastSaveMs = 0;
uint64_t pzemTariffLastSavedTotal_mWh = 0;
uint8_t pzemTariffLastTariff = 0; // detect 07:00/23:00 boundary for forced persistence

bool taskWdtEnabled = false;
TaskHandle_t mainLoopTaskHandle = nullptr;
uint32_t rtuConsecutivePollFailures = 0;
uint32_t rtuUartRecoveries = 0;
uint32_t rtuLastUartRecoveryMs = 0;
bool inverterEverOnline = false;
uint32_t inverterOfflineSinceMs = 0;
RTC_DATA_ATTR uint8_t rtuControlledReboots = 0;
uint32_t rtuWorkerStallEvents = 0;
uint32_t wifiDisconnectedSinceMs = 0;
uint32_t lowHeapSinceMs = 0;
uint32_t lowHeapEvents = 0;
uint32_t loopHeartbeat = 0;
RTC_DATA_ATTR uint8_t wifiWatchdogReboots = 0;

// ---------------- watchdog helpers ----------------
const char* resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXTERNAL";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

void initTaskWatchdog() {
  // Arduino-ESP32 core 3.x normally initializes TWDT before setup().
  // First try to subscribe the Arduino loop task to the existing watchdog.
  // Only initialize TWDT ourselves when the core reports that it is not initialized.
  esp_err_t e = esp_task_wdt_add(NULL);
  if (e == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms = TASK_WDT_TIMEOUT_MS;
    cfg.idle_core_mask = (1U << portNUM_PROCESSORS) - 1U;
    cfg.trigger_panic = true;
    esp_err_t initErr = esp_task_wdt_init(&cfg);
    if (initErr == ESP_OK) e = esp_task_wdt_add(NULL);
    else e = initErr;
  } else if (e == ESP_ERR_INVALID_ARG) {
    // Some core/IDF combinations return INVALID_ARG when the task is already subscribed.
    // The status call below is authoritative.
  }

  taskWdtEnabled = (esp_task_wdt_status(NULL) == ESP_OK);
  Serial.print(F("[WDT] Task watchdog: "));
  Serial.println(taskWdtEnabled ? F("enabled") : F("not available"));
}

inline void feedTaskWatchdog() {
  if (!taskWdtEnabled) return;
  // Only the Arduino loop task is subscribed to TWDT. RTU helpers are also used
  // by the asynchronous write worker; calling esp_task_wdt_reset() from that
  // unsubscribed task produces "task not found" spam on ESP32 Arduino core 3.x.
  if (mainLoopTaskHandle && xTaskGetCurrentTaskHandle() != mainLoopTaskHandle) return;
  esp_task_wdt_reset();
}

void recoverRs485Uart() {
  uint32_t now = millis();
  if (rtuLastUartRecoveryMs && (uint32_t)(now - rtuLastUartRecoveryMs) < RTU_UART_RECOVERY_MIN_INTERVAL_MS) return;
  rtuLastUartRecoveryMs = now;
  Serial.println(F("[WDT] RTU recovery: restarting UART2 / forcing receive mode"));
  // Force the transceiver back to RX first. This is useful after inverter mode/bypass
  // transitions where the serial side can leave stale bytes or the direction line
  // in an unfortunate state.
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  rs485ReceiveMode();
  delay(20);
  clearRs485Input();
  RS485.end();
  delay(80);
  rs485ReceiveMode();
  RS485.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  delay(30);
  clearRs485Input();
  rtuUartRecoveries++;
  rtuConsecutivePollFailures = 0;
}

void serviceRtuWatchdog() {
  // Never run UART recovery/reboot logic while the synchronous OTA HTTP request owns loopTask.
  if (otaInProgress || otaRestartPending) return;
  if (setupMode || !rtuWorkerHandle) return;
  const uint32_t now = millis();

  // A separate heartbeat is required because only the Arduino loop task is
  // subscribed to TWDT. If the RTU task wedges, HTTP may remain alive forever.
  if (rtuWorkerLastMs && (uint32_t)(now - rtuWorkerLastMs) > RTU_WORKER_STALL_MS) {
    rtuWorkerStallEvents++;
    Serial.print(F("[WDT] RTU worker heartbeat stalled for ms="));
    Serial.println((uint32_t)(now - rtuWorkerLastMs));
    Serial.println(F("[WDT] controlled ESP restart to recover RTU worker"));
    delay(50);
    ESP.restart();
  }

  const bool online = inverterOnline();
  if (online) {
    inverterEverOnline = true;
    inverterOfflineSinceMs = 0;
    rtuControlledReboots = 0;
    return;
  }

  if (!inverterEverOnline) return; // don't reboot forever when inverter is physically absent
  if (!inverterOfflineSinceMs) inverterOfflineSinceMs = now;

  // Re-sync UART periodically while the inverter is offline. PollTelemetry already
  // does this after failed polls; this timer is an independent safety net.
  if (!rtuWorkerBusy && !webWriteBusReserved &&
      (!rtuLastUartRecoveryMs || (uint32_t)(now - rtuLastUartRecoveryMs) >= 30000UL)) {
    recoverRs485Uart();
    telemetryForceRequested = true;
  }

  // If communication was healthy earlier and never comes back after a mode/bypass
  // event, one controlled reboot is preferable to staying offline indefinitely.
  // After reboot, a physically disconnected inverter will never set inverterEverOnline,
  // so this does not create an endless reboot loop.
  if ((uint32_t)(now - inverterOfflineSinceMs) >= RTU_OFFLINE_RESTART_MS &&
      rtuControlledReboots < 1) {
    rtuControlledReboots++;
    Serial.println(F("[WDT] inverter RTU offline >90 s after prior online state; restarting ESP"));
    delay(50);
    ESP.restart();
  }
}

void serviceHeapWatchdog() {
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < LOW_HEAP_LIMIT_BYTES) {
    if (!lowHeapSinceMs) {
      lowHeapSinceMs = millis();
      lowHeapEvents++;
      Serial.print(F("[WDT] low heap: "));
      Serial.println(freeHeap);
    } else if ((uint32_t)(millis() - lowHeapSinceMs) >= LOW_HEAP_HOLD_MS) {
      Serial.println(F("[WDT] low heap persisted; restarting"));
      delay(50);
      ESP.restart();
    }
  } else {
    lowHeapSinceMs = 0;
  }
}

void serviceWifiWatchdog() {
  if (setupMode || wifiSsid.length() == 0) {
    wifiDisconnectedSinceMs = 0;
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiDisconnectedSinceMs = 0;
    wifiWatchdogReboots = 0;
    return;
  }

  if (!wifiDisconnectedSinceMs) wifiDisconnectedSinceMs = millis();

  if ((uint32_t)(millis() - wifiDisconnectedSinceMs) >= WIFI_WATCHDOG_REBOOT_MS &&
      wifiWatchdogReboots < WIFI_WATCHDOG_MAX_REBOOTS) {
    wifiWatchdogReboots++;
    Serial.print(F("[WDT] Wi-Fi offline too long; controlled reboot "));
    Serial.println(wifiWatchdogReboots);
    delay(50);
    ESP.restart();
  }
}

// ---------------- helpers ----------------
uint16_t crc16Modbus(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
  }
  return crc;
}

int16_t asS16(uint16_t v) { return (int16_t)v; }

String jsonEscape(const String& s) {
  String r;
  r.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '"' || c == '\\') { r += '\\'; r += c; }
    else if (c == '\n') r += F("\\n");
    else if ((uint8_t)c >= 0x20) r += c;
  }
  return r;
}

String jsonNum(float v, uint8_t decimals = 1) {
  if (isnan(v)) return F("null");
  return String(v, (unsigned int)decimals);
}

void syncLastErrorPublic() {
  size_t n=lastError.length(); if(n>=sizeof(lastErrorPublic))n=sizeof(lastErrorPublic)-1;
  memcpy(lastErrorPublic,lastError.c_str(),n); lastErrorPublic[n]=0;
}

void sendJson(int code, const String& body) {
  web.send(code, "application/json; charset=utf-8", body);
}

void rs485ReceiveMode() {
  digitalWrite(RS485_DE_RE_PIN, LOW);
}

void rs485TransmitMode() {
  digitalWrite(RS485_DE_RE_PIN, HIGH);
  delayMicroseconds(60);
}

void clearRs485Input() {
  while (RS485.available()) RS485.read();
}

// ---------------- RTU ----------------
bool rtuExchange(const uint8_t* req, size_t reqLen,
                 uint8_t* resp, size_t respMax, size_t& respLen,
                 uint32_t timeoutMs = RTU_TIMEOUT_MS) {
  // v0.11.0 invariant: UART2/RS-485 may only be touched by the dedicated RTU worker.
  // This guard catches accidental future calls from HTTP/loop code instead of allowing collisions.
  if (rtuWorkerHandle && xTaskGetCurrentTaskHandle() != rtuWorkerHandle) {
    lastError = F("RTU access outside worker blocked");
    respLen = 0;
    return false;
  }
  respLen = 0;
  clearRs485Input();

  rs485TransmitMode();
  RS485.write(req, reqLen);
  RS485.flush();
  delayMicroseconds(150);
  rs485ReceiveMode();
  rtuTx++;

  uint32_t start = millis();
  uint32_t lastByte = start;
  bool gotAny = false;
  rtuWorkerLastMs = start;

  while ((uint32_t)(millis() - start) < timeoutMs) {
    rtuWorkerLastMs = millis();
    while (RS485.available()) {
      int c = RS485.read();
      if (c >= 0) {
        gotAny = true;
        lastByte = millis();
        if (respLen < respMax) resp[respLen++] = (uint8_t)c;
        else {
          lastError = F("RTU response too long");
          rtuErrors++;
          return false;
        }
      }
    }
    if (gotAny && (uint32_t)(millis() - lastByte) >= RTU_GAP_MS) break;
    feedTaskWatchdog();
    delay(1);
  }

  if (!gotAny) {
    rtuTimeouts++;
    rtuErrors++;
    lastError = F("RTU timeout");
    return false;
  }
  if (respLen < 5) {
    rtuErrors++;
    lastError = F("Short RTU response");
    return false;
  }

  uint16_t gotCrc = (uint16_t)resp[respLen - 2] | ((uint16_t)resp[respLen - 1] << 8);
  uint16_t calcCrc = crc16Modbus(resp, respLen - 2);
  if (gotCrc != calcCrc) {
    rtuErrors++;
    lastError = F("RTU CRC error");
    return false;
  }
  if (resp[0] != req[0]) {
    rtuErrors++;
    lastError = F("Unexpected RTU slave");
    return false;
  }

  rtuRx++;
  lastOkMs = millis();
  lastError = "";
  return true;
}

bool readHoldingUnit(uint8_t slave, uint16_t addr, uint16_t qty, uint16_t* out,
                     uint32_t timeoutMs = RTU_TIMEOUT_MS) {
  if (!qty || qty > 40) return false;

  uint8_t req[8];
  req[0] = slave;
  req[1] = 0x03;
  req[2] = addr >> 8;
  req[3] = addr & 0xFF;
  req[4] = qty >> 8;
  req[5] = qty & 0xFF;
  uint16_t crc = crc16Modbus(req, 6);
  req[6] = crc & 0xFF;
  req[7] = crc >> 8;

  uint8_t resp[128];
  size_t n = 0;
  if (!rtuExchange(req, sizeof(req), resp, sizeof(resp), n, timeoutMs)) return false;
  if (resp[1] & 0x80) {
    lastError = String("Modbus exception 0x") + String(resp[2], HEX);
    rtuErrors++;
    return false;
  }
  if (resp[1] != 0x03 || resp[2] != qty * 2 || n != (size_t)(5 + qty * 2)) {
    lastError = F("Unexpected FC03 response");
    rtuErrors++;
    return false;
  }

  for (uint16_t i = 0; i < qty; ++i) {
    out[i] = ((uint16_t)resp[3 + i * 2] << 8) | resp[4 + i * 2];
  }
  return true;
}

bool readHolding(uint16_t addr, uint16_t qty, uint16_t* out,
                 uint32_t timeoutMs = RTU_TIMEOUT_MS) {
  return readHoldingUnit(modbusSlave, addr, qty, out, timeoutMs);
}

// FC16, one register, same method used by the Python gateway build.
// Dedicated timeout prevents a web write from holding the synchronous HTTP server indefinitely.
bool writeHoldingSingle(uint16_t addr, uint16_t value, uint32_t timeoutMs = RTU_WRITE_TIMEOUT_MS) {
  uint8_t req[11];
  req[0] = modbusSlave;
  req[1] = 0x10;
  req[2] = addr >> 8;
  req[3] = addr & 0xFF;
  req[4] = 0;
  req[5] = 1;
  req[6] = 2;
  req[7] = value >> 8;
  req[8] = value & 0xFF;
  uint16_t crc = crc16Modbus(req, 9);
  req[9] = crc & 0xFF;
  req[10] = crc >> 8;

  uint8_t resp[32];
  size_t n = 0;
  if (!rtuExchange(req, sizeof(req), resp, sizeof(resp), n, timeoutMs)) return false;
  if (resp[1] & 0x80) {
    lastError = String("Write exception 0x") + String(resp[2], HEX);
    rtuErrors++;
    return false;
  }
  if (n != 8 || resp[1] != 0x10 ||
      resp[2] != req[2] || resp[3] != req[3] ||
      resp[4] != 0 || resp[5] != 1) {
    lastError = F("Unexpected FC16 write response");
    rtuErrors++;
    return false;
  }
  return true;
}

bool writeAndVerify(uint16_t addr, uint16_t raw, uint16_t& oldRaw, uint16_t& newRaw) {
  uint16_t v = 0;
  if (!readHolding(addr, 1, &v, RTU_WRITE_TIMEOUT_MS)) return false;
  oldRaw = v;
  newRaw = oldRaw;

  if (oldRaw == raw) return true;

  if (!writeHoldingSingle(addr, raw, RTU_WRITE_TIMEOUT_MS)) return false;

  // FC16 ACK means the request was accepted. The inverter may pause Modbus briefly
  // while saving a setting, so confirm with several short bounded read-backs.
  const uint16_t verifyPauseMs[] = {150, 300, 500, 700};
  bool gotReadback = false;
  for (size_t attempt = 0; attempt < sizeof(verifyPauseMs)/sizeof(verifyPauseMs[0]); ++attempt) {
    rtuWorkerLastMs = millis();
    delay(verifyPauseMs[attempt]);
    rtuWorkerLastMs = millis();
    feedTaskWatchdog();
    if (readHolding(addr, 1, &v, RTU_VERIFY_TIMEOUT_MS)) {
      gotReadback = true;
      newRaw = v;
      if (newRaw == raw) {
        webWrites++;
        rtuQuietUntilMs = millis() + POST_WRITE_QUIET_MS;
        lastError = "";
        return true;
      }
    }
  }

  rtuQuietUntilMs = millis() + POST_WRITE_QUIET_MS;
  if (gotReadback) lastError = F("Write verify mismatch");
  else lastError = F("Write ACK received, but read-back timed out");
  rtuErrors++;
  return false;
}

// PZEM-016 measurement block uses FC04 Input Registers, 0x0000..0x0009.
bool readInputUnit(uint8_t slave, uint16_t addr, uint16_t qty, uint16_t* out, uint32_t timeoutMs=RTU_POLL_TIMEOUT_MS) {
  if(!qty || qty>40) return false;
  uint8_t req[8]={slave,0x04,(uint8_t)(addr>>8),(uint8_t)addr,(uint8_t)(qty>>8),(uint8_t)qty,0,0};
  uint16_t crc=crc16Modbus(req,6); req[6]=crc&0xFF; req[7]=crc>>8;
  uint8_t resp[128]; size_t n=0;
  if(!rtuExchange(req,sizeof(req),resp,sizeof(resp),n,timeoutMs)) return false;
  if(resp[1]&0x80){ lastError=String("PZEM exception 0x")+String(resp[2],HEX); return false; }
  if(resp[1]!=0x04 || resp[2]!=qty*2 || n!=(size_t)(5+qty*2)){ lastError=F("Unexpected PZEM FC04 response"); return false; }
  for(uint16_t i=0;i<qty;i++) out[i]=((uint16_t)resp[3+i*2]<<8)|resp[4+i*2];
  return true;
}

// Forward declaration: reset verification is defined before the tariff persistence implementation.
bool pzemTariffSave(bool force);

// PZEM-016 vendor command 0x42 resets only the meter's internal accumulated-energy counter.
// This is intentionally NOT a normal Modbus register write. It must run only in the RTU worker.
bool resetPzemEnergyCommand() {
  if(!pzemEnabled || pzemSlave==modbusSlave){ lastError=F("PZEM unavailable or address conflict"); return false; }
  uint8_t req[4]={pzemSlave,0x42,0,0};
  uint16_t crc=crc16Modbus(req,2); req[2]=crc&0xFF; req[3]=crc>>8;
  uint8_t resp[16]={0}; size_t n=0;

  clearRs485Input();
  rs485TransmitMode(); RS485.write(req,sizeof(req)); RS485.flush(); delayMicroseconds(150); rs485ReceiveMode(); rtuTx++;
  uint32_t start=millis(), lastByte=start; bool gotAny=false; rtuWorkerLastMs=start;
  while((uint32_t)(millis()-start)<PZEM_RTU_TIMEOUT_MS){
    rtuWorkerLastMs=millis();
    while(RS485.available()){
      int c=RS485.read(); if(c>=0){gotAny=true;lastByte=millis();if(n<sizeof(resp))resp[n++]=(uint8_t)c;else{lastError=F("PZEM reset response too long");rtuErrors++;return false;}}
    }
    if(gotAny && (uint32_t)(millis()-lastByte)>=RTU_GAP_MS) break;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if(!gotAny){rtuTimeouts++;rtuErrors++;lastError=F("PZEM reset timeout");return false;}
  if(n!=4){rtuErrors++;lastError=F("Unexpected PZEM reset response");return false;}
  uint16_t got=(uint16_t)resp[2]|((uint16_t)resp[3]<<8), calc=crc16Modbus(resp,2);
  if(got!=calc || resp[0]!=pzemSlave || resp[1]!=0x42){rtuErrors++;lastError=F("Invalid PZEM reset response");return false;}
  rtuRx++; lastError=""; return true;
}

// Verify reset by FC04 and re-anchor tariff baseline. T1/T2 totals are never cleared here.
bool resetPzemEnergyAndVerify(String& err) {
  uint32_t inverterLastOk=lastOkMs;
  vTaskDelay(pdMS_TO_TICKS(PZEM_BUS_GUARD_BEFORE_MS));
  bool cmdOk=resetPzemEnergyCommand();
  vTaskDelay(pdMS_TO_TICKS(250));
  uint16_t r[10]={0};
  bool readOk=readInputUnit(pzemSlave,0,10,r,PZEM_RTU_TIMEOUT_MS);
  lastOkMs=inverterLastOk;
  if(!readOk){err=cmdOk?F("Reset ACK received, but PZEM read-back failed"):lastError; return false;}
  uint32_t energyRaw=((uint32_t)r[6]<<16)|r[5]; // Wh
  // A lost 0x42 ACK is acceptable only if read-back proves the counter is reset.
  if(energyRaw>1U){err=cmdOk?String("PZEM energy reset verify failed: ")+energyRaw+" Wh":lastError; return false;}
  pzemEnergyKwh=energyRaw/1000.0f;
  pzemTariff.pzemBaseline_Wh=energyRaw; pzemTariff.baselineValid=1; pzemTariffDirty=true;
  if(!pzemTariffSave(true)){err=F("PZEM reset OK, but tariff baseline EEPROM save failed"); return false;}
  pzemUpdatedMs=millis(); pzemOnline=true;
  err=""; return true;
}

void pollPzem016() {
  if(!pzemEnabled || pzemSlave==modbusSlave) { pzemOnline=false; return; }
  uint16_t r[10]={0};
  // Give the shared half-duplex bus a quiet gap after ANENJI traffic. PZEM-016
  // does not need a fast refresh and some units answer noticeably slower.
  rtuWorkerLastMs=millis();
  vTaskDelay(pdMS_TO_TICKS(PZEM_BUS_GUARD_BEFORE_MS));
  rtuWorkerLastMs=millis();

  // PZEM traffic must not make inverterOnline() look healthy: preserve ANENJI lastOkMs.
  uint32_t inverterLastOk=lastOkMs;
  bool ok=readInputUnit(pzemSlave,0,10,r,PZEM_RTU_TIMEOUT_MS);
  lastOkMs=inverterLastOk;

  // Keep a small silent interval before the RTU worker returns to ANENJI polling.
  rtuWorkerLastMs=millis();
  vTaskDelay(pdMS_TO_TICKS(PZEM_BUS_GUARD_AFTER_MS));
  rtuWorkerLastMs=millis();
  if(!ok){
    // Keep the last valid PZEM sample visible through short RS-485 contention
    // (for example while Service/raw operations are being processed).
    // Declare it offline only after 15 s without a successful PZEM frame.
    pzemPollErrors++;
    if(!pzemUpdatedMs || (uint32_t)(millis()-pzemUpdatedMs)>15000UL) pzemOnline=false;
    return;
  }
  uint32_t currentRaw=((uint32_t)r[2]<<16)|r[1];
  uint32_t powerRaw=((uint32_t)r[4]<<16)|r[3];
  uint32_t energyRaw=((uint32_t)r[6]<<16)|r[5];
  pzemVoltage=r[0]/10.0f;
  pzemCurrent=currentRaw/1000.0f;
  pzemPower=powerRaw/10.0f;
  pzemEnergyKwh=energyRaw/1000.0f;
  pzemFrequency=r[7]/10.0f;
  pzemPf=r[8]/100.0f;
  pzemAlarm=(r[9]==0xFFFF);
  pzemOnline=true; pzemUpdatedMs=millis(); pzemPollOk++;
}

// ---------------- PZEM T1/T2 accounting ----------------
uint64_t pzemTariffTotal_mWh(){ return pzemTariff.t1_mWh + pzemTariff.t2_mWh; }

uint8_t currentTariff(){
  // T1 07:00..22:59, T2 23:00..06:59. RTC is kept in local civil time.
  if(!rtcPresent || !rtcCached.valid) return 0;
  return (rtcCached.hour>=7 && rtcCached.hour<23) ? 1 : 2;
}

bool pzemTariffRecordValid(const PzemTariffPersist& r){
  if(r.magic!=PZEM_TARIFF_MAGIC || r.version!=PZEM_TARIFF_VERSION || r.size!=sizeof(PzemTariffPersist)) return false;
  return crc16Data((const uint8_t*)&r,sizeof(PzemTariffPersist)-sizeof(r.crc))==r.crc;
}

void pzemTariffLoad(){
  memset(&pzemTariff,0,sizeof(pzemTariff));
  pzemTariff.magic=PZEM_TARIFF_MAGIC; pzemTariff.version=PZEM_TARIFF_VERSION; pzemTariff.size=sizeof(PzemTariffPersist);
  if(!eepromPresent) return;
  bool found=false; uint32_t best=0;
  for(uint8_t i=0;i<PZEM_TARIFF_SLOT_COUNT;i++){
    PzemTariffPersist r={};
    if(!eepromReadBytes(PZEM_TARIFF_BASE+(uint16_t)i*PZEM_TARIFF_SLOT_SIZE,(uint8_t*)&r,sizeof(r))) continue;
    if(pzemTariffRecordValid(r) && (!found || r.sequence>=best)){ pzemTariff=r; pzemTariffSlot=i; best=r.sequence; found=true; }
  }
  pzemTariffLastSavedTotal_mWh=pzemTariffTotal_mWh();
}

bool pzemTariffSave(bool force=false){
  if(!eepromPresent) return false;
  uint64_t total=pzemTariffTotal_mWh(); uint32_t now=millis();
  if(!force && !pzemTariffDirty) return true;
  // Protect AT24C32: normal tariff accounting is accumulated in RAM and persisted
  // no more often than once every 5 minutes. Energy/power magnitude never bypasses
  // this limit; force=true is reserved for explicit/boundary/rebase events.
  if(!force && (uint32_t)(now-pzemTariffLastSaveMs)<PZEM_TARIFF_SAVE_INTERVAL_MS) return true;
  PzemTariffPersist r=pzemTariff; r.magic=PZEM_TARIFF_MAGIC; r.version=PZEM_TARIFF_VERSION; r.size=sizeof(PzemTariffPersist); r.sequence=pzemTariff.sequence+1;
  r.crc=crc16Data((const uint8_t*)&r,sizeof(r)-sizeof(r.crc));
  uint8_t next=(uint8_t)((pzemTariffSlot+1)%PZEM_TARIFF_SLOT_COUNT);
  if(!eepromWriteBytes(PZEM_TARIFF_BASE+(uint16_t)next*PZEM_TARIFF_SLOT_SIZE,(const uint8_t*)&r,sizeof(r))) return false;
  pzemTariff=r; pzemTariffSlot=next; pzemTariffDirty=false; pzemTariffLastSaveMs=now; pzemTariffLastSavedTotal_mWh=total; return true;
}

void servicePzemTariff(){
  if(!pzemOnline || !isfinite(pzemEnergyKwh)) return;
  uint32_t curWh=(uint32_t)llround((double)pzemEnergyKwh*1000.0);
  if(!pzemTariff.baselineValid){ pzemTariff.pzemBaseline_Wh=curWh; pzemTariff.baselineValid=1; pzemTariffDirty=true; pzemTariffSave(true); return; }
  if(curWh==pzemTariff.pzemBaseline_Wh){ pzemTariffSave(false); return; }
  // PZEM counter reset/replacement/wrap: re-anchor without inventing consumption.
  if(curWh<pzemTariff.pzemBaseline_Wh){ pzemTariff.pzemBaseline_Wh=curWh; pzemTariffDirty=true; pzemTariffSave(true); return; }
  uint32_t deltaWh=curWh-pzemTariff.pzemBaseline_Wh;
  // Reject implausibly huge discontinuities; re-anchor safely.
  if(deltaWh>10000U){ pzemTariff.pzemBaseline_Wh=curWh; pzemTariffDirty=true; pzemTariffSave(true); return; }
  uint8_t tariff=currentTariff();
  if(tariff==0) return; // do not allocate energy when RTC is invalid
  const bool tariffChanged=(pzemTariffLastTariff!=0 && tariff!=pzemTariffLastTariff);
  if(tariff==1) pzemTariff.t1_mWh += (uint64_t)deltaWh*1000ULL;
  else pzemTariff.t2_mWh += (uint64_t)deltaWh*1000ULL;
  pzemTariff.pzemBaseline_Wh=curWh; pzemTariffDirty=true;
  pzemTariffLastTariff=tariff;
  // At 07:00/23:00 persist the boundary immediately; otherwise obey 5-minute limit.
  pzemTariffSave(tariffChanged);
}

// ---------------- telemetry ----------------
const char* inverterModeText(uint8_t mode) {
  switch (mode) {
    case 0: return "Power On"; case 1: return "Standby"; case 2: return "Mains";
    case 3: return "Off-Grid"; case 4: return "Bypass"; case 5: return "Charging";
    case 6: return "Fault"; default: return "Unknown";
  }
}

void appendInverterEvent(uint8_t type, uint8_t bit, uint8_t code, bool active,
                         const char* severity, const char* message) {
  InverterEvent e = {};
  e.id = inverterEventNextId++;
  e.uptimeSec = millis()/1000UL;
  e.type = type; e.bit = bit; e.code = code; e.active = active;
  snprintf(e.severity, sizeof(e.severity), "%s", severity ? severity : "INFO");
  snprintf(e.message, sizeof(e.message), "%s", message ? message : "");
  portENTER_CRITICAL(&inverterEventMux);
  inverterEvents[inverterEventHead] = e;
  inverterEventHead = (uint8_t)((inverterEventHead + 1) % INVERTER_EVENT_CAPACITY);
  if (inverterEventCount < INVERTER_EVENT_CAPACITY) inverterEventCount++;
  portEXIT_CRITICAL(&inverterEventMux);
}

void processInverterMasks(uint32_t faultMask, uint32_t warningMask) {
  if (!inverterMasksInitialized) {
    for (uint8_t bit=0; bit<INVERTER_FAULT_COUNT; ++bit)
      if (faultMask & (1UL<<bit)) appendInverterEvent(1,bit,0,true,"CRITICAL",INVERTER_FAULTS[bit]);
    for (uint8_t bit=0; bit<INVERTER_WARNING_COUNT; ++bit)
      if (warningMask & (1UL<<bit)) appendInverterEvent(2,bit,0,true,"WARNING",INVERTER_WARNINGS[bit]);
    inverterPrevFaultMask=faultMask; inverterPrevWarningMask=warningMask; inverterMasksInitialized=true; return;
  }
  uint32_t fc=faultMask^inverterPrevFaultMask, wc=warningMask^inverterPrevWarningMask;
  for (uint8_t bit=0; bit<INVERTER_FAULT_COUNT; ++bit) { uint32_t m=1UL<<bit; if(fc&m) appendInverterEvent(1,bit,0,(faultMask&m)!=0,"CRITICAL",INVERTER_FAULTS[bit]); }
  for (uint8_t bit=0; bit<INVERTER_WARNING_COUNT; ++bit) { uint32_t m=1UL<<bit; if(wc&m) appendInverterEvent(2,bit,0,(warningMask&m)!=0,"WARNING",INVERTER_WARNINGS[bit]); }
  inverterPrevFaultMask=faultMask; inverterPrevWarningMask=warningMask;
}

void processInverterMode() {
  if (!haveReg(201)) return;
  uint8_t mode=(uint8_t)telemetryRaw[1];
  if (!inverterPrevModeValid) { inverterPrevMode=mode; inverterPrevModeValid=true; return; }
  if (mode==inverterPrevMode) return;
  char msg[88]; snprintf(msg,sizeof(msg),"Mode: %s -> %s",inverterModeText(inverterPrevMode),inverterModeText(mode));
  appendInverterEvent(3,255,mode,true,mode==6?"CRITICAL":"INFO",msg);
  inverterPrevMode=mode;
}

void pollTelemetry() {
  // Called only by the dedicated RTU worker.
  // After a setting write, avoid immediately starting background RTU traffic.
  if ((int32_t)(rtuQuietUntilMs - millis()) > 0) return;

  uint16_t a[20], b[20], d[10];
  // Keep the web UI responsive when RS-485/inverter is disconnected.
  bool okA = readHolding(200, 20, a, RTU_POLL_TIMEOUT_MS);
  // If block A failed, don't burn a second timeout immediately.
  bool okB = false;
  if (okA) {
    delay(15);
    okB = readHolding(220, 20, b, RTU_POLL_TIMEOUT_MS);
  }

  if (okA) {
    for (int i = 0; i < 20; ++i) {
      telemetryRaw[i] = a[i];
      telemetryValid[i] = true;
    }
  }
  if (okB) {
    for (int i = 0; i < 20; ++i) {
      telemetryRaw[20 + i] = b[i];
      telemetryValid[20 + i] = true;
    }
  }

  bool okDiag = false;
  if (okA) {
    delay(15);
    okDiag = readHolding(100, 10, d, RTU_POLL_TIMEOUT_MS);
  }
  if (okDiag) {
    for (int i=0;i<10;++i) inverterDiagRaw[i]=d[i];
    inverterDiagValid=true; inverterDiagUpdatedMs=millis();
    inverterFaultMask=((uint32_t)d[0]<<16)|d[1];
    inverterWarningMask=((uint32_t)d[8]<<16)|d[9];
    processInverterMasks(inverterFaultMask,inverterWarningMask);
  }

  if (okA || okB) {
    telemetryUpdatedMs = millis();
    rtuConsecutivePollFailures = 0;
    processInverterMode();
  } else {
    rtuConsecutivePollFailures++;
    if (rtuConsecutivePollFailures >= RTU_RECOVERY_AFTER_FAILED_POLLS) {
      recoverRs485Uart();
    }
  }
}

bool haveReg(uint16_t reg) {
  return reg >= 200 && reg <= 239 && telemetryValid[reg - 200];
}

float regScaled(uint16_t reg, float scale, bool signedValue = true) {
  if (!haveReg(reg)) return NAN;
  int32_t v = signedValue ? asS16(telemetryRaw[reg - 200]) : telemetryRaw[reg - 200];
  return ((float)v) / scale;
}

bool inverterOnline() {
  // Two missed poll periods + margin => offline.
  if (!telemetryUpdatedMs || !lastOkMs) return false;
  uint32_t now = millis();
  return (uint32_t)(now - lastOkMs) < (POLL_OFFLINE_INTERVAL_MS + 2500);
}

// ---------------- battery-system detection ----------------
BatterySystemDetect batteryDetectCache = {0,false,NAN,NAN,NAN,String()};
uint32_t batteryDetectCacheMs = 0;
const uint32_t BATTERY_DETECT_CACHE_TTL_MS = 30000;

// We do NOT identify the exact inverter model. We identify the battery-voltage family
// from two independent clues: live battery voltage (reg 215) and configured Bulk/Float.
// Profiles are writable only when the clues agree.
uint8_t voltageFamily(float v) {
  if (v >= 18.0f && v <= 35.0f) return 24;
  if (v >= 36.0f && v <= 65.0f) return 48;
  return 0;
}

BatterySystemDetect detectBatterySystemFresh() {
  BatterySystemDetect d = {0,false,NAN,NAN,NAN,String()};

  // Never touch RS-485 from HTTP. Live voltage comes from telemetry cache; Bulk/Float
  // come from the settings cache maintained by the RTU worker.
  if (haveReg(215)) d.batteryV = regScaled(215, 10, false);

  uint16_t bulkRaw=0, floatRaw=0;
  bool bulkOk = getCachedSettingRaw(324, bulkRaw);
  bool floatOk = getCachedSettingRaw(325, floatRaw);
  if (bulkOk) d.bulkV = bulkRaw/10.0f;
  if (floatOk) d.floatV = floatRaw/10.0f;

  uint8_t fBat = isnan(d.batteryV) ? 0 : voltageFamily(d.batteryV);
  uint8_t fBulk = isnan(d.bulkV) ? 0 : voltageFamily(d.bulkV);
  uint8_t fFloat = isnan(d.floatV) ? 0 : voltageFamily(d.floatV);

  uint8_t cfgFamily = 0;
  if (fBulk && fFloat && fBulk == fFloat) cfgFamily = fBulk;
  else if (fBulk && !fFloat) cfgFamily = fBulk;
  else if (!fBulk && fFloat) cfgFamily = fFloat;

  if (fBat && cfgFamily && fBat == cfgFamily) {
    d.systemV = fBat; d.verified = true;
    d.reason = F("battery voltage + cached charge settings agree");
  } else if (fBat && cfgFamily && fBat != cfgFamily) {
    d.reason = F("battery voltage and cached charge settings disagree");
  } else if (fBat) {
    d.systemV = fBat; d.reason = F("only live battery voltage available; settings refresh pending");
  } else if (cfgFamily) {
    d.systemV = cfgFamily; d.reason = F("only cached charge settings available; not verified");
  } else {
    d.reason = F("not enough cached data yet");
  }
  return d;
}

bool isBatteryVoltageRegister(uint16_t reg) {
  return reg==323 || reg==324 || reg==325 || reg==326 || reg==327 || reg==329 || reg==334;
}

// ---------------- setting metadata ----------------
BatterySystemDetect detectBatterySystem() {
  if (batteryDetectCacheMs && (uint32_t)(millis()-batteryDetectCacheMs) < BATTERY_DETECT_CACHE_TTL_MS)
    return batteryDetectCache;
  batteryDetectCache=detectBatterySystemFresh();
  batteryDetectCacheMs=millis();
  return batteryDetectCache;
}

const SettingDef SETTINGS[] = {
  {300,"Основные","AC output mode",SK_SELECT,1,0,4,true},
  {301,"Основные","Приоритет выхода",SK_SELECT,1,1,4,true},
  {302,"Прочие","Диапазон входного напряжения",SK_SELECT,1,0,2,true},
  {303,"Прочие","Зуммер",SK_SELECT,1,0,3,true},
  {305,"Прочие","Подсветка LCD",SK_SELECT,1,0,1,true},
  {306,"Прочие","Автовозврат LCD",SK_SELECT,1,0,1,true},
  {307,"Прочие","Энергосбережение",SK_SELECT,1,0,1,true},
  {308,"Прочие","Автоперезапуск после перегрузки",SK_SELECT,1,0,1,true},
  {309,"Прочие","Автоперезапуск после перегрева",SK_SELECT,1,0,1,true},
  {310,"Прочие","Overload bypass",SK_SELECT,1,0,1,true},
  {313,"АКБ","Выравнивание АКБ",SK_SELECT,1,0,1,true},
  {320,"Основные","Выходное напряжение",SK_SELECT,1,2200,2400,true},
  {321,"Основные","Выходная частота",SK_SELECT,1,5000,6000,true},
  {322,"АКБ","Тип АКБ",SK_SELECT,1,0,2,true},
  {323,"АКБ","Защита от перенапряжения АКБ",SK_NUMBER,10,0,6553.5,true},
  {324,"АКБ","Bulk / CV",SK_NUMBER,10,0,6553.5,true},
  {325,"АКБ","Float",SK_NUMBER,10,0,6553.5,true},
  {326,"АКБ","Возврат к сети, напряжение",SK_NUMBER,10,0,6553.5,true},
  {327,"АКБ","Low DC в режиме сети",SK_NUMBER,10,0,6553.5,true},
  {329,"АКБ","Low DC off-grid",SK_NUMBER,10,0,6553.5,true},
  {330,"АКБ","CV -> Float, время",SK_NUMBER,1,0,600,true},
  {331,"АКБ","Приоритет зарядки",SK_SELECT,1,1,4,true},
  {332,"АКБ","Макс. ток зарядки",SK_NUMBER,10,0,6553.5,true},
  {333,"АКБ","Макс. ток зарядки от сети",SK_NUMBER,10,0,6553.5,true},
  {334,"АКБ","Напряжение equalization",SK_NUMBER,10,0,6553.5,true},
  {335,"АКБ","Equalization time",SK_NUMBER,1,0,180,true},
  {336,"АКБ","Equalization timeout",SK_NUMBER,1,0,300,true},
  {337,"АКБ","Equalization interval",SK_NUMBER,1,1,90,true},
  {341,"SOC","Low DC SOC в режиме сети",SK_NUMBER,1,0,100,true},
  {342,"SOC","Recovery SOC",SK_NUMBER,1,0,100,true},
  {343,"SOC","Off-grid cut-off SOC",SK_NUMBER,1,0,100,true},
  {344,"Основные","Макс. отдача в сеть",SK_NUMBER,1,0,6200,true},
  {351,"АКБ","Макс. ток разряда АКБ",SK_NUMBER,1,0,65535,true},
  {406,"Remote","Turn-on mode",SK_SELECT,1,0,2,true}
};

const size_t SETTINGS_COUNT = sizeof(SETTINGS) / sizeof(SETTINGS[0]);

const SettingDef* findSetting(uint16_t reg) {
  for (size_t i = 0; i < SETTINGS_COUNT; ++i)
    if (SETTINGS[i].reg == reg) return &SETTINGS[i];
  return nullptr;
}

bool validSelectRaw(uint16_t reg, uint16_t raw) {
  switch (reg) {
    case 300: return raw <= 4;
    case 301: return raw >= 1 && raw <= 4;
    case 302: return raw <= 2;
    case 303: return raw <= 3;
    case 305: case 306: case 307: case 308: case 309: case 310: case 313:
      return raw <= 1;
    case 320: return raw == 2200 || raw == 2300 || raw == 2400;
    case 321: return raw == 5000 || raw == 6000;
    case 322: return raw <= 2;
    case 331: return raw >= 1 && raw <= 4;
    case 406: return raw <= 2;
    default: return true;
  }
}

bool validateRaw(uint16_t reg, uint16_t raw, String& err) {
  const SettingDef* s = findSetting(reg);
  if (!s || !s->writable) {
    err = F("Register is not in writable allow-list");
    return false;
  }
  if (s->kind == SK_SELECT) {
    if (!validSelectRaw(reg, raw)) {
      err = F("Invalid select value");
      return false;
    }
    return true;
  }
  float userVal = ((float)raw) / s->scale;
  if (userVal < s->minVal || userVal > s->maxVal) {
    err = F("Value outside allowed range");
    return false;
  }
  return true;
}

// ---------------- profiles ----------------
const ProfileReg PROFILE_1P[] = {
  {313,0}, {322,2}, {332,150}, {333,100},
  {324,280}, {325,272}, {327,232}, {329,220},
  {341,20}, {342,30}, {343,15}, {351,80}
};

const ProfileReg PROFILE_2P[] = {
  {313,0}, {322,2}, {332,150}, {333,100},
  {324,280}, {325,272}, {327,232}, {329,220},
  {341,20}, {342,30}, {343,15}, {351,120}
};

// Historical 48 V / 51.2 V profiles from the original Python application.
// Visible for reference, but blocked from writing on this 24 V inverter.
const ProfileReg PROFILE_48V_STANDARD[] = {
  {313,0}, {322,2}, {332,100}, {333,100},
  {324,564}, {325,540}, {327,460}, {329,440},
  {341,20}, {342,30}, {343,15}, {351,95}
};

const ProfileReg PROFILE_48V_LONG_LIFE[] = {
  {313,0}, {322,2}, {332,100}, {333,100},
  {324,560}, {325,540}, {327,460}, {329,440},
  {341,20}, {342,30}, {343,15}, {351,80}
};

// ---------------- Modbus RTU gateway removed in v0.10.0 ----------------

// ---------------- small JSON parser helpers ----------------
bool jsonFindNumber(const String& body, const char* key, double& value) {
  String needle = String("\"") + key + "\"";
  int p = body.indexOf(needle);
  if (p < 0) return false;
  p = body.indexOf(':', p + needle.length());
  if (p < 0) return false;
  p++;
  while (p < (int)body.length() && isspace((unsigned char)body[p])) p++;
  int e = p;
  while (e < (int)body.length()) {
    char c = body[e];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
      e++;
    else
      break;
  }
  if (e == p) return false;
  value = body.substring(p, e).toDouble();
  return true;
}

bool jsonFindString(const String& body, const char* key, String& value) {
  String needle = String("\"") + key + "\"";
  int p = body.indexOf(needle);
  if (p < 0) return false;
  p = body.indexOf(':', p + needle.length());
  if (p < 0) return false;
  p++;
  while (p < (int)body.length() && isspace((unsigned char)body[p])) p++;
  if (p >= (int)body.length() || body[p] != '"') return false;
  p++;
  int e = body.indexOf('"', p);
  if (e < 0) return false;
  value = body.substring(p, e);
  return true;
}

bool jsonFindBool(const String& body, const char* key, bool& value) {
  String needle = String("\"") + key + "\"";
  int p = body.indexOf(needle);
  if (p < 0) return false;
  p = body.indexOf(':', p + needle.length());
  if (p < 0) return false;
  p++;
  while (p < (int)body.length() && isspace((unsigned char)body[p])) p++;
  if (body.substring(p, p + 4) == "true") { value = true; return true; }
  if (body.substring(p, p + 5) == "false") { value = false; return true; }
  return false;
}

bool parseIp(const String& s, IPAddress& ip) {
  return ip.fromString(s);
}

String macSuffix() {
  uint64_t chip = ESP.getEfuseMac();
  char buf[7];
  snprintf(buf, sizeof(buf), "%06llX", (unsigned long long)(chip & 0xFFFFFFULL));
  return String(buf);
}

String generateDevicePassword() {
  static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  char buf[14];
  uint32_t r1 = esp_random(), r2 = esp_random();
  uint64_t r = ((uint64_t)r1 << 32) | r2;
  for (int i = 0; i < 12; ++i) {
    buf[i] = alphabet[(r >> ((i % 10) * 5)) & 31];
    if (i == 3 || i == 7) buf[i] = '-';
  }
  buf[12] = 0;
  return String(buf);
}

void loadPersistentConfig() {
  wifiPrefs.begin("wifi_cfg", false);
  devicePrefs.begin("device_cfg", false);
  appPrefs.begin("anenji", false);
  loadScheduleTasks();

  wifiSsid = wifiPrefs.getString("ssid", "");
  wifiPass = wifiPrefs.getString("pass", "");
  netUseStatic = wifiPrefs.getBool("static", false);
  netLocalIp = wifiPrefs.getString("ip", "192.168.88.158");
  netGateway = wifiPrefs.getString("gw", "192.168.88.1");
  netMask = wifiPrefs.getString("mask", "255.255.255.0");
  netDns = wifiPrefs.getString("dns", "192.168.88.1");

  setupApPassword = devicePrefs.getString("setup_pass", "");
  if (setupApPassword.length() < 8) {
    setupApPassword = generateDevicePassword();
    devicePrefs.putString("setup_pass", setupApPassword);
  }

  adminPassword = devicePrefs.getString("admin_pass", "");
  if (adminPassword.length() < 8) {
    adminPassword = setupApPassword;
    devicePrefs.putString("admin_pass", adminPassword);
  }

  activeProfile = appPrefs.getString("profile", "ANJ_24V_8S_1P");
  uint32_t savedSlave = appPrefs.getUInt("modbus_slave", DEFAULT_MODBUS_SLAVE);
  modbusSlave = (savedSlave >= 1 && savedSlave <= 247) ? (uint8_t)savedSlave : DEFAULT_MODBUS_SLAVE;
  ntpServer = appPrefs.getString("ntp_server", NTP_DEFAULT_SERVER);
  ntpServer.trim(); if(ntpServer.length()<3 || ntpServer.length()>63) ntpServer=NTP_DEFAULT_SERVER;
  ntpUtcOffsetMin = (int16_t)appPrefs.getInt("ntp_utc_min", 180);
  if (ntpUtcOffsetMin < -720 || ntpUtcOffsetMin > 840) ntpUtcOffsetMin = 180;
  pzemEnabled = appPrefs.getBool("pzem_en", true);
  uint32_t ps=appPrefs.getUInt("pzem_slave", PZEM_DEFAULT_SLAVE);
  pzemSlave=(ps>=1 && ps<=247)?(uint8_t)ps:PZEM_DEFAULT_SLAVE;
  setupApSsid = String("ANENJI-SETUP-") + macSuffix();

  // Recovery credentials are intentionally printed to the local serial console.
  // They are not exposed through the normal web UI.
  Serial.println();
  Serial.println(F("========== ANENJI AUTH =========="));
  Serial.print(F("Setup AP:       ")); Serial.println(setupApSsid);
  Serial.print(F("Setup password: ")); Serial.println(setupApPassword);
  Serial.print(F("Admin password: ")); Serial.println(adminPassword);
  Serial.println(F("================================="));
}


// ---------------- DS1307/DS3231-common RTC + scheduler ----------------
uint8_t bcdToDec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
uint8_t decToBcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool rtcRead(RtcDateTime& dt) {
  dt.valid = false;
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)RTC_I2C_ADDR, 7) != 7) return false;

  uint8_t secRaw = Wire.read();
  uint8_t minRaw = Wire.read();
  uint8_t hourRaw = Wire.read();
  Wire.read(); // DOW: scheduler calculates weekday from the date itself
  uint8_t dayRaw = Wire.read();
  uint8_t monthRaw = Wire.read();
  uint8_t yearRaw = Wire.read();

  // DS1307: CH=1 in seconds bit7 means oscillator stopped. On DS3231 this bit is 0.
  bool oscillatorStopped = (secRaw & 0x80) != 0;
  secRaw &= 0x7F;

  uint8_t hour;
  if (hourRaw & 0x40) { // 12-hour mode
    uint8_t h12=bcdToDec(hourRaw & 0x1F);
    bool pm=(hourRaw & 0x20)!=0;
    hour=(uint8_t)((h12%12)+(pm?12:0));
  } else hour=bcdToDec(hourRaw & 0x3F);

  dt.second = bcdToDec(secRaw);
  dt.minute = bcdToDec(minRaw & 0x7F);
  dt.hour = hour;
  dt.day = bcdToDec(dayRaw & 0x3F);
  dt.month = bcdToDec(monthRaw & 0x1F);
  dt.year = 2000 + bcdToDec(yearRaw);

  dt.valid = !oscillatorStopped &&
             dt.year >= 2024 && dt.year <= 2099 &&
             dt.month >= 1 && dt.month <= 12 &&
             dt.day >= 1 && dt.day <= 31 &&
             dt.hour <= 23 && dt.minute <= 59 && dt.second <= 59;
  return true;
}

bool rtcWrite(const RtcDateTime& dt) {
  if (dt.year < 2024 || dt.year > 2099 || dt.month < 1 || dt.month > 12 ||
      dt.day < 1 || dt.day > 31 || dt.hour > 23 || dt.minute > 59 || dt.second > 59) return false;
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.write(decToBcd(dt.second) & 0x7F); // DS1307 CH=0 -> oscillator enabled
  Wire.write(decToBcd(dt.minute));
  Wire.write(decToBcd(dt.hour));         // force 24-hour mode
  Wire.write((uint8_t)1);
  Wire.write(decToBcd(dt.day));
  Wire.write(decToBcd(dt.month));
  Wire.write(decToBcd((uint8_t)(dt.year - 2000)));
  return Wire.endTransmission() == 0;
}

// 0=Monday ... 6=Sunday
uint8_t weekdayMon0(uint16_t y, uint8_t m, uint8_t d) {
  // Sakamoto: 0=Sunday. Convert to Monday-based.
  static const uint8_t t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  uint16_t yy = y;
  if (m < 3) yy--;
  uint8_t sun0 = (yy + yy/4 - yy/100 + yy/400 + t[m-1] + d) % 7;
  return (uint8_t)((sun0 + 6) % 7);
}

String rtcIso(const RtcDateTime& dt) {
  if (!dt.valid) return String();
  char b[24];
  snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02u",
           dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  return String(b);
}


// ---------------- AT24C32 + battery statistics ----------------
uint16_t crc16Data(const uint8_t* data, size_t len) { return crc16Modbus(data,len); }

bool eepromReadBytes(uint16_t addr, uint8_t* dst, size_t len) {
  if (!eepromPresent || !dst || addr + len > AT24C32_SIZE) return false;
  while (len) {
    uint8_t chunk=(uint8_t)min((size_t)28,len);
    Wire.beginTransmission(eepromI2cAddr);
    Wire.write((uint8_t)(addr>>8)); Wire.write((uint8_t)addr);
    if (Wire.endTransmission(false)!=0) return false;
    int got=Wire.requestFrom((int)eepromI2cAddr,(int)chunk);
    if (got!=chunk) return false;
    for(uint8_t i=0;i<chunk;i++) *dst++=Wire.read();
    addr+=chunk; len-=chunk;
  }
  return true;
}

bool eepromWriteBytes(uint16_t addr, const uint8_t* src, size_t len) {
  if (!eepromPresent || !src || addr + len > AT24C32_SIZE) return false;
  while (len) {
    uint8_t room=(uint8_t)(AT24C32_PAGE_SIZE-(addr%AT24C32_PAGE_SIZE));
    uint8_t chunk=(uint8_t)min((size_t)room,len);
    chunk=(uint8_t)min((int)chunk,26); // 2 address bytes + payload within Wire buffer
    Wire.beginTransmission(eepromI2cAddr);
    Wire.write((uint8_t)(addr>>8)); Wire.write((uint8_t)addr);
    for(uint8_t i=0;i<chunk;i++) Wire.write(src[i]);
    if (Wire.endTransmission()!=0) return false;
    delay(6); // AT24C32 write-cycle time
    addr+=chunk; src+=chunk; len-=chunk;
    feedTaskWatchdog();
  }
  return true;
}

void detectExternalEeprom() {
  eepromPresent=false; eepromI2cAddr=0;
  for(uint8_t a=AT24C32_ADDR_FIRST;a<=AT24C32_ADDR_LAST;a++) {
    if(i2cProbe(a)){eepromPresent=true;eepromI2cAddr=a;break;}
  }
}

uint32_t rtcDateKey() {
  if(!rtcPresent || !rtcCached.valid) return 0;
  return (uint32_t)rtcCached.year*10000UL+(uint32_t)rtcCached.month*100UL+rtcCached.day;
}

float profileNominalVoltage() {
  return activeProfile.startsWith("ANJ_48V") ? 51.2f : 25.6f;
}
uint32_t profileNominalEnergy_mWh() {
  if(activeProfile=="ANJ_24V_8S_1P") return 2560000UL;
  if(activeProfile=="ANJ_24V_8S_2P") return 5120000UL;
  return 5120000UL;
}
uint32_t profileNominalCapacity_mAh() {
  const float v=profileNominalVoltage();
  const uint32_t e=profileNominalEnergy_mWh();
  return (v>1.0f) ? (uint32_t)lround((double)e/(double)v) : 0;
}
uint32_t calibratedCapacity_mAh() {
  const float v=profileNominalVoltage();
  return (batteryStats.calibratedEnergy_mWh && v>1.0f)
    ? (uint32_t)lround((double)batteryStats.calibratedEnergy_mWh/(double)v) : 0;
}
bool calibratedCapacityPlausible() {
  const uint32_t nom=profileNominalCapacity_mAh();
  const uint32_t cal=calibratedCapacity_mAh();
  if(!nom || !cal) return false;
  // A SOC-derived estimate far outside the profile is treated as invalid.
  // This protects against the coarse inverter SOC typical for LiFePO4.
  return cal >= (uint32_t)((uint64_t)nom*60/100) && cal <= (uint32_t)((uint64_t)nom*140/100);
}
uint32_t batteryReferenceEnergy_mWh() {
  // EFC is intentionally anchored to DESIGN/profile capacity, not to SOC-derived calibration.
  return profileNominalEnergy_mWh();
}
uint32_t batteryReferenceCapacity_mAh() {
  return profileNominalCapacity_mAh();
}
float batteryEfc() {
  // LiFePO4 EFC = cumulative discharged Ah / nominal design Ah.
  // Partial discharges accumulate. Calibration cannot retroactively change cycle count.
  const uint32_t ref=profileNominalCapacity_mAh();
  return ref ? (float)((double)batteryStats.discharged_mAh/(double)ref) : 0.0f;
}

void batteryStatsDefaults() {
  memset(&batteryStats,0,sizeof(batteryStats));
  batteryStats.magic=BAT_STATS_MAGIC; batteryStats.version=BAT_STATS_VERSION;
  batteryStats.size=sizeof(BatteryStatsPersist); batteryStats.sequence=1;
  batteryStats.dayDate=rtcDateKey();
  batteryStats.dayStartCharged_mWh=0; batteryStats.dayStartDischarged_mWh=0;
  batteryStats.dayStartDischarged_mAh=0;
  batteryStats.firstDate=batteryStats.dayDate;
  batteryStatsSlot=0; batteryDailySlot=0;
  batteryStatsDirty=true;
}

bool batteryStatsRecordValid(const BatteryStatsPersist& r) {
  if(r.magic!=BAT_STATS_MAGIC || r.version!=BAT_STATS_VERSION || r.size!=sizeof(BatteryStatsPersist)) return false;
  uint16_t c=crc16Data((const uint8_t*)&r,sizeof(BatteryStatsPersist)-sizeof(r.crc));
  return c==r.crc;
}

void batteryStatsLoad() {
  batteryStatsDefaults();
  if(!eepromPresent) return;
  bool found=false; BatteryStatsPersist best={}; uint8_t bestSlot=0;
  for(uint8_t i=0;i<BAT_STATS_SLOT_COUNT;i++){
    BatteryStatsPersist r={};
    if(!eepromReadBytes((uint16_t)i*BAT_STATS_SLOT_SIZE,(uint8_t*)&r,sizeof(r))) continue;
    if(!batteryStatsRecordValid(r)) continue;
    if(!found || (int32_t)(r.sequence-best.sequence)>0){best=r;bestSlot=i;found=true;}
  }
  if(found){
    batteryStats=best;batteryStatsSlot=bestSlot;batteryStatsDirty=false;
  } else {
    // One-time migration from v2. Ah throughput was not stored then, so derive
    // a close estimate from Wh / nominal pack voltage. From this version on,
    // Ah is integrated directly from battery current.
    bool oldFound=false; BatteryStatsPersistV2 oldBest={}; uint8_t oldSlot=0;
    for(uint8_t i=0;i<BAT_STATS_SLOT_COUNT;i++){
      BatteryStatsPersistV2 r={};
      if(!eepromReadBytes((uint16_t)i*BAT_STATS_SLOT_SIZE,(uint8_t*)&r,sizeof(r))) continue;
      if(r.magic!=BAT_STATS_MAGIC || r.version!=2 || r.size!=sizeof(BatteryStatsPersistV2)) continue;
      uint16_t c=crc16Data((const uint8_t*)&r,sizeof(BatteryStatsPersistV2)-sizeof(r.crc));
      if(c!=r.crc) continue;
      if(!oldFound || (int32_t)(r.sequence-oldBest.sequence)>0){oldBest=r;oldSlot=i;oldFound=true;}
    }
    if(oldFound){
      batteryStatsDefaults();
      batteryStats.sequence=oldBest.sequence;
      batteryStats.charged_mWh=oldBest.charged_mWh;
      batteryStats.discharged_mWh=oldBest.discharged_mWh;
      const double nv=profileNominalVoltage();
      batteryStats.charged_mAh=(uint64_t)llround((double)oldBest.charged_mWh/nv);
      batteryStats.discharged_mAh=(uint64_t)llround((double)oldBest.discharged_mWh/nv);
      batteryStats.chargeSeconds=oldBest.chargeSeconds;
      batteryStats.dischargeSeconds=oldBest.dischargeSeconds;
      batteryStats.maxCharge_mA=oldBest.maxCharge_mA;
      batteryStats.maxDischarge_mA=oldBest.maxDischarge_mA;
      batteryStats.calibratedEnergy_mWh=oldBest.calibratedEnergy_mWh;
      batteryStats.dayDate=oldBest.dayDate;
      batteryStats.dayStartCharged_mWh=oldBest.dayStartCharged_mWh;
      batteryStats.dayStartDischarged_mWh=oldBest.dayStartDischarged_mWh;
      batteryStats.dayStartDischarged_mAh=(uint64_t)llround((double)oldBest.dayStartDischarged_mWh/nv);
      batteryStats.firstDate=oldBest.firstDate;
      batteryStats.calibrationActive=oldBest.calibrationActive;
      batteryStats.calibrationStartSoc=oldBest.calibrationStartSoc;
      batteryStats.calibrationEndSoc=oldBest.calibrationEndSoc;
      batteryStats.calibrationNetDischarge_mWh=oldBest.calibrationNetDischarge_mWh;
      batteryStatsSlot=oldSlot;
      batteryStatsDirty=true;
    }
  }
  // Controlled calibration uses RAM baselines for Ah throughput.  It is intentionally
  // not resumed after an ESP32 reboot because that would make the Ah result ambiguous.
  batteryCalibration = {};
  batteryCalibration.endSoc=batteryStats.calibrationEndSoc?batteryStats.calibrationEndSoc:20;
  if(batteryStats.calibrationActive){
    batteryStats.calibrationActive=0;
    batteryStats.calibrationNetDischarge_mWh=0;
    batteryStatsDirty=true;
    strncpy(batteryCalibration.status,"cancelled after reboot; start a new controlled discharge",sizeof(batteryCalibration.status)-1);
  } else {
    strncpy(batteryCalibration.status,"idle",sizeof(batteryCalibration.status)-1);
  }
  batteryLastSavedCharged_mWh=batteryStats.charged_mWh;
  batteryLastSavedDischarged_mWh=batteryStats.discharged_mWh;
  batteryStatsLastSaveMs=millis();
  uint32_t bestDate=0;
  for(uint8_t i=0;i<BAT_DAILY_SLOT_COUNT;i++){
    BatteryDailyRecord d={};
    if(!eepromReadBytes(BAT_DAILY_BASE+(uint16_t)i*BAT_DAILY_SLOT_SIZE,(uint8_t*)&d,sizeof(d)))continue;
    uint16_t c=crc16Data((const uint8_t*)&d,sizeof(d)-sizeof(d.crc));
    if(d.magic==BAT_DAILY_MAGIC && c==d.crc && d.date>=bestDate){bestDate=d.date;batteryDailySlot=i;}
  }
  if(batteryStatsDirty) batteryStatsSave(true);
}

void batteryStatsSave(bool force) {
  if(!eepromPresent || !batteryStatsDirty) return;
  uint64_t deltaC=batteryStats.charged_mWh-batteryLastSavedCharged_mWh;
  uint64_t deltaD=batteryStats.discharged_mWh-batteryLastSavedDischarged_mWh;
  if(!force && (uint32_t)(millis()-batteryStatsLastSaveMs)<BAT_STATS_SAVE_INTERVAL_MS &&
     deltaC<BAT_STATS_DIRTY_ENERGY_MWH && deltaD<BAT_STATS_DIRTY_ENERGY_MWH) return;
  batteryStats.magic=BAT_STATS_MAGIC; batteryStats.version=BAT_STATS_VERSION; batteryStats.size=sizeof(BatteryStatsPersist);
  batteryStats.calibrationActive=batteryCalibration.active?1:0;
  batteryStats.calibrationStartSoc=batteryCalibration.startSoc;
  batteryStats.calibrationEndSoc=batteryCalibration.endSoc;
  batteryStats.calibrationNetDischarge_mWh=batteryCalibration.netDischarge_mWh;
  batteryStats.sequence++;
  batteryStats.crc=crc16Data((const uint8_t*)&batteryStats,sizeof(BatteryStatsPersist)-sizeof(batteryStats.crc));
  uint8_t next=(uint8_t)((batteryStatsSlot+1)%BAT_STATS_SLOT_COUNT);
  if(eepromWriteBytes((uint16_t)next*BAT_STATS_SLOT_SIZE,(const uint8_t*)&batteryStats,sizeof(batteryStats))){
    batteryStatsSlot=next; batteryStatsDirty=false; batteryStatsLastSaveMs=millis();
    batteryLastSavedCharged_mWh=batteryStats.charged_mWh;
    batteryLastSavedDischarged_mWh=batteryStats.discharged_mWh;
  }
}

void appendDailyRecord(uint32_t date,uint64_t charged,uint64_t discharged,uint64_t discharged_mAh) {
  if(!eepromPresent || !date) return;
  BatteryDailyRecord d={}; d.magic=BAT_DAILY_MAGIC; d.date=date;
  d.charged_mWh=(uint32_t)min(charged,(uint64_t)0xFFFFFFFFULL);
  d.discharged_mWh=(uint32_t)min(discharged,(uint64_t)0xFFFFFFFFULL);
  d.discharged_mAh=(uint32_t)min(discharged_mAh,(uint64_t)0xFFFFFFFFULL);
  uint32_t ref=batteryReferenceCapacity_mAh();
  d.efc_milli=ref?(uint32_t)((discharged_mAh*1000ULL)/ref):0;
  d.crc=crc16Data((const uint8_t*)&d,sizeof(d)-sizeof(d.crc));
  batteryDailySlot=(uint8_t)((batteryDailySlot+1)%BAT_DAILY_SLOT_COUNT);
  eepromWriteBytes(BAT_DAILY_BASE+(uint16_t)batteryDailySlot*BAT_DAILY_SLOT_SIZE,(const uint8_t*)&d,sizeof(d));
}

void serviceBatteryDayRollover() {
  uint32_t today=rtcDateKey(); if(!today) return;
  if(!batteryStats.dayDate){batteryStats.dayDate=today;batteryStats.dayStartCharged_mWh=batteryStats.charged_mWh;batteryStats.dayStartDischarged_mWh=batteryStats.discharged_mWh;batteryStats.dayStartDischarged_mAh=batteryStats.discharged_mAh;if(!batteryStats.firstDate)batteryStats.firstDate=today;batteryStatsDirty=true;return;}
  if(today!=batteryStats.dayDate){
    uint64_t c=batteryStats.charged_mWh-batteryStats.dayStartCharged_mWh;
    uint64_t d=batteryStats.discharged_mWh-batteryStats.dayStartDischarged_mWh;
    uint64_t dAh=batteryStats.discharged_mAh-batteryStats.dayStartDischarged_mAh;
    appendDailyRecord(batteryStats.dayDate,c,d,dAh);
    batteryStats.dayDate=today; batteryStats.dayStartCharged_mWh=batteryStats.charged_mWh; batteryStats.dayStartDischarged_mWh=batteryStats.discharged_mWh; batteryStats.dayStartDischarged_mAh=batteryStats.discharged_mAh;
    batteryStatsDirty=true; batteryStatsSave(true);
  }
}

void serviceBatteryStats() {
  serviceBatteryDayRollover();
  uint32_t stamp=telemetryUpdatedMs;
  if(!stamp || stamp==batteryStatsLastTelemetryStamp) { batteryStatsSave(false); return; }
  uint32_t dtMs=batteryStatsLastTelemetryStamp ? (uint32_t)(stamp-batteryStatsLastTelemetryStamp) : 0;
  batteryStatsLastTelemetryStamp=stamp;
  if(!dtMs || dtMs>15000) { batteryStatsSave(false); return; }
  if(!haveReg(215) || (!haveReg(232) && !haveReg(216))) { batteryStatsSave(false); return; }
  float v=regScaled(215,10,false);
  float rawA=haveReg(232)?regScaled(232,10,true):regScaled(216,10,true);
  float soc=haveReg(229)?regScaled(229,1,false):NAN;
  if(isnan(v)||isnan(rawA)||v<10||v>70) return;
  // Measured zero is approximately -1.4 A, therefore I(real) = I(reg232) + 1.4 A.
  // Apply the existing 0.20 A deadband after offset correction.
  float a=rawA + BAT_STATS_CURRENT_OFFSET_A;
  if(fabsf(a)<=BAT_CURRENT_DEADBAND_A) a=0.0f;
  batteryStatsRawCurrentA=rawA;
  batteryStatsCountedCurrentA=a;
  double p=(double)v*(double)a;
  double mWh=fabs(p)*(double)dtMs/3600.0;
  double mAh=fabs((double)a)*(double)dtMs/3600.0;
  if(a>BAT_CURRENT_DEADBAND_A){
    batteryChargeFrac_mWh+=mWh; uint64_t whole=(uint64_t)batteryChargeFrac_mWh;
    if(whole){batteryStats.charged_mWh+=whole;batteryChargeFrac_mWh-=whole;batteryStatsDirty=true;}
    batteryChargeFrac_mAh+=mAh; uint64_t wholeAh=(uint64_t)batteryChargeFrac_mAh;
    if(wholeAh){batteryStats.charged_mAh+=wholeAh;batteryChargeFrac_mAh-=wholeAh;batteryStatsDirty=true;}
    batteryStats.chargeSeconds+=dtMs/1000;
    int32_t ma=(int32_t)lround(a*1000.0f);if(ma>batteryStats.maxCharge_mA){batteryStats.maxCharge_mA=ma;batteryStatsDirty=true;}
    if(batteryCalibration.active) batteryCalibration.netDischarge_mWh-= (int64_t)whole;
  } else if(a<-BAT_CURRENT_DEADBAND_A){
    batteryDischargeFrac_mWh+=mWh; uint64_t whole=(uint64_t)batteryDischargeFrac_mWh;
    if(whole){batteryStats.discharged_mWh+=whole;batteryDischargeFrac_mWh-=whole;batteryStatsDirty=true;}
    batteryDischargeFrac_mAh+=mAh; uint64_t wholeAh=(uint64_t)batteryDischargeFrac_mAh;
    if(wholeAh){batteryStats.discharged_mAh+=wholeAh;batteryDischargeFrac_mAh-=wholeAh;batteryStatsDirty=true;}
    batteryStats.dischargeSeconds+=dtMs/1000;
    int32_t ma=(int32_t)lround((-a)*1000.0f);if(ma>batteryStats.maxDischarge_mA){batteryStats.maxDischarge_mA=ma;batteryStatsDirty=true;}
    if(batteryCalibration.active) batteryCalibration.netDischarge_mWh+= (int64_t)whole;
  }
  if(batteryCalibration.netDischarge_mWh<0) batteryCalibration.netDischarge_mWh=0;

  if(!isnan(soc)){
    uint8_t si=(uint8_t)constrain((int)lround(soc),0,100);
    // When the target SOC is reached, stop forced battery operation by restoring
    // register 301 to the value that was present before calibration.  Calibration
    // remains active until the user presses Finish, so the measured segment can be reviewed.
    if(batteryCalibration.active && !batteryCalibration.targetReached && !batteryCalibration.restoreQueued &&
       si<=batteryCalibration.endSoc){
      batteryCalibration.targetReached=true;
      if(batteryCalibration.previousOutputPriorityValid && rtuJobQueue){
        RtuJob stopJob={}; stopJob.type=RTU_JOB_CAL_RESTORE; stopJob.id=rtuNextJobId++;
        stopJob.reg=301; stopJob.raw=batteryCalibration.previousOutputPriority;
        if(xQueueSend(rtuJobQueue,&stopJob,0)==pdTRUE){
          batteryCalibration.restoreQueued=true;
          snprintf(batteryCalibration.status,sizeof(batteryCalibration.status),"target %u%% reached; restoring output priority",batteryCalibration.endSoc);
        } else {
          snprintf(batteryCalibration.status,sizeof(batteryCalibration.status),"target reached; RTU queue full - stop discharge manually");
        }
      } else {
        snprintf(batteryCalibration.status,sizeof(batteryCalibration.status),"target reached; stop discharge manually");
      }
      batteryStatsDirty=true; batteryStatsSave(true);
    }
  }
  batteryStatsSave(false);
}

void handleBatteryStatsGet(){
  uint64_t todayC=(batteryStats.charged_mWh>=batteryStats.dayStartCharged_mWh)
    ? batteryStats.charged_mWh-batteryStats.dayStartCharged_mWh : batteryStats.charged_mWh;
  uint64_t todayD=(batteryStats.discharged_mWh>=batteryStats.dayStartDischarged_mWh)
    ? batteryStats.discharged_mWh-batteryStats.dayStartDischarged_mWh : batteryStats.discharged_mWh;
  uint32_t ref=batteryReferenceEnergy_mWh();
  String j; j.reserve(2600); j=F("{\"ok\":true");
  j+=F(",\"eeprom_present\":");j+=eepromPresent?F("true"):F("false");
  j+=F(",\"eeprom_addr\":");if(eepromPresent)j+=eepromI2cAddr;else j+=F("null");
  j+=F(",\"charged_kwh\":");j+=String((double)batteryStats.charged_mWh/1000000.0,3);
  j+=F(",\"discharged_kwh\":");j+=String((double)batteryStats.discharged_mWh/1000000.0,3);
  j+=F(",\"efc\":");j+=String(batteryEfc(),3);
  j+=F(",\"charged_ah\":");j+=String((double)batteryStats.charged_mAh/1000.0,2);
  j+=F(",\"discharged_ah\":");j+=String((double)batteryStats.discharged_mAh/1000.0,2);
  j+=F(",\"raw_battery_current_a\":");if(isnan(batteryStatsRawCurrentA))j+=F("null");else j+=String(batteryStatsRawCurrentA,1);
  j+=F(",\"stats_battery_current_a\":");if(isnan(batteryStatsCountedCurrentA))j+=F("null");else j+=String(batteryStatsCountedCurrentA,1);
  j+=F(",\"stats_current_offset_a\":");j+=String(BAT_STATS_CURRENT_OFFSET_A,1);
  j+=F(",\"reference_capacity_ah\":");j+=String(profileNominalCapacity_mAh()/1000.0f,1);
  j+=F(",\"capacity_source\":\"profile\"");
  j+=F(",\"reference_energy_kwh\":");j+=String(ref/1000000.0f,3);
  j+=F(",\"calibrated_energy_kwh\":");if(batteryStats.calibratedEnergy_mWh)j+=String(batteryStats.calibratedEnergy_mWh/1000000.0f,3);else j+=F("null");
  j+=F(",\"calibrated_capacity_ah\":");if(batteryStats.calibratedEnergy_mWh)j+=String(calibratedCapacity_mAh()/1000.0f,1);else j+=F("null");
  j+=F(",\"calibrated_capacity_valid\":");j+=calibratedCapacityPlausible()?F("true"):F("false");
  j+=F(",\"charge_hours\":");j+=String(batteryStats.chargeSeconds/3600.0f,1);
  j+=F(",\"discharge_hours\":");j+=String(batteryStats.dischargeSeconds/3600.0f,1);
  j+=F(",\"max_charge_a\":");j+=String(batteryStats.maxCharge_mA/1000.0f,1);
  j+=F(",\"max_discharge_a\":");j+=String(batteryStats.maxDischarge_mA/1000.0f,1);
  j+=F(",\"today_charged_kwh\":");j+=String((double)todayC/1000000.0,3);
  j+=F(",\"today_discharged_kwh\":");j+=String((double)todayD/1000000.0,3);
  j+=F(",\"first_date\":");j+=batteryStats.firstDate;
  j+=F(",\"calibration\":{\"active\":");j+=batteryCalibration.active?F("true"):F("false");
  j+=F(",\"start_soc\":");j+=batteryCalibration.startSoc;
  j+=F(",\"end_soc\":");j+=batteryCalibration.endSoc;
  j+=F(",\"net_discharge_kwh\":");j+=String((double)batteryCalibration.netDischarge_mWh/1000000.0,3);
  double calNetAh=0.0;
  if(batteryCalibration.active){
    int64_t dd=(int64_t)batteryStats.discharged_mAh-(int64_t)batteryCalibration.startDischarged_mAh;
    int64_t cc=(int64_t)batteryStats.charged_mAh-(int64_t)batteryCalibration.startCharged_mAh;
    int64_t net=dd-cc; if(net<0)net=0; calNetAh=(double)net/1000.0;
  }
  j+=F(",\"net_discharge_ah\":");j+=String(calNetAh,2);
  j+=F(",\"current_soc\":");if(haveReg(229))j+=String(regScaled(229,1,false),0);else j+=F("null");
  j+=F(",\"target_reached\":");j+=batteryCalibration.targetReached?F("true"):F("false");
  j+=F(",\"auto_discharge\":true");
  j+=F(",\"previous_priority\":");if(batteryCalibration.previousOutputPriorityValid)j+=batteryCalibration.previousOutputPriority;else j+=F("null");
  j+=F(",\"elapsed_sec\":");j+=batteryCalibration.active?((millis()-batteryCalibration.startedMs)/1000UL):0;
  j+=F(",\"status\":\"");j+=jsonEscape(String(batteryCalibration.status));j+=F("\"}");
  j+=F(",\"history\":[");
  bool first=true;
  if(eepromPresent){
    for(uint8_t n=0;n<BAT_DAILY_SLOT_COUNT;n++){
      int idx=(int)batteryDailySlot-(int)n;while(idx<0)idx+=BAT_DAILY_SLOT_COUNT;
      BatteryDailyRecord d={};if(!eepromReadBytes(BAT_DAILY_BASE+(uint16_t)idx*BAT_DAILY_SLOT_SIZE,(uint8_t*)&d,sizeof(d)))continue;
      uint16_t c=crc16Data((const uint8_t*)&d,sizeof(d)-sizeof(d.crc));if(d.magic!=BAT_DAILY_MAGIC||c!=d.crc||!d.date)continue;
      if(!first)j+=',';first=false;
      j+=F("{\"date\":");j+=d.date;j+=F(",\"charged_kwh\":");j+=String(d.charged_mWh/1000000.0f,3);
      j+=F(",\"discharged_kwh\":");j+=String(d.discharged_mWh/1000000.0f,3);j+=F(",\"discharged_ah\":");j+=String(d.discharged_mAh/1000.0f,2);j+=F(",\"efc\":");j+=String(d.efc_milli/1000.0f,3);j+='}';
      if(n>=29)break;
    }
  }
  j+=F("]}");sendJson(200,j);
}

void handleBatteryCalibrationStart(){
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  if(!rtuJobQueue){sendJson(503,F("{\"ok\":false,\"error\":\"RTU worker unavailable\"}"));return;}
  if(batteryCalibration.active){sendJson(409,F("{\"ok\":false,\"error\":\"Calibration already active\"}"));return;}
  if(!haveReg(229)){sendJson(409,F("{\"ok\":false,\"error\":\"Battery SOC unavailable\"}"));return;}
  int soc=(int)lround(regScaled(229,1,false));
  String body=web.arg("plain");double endD=20;jsonFindNumber(body,"end_soc",endD);
  if(soc<90){sendJson(409,F("{\"ok\":false,\"error\":\"Start controlled discharge near full battery (SOC >= 90%)\"}"));return;}
  if(endD<10||endD>50||soc<=endD+10){sendJson(400,F("{\"ok\":false,\"error\":\"Invalid end_soc\"}"));return;}
  if(webWriteBusReserved || uxQueueMessagesWaiting(rtuJobQueue)>=5){sendJson(409,F("{\"ok\":false,\"error\":\"RTU is busy\"}"));return;}

  RtuJob job={}; job.type=RTU_JOB_CAL_START; job.id=rtuNextJobId++; if(!job.id)job.id=rtuNextJobId++;
  job.reg=301; job.raw=2; job.addr=(uint16_t)soc; job.count=(uint16_t)endD;
  webWriteBusReserved=true;
  portENTER_CRITICAL(&webWriteResultMux);
  webWriteResult.id=job.id; webWriteResult.done=false; webWriteResult.ok=false; webWriteResult.verified=false;
  webWriteResult.reg=301; webWriteResult.oldRaw=0; webWriteResult.newRaw=0; webWriteResult.error[0]=0;
  portEXIT_CRITICAL(&webWriteResultMux);
  if(xQueueSend(rtuJobQueue,&job,0)!=pdTRUE){webWriteBusReserved=false;sendJson(503,F("{\"ok\":false,\"error\":\"RTU queue full\"}"));return;}
  String j=F("{\"ok\":true,\"queued\":true,\"job_id\":");j+=job.id;j+=F(",\"register\":301,\"value\":2}");sendJson(202,j);
}

bool queueCalibrationRestore(){
  if(!batteryCalibration.previousOutputPriorityValid || !rtuJobQueue || batteryCalibration.restoreQueued || batteryCalibration.outputRestored) return false;
  RtuJob job={};job.type=RTU_JOB_CAL_RESTORE;job.id=rtuNextJobId++;job.reg=301;job.raw=batteryCalibration.previousOutputPriority;
  if(xQueueSend(rtuJobQueue,&job,0)!=pdTRUE)return false;
  batteryCalibration.restoreQueued=true; return true;
}

void handleBatteryCalibrationFinish(){
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  if(!batteryCalibration.active){sendJson(409,F("{\"ok\":false,\"error\":\"Calibration is not active\"}"));return;}
  if(!haveReg(229)){sendJson(409,F("{\"ok\":false,\"error\":\"Battery SOC unavailable\"}"));return;}
  int endSoc=(int)lround(regScaled(229,1,false));
  int delta=(int)batteryCalibration.startSoc-endSoc;
  int64_t dd=(int64_t)batteryStats.discharged_mAh-(int64_t)batteryCalibration.startDischarged_mAh;
  int64_t cc=(int64_t)batteryStats.charged_mAh-(int64_t)batteryCalibration.startCharged_mAh;
  int64_t netmAh=dd-cc; if(netmAh<0)netmAh=0;
  if(delta<10){sendJson(409,F("{\"ok\":false,\"error\":\"SOC range is too small; discharge at least 10% before finishing\"}"));return;}
  if(netmAh<1000){sendJson(409,F("{\"ok\":false,\"error\":\"Measured discharge is too small\"}"));return;}

  const double measuredAh=(double)netmAh/1000.0;
  const double nominalAh=(double)profileNominalCapacity_mAh()/1000.0;
  const double socEstimateAh=measuredAh*100.0/(double)delta;
  // Inverter SOC on LiFePO4 can be very coarse. Never overwrite capacity from a
  // short SOC span or from an estimate that is implausible for the active profile.
  const bool socSpanGood=(delta>=60);
  const bool estimatePlausible=(nominalAh>0.0 && socEstimateAh>=nominalAh*0.60 && socEstimateAh<=nominalAh*1.40);
  const bool saveEstimate=socSpanGood && estimatePlausible;

  if(saveEstimate){
    const double fullWh=socEstimateAh*profileNominalVoltage();
    batteryStats.calibratedEnergy_mWh=(uint32_t)lround(fullWh*1000.0);
    snprintf(batteryCalibration.status,sizeof(batteryCalibration.status),
             "complete: %.1f Ah, SOC span %d%%",socEstimateAh,delta);
  }else{
    snprintf(batteryCalibration.status,sizeof(batteryCalibration.status),
             "measurement complete: %.1f Ah; SOC estimate rejected (%d%% span)",measuredAh,delta);
  }
  bool restoreQueued=queueCalibrationRestore();
  batteryCalibration.active=false;
  batteryStatsDirty=true;batteryStatsSave(true);

  String j=F("{\"ok\":true,\"saved\":");j+=saveEstimate?F("true"):F("false");
  j+=F(",\"capacity_ah\":");j+=String(socEstimateAh,1);
  j+=F(",\"nominal_capacity_ah\":");j+=String(nominalAh,1);
  j+=F(",\"start_soc\":");j+=batteryCalibration.startSoc;
  j+=F(",\"end_soc\":");j+=endSoc;
  j+=F(",\"soc_span\":");j+=delta;
  j+=F(",\"net_discharge_ah\":");j+=String(measuredAh,2);
  j+=F(",\"restore_queued\":");j+=restoreQueued?F("true"):F("false");
  j+=F(",\"warning\":\"");
  if(!saveEstimate){
    if(!socSpanGood) j+=F("SOC span below 60%; inverter SOC is too coarse for capacity extrapolation");
    else j+=F("SOC-derived capacity is implausible for active battery profile");
  }
  j+=F("\"}");sendJson(200,j);
}

void handleBatteryCalibrationCancel(){
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  bool restoreQueued=queueCalibrationRestore();
  batteryCalibration.active=false;strncpy(batteryCalibration.status,"cancelled",sizeof(batteryCalibration.status)-1);
  batteryStatsDirty=true;batteryStatsSave(true);
  String j=F("{\"ok\":true,\"restore_queued\":");j+=restoreQueued?F("true"):F("false");j+='}';sendJson(200,j);
}

void handleBatteryCalibrationManual(){
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  String body=web.arg("plain");double ah=0;if(!jsonFindNumber(body,"capacity_ah",ah)||ah<10||ah>1000){sendJson(400,F("{\"ok\":false,\"error\":\"capacity_ah 10..1000 required\"}"));return;}
  double wh=ah*profileNominalVoltage();batteryStats.calibratedEnergy_mWh=(uint32_t)lround(wh*1000.0);batteryStatsDirty=true;batteryStatsSave(true);sendJson(200,F("{\"ok\":true}"));
}

bool eraseBatteryStatsEeprom() {
  if(!eepromPresent) return false;
  const uint16_t endAddr=BAT_DAILY_BASE+(uint16_t)BAT_DAILY_SLOT_COUNT*BAT_DAILY_SLOT_SIZE;
  uint8_t blank[32]; memset(blank,0xFF,sizeof(blank));
  for(uint16_t a=0;a<endAddr;a+=sizeof(blank)){
    size_t n=min((size_t)sizeof(blank),(size_t)(endAddr-a));
    if(!eepromWriteBytes(a,blank,n)) return false;
  }
  // Verify all statistics slots. A stale valid slot must never resurrect after reboot.
  for(uint8_t i=0;i<BAT_STATS_SLOT_COUNT;i++){
    BatteryStatsPersist r={};
    if(!eepromReadBytes((uint16_t)i*BAT_STATS_SLOT_SIZE,(uint8_t*)&r,sizeof(r))) return false;
    if(batteryStatsRecordValid(r)) return false;
  }
  // Verify that daily history no longer contains a valid record either.
  for(uint8_t i=0;i<BAT_DAILY_SLOT_COUNT;i++){
    BatteryDailyRecord d={};
    if(!eepromReadBytes(BAT_DAILY_BASE+(uint16_t)i*BAT_DAILY_SLOT_SIZE,(uint8_t*)&d,sizeof(d))) return false;
    uint16_t c=crc16Data((const uint8_t*)&d,sizeof(d)-sizeof(d.crc));
    if(d.magic==BAT_DAILY_MAGIC && c==d.crc) return false;
  }
  return true;
}

void handleBatteryStatsReset(){
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  String body=web.arg("plain");
  String confirm, mode;
  if(!jsonFindString(body,"confirm",confirm) || confirm!="RESET"){
    sendJson(400,F("{\"ok\":false,\"error\":\"confirm RESET required\"}")); return;
  }
  if(!jsonFindString(body,"mode",mode) || (mode!="stats" && mode!="all")){
    sendJson(400,F("{\"ok\":false,\"error\":\"mode must be stats or all\"}")); return;
  }

  const bool keepCalibration=(mode=="stats");
  const uint32_t savedCalibration=keepCalibration?batteryStats.calibratedEnergy_mWh:0;
  bool storageErased=true;
  if(eepromPresent) storageErased=eraseBatteryStatsEeprom();

  batteryCalibration.active=false;
  batteryCalibration.startSoc=0; batteryCalibration.endSoc=20;
  batteryCalibration.netDischarge_mWh=0; batteryCalibration.startedMs=0;
  memset(batteryCalibration.status,0,sizeof(batteryCalibration.status));

  batteryStatsDefaults();
  batteryStats.sequence=0;
  batteryStatsSlot=BAT_STATS_SLOT_COUNT-1; // first new write goes to slot 0
  batteryStats.calibratedEnergy_mWh=savedCalibration;
  batteryStatsDirty=true;
  batteryChargeFrac_mWh=0; batteryDischargeFrac_mWh=0;
  batteryChargeFrac_mAh=0; batteryDischargeFrac_mAh=0;
  batteryStatsLastTelemetryStamp=telemetryUpdatedMs;
  batteryLastSavedCharged_mWh=0; batteryLastSavedDischarged_mWh=0;
  batteryDailySlot=0;

  bool persisted=false;
  if(eepromPresent && storageErased){
    batteryStatsSave(true);
    persisted=!batteryStatsDirty;
  }

  // Reset must always take effect in RAM. EEPROM persistence is reported
  // separately so a missing/faulty AT24C32 cannot make the UI look unchanged.
  String r=F("{\"ok\":true,\"reset\":true,\"mode\":\""); r+=mode;
  r+=F("\",\"eeprom_present\":"); r+=eepromPresent?F("true"):F("false");
  r+=F(",\"storage_erased\":"); r+=storageErased?F("true"):F("false");
  r+=F(",\"persisted\":"); r+=persisted?F("true"):F("false");
  r+=F(",\"calibration_kept\":"); r+=keepCalibration?F("true"):F("false");
  r+=F(",\"charged_kwh\":0,\"discharged_kwh\":0,\"charged_ah\":0,\"discharged_ah\":0,\"efc\":0}");
  sendJson(200,r);
}

void loadScheduleTasks() {
  schedPrefs.begin("scheduler", false);
  for (uint8_t i=0; i<SCHEDULE_TASK_COUNT; ++i) {
    char k[8];
    snprintf(k,sizeof(k),"e%u",i); scheduleTasks[i].enabled=schedPrefs.getBool(k,false);
    snprintf(k,sizeof(k),"d%u",i); scheduleTasks[i].daysMask=(uint8_t)schedPrefs.getUChar(k,127);
    snprintf(k,sizeof(k),"h%u",i); scheduleTasks[i].hour=(uint8_t)schedPrefs.getUChar(k,0);
    snprintf(k,sizeof(k),"m%u",i); scheduleTasks[i].minute=(uint8_t)schedPrefs.getUChar(k,0);
    snprintf(k,sizeof(k),"r%u",i); scheduleTasks[i].reg=(uint16_t)schedPrefs.getUShort(k,301);
    snprintf(k,sizeof(k),"v%u",i); scheduleTasks[i].raw=(uint16_t)schedPrefs.getUShort(k,1);
    if (scheduleTasks[i].daysMask == 0 || scheduleTasks[i].hour > 23 ||
        scheduleTasks[i].minute > 59) scheduleTasks[i].enabled=false;
  }
}

void saveScheduleTask(uint8_t i) {
  if (i >= SCHEDULE_TASK_COUNT) return;
  char k[8];
  snprintf(k,sizeof(k),"e%u",i); schedPrefs.putBool(k,scheduleTasks[i].enabled);
  snprintf(k,sizeof(k),"d%u",i); schedPrefs.putUChar(k,scheduleTasks[i].daysMask);
  snprintf(k,sizeof(k),"h%u",i); schedPrefs.putUChar(k,scheduleTasks[i].hour);
  snprintf(k,sizeof(k),"m%u",i); schedPrefs.putUChar(k,scheduleTasks[i].minute);
  snprintf(k,sizeof(k),"r%u",i); schedPrefs.putUShort(k,scheduleTasks[i].reg);
  snprintf(k,sizeof(k),"v%u",i); schedPrefs.putUShort(k,scheduleTasks[i].raw);
}

bool executeScheduleTask(uint8_t i) {
  if (i >= SCHEDULE_TASK_COUNT || !rtuJobQueue) return false;
  ScheduleTask &t = scheduleTasks[i];
  String err;
  if (!validateRaw(t.reg, t.raw, err)) {
    schedulerErrors++;
    schedulerLast = String("Task ") + (i+1) + ": " + err;
    return false;
  }

  RtuJob job = {};
  job.type = RTU_JOB_SCHEDULE;
  job.id = rtuNextJobId++;
  job.reg = t.reg;
  job.raw = t.raw;
  job.scheduleSlot = i;
  if (xQueueSend(rtuJobQueue, &job, 0) != pdTRUE) {
    schedulerErrors++;
    schedulerLast = String("Task ") + (i+1) + ": RTU queue full";
    return false;
  }
  schedulerLast = String("Task ") + (i+1) + ": queued";
  return true;
}

int64_t civilDaysFromEpoch(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y-399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d-1;
  const unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

int64_t rtcCivilSeconds(const RtcDateTime& d) {
  return civilDaysFromEpoch(d.year,d.month,d.day)*86400LL + (int64_t)d.hour*3600LL + (int64_t)d.minute*60LL + d.second;
}

bool ntpLocalDateTime(RtcDateTime& out) {
  struct tm tmNow;
  if (!getLocalTime(&tmNow, 50)) return false;
  int y=tmNow.tm_year+1900;
  if (y < 2024 || y > 2099) return false;
  out={(uint16_t)y,(uint8_t)(tmNow.tm_mon+1),(uint8_t)tmNow.tm_mday,(uint8_t)tmNow.tm_hour,(uint8_t)tmNow.tm_min,(uint8_t)tmNow.tm_sec,true};
  return true;
}

void serviceNtpRtcSync() {
  if (setupMode || WiFi.status()!=WL_CONNECTED) return;
  uint32_t now=millis();
  if (!ntpConfigured) {
    // Fixed UTC offset only; no DST/seasonal conversion. DS1307 stores local civil time.
    configTime((long)ntpUtcOffsetMin * 60L, 0, ntpServer.c_str(), NTP_FALLBACK_SERVER);
    ntpConfigured=true; ntpLastAttemptMs=now;
    Serial.println(F("[NTP] client configured"));
  }
  bool due=!ntpEverSynced || (uint32_t)(now-ntpLastSyncMs)>=NTP_RESYNC_MS;
  if (!due) return;
  if (ntpLastAttemptMs && (uint32_t)(now-ntpLastAttemptMs)<NTP_RETRY_MS && ntpEverSynced==false) return;
  ntpLastAttemptMs=now;
  RtcDateTime ndt;
  if (!ntpLocalDateTime(ndt)) return;
  RtcDateTime old=rtcCached;
  if (rtcPresent && old.valid) ntpLastCorrectionSec=(int32_t)(rtcCivilSeconds(ndt)-rtcCivilSeconds(old)); else ntpLastCorrectionSec=0;
  if (!rtcWrite(ndt)) { Serial.println(F("[NTP] RTC write failed")); return; }
  rtcPresent=true; rtcCached=ndt; rtcCachedAtMs=now; schedulerLastMinuteKey=0xFFFFFFFFUL;
  ntpEverSynced=true; ntpLastSyncMs=now;
  Serial.print(F("[NTP] DS1307 synchronized; correction s=")); Serial.println(ntpLastCorrectionSec);
}

void serviceScheduler() {
  static uint32_t lastCheckMs=0;
  if (setupMode || (uint32_t)(millis()-lastCheckMs) < 1000) return;
  lastCheckMs=millis();

  RtcDateTime dt;
  if (!rtcRead(dt)) { rtcPresent=false; return; }
  rtcPresent=true;
  rtcCached=dt;
  rtcCachedAtMs=millis();
  if (!dt.valid) return;

  uint32_t dayKey=(uint32_t)dt.year*10000UL+(uint32_t)dt.month*100UL+dt.day;
  uint32_t minuteKey=(dayKey*1440UL)+(uint32_t)dt.hour*60UL+dt.minute;
  if (minuteKey == schedulerLastMinuteKey) return;
  schedulerLastMinuteKey = minuteKey;

  uint8_t wd=weekdayMon0(dt.year,dt.month,dt.day);
  uint8_t bit=(uint8_t)(1U<<wd);
  for (uint8_t i=0;i<SCHEDULE_TASK_COUNT;++i) {
    const ScheduleTask &t=scheduleTasks[i];
    if (!t.enabled || !(t.daysMask & bit)) continue;
    if (t.hour==dt.hour && t.minute==dt.minute) executeScheduleTask(i);
  }
}

void handleRtcGet() {
  // HTTP must never wait on I2C. serviceScheduler() refreshes this cache.
  bool busOk=rtcPresent;
  RtcDateTime dt=rtcCached;
  String j=F("{\"ok\":true,\"present\":");
  j+=busOk?F("true"):F("false");
  j+=F(",\"valid\":"); j+=(busOk&&dt.valid)?F("true"):F("false");
  j+=F(",\"type\":\"DS1307/compatible\"");
  j+=F(",\"eeprom_present\":"); j+=eepromPresent?F("true"):F("false");
  j+=F(",\"eeprom_addr\":"); if(eepromPresent) j+=eepromI2cAddr; else j+=F("null");
  j+=F(",\"time\":\""); if(busOk&&dt.valid)j+=rtcIso(dt); j+=F("\"");
  j+=F(",\"cache_age_ms\":"); if(rtcCachedAtMs)j+=(millis()-rtcCachedAtMs);else j+=F("null");
  j+=F(",\"ntp_server\":\""); j+=jsonEscape(ntpServer); j+=F("\"");
  j+=F(",\"ntp_utc_offset_min\":"); j+=ntpUtcOffsetMin;
  j+=F(",\"ntp_synced\":"); j+=ntpEverSynced?F("true"):F("false");
  j+=F(",\"ntp_last_correction_s\":"); j+=ntpLastCorrectionSec;
  j+=F(",\"ntp_sync_age_s\":"); if(ntpEverSynced) j+=(millis()-ntpLastSyncMs)/1000UL; else j+=F("null");
  j+=F(",\"runs\":");j+=schedulerRuns;
  j+=F(",\"errors\":");j+=schedulerErrors;
  j+=F(",\"last\":\"");j+=jsonEscape(schedulerLast);j+=F("\"}");
  sendJson(200,j);
}

void handleRtcSet() {
  if (!adminAuthorized()) { sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}")); return; }
  String body=web.arg("plain");
  double y,mo,d,h,mi,se;
  if (!jsonFindNumber(body,"year",y)||!jsonFindNumber(body,"month",mo)||!jsonFindNumber(body,"day",d)||
      !jsonFindNumber(body,"hour",h)||!jsonFindNumber(body,"minute",mi)||!jsonFindNumber(body,"second",se)) {
    sendJson(400,F("{\"ok\":false,\"error\":\"year/month/day/hour/minute/second required\"}")); return;
  }
  RtcDateTime dt={(uint16_t)y,(uint8_t)mo,(uint8_t)d,(uint8_t)h,(uint8_t)mi,(uint8_t)se,true};
  if (!rtcWrite(dt)) { sendJson(500,F("{\"ok\":false,\"error\":\"RTC write failed\"}")); return; }
  rtcPresent=true; rtcCached=dt; rtcCachedAtMs=millis(); schedulerLastMinuteKey=0xFFFFFFFFUL;
  sendJson(200,F("{\"ok\":true,\"saved\":true}"));
}

void handleNtpConfigSet() {
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  String body=web.arg("plain"), server; double offMin=ntpUtcOffsetMin;
  if(!jsonFindString(body,"server",server)){sendJson(400,F("{\"ok\":false,\"error\":\"server required\"}"));return;}
  jsonFindNumber(body,"utc_offset_min",offMin);
  server.trim();
  if(server.length()<3 || server.length()>63 || server.indexOf(' ')>=0){sendJson(400,F("{\"ok\":false,\"error\":\"invalid NTP server\"}"));return;}
  if(offMin < -720 || offMin > 840){sendJson(400,F("{\"ok\":false,\"error\":\"UTC offset must be -12:00..+14:00\"}"));return;}
  ntpServer=server; ntpUtcOffsetMin=(int16_t)lround(offMin);
  appPrefs.putString("ntp_server",ntpServer); appPrefs.putInt("ntp_utc_min",ntpUtcOffsetMin);
  ntpConfigured=false; ntpEverSynced=false; ntpLastAttemptMs=0;
  String j=String("{\"ok\":true,\"server\":\"")+jsonEscape(ntpServer)+"\",\"utc_offset_min\":"+String(ntpUtcOffsetMin)+"}"; sendJson(200,j);
}

void handlePzemConfigSet() {
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  String body=web.arg("plain"); double a=1,en=1;
  jsonFindNumber(body,"address",a); jsonFindNumber(body,"enabled",en);
  if(a<1 || a>247 || (uint8_t)a==modbusSlave){sendJson(400,F("{\"ok\":false,\"error\":\"PZEM address must be 1..247 and different from inverter\"}"));return;}
  pzemSlave=(uint8_t)a; pzemEnabled=(en!=0); appPrefs.putUInt("pzem_slave",pzemSlave); appPrefs.putBool("pzem_en",pzemEnabled);
  pzemOnline=false; pzemLastPollMs=0;
  sendJson(200,F("{\"ok\":true}"));
}

void handlePzemEnergyReset() {
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  String body=web.arg("plain"), confirm;
  if(!jsonFindString(body,"confirm",confirm) || confirm!="RESET_PZEM"){
    sendJson(400,F("{\"ok\":false,\"error\":\"confirm RESET_PZEM required\"}"));return;
  }
  if(!pzemEnabled || pzemSlave==modbusSlave || !rtuJobQueue){sendJson(503,F("{\"ok\":false,\"error\":\"PZEM/RTU unavailable\"}"));return;}
  if(webWriteBusReserved || uxQueueMessagesWaiting(rtuJobQueue)>=5){sendJson(409,F("{\"ok\":false,\"error\":\"RTU is busy\"}"));return;}
  RtuJob job={}; job.type=RTU_JOB_PZEM_RESET; job.id=rtuNextJobId++; if(!job.id)job.id=rtuNextJobId++;
  webWriteBusReserved=true;
  portENTER_CRITICAL(&webWriteResultMux);
  webWriteResult.id=job.id;webWriteResult.done=false;webWriteResult.ok=false;webWriteResult.verified=false;
  webWriteResult.reg=0;webWriteResult.oldRaw=0;webWriteResult.newRaw=0;webWriteResult.error[0]=0;
  portEXIT_CRITICAL(&webWriteResultMux);
  if(xQueueSend(rtuJobQueue,&job,0)!=pdTRUE){webWriteBusReserved=false;sendJson(503,F("{\"ok\":false,\"error\":\"RTU queue full\"}"));return;}
  String j=F("{\"ok\":true,\"queued\":true,\"job_id\":");j+=job.id;j+=F(",\"t1_t2_preserved\":true}");sendJson(202,j);
}

void handlePzemTariffSet() {
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  String body=web.arg("plain"); double t1=0,t2=0;
  if(!jsonFindNumber(body,"t1_kwh",t1)||!jsonFindNumber(body,"t2_kwh",t2)||t1<0||t2<0||t1>99999999||t2>99999999){
    sendJson(400,F("{\"ok\":false,\"error\":\"t1_kwh and t2_kwh required\"}"));return;
  }
  pzemTariff.t1_mWh=(uint64_t)llround(t1*1000000.0);
  pzemTariff.t2_mWh=(uint64_t)llround(t2*1000000.0);
  if(pzemOnline && isfinite(pzemEnergyKwh)){pzemTariff.pzemBaseline_Wh=(uint32_t)llround((double)pzemEnergyKwh*1000.0);pzemTariff.baselineValid=1;}
  else pzemTariff.baselineValid=0;
  pzemTariffDirty=true;
  bool saved=pzemTariffSave(true);
  String j=F("{\"ok\":true,\"persisted\":");j+=saved?F("true"):F("false");j+='}';sendJson(200,j);
}

void handleRtcNtpSync() {
  if (!adminAuthorized()) { sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}")); return; }
  if (WiFi.status()!=WL_CONNECTED) { sendJson(503,F("{\"ok\":false,\"error\":\"Wi-Fi offline\"}")); return; }
  if (!ntpConfigured) { configTime((long)ntpUtcOffsetMin * 60L, 0, ntpServer.c_str(), NTP_FALLBACK_SERVER); ntpConfigured=true; }
  RtcDateTime ndt={0,0,0,0,0,0,false}; uint32_t deadline=millis()+3500;
  while(!ntpLocalDateTime(ndt) && (int32_t)(deadline-millis())>0) { delay(50); feedTaskWatchdog(); }
  if (!ndt.valid) { sendJson(504,F("{\"ok\":false,\"error\":\"NTP time unavailable\"}")); return; }
  RtcDateTime old=rtcCached;
  if(rtcPresent&&old.valid) ntpLastCorrectionSec=(int32_t)(rtcCivilSeconds(ndt)-rtcCivilSeconds(old)); else ntpLastCorrectionSec=0;
  if(!rtcWrite(ndt)){sendJson(500,F("{\"ok\":false,\"error\":\"RTC write failed\"}"));return;}
  rtcPresent=true;rtcCached=ndt;rtcCachedAtMs=millis();ntpEverSynced=true;ntpLastSyncMs=millis();ntpLastAttemptMs=millis();schedulerLastMinuteKey=0xFFFFFFFFUL;
  sendJson(200,F("{\"ok\":true,\"synced\":true}"));
}

void handleScheduleGet() {
  String j; j.reserve(1800); j=F("{\"ok\":true,\"tasks\":[");
  for (uint8_t i=0;i<SCHEDULE_TASK_COUNT;++i) {
    if (i) j+=',';
    const ScheduleTask&t=scheduleTasks[i];
    j+=F("{\"slot\":");j+=i;
    j+=F(",\"enabled\":");j+=t.enabled?F("true"):F("false");
    j+=F(",\"days\":");j+=t.daysMask;
    j+=F(",\"hour\":");j+=t.hour;
    j+=F(",\"minute\":");j+=t.minute;
    j+=F(",\"register\":");j+=t.reg;
    j+=F(",\"raw\":");j+=t.raw;
    j+='}';
  }
  j+=F("]}"); sendJson(200,j);
}

void handleScheduleSet() {
  if (!adminAuthorized()) { sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}")); return; }
  String body=web.arg("plain");
  double slotD,daysD,hD,mD,regD,rawD; bool enabled=false;
  if (!jsonFindNumber(body,"slot",slotD)||!jsonFindBool(body,"enabled",enabled)||
      !jsonFindNumber(body,"days",daysD)||!jsonFindNumber(body,"hour",hD)||!jsonFindNumber(body,"minute",mD)||
      !jsonFindNumber(body,"register",regD)||!jsonFindNumber(body,"raw",rawD)) {
    sendJson(400,F("{\"ok\":false,\"error\":\"slot/enabled/days/hour/minute/register/raw required\"}")); return;
  }
  int slot=(int)slotD;
  if (slot<0||slot>=SCHEDULE_TASK_COUNT||daysD<1||daysD>127||hD<0||hD>23||mD<0||mD>59||
      regD<0||regD>65535||rawD<0||rawD>65535) {
    sendJson(400,F("{\"ok\":false,\"error\":\"Invalid scheduler values\"}")); return;
  }
  uint16_t reg=(uint16_t)regD,raw=(uint16_t)rawD;
  String verr;
  if (!validateRaw(reg,raw,verr)) {
    String j=F("{\"ok\":false,\"error\":\"");j+=jsonEscape(verr);j+=F("\"}");sendJson(400,j);return;
  }
  scheduleTasks[slot].enabled=enabled;
  scheduleTasks[slot].daysMask=(uint8_t)daysD;
  scheduleTasks[slot].hour=(uint8_t)hD;
  scheduleTasks[slot].minute=(uint8_t)mD;
  scheduleTasks[slot].reg=reg;
  scheduleTasks[slot].raw=raw;
  saveScheduleTask((uint8_t)slot);
  sendJson(200,F("{\"ok\":true,\"saved\":true}"));
}

void appendMaskNamesJson(String& j, uint32_t mask, const char* const* names, uint8_t count) {
  j+='['; bool first=true;
  for(uint8_t bit=0;bit<count;++bit){
    if(!(mask&(1UL<<bit))) continue;
    if(!first) j+=','; first=false;
    j+=F("{\"bit\":"); j+=bit; j+=F(",\"text\":\""); j+=jsonEscape(String(names[bit])); j+=F("\"}");
  }
  j+=']';
}

void handleEvents() {
  String j; j.reserve(6000); j=F("{\"ok\":true,\"events\":[");
  uint8_t count,head;
  portENTER_CRITICAL(&inverterEventMux); count=inverterEventCount; head=inverterEventHead; portEXIT_CRITICAL(&inverterEventMux);
  for(uint8_t n=0;n<count;++n){
    uint8_t idx=(uint8_t)((head+INVERTER_EVENT_CAPACITY-1-n)%INVERTER_EVENT_CAPACITY); InverterEvent e;
    portENTER_CRITICAL(&inverterEventMux); e=inverterEvents[idx]; portEXIT_CRITICAL(&inverterEventMux);
    if(n)j+=','; j+=F("{\"id\":");j+=e.id; j+=F(",\"uptime_s\":");j+=e.uptimeSec;
    j+=F(",\"type\":\"");j+=e.type==1?F("fault"):(e.type==2?F("warning"):F("mode"));j+='"';
    j+=F(",\"severity\":\"");j+=e.severity;j+='"'; j+=F(",\"bit\":");if(e.bit==255)j+=F("null");else j+=e.bit;
    j+=F(",\"code\":");j+=e.code; j+=F(",\"active\":");j+=e.active?F("true"):F("false");
    j+=F(",\"message\":\"");j+=jsonEscape(String(e.message));j+=F("\"}");
  }
  j+=F("]}"); sendJson(200,j);
}

// ---------------- HTTP API ----------------
void sendJsonStatus() {
  String j;
  j.reserve(6500);

  j += F("{\"ok\":true");
  j += F(",\"firmware_version\":\""); j += FW_VERSION; j += '\"';
  j += F(",\"ota_last_ok\":"); j += devicePrefs.getBool("ota_last_ok", false) ? F("true") : F("false");
  j += F(",\"ota_last_from\":\""); j += jsonEscape(devicePrefs.getString("ota_last_from", "")); j += '\"';
  j += F(",\"ota_last_to\":\""); j += jsonEscape(devicePrefs.getString("ota_last_to", "")); j += '\"';
  j += F(",\"wifi_ip\":\""); j += WiFi.localIP().toString(); j += '"';
  j += F(",\"wifi_online\":"); j += (WiFi.status() == WL_CONNECTED) ? F("true") : F("false");
  j += F(",\"inverter_online\":"); j += inverterOnline() ? F("true") : F("false");
  j += F(",\"last_ok_age_ms\":");
  if (lastOkMs) j += (millis() - lastOkMs); else j += F("null");
  j += F(",\"rssi\":"); j += WiFi.RSSI();
  j += F(",\"uptime_s\":"); j += millis() / 1000;
  j += F(",\"modbus_slave\":"); j += modbusSlave;
  j += F(",\"rtu_tx\":"); j += rtuTx;
  j += F(",\"rtu_rx\":"); j += rtuRx;
  j += F(",\"rtu_errors\":"); j += rtuErrors;
  j += F(",\"rtu_timeouts\":"); j += rtuTimeouts;
  j += F(",\"web_writes\":"); j += webWrites;
  j += F(",\"rtc_present\":"); j += rtcPresent ? F("true") : F("false");
  j += F(",\"eeprom_present\":"); j += eepromPresent ? F("true") : F("false");
  j += F(",\"battery_efc\":"); j += String(batteryEfc(),2);
  j += F(",\"scheduler_runs\":"); j += schedulerRuns;
  j += F(",\"scheduler_errors\":"); j += schedulerErrors;
  j += F(",\"web_write_errors\":"); j += webWriteErrors;
  j += F(",\"task_wdt\":"); j += taskWdtEnabled ? F("true") : F("false");
  j += F(",\"rtu_poll_failures\":"); j += rtuConsecutivePollFailures;
  j += F(",\"rtu_uart_recoveries\":"); j += rtuUartRecoveries;
  j += F(",\"rtu_worker_stall_events\":"); j += rtuWorkerStallEvents;
  j += F(",\"rtu_controlled_reboots\":"); j += rtuControlledReboots;
  j += F(",\"inverter_ever_online\":"); j += inverterEverOnline ? F("true") : F("false");
  j += F(",\"inverter_offline_ms\":"); j += inverterOfflineSinceMs ? (uint32_t)(millis()-inverterOfflineSinceMs) : 0;
  j += F(",\"wifi_watchdog_reboots\":"); j += wifiWatchdogReboots;
  j += F(",\"free_heap\":"); j += ESP.getFreeHeap();
  j += F(",\"min_free_heap\":"); j += ESP.getMinFreeHeap();
  j += F(",\"low_heap_events\":"); j += lowHeapEvents;
  j += F(",\"loop_heartbeat\":"); j += loopHeartbeat;
  j += F(",\"reset_reason\":\""); j += resetReasonText(esp_reset_reason()); j += '"';
  j += F(",\"write_enabled\":"); j += ALLOW_WEB_WRITES ? F("true") : F("false");
  j += F(",\"write_busy\":"); j += webWriteBusReserved ? F("true") : F("false");
  j += F(",\"rtu_worker_busy\":"); j += rtuWorkerBusy ? F("true") : F("false");
  j += F(",\"rtu_queue_depth\":"); j += rtuJobQueue ? uxQueueMessagesWaiting(rtuJobQueue) : 0;
  j += F(",\"rtu_worker_heartbeat\":"); j += rtuWorkerHeartbeat;
  j += F(",\"rtu_worker_age_ms\":"); j += rtuWorkerLastMs ? (uint32_t)(millis()-rtuWorkerLastMs) : 0;
  j += F(",\"settings_refreshing\":"); j += (settingsRefreshBusy || settingsRefreshRequested) ? F("true") : F("false");
  j += F(",\"last_error\":\""); j += jsonEscape(String(lastErrorPublic)); j += '"';
  j += F(",\"age_ms\":"); j += telemetryUpdatedMs ? millis() - telemetryUpdatedMs : 0;
  j += F(",\"active_profile\":\""); j += jsonEscape(activeProfile); j += '"';
  j += F(",\"alerts\":{\"valid\":"); j += inverterDiagValid?F("true"):F("false");
  j += F(",\"age_ms\":"); if(inverterDiagUpdatedMs) j+=(uint32_t)(millis()-inverterDiagUpdatedMs); else j+=F("null");
  j += F(",\"fault_raw\":"); j += inverterFaultMask;
  j += F(",\"warning_raw\":"); j += inverterWarningMask;
  j += F(",\"operation_mode\":"); if(haveReg(201)) j+=telemetryRaw[1]; else j+=F("null");
  j += F(",\"operation_mode_text\":\""); j += haveReg(201)?inverterModeText((uint8_t)telemetryRaw[1]):"Unknown"; j += '"';
  j += F(",\"faults\":"); appendMaskNamesJson(j,inverterFaultMask,INVERTER_FAULTS,INVERTER_FAULT_COUNT);
  j += F(",\"warnings\":"); appendMaskNamesJson(j,inverterWarningMask,INVERTER_WARNINGS,INVERTER_WARNING_COUNT);
  j += F(",\"event_count\":"); j += inverterEventCount; j += '}';
  j += F(",\"pzem\":{\"enabled\":"); j += pzemEnabled?F("true"):F("false");
  j += F(",\"online\":"); j += pzemOnline?F("true"):F("false");
  j += F(",\"address\":"); j += pzemSlave;
  j += F(",\"age_ms\":"); if(pzemUpdatedMs) j+=(uint32_t)(millis()-pzemUpdatedMs); else j+=F("null");
  j += F(",\"voltage\":"); j += jsonNum(pzemVoltage,1);
  j += F(",\"current\":"); j += jsonNum(pzemCurrent,3);
  j += F(",\"power\":"); j += jsonNum(pzemPower,1);
  j += F(",\"energy_kwh\":"); j += jsonNum(pzemEnergyKwh,3);
  j += F(",\"frequency\":"); j += jsonNum(pzemFrequency,1);
  j += F(",\"pf\":"); j += jsonNum(pzemPf,2);
  j += F(",\"alarm\":"); j += pzemAlarm?F("true"):F("false");
  j += F(",\"poll_ok\":"); j += pzemPollOk; j += F(",\"poll_errors\":"); j += pzemPollErrors;
  j += F(",\"tariff\":"); j += currentTariff();
  j += F(",\"t1_kwh\":"); j += String((double)pzemTariff.t1_mWh/1000000.0,3);
  j += F(",\"t2_kwh\":"); j += String((double)pzemTariff.t2_mWh/1000000.0,3);
  j += F(",\"meter_total_kwh\":"); j += String((double)pzemTariffTotal_mWh()/1000000.0,3);
  j += F(",\"tariff_saved\":"); j += eepromPresent?F("true"):F("false"); j += '}';

  j += F(",\"telemetry\":{");
  j += F("\"mode\":"); j += jsonNum(regScaled(201, 1, false), 0);
  j += F(",\"grid_voltage\":"); j += jsonNum(regScaled(202, 10), 1);
  j += F(",\"grid_frequency\":"); j += jsonNum(regScaled(203, 100), 2);
  j += F(",\"grid_power\":"); j += jsonNum(regScaled(204, 1), 0);
  j += F(",\"inverter_voltage\":"); j += jsonNum(regScaled(205, 10), 1);
  j += F(",\"inverter_current\":"); j += jsonNum(regScaled(206, 10), 1);
  j += F(",\"inverter_frequency\":"); j += jsonNum(regScaled(207, 100), 2);
  j += F(",\"inverter_power\":"); j += jsonNum(regScaled(208, 1), 0);
  j += F(",\"inverter_charging_power\":"); j += jsonNum(regScaled(209, 1), 0);
  j += F(",\"output_voltage\":"); j += jsonNum(regScaled(210, 10), 1);
  j += F(",\"output_current\":"); j += jsonNum(regScaled(211, 10), 1);
  j += F(",\"output_frequency\":"); j += jsonNum(regScaled(212, 100), 2);
  j += F(",\"output_active_power\":"); j += jsonNum(regScaled(213, 1), 0);
  j += F(",\"output_apparent_power\":"); j += jsonNum(regScaled(214, 1), 0);
  j += F(",\"battery_voltage\":"); j += jsonNum(regScaled(215, 10), 1);
  j += F(",\"battery_current\":"); j += jsonNum(regScaled(216, 10), 1);
  j += F(",\"battery_power\":"); j += jsonNum(regScaled(217, 1), 0);
  j += F(",\"pv_voltage\":"); j += jsonNum(regScaled(219, 10), 1);
  j += F(",\"pv_current\":"); j += jsonNum(regScaled(220, 10), 1);
  j += F(",\"pv_power\":"); j += jsonNum(regScaled(223, 1), 0);
  j += F(",\"pv_charging_power\":"); j += jsonNum(regScaled(224, 1), 0);
  j += F(",\"load_percent\":"); j += jsonNum(regScaled(225, 1, false), 0);
  j += F(",\"dcdc_temperature\":"); j += jsonNum(regScaled(226, 1), 0);
  j += F(",\"inverter_temperature\":"); j += jsonNum(regScaled(227, 1), 0);
  j += F(",\"battery_soc\":"); j += jsonNum(regScaled(229, 1, false), 0);
  j += F(",\"net_battery_current\":"); j += jsonNum(regScaled(232, 10), 1);
  j += F(",\"inverter_charge_current\":"); j += jsonNum(regScaled(233, 10), 1);
  j += F(",\"pv_charge_current\":"); j += jsonNum(regScaled(234, 10), 1);
  j += F("}}");

  sendJson(200, j);
}

void handleRefresh() {
  telemetryForceRequested = true;
  sendJsonStatus();
}

// Short settings cache: avoids repeatedly hammering RS-485 while opening/reopening UI.
// ?refresh=1 forces a fresh batch read.
uint16_t settingsCacheRaw[SETTINGS_COUNT];
bool settingsCacheValid[SETTINGS_COUNT];
uint32_t settingsCacheMs = 0;
const uint32_t SETTINGS_CACHE_TTL_MS = 15000;

bool getRawCacheValue(uint16_t reg, uint16_t& raw) {
  // Telemetry cache (200..239) is continuously refreshed by the RTU worker.
  if (reg >= 200 && reg <= 239 && telemetryUpdatedMs && telemetryValid[reg - 200] &&
      (uint32_t)(millis() - telemetryUpdatedMs) <= 15000UL) {
    raw = telemetryRaw[reg - 200];
    return true;
  }
  // Settings cache contains registers that have already been read by settings refresh.
  int idx = settingIndexByReg(reg);
  if (idx >= 0 && settingsCacheValid[idx] && settingsCacheMs &&
      (uint32_t)(millis() - settingsCacheMs) <= SETTINGS_CACHE_TTL_MS) {
    raw = settingsCacheRaw[idx];
    return true;
  }
  return false;
}

void sendRawValues(uint16_t addr, uint16_t count, const uint16_t* values, const char* source) {
  String j=F("{\"ok\":true,\"addr\":"); j+=addr; j+=F(",\"count\":"); j+=count;
  j+=F(",\"values\":[");
  for(uint16_t i=0;i<count;++i){ if(i)j+=','; j+=values[i]; }
  j+=F("],\"source\":\""); j+=source; j+=F("\"}");
  sendJson(200,j);
}

void handleRaw() {
  String addrArg;
  if (web.hasArg("addr")) addrArg = web.arg("addr");
  else if (web.hasArg("start")) addrArg = web.arg("start");
  else { sendJson(400, F("{\"ok\":false,\"error\":\"addr/start required\"}")); return; }

  long av = addrArg.toInt();
  long cv = web.hasArg("count") ? web.arg("count").toInt() : 1;
  if (av < 0 || av > 65535 || cv < 1 || cv > 20 || av + cv - 1 > 65535) {
    sendJson(400, F("{\"ok\":false,\"error\":\"addr 0..65535, count 1..20\"}")); return;
  }
  const bool forceRefresh = web.hasArg("refresh") && web.arg("refresh") != "0";
  uint16_t cached[20]; bool allCached=!forceRefresh;
  if(allCached){
    for(uint16_t i=0;i<(uint16_t)cv;++i){
      if(!getRawCacheValue((uint16_t)av+i,cached[i])){ allCached=false; break; }
    }
  }
  if(allCached){ sendRawValues((uint16_t)av,(uint16_t)cv,cached,"cache"); return; }
  if (!rtuJobQueue) { sendJson(503, F("{\"ok\":false,\"error\":\"RTU worker unavailable\"}")); return; }

  RtuJob job = {};
  job.type = RTU_JOB_RAW; job.id = rtuNextJobId++; if(!job.id) job.id=rtuNextJobId++;
  job.addr=(uint16_t)av; job.count=(uint16_t)cv;
  portENTER_CRITICAL(&rawReadResultMux);
  rawReadResult.id=job.id; rawReadResult.done=false; rawReadResult.ok=false; rawReadResult.addr=job.addr; rawReadResult.count=job.count; rawReadResult.error[0]=0;
  portEXIT_CRITICAL(&rawReadResultMux);
  if (xQueueSend(rtuJobQueue,&job,0)!=pdTRUE) { sendJson(503,F("{\"ok\":false,\"error\":\"RTU queue full\"}")); return; }

  // RTU I/O remains owned exclusively by rtuWorkerTask.  The HTTP request waits
  // briefly for that worker so API clients receive values in this same request.
  const uint32_t deadline=millis()+2500UL;
  while((int32_t)(deadline-millis())>0){
    RawReadResult r;
    portENTER_CRITICAL(&rawReadResultMux); r=rawReadResult; portEXIT_CRITICAL(&rawReadResultMux);
    if(r.id==job.id && r.done){
      if(r.ok){ sendRawValues(r.addr,r.count,r.values,"rtu"); return; }
      String j=F("{\"ok\":false,\"addr\":");j+=r.addr;j+=F(",\"count\":");j+=r.count;
      j+=F(",\"error\":\"");j+=jsonEscape(String(r.error));j+=F("\"}");sendJson(502,j);return;
    }
    feedTaskWatchdog();
    delay(10);
  }
  // Compatibility fallback: unusually slow RTU operations can still be polled
  // through the pre-existing /api/raw/status endpoint.
  String j=F("{\"ok\":true,\"queued\":true,\"job_id\":"); j+=job.id;
  j+=F(",\"pending\":true,\"status\":\"/api/raw/status?id=");j+=job.id;j+=F("\"}");
  sendJson(202,j);
}

void handleRawStatus() {
  uint32_t wantId=web.hasArg("id")?(uint32_t)strtoul(web.arg("id").c_str(),nullptr,10):0;
  RawReadResult r;
  portENTER_CRITICAL(&rawReadResultMux); r=rawReadResult; portEXIT_CRITICAL(&rawReadResultMux);
  if(!wantId || r.id!=wantId || !r.done){ sendJson(200,F("{\"ok\":true,\"done\":false}")); return; }
  String j=F("{\"ok\":"); j+=r.ok?F("true"):F("false"); j+=F(",\"done\":true,\"addr\":"); j+=r.addr; j+=F(",\"count\":"); j+=r.count;
  j+=F(",\"values\":["); if(r.ok){ for(uint16_t i=0;i<r.count;++i){ if(i)j+=','; j+=r.values[i]; } } j+=F("]");
  j+=F(",\"error\":\""); j+=jsonEscape(String(r.error)); j+=F("\"}"); sendJson(200,j);
}


void settingsCacheInvalidate() {
  settingsCacheMs = 0;
  for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
    settingsCacheValid[i] = false;
  }
}

int settingIndexByReg(uint16_t reg) {
  for (size_t i=0;i<SETTINGS_COUNT;++i) if (SETTINGS[i].reg==reg) return (int)i;
  return -1;
}

bool getCachedSettingRaw(uint16_t reg, uint16_t& raw) {
  int idx=settingIndexByReg(reg);
  if(idx<0 || !settingsCacheValid[idx]) return false;
  raw=settingsCacheRaw[idx];
  return true;
}

void cacheSettingsBlock(uint16_t startReg, uint16_t count) {
  uint16_t vals[20];
  if (count > 20) return;
  bool ok = readHolding(startReg, count, vals);
  if (!ok) return;
  for (uint16_t k=0;k<count;++k) {
    int idx=settingIndexByReg(startReg+k);
    if (idx>=0) {
      settingsCacheRaw[idx]=vals[k];
      settingsCacheValid[idx]=true;
    }
  }
}

void refreshSettingsCache() {
  for (size_t i=0;i<SETTINGS_COUNT;++i) settingsCacheValid[i]=false;

  // 6 RTU transactions instead of ~30 individual reads.
  cacheSettingsBlock(300,11);  // 300..310
  cacheSettingsBlock(313,1);
  cacheSettingsBlock(320,18);  // 320..337
  cacheSettingsBlock(341,4);   // 341..344
  cacheSettingsBlock(351,1);
  cacheSettingsBlock(406,1);

  settingsCacheMs=millis();
}

void handleSettings() {
  bool force = web.hasArg("refresh") && web.arg("refresh") == "1";
  bool cacheFresh = settingsCacheMs &&
                    ((uint32_t)(millis()-settingsCacheMs) < SETTINGS_CACHE_TTL_MS);

  if ((force || !cacheFresh) && !settingsRefreshBusy) settingsRefreshRequested = true;

  String j;
  j.reserve(6500);
  j += F("{\"ok\":true,\"write_enabled\":");
  j += ALLOW_WEB_WRITES ? F("true") : F("false");
  j += F(",\"cached\":");
  j += cacheFresh ? F("true") : F("false");
  j += F(",\"refreshing\":"); j += (settingsRefreshBusy || settingsRefreshRequested) ? F("true") : F("false");
  j += F(",\"refresh_generation\":"); j += settingsRefreshGeneration;
  j += F(",\"cache_age_ms\":");
  j += settingsCacheMs ? (uint32_t)(millis()-settingsCacheMs) : 0;
  j += F(",\"data\":[");

  for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
    if (i) j += ',';
    const SettingDef &d=SETTINGS[i];

    j += F("{\"reg\":"); j += d.reg;
    j += F(",\"group\":\""); j += jsonEscape(d.group); j += '"';
    j += F(",\"label\":\""); j += jsonEscape(d.label); j += '"';
    j += F(",\"kind\":\""); j += d.kind == SK_SELECT ? F("select") : F("number"); j += '"';
    j += F(",\"scale\":"); j += String(d.scale, 1);
    j += F(",\"min\":"); j += String(d.minVal, 1);
    j += F(",\"max\":"); j += String(d.maxVal, 1);
    j += F(",\"ok\":"); j += settingsCacheValid[i] ? F("true") : F("false");

    if (settingsCacheValid[i]) {
      uint16_t v=settingsCacheRaw[i];
      j += F(",\"raw\":"); j += v;
      j += F(",\"value\":"); j += String(((float)v) / d.scale, 1);
    } else {
      j += F(",\"error\":\"read failed\"");
    }
    j += '}';
  }

  j += F("]}");
  sendJson(200, j);
}

void handleProfiles() {
  BatterySystemDetect d = detectBatterySystem();
  bool c24 = d.verified && d.systemV == 24;
  bool c48 = d.verified && d.systemV == 48;

  String j;
  j.reserve(3600);
  j = F("{\"ok\":true,\"write_enabled\":");
  j += ALLOW_WEB_WRITES ? F("true") : F("false");
  j += F(",\"detected_system_v\":"); if(d.systemV) j+=d.systemV; else j+=F("null");
  j += F(",\"system_verified\":"); j += d.verified ? F("true") : F("false");
  j += F(",\"detect_reason\":\""); j += jsonEscape(d.reason); j += '"';
  j += F(",\"battery_v\":"); if(isnan(d.batteryV)) j+=F("null"); else j+=String(d.batteryV,1);
  j += F(",\"bulk_v\":"); if(isnan(d.bulkV)) j+=F("null"); else j+=String(d.bulkV,1);
  j += F(",\"float_v\":"); if(isnan(d.floatV)) j+=F("null"); else j+=String(d.floatV,1);
  j += F(",\"active_profile\":\""); j += jsonEscape(activeProfile); j += '"';
  j += F(",\"profiles\":{");

  j += F("\"ANJ_24V_8S_1P\":{\"label\":\"1× LiFePO4 8S / 25.6V / 100Ah\",\"description\":\"24 V system: one 8S LiFePO4 battery.\",\"system_v\":24,\"compatible\":");
  j += c24?F("true"):F("false");
  j += F(",\"capacity_ah\":100,\"energy_kwh\":2.56,\"bulk_v\":28.0,\"float_v\":27.2,\"low_mains_v\":23.2,\"cutoff_v\":22.0,\"max_charge_a\":15,\"max_utility_charge_a\":10,\"max_discharge_a\":80,\"reserve_soc\":15}");

  j += F(",\"ANJ_24V_8S_2P\":{\"label\":\"2× LiFePO4 8S parallel / 25.6V / 200Ah\",\"description\":\"24 V system: two identical 8S batteries in parallel.\",\"system_v\":24,\"compatible\":");
  j += c24?F("true"):F("false");
  j += F(",\"capacity_ah\":200,\"energy_kwh\":5.12,\"bulk_v\":28.0,\"float_v\":27.2,\"low_mains_v\":23.2,\"cutoff_v\":22.0,\"max_charge_a\":15,\"max_utility_charge_a\":10,\"max_discharge_a\":120,\"reserve_soc\":15}");

  j += F(",\"ANJ_48V_STANDARD\":{\"label\":\"2× ANJ 25.6V series / 51.2V — Standard\",\"description\":\"48 V profile from the original Python application: two 25.6 V batteries in series.\",\"system_v\":48,\"compatible\":");
  j += c48?F("true"):F("false");
  j += F(",\"capacity_ah\":100,\"energy_kwh\":5.12,\"bulk_v\":56.4,\"float_v\":54.0,\"low_mains_v\":46.0,\"cutoff_v\":44.0,\"max_charge_a\":10,\"max_utility_charge_a\":10,\"max_discharge_a\":95,\"reserve_soc\":15}");

  j += F(",\"ANJ_48V_LONG_LIFE\":{\"label\":\"2× ANJ 25.6V series / 51.2V — Long Life\",\"description\":\"Long-life 48 V profile from the original Python application: two 25.6 V batteries in series.\",\"system_v\":48,\"compatible\":");
  j += c48?F("true"):F("false");
  j += F(",\"capacity_ah\":100,\"energy_kwh\":5.12,\"bulk_v\":56.0,\"float_v\":54.0,\"low_mains_v\":46.0,\"cutoff_v\":44.0,\"max_charge_a\":10,\"max_utility_charge_a\":10,\"max_discharge_a\":80,\"reserve_soc\":15}");

  j += F("}}");
  sendJson(200,j);
}

void publishWebWriteResult(uint32_t id, bool ok, bool verified, uint16_t reg,
                           uint16_t oldRaw, uint16_t newRaw, const String& error) {
  portENTER_CRITICAL(&webWriteResultMux);
  webWriteResult.id = id;
  webWriteResult.done = true;
  webWriteResult.ok = ok;
  webWriteResult.verified = verified;
  webWriteResult.reg = reg;
  webWriteResult.oldRaw = oldRaw;
  webWriteResult.newRaw = newRaw;
  size_t n = error.length();
  if (n >= sizeof(webWriteResult.error)) n = sizeof(webWriteResult.error) - 1;
  memcpy(webWriteResult.error, error.c_str(), n);
  webWriteResult.error[n] = 0;
  portEXIT_CRITICAL(&webWriteResultMux);
}

void copyError(char* dst, size_t dstSize, const String& err) {
  size_t n=err.length(); if(n>=dstSize)n=dstSize-1; memcpy(dst,err.c_str(),n); dst[n]=0;
}

void publishRawResult(const RtuJob& job, bool ok, const uint16_t* vals, const String& err) {
  portENTER_CRITICAL(&rawReadResultMux);
  rawReadResult.id=job.id; rawReadResult.done=true; rawReadResult.ok=ok; rawReadResult.addr=job.addr; rawReadResult.count=job.count;
  if(ok && vals) for(uint16_t i=0;i<job.count && i<20;++i) rawReadResult.values[i]=vals[i];
  copyError(rawReadResult.error,sizeof(rawReadResult.error),err);
  portEXIT_CRITICAL(&rawReadResultMux);
}

void publishProfileResult(uint32_t id, bool ok, bool partial, const String& profile, const String& err,
                          uint16_t attempted, uint16_t applied, uint16_t failed,
                          const ProfileFailure* failures) {
  portENTER_CRITICAL(&profileApplyResultMux);
  profileApplyResult = {};
  profileApplyResult.id=id; profileApplyResult.done=true; profileApplyResult.ok=ok; profileApplyResult.partial=partial;
  profileApplyResult.attempted=attempted; profileApplyResult.applied=applied; profileApplyResult.failed=failed;
  copyError(profileApplyResult.profile,sizeof(profileApplyResult.profile),profile);
  copyError(profileApplyResult.error,sizeof(profileApplyResult.error),err);
  const uint16_t n = failed < 16 ? failed : 16;
  for(uint16_t i=0;i<n;++i){
    profileApplyResult.failures[i].reg=failures[i].reg;
    profileApplyResult.failures[i].raw=failures[i].raw;
    copyError(profileApplyResult.failures[i].error,sizeof(profileApplyResult.failures[i].error),String(failures[i].error));
  }
  portEXIT_CRITICAL(&profileApplyResultMux);
}

bool performCheckedWrite(uint16_t reg, uint16_t raw, uint16_t& oldRaw, uint16_t& newRaw, String& err) {
  bool ok=true;
  if(reg==333){ uint16_t maxCharge=0; if(!readHolding(332,1,&maxCharge,RTU_WRITE_TIMEOUT_MS)){ok=false;err=lastError;} else if(raw>maxCharge){ok=false;err=F("Utility charge current cannot exceed total max charge current");} }
  if(ok && reg==332){ uint16_t utility=0; if(!readHolding(333,1,&utility,RTU_WRITE_TIMEOUT_MS)){ok=false;err=lastError;} else if(utility>raw){ok=false;err=F("Total max charge current cannot be below utility charge current");} }
  if(ok){ ok=writeAndVerify(reg,raw,oldRaw,newRaw); if(!ok)err=lastError.length()?lastError:F("Write/read-back failed"); }
  if(ok){ int idx=settingIndexByReg(reg); if(idx>=0){settingsCacheRaw[idx]=newRaw; settingsCacheValid[idx]=true;} if(settingsCacheMs) settingsCacheMs=millis(); if(isBatteryVoltageRegister(reg))batteryDetectCacheMs=0; }
  return ok;
}

void rtuWorkerTask(void* parameter) {
  uint32_t workerLastPoll=0;
  for(;;){
    rtuWorkerHeartbeat++;
    rtuWorkerLastMs=millis();
    if(otaInProgress){ vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    RtuJob job={};
    bool haveJob = rtuJobQueue && xQueueReceive(rtuJobQueue,&job,pdMS_TO_TICKS(20))==pdTRUE;
    if(haveJob){
      rtuWorkerBusy=true;
      if(job.type==RTU_JOB_WRITE){
        webWriteBusReserved=true;
        uint16_t oldRaw=0,newRaw=0; String err;
        bool ok=performCheckedWrite(job.reg,job.raw,oldRaw,newRaw,err);
        if(!ok) webWriteErrors++;
        publishWebWriteResult(job.id,ok,ok,job.reg,oldRaw,newRaw,err);
        rtuQuietUntilMs=millis()+POST_WRITE_QUIET_MS;
        webWriteBusReserved=false;
      } else if(job.type==RTU_JOB_CAL_START){
        webWriteBusReserved=true;
        uint16_t oldRaw=0,newRaw=0; String err;
        bool ok=performCheckedWrite(301,2,oldRaw,newRaw,err);
        if(ok){
          batteryCalibration = {};
          batteryCalibration.active=true; batteryCalibration.startSoc=(uint8_t)job.addr; batteryCalibration.endSoc=(uint8_t)job.count;
          batteryCalibration.startCharged_mAh=batteryStats.charged_mAh; batteryCalibration.startDischarged_mAh=batteryStats.discharged_mAh;
          batteryCalibration.startCharged_mWh=batteryStats.charged_mWh; batteryCalibration.startDischarged_mWh=batteryStats.discharged_mWh;
          batteryCalibration.previousOutputPriority=oldRaw; batteryCalibration.previousOutputPriorityValid=true;
          batteryCalibration.startedMs=millis(); batteryCalibration.netDischarge_mWh=0;
          snprintf(batteryCalibration.status,sizeof(batteryCalibration.status),"controlled discharge: reg301 %u -> 2 (SBU); target %u%%",oldRaw,batteryCalibration.endSoc);
          batteryStatsDirty=true;
        } else webWriteErrors++;
        publishWebWriteResult(job.id,ok,ok,301,oldRaw,newRaw,err);
        rtuQuietUntilMs=millis()+POST_WRITE_QUIET_MS; webWriteBusReserved=false;
      } else if(job.type==RTU_JOB_CAL_RESTORE){
        webWriteBusReserved=true;
        uint16_t oldRaw=0,newRaw=0; String err;
        bool ok=performCheckedWrite(301,job.raw,oldRaw,newRaw,err);
        batteryCalibration.restoreQueued=false;
        if(ok){batteryCalibration.outputRestored=true;snprintf(batteryCalibration.status,sizeof(batteryCalibration.status),"battery discharge stopped; reg301 restored to %u",job.raw);}
        else {snprintf(batteryCalibration.status,sizeof(batteryCalibration.status),"failed to restore reg301=%u: %s",job.raw,err.c_str());}
        rtuQuietUntilMs=millis()+POST_WRITE_QUIET_MS; webWriteBusReserved=false;
      } else if(job.type==RTU_JOB_PZEM_RESET){
        // Keep UART2 single-owner invariant. Reset only PZEM internal kWh; virtual T1/T2 stay intact.
        String err; bool ok=resetPzemEnergyAndVerify(err);
        publishWebWriteResult(job.id,ok,ok,0,0,0,err);
        webWriteBusReserved=false;
      } else if(job.type==RTU_JOB_RAW){
        uint16_t vals[20]={0}; bool ok=readHolding(job.addr,job.count,vals,RTU_TIMEOUT_MS); String err=ok?String():lastError;
        publishRawResult(job,ok,vals,err);
      } else if(job.type==RTU_JOB_SCHEDULE){
        webWriteBusReserved=true;
        uint16_t oldRaw=0,newRaw=0; String err; bool ok=performCheckedWrite(job.reg,job.raw,oldRaw,newRaw,err);
        if(ok){ schedulerRuns++; schedulerLast=String("Task ")+(job.scheduleSlot+1)+": reg "+job.reg+" "+oldRaw+" -> "+newRaw; }
        else { schedulerErrors++; schedulerLast=String("Task ")+(job.scheduleSlot+1)+": "+err; }
        rtuQuietUntilMs=millis()+POST_WRITE_QUIET_MS; webWriteBusReserved=false;
      } else if(job.type==RTU_JOB_PROFILE){
        webWriteBusReserved=true;
        const ProfileReg* rows=nullptr; size_t count=0; String profile;
        if(job.profileId==1){rows=PROFILE_1P;count=sizeof(PROFILE_1P)/sizeof(PROFILE_1P[0]);profile="ANJ_24V_8S_1P";}
        else if(job.profileId==2){rows=PROFILE_2P;count=sizeof(PROFILE_2P)/sizeof(PROFILE_2P[0]);profile="ANJ_24V_8S_2P";}
        else if(job.profileId==3){rows=PROFILE_48V_STANDARD;count=sizeof(PROFILE_48V_STANDARD)/sizeof(PROFILE_48V_STANDARD[0]);profile="ANJ_48V_STANDARD";}
        else if(job.profileId==4){rows=PROFILE_48V_LONG_LIFE;count=sizeof(PROFILE_48V_LONG_LIFE)/sizeof(PROFILE_48V_LONG_LIFE[0]);profile="ANJ_48V_LONG_LIFE";}
        bool ok=rows!=nullptr; bool partial=false; String err;
        uint16_t attempted=0,applied=0,failed=0;
        ProfileFailure failures[16] = {};
        if(ok){
          for(size_t i=0;i<count;++i){
            attempted++;
            String oneErr;
            bool oneOk=validateRaw(rows[i].reg,rows[i].raw,oneErr);
            uint16_t oldRaw=0,newRaw=0;
            if(oneOk) oneOk=performCheckedWrite(rows[i].reg,rows[i].raw,oldRaw,newRaw,oneErr);
            if(oneOk){
              applied++;
            } else {
              webWriteErrors++;
              if(failed<16){
                failures[failed].reg=rows[i].reg;
                failures[failed].raw=rows[i].raw;
                copyError(failures[failed].error,sizeof(failures[failed].error),oneErr.length()?oneErr:F("Write/read-back failed"));
              }
              failed++;
            }
            // A rejected register (for example Modbus exception 0x03) must not abort the profile.
            vTaskDelay(pdMS_TO_TICKS(80));
          }
          partial=failed>0;
          // Keep the selected profile for battery capacity/statistics even if some registers were rejected.
          activeProfile=profile;
          appPrefs.putString("profile",activeProfile);
          settingsCacheMs=millis();
          batteryDetectCacheMs=0;
          if(partial){
            err=F("Profile selected; ");
            err+=applied; err+=F("/"); err+=attempted;
            err+=F(" registers written, "); err+=failed; err+=F(" failed");
          }
        } else {
          err=F("Unknown profile job");
        }
        publishProfileResult(job.id,ok,partial,profile,err,attempted,applied,failed,failures);
        rtuQuietUntilMs=millis()+POST_WRITE_QUIET_MS; webWriteBusReserved=false;
      }
      syncLastErrorPublic();
      rtuWorkerBusy=false;
    }

    if(settingsRefreshRequested && !webWriteBusReserved){
      settingsRefreshRequested=false; settingsRefreshBusy=true; rtuWorkerBusy=true;
      refreshSettingsCache(); settingsRefreshGeneration++; batteryDetectCacheMs=0; syncLastErrorPublic();
      settingsRefreshBusy=false; rtuWorkerBusy=false;
    }

    uint32_t now=millis();
    uint32_t every=inverterOnline()?POLL_INTERVAL_MS:POLL_OFFLINE_INTERVAL_MS;
    if(!setupMode && !webWriteBusReserved && (telemetryForceRequested || (uint32_t)(now-workerLastPoll)>=every)){
      telemetryForceRequested=false; workerLastPoll=now; rtuWorkerBusy=true; pollTelemetry(); syncLastErrorPublic(); rtuWorkerBusy=false;
    }
    now=millis();
    if(!setupMode && !webWriteBusReserved && pzemEnabled && (uint32_t)(now-pzemLastPollMs)>=PZEM_POLL_INTERVAL_MS){
      pzemLastPollMs=now; rtuWorkerBusy=true; pollPzem016(); syncLastErrorPublic(); rtuWorkerBusy=false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void handleWriteStatus() {
  uint32_t wantId = web.hasArg("id") ? (uint32_t)strtoul(web.arg("id").c_str(), nullptr, 10) : 0;
  WebWriteResult r;
  portENTER_CRITICAL(&webWriteResultMux);
  r = webWriteResult;
  portEXIT_CRITICAL(&webWriteResultMux);

  if (!wantId || r.id != wantId || !r.done) {
    String j = F("{\"ok\":true,\"done\":false,\"busy\":");
    j += webWriteBusReserved ? F("true") : F("false");
    j += '}';
    sendJson(200, j);
    return;
  }

  String j = F("{\"ok\":"); j += r.ok ? F("true") : F("false");
  j += F(",\"done\":true,\"verified\":"); j += r.verified ? F("true") : F("false");
  j += F(",\"register\":"); j += r.reg;
  j += F(",\"old_raw\":"); j += r.oldRaw;
  j += F(",\"new_raw\":"); j += r.newRaw;
  const SettingDef* sd = findSetting(r.reg);
  j += F(",\"new_value\":");
  if (sd && sd->kind != SK_SELECT) j += String(((float)r.newRaw) / sd->scale, (unsigned int)(sd->scale == 10 ? 1 : 0));
  else j += r.newRaw;
  j += F(",\"error\":\""); j += jsonEscape(String(r.error)); j += F("\"}");
  sendJson(200, j);
}

void handleWrite() {
  if (!ALLOW_WEB_WRITES) {
    sendJson(403, F("{\"ok\":false,\"error\":\"Web writes disabled\"}"));
    return;
  }
  if (!rtuJobQueue) {
    sendJson(503, F("{\"ok\":false,\"error\":\"Write worker unavailable\"}"));
    return;
  }
  if (webWriteBusReserved || uxQueueMessagesWaiting(rtuJobQueue) >= 5) {
    sendJson(409, F("{\"ok\":false,\"error\":\"Another write is already in progress\"}"));
    return;
  }

  String body = web.arg("plain");
  double regD, valueD;
  if (!jsonFindNumber(body, "register", regD) || !jsonFindNumber(body, "value", valueD)) {
    sendJson(400, F("{\"ok\":false,\"error\":\"JSON register/value required\"}"));
    return;
  }

  uint16_t reg = (uint16_t)regD;
  const SettingDef* sd = findSetting(reg);
  if (!sd || !sd->writable) {
    sendJson(403, F("{\"ok\":false,\"error\":\"Register not writable\"}"));
    return;
  }

  uint16_t raw = (sd->kind == SK_SELECT) ? (uint16_t)lround(valueD)
                                          : (uint16_t)lround(valueD * sd->scale);
  String verr;
  if (!validateRaw(reg, raw, verr)) {
    String j = F("{\"ok\":false,\"error\":\"");
    j += jsonEscape(verr); j += F("\"}");
    sendJson(400, j);
    return;
  }

  RtuJob job = {};
  job.type = RTU_JOB_WRITE;
  job.id = rtuNextJobId++;
  if (job.id == 0) job.id = rtuNextJobId++;
  job.reg = reg;
  job.raw = raw;

  // The RTU worker serializes this with every other Modbus operation.
  webWriteBusReserved = true;
  portENTER_CRITICAL(&webWriteResultMux);
  webWriteResult.id = job.id;
  webWriteResult.done = false;
  webWriteResult.ok = false;
  webWriteResult.verified = false;
  webWriteResult.reg = reg;
  webWriteResult.oldRaw = 0;
  webWriteResult.newRaw = 0;
  webWriteResult.error[0] = 0;
  portEXIT_CRITICAL(&webWriteResultMux);

  if (xQueueSend(rtuJobQueue, &job, 0) != pdTRUE) {
    webWriteBusReserved = false;
    sendJson(503, F("{\"ok\":false,\"error\":\"Write queue is full\"}"));
    return;
  }

  String j = F("{\"ok\":true,\"queued\":true,\"job_id\":");
  j += job.id;
  j += F(",\"register\":"); j += reg;
  j += F("}");
  sendJson(202, j);
}

void handleProfileApply() {
  if(!ALLOW_WEB_WRITES){sendJson(403,F("{\"ok\":false,\"error\":\"Web writes disabled\"}"));return;}
  if(!rtuJobQueue){sendJson(503,F("{\"ok\":false,\"error\":\"RTU worker unavailable\"}"));return;}
  String profile; if(!jsonFindString(web.arg("plain"),"profile",profile)){sendJson(400,F("{\"ok\":false,\"error\":\"profile required\"}"));return;}
  uint8_t pid=0,requiredSystem=0;
  if(profile=="ANJ_24V_8S_1P"){pid=1;requiredSystem=24;} else if(profile=="ANJ_24V_8S_2P"){pid=2;requiredSystem=24;}
  else if(profile=="ANJ_48V_STANDARD"){pid=3;requiredSystem=48;} else if(profile=="ANJ_48V_LONG_LIFE"){pid=4;requiredSystem=48;}
  else {sendJson(400,F("{\"ok\":false,\"error\":\"Unknown profile\"}"));return;}
  BatterySystemDetect d=detectBatterySystem();
  if(!d.verified || d.systemV!=requiredSystem){ String e=F("{\"ok\":false,\"error\":\"Profile blocked: battery system not verified as ");e+=requiredSystem;e+=F(" V\"}");sendJson(409,e);return; }
  RtuJob job={};job.type=RTU_JOB_PROFILE;job.id=rtuNextJobId++;job.profileId=pid;
  portENTER_CRITICAL(&profileApplyResultMux);profileApplyResult={};profileApplyResult.id=job.id;profileApplyResult.done=false;portEXIT_CRITICAL(&profileApplyResultMux);
  if(xQueueSend(rtuJobQueue,&job,0)!=pdTRUE){sendJson(503,F("{\"ok\":false,\"error\":\"RTU queue full\"}"));return;}
  String j=F("{\"ok\":true,\"queued\":true,\"job_id\":");j+=job.id;j+=F("}");sendJson(202,j);
}

void handleProfileStatus(){
  uint32_t wantId=web.hasArg("id")?(uint32_t)strtoul(web.arg("id").c_str(),nullptr,10):0; ProfileApplyResult r;
  portENTER_CRITICAL(&profileApplyResultMux);r=profileApplyResult;portEXIT_CRITICAL(&profileApplyResultMux);
  if(!wantId||r.id!=wantId||!r.done){sendJson(200,F("{\"ok\":true,\"done\":false}"));return;}
  String j=F("{\"ok\":");j+=r.ok?F("true"):F("false");
  j+=F(",\"done\":true,\"partial\":");j+=r.partial?F("true"):F("false");
  j+=F(",\"profile\":\"");j+=jsonEscape(String(r.profile));j+=F("\"");
  j+=F(",\"attempted\":");j+=r.attempted;
  j+=F(",\"applied\":");j+=r.applied;
  j+=F(",\"failed\":");j+=r.failed;
  j+=F(",\"failures\":[");
  const uint16_t n=r.failed<16?r.failed:16;
  for(uint16_t i=0;i<n;++i){
    if(i)j+=',';
    j+=F("{\"reg\":");j+=r.failures[i].reg;
    j+=F(",\"raw\":");j+=r.failures[i].raw;
    j+=F(",\"error\":\"");j+=jsonEscape(String(r.failures[i].error));j+=F("\"}");
  }
  j+=F("],\"error\":\"");j+=jsonEscape(String(r.error));j+=F("\"}");
  sendJson(200,j);
}


// ---------------- EyeBond / SmartESS datalogger service ----------------
static uint16_t dlBe16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }
static void dlPut16(uint8_t* p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }

bool dlWaitBytes(WiFiClient& c, uint8_t* dst, size_t len, uint32_t timeoutMs) {
  size_t got=0; uint32_t started=millis();
  while(got<len && (uint32_t)(millis()-started)<timeoutMs) {
    while(c.available() && got<len) { int v=c.read(); if(v>=0) dst[got++]=(uint8_t)v; }
    if(!c.connected() && !c.available()) break;
    feedTaskWatchdog(); delay(2);
  }
  return got==len;
}

bool dlWriteFrame(WiFiClient& c, uint16_t tid, uint8_t fcode, const uint8_t* body, size_t bodyLen) {
  if(bodyLen>240) return false;
  uint8_t frame[248];
  dlPut16(frame,tid); dlPut16(frame+2,1); dlPut16(frame+4,(uint16_t)(bodyLen+2));
  frame[6]=0xff; frame[7]=fcode;
  if(bodyLen) memcpy(frame+8,body,bodyLen);
  return c.write(frame,bodyLen+8)==bodyLen+8;
}

bool dlReadFrame(WiFiClient& c, uint8_t expectedFcode, uint8_t* body, size_t bodyCap,
                 size_t& bodyLen, String& err, uint32_t timeoutMs=1800) {
  uint8_t h[8]; bodyLen=0;
  if(!dlWaitBytes(c,h,sizeof(h),timeoutMs)){err="TCP response header timeout";return false;}
  uint16_t wireLen=dlBe16(h+4);
  if(wireLen<2){err="Invalid datalogger frame length";return false;}
  bodyLen=wireLen-2;
  if(bodyLen>bodyCap){err="Datalogger frame too large";return false;}
  if(h[7]!=expectedFcode){err=String("Unexpected function code ")+h[7];return false;}
  if(bodyLen && !dlWaitBytes(c,body,bodyLen,timeoutMs)){err="TCP response body timeout";return false;}
  return true;
}

bool dlTargetFromBody(const String& body, IPAddress& target, String& targetText, String& err) {
  targetText=""; jsonFindString(body,"ip",targetText);
  if(!targetText.length() && dataloggerServiceMode) targetText=WiFi.gatewayIP().toString();
  if(!targetText.length()){err="Datalogger IP required";return false;}
  if(!target.fromString(targetText)){err="Invalid datalogger IP";return false;}
  return true;
}

bool dlOpenSession(IPAddress target, WiFiClient& client, String& err) {
  if(WiFi.status()!=WL_CONNECTED){err="ESP32 STA is not connected";return false;}
  IPAddress local=WiFi.localIP();
  if(local.toString()=="0.0.0.0"){err="ESP32 has no STA IP";return false;}
  dataloggerServer.begin(); dataloggerServer.setNoDelay(true);
  dataloggerUdp.stop();
  if(!dataloggerUdp.begin(0)){err="UDP start failed";return false;}
  String cmd=String("set>server=")+local.toString()+":"+String(DATALOGGER_TCP_PORT)+";";
  if(!dataloggerUdp.beginPacket(target,DATALOGGER_UDP_PORT)){err="UDP beginPacket failed";return false;}
  dataloggerUdp.write((const uint8_t*)cmd.c_str(),cmd.length());
  if(!dataloggerUdp.endPacket()){err="UDP send failed";return false;}
  String rsp; uint32_t started=millis();
  while((uint32_t)(millis()-started)<5000UL){
    int n=dataloggerUdp.parsePacket();
    if(n>0){while(n-->0){int ch=dataloggerUdp.read();if(ch>=0)rsp+=(char)ch;}break;}
    feedTaskWatchdog(); delay(5);
  }
  if(!rsp.startsWith("rsp>server=")){err=String("Unexpected UDP reply: ")+rsp;return false;}
  started=millis();
  while((uint32_t)(millis()-started)<5000UL){
    WiFiClient c=dataloggerServer.available();
    if(c){client=c;client.setNoDelay(true);dataloggerLastTarget=target.toString();return true;}
    feedTaskWatchdog();delay(5);
  }
  err="Datalogger did not connect to TCP 8899";return false;
}

void dlCloseSession(WiFiClient& c){if(c)c.stop();dataloggerUdp.stop();}

String dlInfoJson(IPAddress target,String& err){
  WiFiClient c;if(!dlOpenSession(target,c,err))return "";
  const uint8_t pars[]={1,2,5,6,7,11,12,48,3,4,14,34,41};
  if(!dlWriteFrame(c,1,2,pars,sizeof(pars))){err="Info request write failed";dlCloseSession(c);return "";}
  String j="{\"ok\":true,\"ip\":\""+target.toString()+"\",\"params\":{";bool first=true;
  for(size_t i=0;i<sizeof(pars);++i){uint8_t b[220];size_t n=0;if(!dlReadFrame(c,2,b,sizeof(b),n,err)){dlCloseSession(c);return "";}if(n<2){err="Short info response";dlCloseSession(c);return "";}uint8_t par=b[1];String val;for(size_t k=2;k<n;++k)val+=(char)b[k];if(!first)j+=',';first=false;j+='"';j+=par;j+=F("\":\"");j+=jsonEscape(val);j+='"';}
  j+=F("}}");dlCloseSession(c);dataloggerLastInfo=j;dataloggerLastOkMs=millis();return j;
}

void handleDataloggerStatus(){
  String j=F("{\"ok\":true,\"experimental\":true,\"service_mode\":");j+=dataloggerServiceMode?F("true"):F("false");
  j+=F(",\"sta_connected\":");j+=(WiFi.status()==WL_CONNECTED)?F("true"):F("false");
  j+=F(",\"sta_ssid\":\"");j+=jsonEscape(WiFi.SSID());j+='"';
  j+=F(",\"sta_ip\":\"");j+=WiFi.localIP().toString();j+='"';
  j+=F(",\"gateway\":\"");j+=WiFi.gatewayIP().toString();j+='"';
  j+=F(",\"service_ap_running\":");j+=setupApRunning?F("true"):F("false");
  j+=F(",\"service_ap_ip\":\"");j+=WiFi.softAPIP().toString();j+='"';
  j+=F(",\"last_target\":\"");j+=jsonEscape(dataloggerLastTarget);j+='"';
  j+=F(",\"last_ok_age_ms\":");j+=dataloggerLastOkMs?(uint32_t)(millis()-dataloggerLastOkMs):0;j+='}';sendJson(200,j);
}

void handleDataloggerServiceApStart(){
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  WiFi.mode(WIFI_AP_STA);
  if(!setupApRunning){WiFi.softAPConfig(SETUP_AP_IP,SETUP_AP_GW,SETUP_AP_MASK);setupApRunning=WiFi.softAP(setupApSsid.c_str(),setupApPassword.c_str());}
  if(!setupApRunning){sendJson(500,F("{\"ok\":false,\"error\":\"Service AP start failed\"}"));return;}
  String j=F("{\"ok\":true,\"ssid\":\"");j+=jsonEscape(setupApSsid);j+=F("\",\"password\":\"");j+=jsonEscape(setupApPassword);j+=F("\",\"ip\":\"");j+=WiFi.softAPIP().toString();j+=F("\",\"note\":\"Connect to this AP before switching ESP32 STA to the datalogger AP\"}");sendJson(200,j);
}

void handleDataloggerConnect(){
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  if(!setupApRunning){sendJson(409,F("{\"ok\":false,\"error\":\"Start service AP first\"}"));return;}
  String body=web.arg("plain"),ssid,pass;if(!jsonFindString(body,"ssid",ssid)||!ssid.length()){sendJson(400,F("{\"ok\":false,\"error\":\"ssid required\"}"));return;}jsonFindString(body,"password",pass);
  WiFi.mode(WIFI_AP_STA);WiFi.disconnect(false,false);delay(100);WiFi.begin(ssid.c_str(),pass.c_str());
  uint32_t started=millis();while(WiFi.status()!=WL_CONNECTED&&(uint32_t)(millis()-started)<15000UL){feedTaskWatchdog();delay(100);}
  if(WiFi.status()!=WL_CONNECTED){sendJson(504,F("{\"ok\":false,\"error\":\"Could not connect to datalogger AP; service AP remains active\"}"));return;}
  dataloggerServiceMode=true;String j=F("{\"ok\":true,\"sta_ip\":\"");j+=WiFi.localIP().toString();j+=F("\",\"datalogger_ip\":\"");j+=WiFi.gatewayIP().toString();j+=F("\"}");sendJson(200,j);
}

void handleDataloggerRestore(){
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  dataloggerServiceMode=false;WiFi.disconnect(false,false);delay(100);if(wifiSsid.length())WiFi.begin(wifiSsid.c_str(),wifiPass.c_str());
  sendJson(202,F("{\"ok\":true,\"restoring\":true,\"note\":\"ESP32 STA is reconnecting to saved home Wi-Fi; service AP remains available during recovery\"}"));
}

void handleDataloggerInfo(){String body=web.arg("plain"),err,targetText;IPAddress target;if(!dlTargetFromBody(body,target,targetText,err)){sendJson(400,String("{\"ok\":false,\"error\":\"")+jsonEscape(err)+"\"}");return;}String j=dlInfoJson(target,err);if(!j.length()){sendJson(502,String("{\"ok\":false,\"error\":\"")+jsonEscape(err)+"\"}");return;}sendJson(200,j);}

void handleDataloggerPing(){
  String body=web.arg("plain"),err,targetText;IPAddress target;if(!dlTargetFromBody(body,target,targetText,err)){sendJson(400,String("{\"ok\":false,\"error\":\"")+jsonEscape(err)+"\"}");return;}
  WiFiClient c;if(!dlOpenSession(target,c,err)){sendJson(502,String("{\"ok\":false,\"error\":\"")+jsonEscape(err)+"\"}");return;}
  time_t now=time(nullptr);struct tm t={};gmtime_r(&now,&t);uint8_t b[8];b[0]=(uint8_t)((t.tm_year+1900-2000)&0xff);b[1]=(uint8_t)(t.tm_mon+1);b[2]=(uint8_t)t.tm_mday;b[3]=(uint8_t)t.tm_hour;b[4]=(uint8_t)t.tm_min;b[5]=(uint8_t)t.tm_sec;dlPut16(b+6,300);
  if(!dlWriteFrame(c,0xbeef,1,b,sizeof(b))){err="Ping write failed";dlCloseSession(c);sendJson(502,String("{\"ok\":false,\"error\":\"")+jsonEscape(err)+"\"}");return;}
  uint8_t r[220];size_t n=0;if(!dlReadFrame(c,1,r,sizeof(r),n,err,1200)){dlCloseSession(c);sendJson(502,String("{\"ok\":false,\"error\":\"")+jsonEscape(err)+"\"}");return;}
  String hex;const char* hd="0123456789abcdef";for(size_t i=0;i<n;++i){hex+=hd[r[i]>>4];hex+=hd[r[i]&15];}dlCloseSession(c);dataloggerLastOkMs=millis();sendJson(200,String("{\"ok\":true,\"response_hex\":\"")+hex+"\"}");
}

void handleDataloggerSetParam(){
  if(!adminAuthorized()){sendJson(401,F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));return;}
  String body=web.arg("plain"),name,value,err,targetText;IPAddress target;if(!jsonFindString(body,"name",name)||!jsonFindString(body,"value",value)){sendJson(400,F("{\"ok\":false,\"error\":\"name/value required\"}"));return;}
  uint8_t par=0;if(name=="ssid")par=41;else if(name=="password")par=43;else if(name=="restart")par=29;else{sendJson(400,F("{\"ok\":false,\"error\":\"Only ssid, password, restart are allowed\"}"));return;}
  if(!dlTargetFromBody(body,target,targetText,err)){sendJson(400,String("{\"ok\":false,\"error\":\"")+jsonEscape(err)+"\"}");return;}WiFiClient c;if(!dlOpenSession(target,c,err)){sendJson(502,String("{\"ok\":false,\"error\":\"")+jsonEscape(err)+"\"}");return;}
  uint8_t b[130];size_t n=1+value.length();if(n>sizeof(b)){dlCloseSession(c);sendJson(400,F("{\"ok\":false,\"error\":\"value too long\"}"));return;}b[0]=par;memcpy(b+1,value.c_str(),value.length());
  if(!dlWriteFrame(c,1,3,b,n)){err="Set parameter write failed";dlCloseSession(c);sendJson(502,String("{\"ok\":false,\"error\":\"")+jsonEscape(err)+"\"}");return;}uint8_t r[16];size_t rn=0;if(!dlReadFrame(c,3,r,sizeof(r),rn,err)){dlCloseSession(c);sendJson(502,String("{\"ok\":false,\"error\":\"")+jsonEscape(err)+"\"}");return;}dlCloseSession(c);if(rn<2){sendJson(502,F("{\"ok\":false,\"error\":\"short set-param response\"}"));return;}
  String j=F("{\"ok\":");j+=(r[0]==0)?F("true"):F("false");j+=F(",\"status\":");j+=r[0];j+=F(",\"param\":");j+=r[1];j+=F("}");sendJson(200,j);
}

// ---------------- Network / provisioning API ----------------
bool requestIsFromSetupAp() {
  return setupApRunning && WiFi.softAPgetStationNum() > 0;
}

bool adminAuthorized() {
  if (setupMode) return true; // Setup AP itself is WPA2 protected and physical/first-boot gated.
  if (!web.hasHeader("X-ANENJI-Admin")) return false;
  return web.header("X-ANENJI-Admin") == adminPassword;
}

void handleAdminAuthCheck() {
  if (!adminAuthorized()) {
    sendJson(401, F("{\"ok\":false,\"error\":\"Invalid admin password\"}"));
    return;
  }
  sendJson(200, F("{\"ok\":true,\"authorized\":true}"));
}

void handleNetworkGet() {
  String j;
  j.reserve(900);
  j += F("{\"ok\":true,\"configured\":");
  j += wifiSsid.length() ? F("true") : F("false");
  j += F(",\"setup_mode\":"); j += setupMode ? F("true") : F("false");
  j += F(",\"dhcp\":"); j += netUseStatic ? F("false") : F("true");
  j += F(",\"configured_ip\":\""); j += jsonEscape(netLocalIp); j += '"';
  j += F(",\"gateway\":\""); j += jsonEscape(netGateway); j += '"';
  j += F(",\"mask\":\""); j += jsonEscape(netMask); j += '"';
  j += F(",\"dns\":\""); j += jsonEscape(netDns); j += '"';
  j += F(",\"current_ip\":\""); j += WiFi.localIP().toString(); j += '"';
  j += F(",\"current_gateway\":\""); j += WiFi.gatewayIP().toString(); j += '"';
  j += F(",\"current_mask\":\""); j += WiFi.subnetMask().toString(); j += '"';
  j += F(",\"current_dns\":\""); j += WiFi.dnsIP().toString(); j += '"';
  j += F(",\"ssid\":\""); j += jsonEscape(wifiSsid); j += '"';
  j += F(",\"setup_ssid\":\""); j += jsonEscape(setupApSsid); j += '"';
  j += '}';
  sendJson(200, j);
}

void handleWifiScan() {
  const bool startRequested = web.hasArg("start") && web.arg("start") == "1";

  if (startRequested) {
    int state = WiFi.scanComplete();
    if (state == WIFI_SCAN_RUNNING || state >= 0) {
      WiFi.scanDelete();
      delay(10);
    }

    int rc = WiFi.scanNetworks(true, true); // async=true, show_hidden=true
    if (rc == WIFI_SCAN_FAILED) {
      sendJson(500, F("{\"ok\":false,\"scanning\":false,\"error\":\"Wi-Fi scan start failed\"}"));
      return;
    }

    sendJson(202, F("{\"ok\":true,\"scanning\":true}"));
    return;
  }

  int n = WiFi.scanComplete();

  if (n == WIFI_SCAN_RUNNING) {
    sendJson(202, F("{\"ok\":true,\"scanning\":true}"));
    return;
  }

  if (n == WIFI_SCAN_FAILED) {
    sendJson(200, F("{\"ok\":true,\"scanning\":false,\"scan_failed\":true,\"networks\":[]}"));
    return;
  }

  String j;
  j.reserve(256 + max(0, n) * 110);
  j = F("{\"ok\":true,\"scanning\":false,\"networks\":[");

  bool first = true;
  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;

    // Keep only the strongest entry for duplicate SSIDs.
    bool weakerDuplicate = false;
    for (int k = 0; k < n; ++k) {
      if (k != i && WiFi.SSID(k) == ssid && WiFi.RSSI(k) > WiFi.RSSI(i)) {
        weakerDuplicate = true;
        break;
      }
    }
    if (weakerDuplicate) continue;

    if (!first) j += ',';
    first = false;
    j += F("{\"ssid\":\""); j += jsonEscape(ssid); j += '"';
    j += F(",\"rssi\":"); j += WiFi.RSSI(i);
    j += F(",\"open\":");
    j += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? F("true") : F("false");
    j += '}';
  }

  j += F("]}");
  WiFi.scanDelete();
  sendJson(200, j);
}

void saveWifiConfig(const String& ssid, const String& pass, bool dhcp,
                    const String& ip, const String& gw,
                    const String& mask, const String& dns) {
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("pass", pass);
  wifiPrefs.putBool("static", !dhcp);
  wifiPrefs.putString("ip", ip);
  wifiPrefs.putString("gw", gw);
  wifiPrefs.putString("mask", mask);
  wifiPrefs.putString("dns", dns);
}

void handleNetworkSet() {
  if (!adminAuthorized()) {
    sendJson(401, F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));
    return;
  }

  String body = web.arg("plain");
  bool dhcp = true;
  if (!jsonFindBool(body, "dhcp", dhcp)) {
    sendJson(400, F("{\"ok\":false,\"error\":\"dhcp boolean required\"}"));
    return;
  }

  String ssid = wifiSsid, pass = wifiPass;
  jsonFindString(body, "ssid", ssid);
  // Empty password is valid for an open network, so only overwrite if key is present.
  String suppliedPass;
  if (jsonFindString(body, "password", suppliedPass)) pass = suppliedPass;

  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 63) {
    sendJson(400, F("{\"ok\":false,\"error\":\"Invalid SSID/password length\"}"));
    return;
  }

  String ip = netLocalIp, gw = netGateway, mask = netMask, dns = netDns;
  jsonFindString(body, "ip", ip);
  jsonFindString(body, "gateway", gw);
  jsonFindString(body, "mask", mask);
  jsonFindString(body, "dns", dns);

  if (!dhcp) {
    IPAddress a,b,c,d;
    if (!parseIp(ip,a) || !parseIp(gw,b) || !parseIp(mask,c) || !parseIp(dns,d)) {
      sendJson(400, F("{\"ok\":false,\"error\":\"Invalid IPv4 address/gateway/mask/DNS\"}"));
      return;
    }
  }

  saveWifiConfig(ssid, pass, dhcp, ip, gw, mask, dns);

  // Verify NVS before reboot and update RAM copy as well.
  String verifySsid = wifiPrefs.getString("ssid", "");
  if (verifySsid != ssid) {
    sendJson(500, F("{\"ok\":false,\"error\":\"Wi-Fi settings NVS verification failed\"}"));
    return;
  }
  wifiSsid = ssid;
  wifiPass = pass;
  netUseStatic = !dhcp;
  netLocalIp = ip; netGateway = gw; netMask = mask; netDns = dns;

  String reply = F("{\"ok\":true,\"saved\":true,\"rebooting\":true,\"ssid\":\"");
  reply += jsonEscape(ssid);
  reply += F("\"}");
  sendJson(200, reply);
  delay(700);
  WiFi.disconnect(true, true); // erase Arduino Wi-Fi driver's remembered AP before reboot
  delay(100);
  ESP.restart();
}

void handleEnterSetup() {
  if (!adminAuthorized()) {
    sendJson(401, F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));
    return;
  }
  devicePrefs.putBool("setup_once", true);
  sendJson(200, F("{\"ok\":true,\"rebooting\":true}"));
  delay(400);
  ESP.restart();
}

void handleWifiForget() {
  if (!adminAuthorized()) {
    sendJson(401, F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));
    return;
  }
  wifiPrefs.clear(); // only Wi-Fi namespace; ANENJI/app/device settings remain.
  sendJson(200, F("{\"ok\":true,\"wifi_erased\":true,\"rebooting\":true}"));
  delay(400);
  ESP.restart();
}

void handleCaptiveRedirect() {
  if (setupMode) {
    web.sendHeader("Location", String("http://") + SETUP_AP_IP.toString() + "/", true);
    web.send(302, "text/plain", "");
  } else {
    sendJson(404, F("{\"ok\":false,\"error\":\"Not found\"}"));
  }
}

// ---------------- Modbus / RS-485 configuration API ----------------
void handleModbusConfigGet() {
  String j;
  j.reserve(240);
  j = F("{\"ok\":true,\"slave\":");
  j += modbusSlave;
  j += F(",\"baud\":"); j += RS485_BAUD;
  j += F(",\"format\":\"8N1\"}");
  sendJson(200, j);
}

void handleModbusConfigSet() {
  if (!adminAuthorized()) {
    sendJson(401, F("{\"ok\":false,\"error\":\"Admin authorization required\"}"));
    return;
  }

  String body = web.arg("plain");
  long slave = -1;
  String needle = F("\"slave\"");
  int p = body.indexOf(needle);
  if (p >= 0) {
    p = body.indexOf(':', p + needle.length());
    if (p >= 0) {
      p++;
      while (p < (int)body.length() && isspace((unsigned char)body[p])) p++;
      int e = p;
      while (e < (int)body.length() && isdigit((unsigned char)body[e])) e++;
      if (e > p) slave = body.substring(p, e).toInt();
    }
  }

  if (slave < 1 || slave > 247) {
    sendJson(400, F("{\"ok\":false,\"error\":\"Modbus RTU slave address must be 1..247\"}"));
    return;
  }

  modbusSlave = (uint8_t)slave;
  appPrefs.putUInt("modbus_slave", modbusSlave);

  String j = F("{\"ok\":true,\"slave\":");
  j += modbusSlave;
  j += F(",\"saved\":true}");
  sendJson(200, j);
}

// ---------------- HTML ----------------
void handleRoot() {
  static const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="ru"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ANENJI ESP32</title>
<style>
:root{color-scheme:dark}*{box-sizing:border-box}
body{font-family:system-ui,Arial,sans-serif;background:#111827;color:#e5e7eb;margin:0;padding:14px}
.wrap{max-width:1180px;margin:auto}h1{font-size:24px;margin:6px 0 13px}h2{font-size:19px;margin:0 0 12px}
.card{background:#1f2937;border:1px solid #374151;border-radius:14px;padding:15px;margin:12px 0}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:9px}
.metric,.kvbox{background:#111827;border-radius:10px;padding:10px}.metric small,.kvbox small{display:block;color:#9ca3af;font-size:12px}.metric b,.kvbox b{font-size:20px}
button{background:#2563eb;color:#fff;border:0;border-radius:8px;padding:9px 13px;font-weight:700;cursor:pointer}
button.danger{background:#b45309}button:disabled{opacity:.45;cursor:not-allowed}
input,select{background:#111827;color:#e5e7eb;border:1px solid #4b5563;border-radius:7px;padding:8px}
table{width:100%;border-collapse:collapse;font-size:14px}th,td{padding:7px;border-bottom:1px solid #374151;text-align:left;vertical-align:middle}
.small{font-size:12px;color:#9ca3af}.ok{color:#86efac}.bad{color:#fca5a5}.group{color:#93c5fd;font-weight:700}
.toolbar{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.scroll{overflow:auto}.value{width:110px}
.langbar{display:flex;justify-content:flex-end;align-items:center;gap:6px;margin:-4px 0 10px}
.langbar span{font-size:12px;color:#9ca3af}
.langbar button{padding:6px 10px;background:#374151}
.langbar button.active{background:#2563eb}

.settings-groups{display:grid;gap:14px;margin-top:12px}
.settings-group{background:#111827;border:1px solid #374151;border-radius:12px;overflow:hidden}
.settings-group h3{margin:0;padding:10px 12px;background:#172033;color:#93c5fd;font-size:15px}
.settings-group table{margin:0}
.settings-group th{color:#9ca3af;font-size:12px}
.settings-current{font-weight:700;white-space:nowrap}
.settings-current .raw{display:block;font-weight:400;color:#6b7280;font-size:11px;margin-top:2px}
.settings-new input,.settings-new select{min-width:120px;max-width:220px;width:100%}
@media(max-width:700px){.settings-group table{font-size:12px}.settings-group th,.settings-group td{padding:6px}.settings-new input,.settings-new select{min-width:90px}}

pre{white-space:pre-wrap;background:#111827;border-radius:9px;padding:10px;max-height:300px;overflow:auto}
.flow{position:relative;display:grid;grid-template-columns:1fr 90px 1.15fr 90px 1fr;grid-template-rows:auto 34px auto 34px auto;gap:8px;align-items:center;margin:4px 0 16px}
.flow-node{position:relative;z-index:2;background:#111827;border:1px solid #374151;border-radius:13px;padding:12px;text-align:center;min-width:0}

/* PZEM -> house/common-load path. It represents the common mains feed, not ANENJI reg213. */
#pzemHouseSvg{position:absolute;inset:0;width:100%;height:100%;z-index:1;pointer-events:none;overflow:visible}
#pzemHousePath{fill:none;stroke:#60a5fa;stroke-width:2;stroke-dasharray:7 7;opacity:.25}
#pzemHousePath.active{opacity:.9;animation:pzemDash 1.1s linear infinite}
#pzemHouseArrow{fill:#93c5fd;opacity:0}
#pzemHouseArrow.active{opacity:1}
@keyframes pzemDash{to{stroke-dashoffset:-28}}

.flow-node .ico{font-size:28px}.flow-node b{display:block;font-size:20px}.flow-node small{display:block;color:#9ca3af}
.flow-input{grid-column:1;grid-row:1}.flow-pv{grid-column:3;grid-row:1}.flow-grid{grid-column:1;grid-row:3}.flow-center{grid-column:3;grid-row:3}.flow-load{grid-column:5;grid-row:3}.flow-bat{grid-column:3;grid-row:5}
#inputDown{grid-column:1;grid-row:2}
#pvDown{grid-column:3;grid-row:2}
#gridLine{grid-column:2;grid-row:3;width:100%}
#loadLine{grid-column:4;grid-row:3;width:100%}
#batVert{grid-column:3;grid-row:4}
.flow-line{height:34px;position:relative;overflow:visible}
.flow-v{width:34px;height:34px;justify-self:center;position:relative;overflow:visible}

/* Base connector */
.flow-line::before{content:"";position:absolute;left:0;right:0;top:50%;height:2px;background:#4b5563;transform:translateY(-50%)}
.flow-v::before{content:"";position:absolute;top:0;bottom:0;left:50%;width:2px;background:#4b5563;transform:translateX(-50%)}

/* Fixed arrow at destination. Hidden while idle. */
.flow-line::after,.flow-v::after{
  content:"";position:absolute;z-index:3;opacity:0;
  width:0;height:0
}
.flow-line.right::after{
  opacity:1;right:-1px;top:50%;transform:translateY(-50%);
  border-top:7px solid transparent;border-bottom:7px solid transparent;border-left:11px solid #86efac
}
.flow-line.left::after{
  opacity:1;left:-1px;top:50%;transform:translateY(-50%);
  border-top:7px solid transparent;border-bottom:7px solid transparent;border-right:11px solid #fca5a5
}
.flow-v.down::after{
  opacity:1;left:50%;bottom:-1px;transform:translateX(-50%);
  border-left:7px solid transparent;border-right:7px solid transparent;border-top:11px solid #86efac
}
.flow-v.up::after{
  opacity:1;left:50%;top:-1px;transform:translateX(-50%);
  border-left:7px solid transparent;border-right:7px solid transparent;border-bottom:11px solid #fca5a5
}

/* Moving pulse. This makes the direction obvious even at a glance. */
.flow-line.active span,.flow-v.active span{
  position:absolute;z-index:2;display:block;width:8px;height:8px;border-radius:50%;
  box-shadow:0 0 8px currentColor
}
.flow-line.active span{top:50%;transform:translateY(-50%)}
.flow-v.active span{left:50%;transform:translateX(-50%)}

.flow-line.right span{color:#22c55e;background:#22c55e;animation:flowRight 1.25s linear infinite}
.flow-line.left span{color:#ef4444;background:#ef4444;animation:flowLeft 1.25s linear infinite}
.flow-v.down span{color:#22c55e;background:#22c55e;animation:flowDown 1.25s linear infinite}
.flow-v.up span{color:#ef4444;background:#ef4444;animation:flowUp 1.25s linear infinite}

@keyframes flowRight{0%{left:3%;opacity:.15}20%{opacity:1}100%{left:88%;opacity:.15}}
@keyframes flowLeft{0%{left:88%;opacity:.15}20%{opacity:1}100%{left:3%;opacity:.15}}
@keyframes flowDown{0%{top:3%;opacity:.15}20%{opacity:1}100%{top:88%;opacity:.15}}
@keyframes flowUp{0%{top:88%;opacity:.15}20%{opacity:1}100%{top:3%;opacity:.15}}

.flow-line.active::before,.flow-v.active::before{background:#22c55e}
.flow-line.export::before,.flow-v.discharge::before{background:#ef4444}

/* Offline: no animation and no stale arrows. */
.flow.offline .flow-line::before,.flow.offline .flow-v::before{background:#374151}
.flow.offline .flow-line::after,.flow.offline .flow-v::after{opacity:0}
.flow.offline .flow-line span,.flow.offline .flow-v span{display:none}
.flow.offline .flow-node{opacity:.45}

.badge{display:inline-flex;align-items:center;gap:6px;padding:4px 8px;border-radius:999px;font-size:12px;font-weight:700}
.badge::before{content:"";width:8px;height:8px;border-radius:50%;background:currentColor}
.badge.online{color:#86efac;background:#14532d55}.badge.offline{color:#fca5a5;background:#7f1d1d55}.badge.setup{color:#fde68a;background:#78350f55}
.alert-summary{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:9px;margin-bottom:10px}
.alert-box{background:#111827;border:1px solid #374151;border-radius:10px;padding:10px}.alert-box small{display:block;color:#9ca3af}.alert-box b{font-size:20px}
.alert-box.warning{border-color:#a16207;background:#42200655}.alert-box.critical{border-color:#b91c1c;background:#450a0a66}
.alert-list{display:grid;gap:7px}.alert-row{padding:8px 10px;border-radius:8px;background:#111827;border-left:4px solid #6b7280}
.alert-row.warning{border-left-color:#f59e0b}.alert-row.critical{border-left-color:#ef4444}.alert-row.clear{opacity:.62}.alert-row small{display:block;color:#9ca3af;margin-top:2px}
.info-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:9px}.info-box{background:#111827;border-radius:10px;padding:11px}.info-box h3{margin:0 0 7px;font-size:15px}
.kv{display:flex;justify-content:space-between;gap:12px;padding:4px 0;border-bottom:1px dashed #374151}.kv:last-child{border-bottom:0}
.netgrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:8px}.netgrid label{display:flex;flex-direction:column;gap:4px}
@media(max-width:650px){.flow{grid-template-columns:1fr 42px 1fr;grid-template-rows:auto 32px auto 32px auto}.flow-input{grid-column:1;grid-row:1}.flow-grid{grid-column:1;grid-row:3}.flow-center{grid-column:3;grid-row:3}.flow-load{grid-column:3;grid-row:5}.flow-bat{grid-column:1;grid-row:5}#gridLine{grid-column:2;grid-row:3}#loadLine{display:none}#batVert{display:none}}

#linkLostOverlay{position:fixed;left:12px;right:12px;top:12px;z-index:9999;display:none;pointer-events:none}
#linkLostOverlay.show{display:block}
#linkLostBox{max-width:900px;margin:0 auto;background:#7a1010;border:3px solid #ff5a5a;border-radius:14px;padding:12px 16px;text-align:center;box-shadow:0 8px 28px rgba(255,0,0,.45);animation:linkAlertPulse 1.1s infinite alternate;pointer-events:auto}
#linkLostBox h2{margin:0 0 4px 0;font-size:1.15rem}
#linkLostBox p{margin:3px 0}
#linkLostSince{font-size:.9em}
@keyframes linkAlertPulse{from{box-shadow:0 8px 24px rgba(255,0,0,.35);transform:scale(1)}to{box-shadow:0 8px 40px rgba(255,0,0,.85);transform:scale(1.01)}}

/* Browser-like application tabs */
.app-tabs{display:flex;gap:6px;overflow-x:auto;position:sticky;top:0;z-index:900;background:#111827eF;backdrop-filter:blur(8px);padding:8px 0 9px;margin:0 0 10px;border-bottom:1px solid #374151}
.app-tab{white-space:nowrap;background:#253247;color:#cbd5e1;border:1px solid #374151;border-bottom-color:#4b5563;border-radius:10px 10px 4px 4px;padding:8px 13px;font-weight:700}
.app-tab.active{background:#2563eb;color:#fff;border-color:#3b82f6}
.tab-page{display:none}
.tab-page.active{display:block}
.tab-page>.card:first-child{margin-top:0}
@media(max-width:650px){.app-tabs{margin-left:-4px;margin-right:-4px}.app-tab{padding:8px 10px;font-size:13px}}
</style></head><body><div class="wrap">
<h1>ANENJI · ESP32 Wi‑Fi / RS‑485</h1><div class="langbar"><span id="langLabel">Язык</span><button id="langRu" onclick="setLang('ru')">RU</button><button id="langEn" onclick="setLang('en')">EN</button></div>

<div class="app-tabs" id="appTabs">
 <button class="app-tab active" data-page="overview" onclick="showTab('overview')">Обзор</button>
 <button class="app-tab" data-page="battery" onclick="showTab('battery')">Батарея</button>
 <button class="app-tab" data-page="settings" onclick="showTab('settings')">Настройки</button>
 <button class="app-tab" data-page="scheduler" onclick="showTab('scheduler')">Планировщик</button>
 <button class="app-tab" data-page="network" onclick="showTab('network')">Сеть</button>
 <button class="app-tab" data-page="service" onclick="showTab('service')">Сервис</button>
</div>
<div id="tabHost"></div>


<div class="card"><h2>Состояние / аварии</h2>
 <div class="toolbar" style="margin-bottom:10px">
  <span id="stateWifiBadge" class="badge offline">Wi‑Fi OFFLINE</span>
  <span id="stateInvBadge" class="badge offline">Инвертор OFFLINE</span>
 </div>
 <div class="alert-summary"><div class="alert-box"><small>Режим</small><b id="alertMode">—</b></div><div class="alert-box critical"><small>Активные аварии</small><b id="alertFaultCount">0</b></div><div class="alert-box warning"><small>Активные предупреждения</small><b id="alertWarningCount">0</b></div></div>
 <div id="activeAlerts" class="alert-list"><div class="small">Нет активных аварий.</div></div>
 <details style="margin-top:10px"><summary>Журнал событий</summary><div id="eventLog" class="alert-list" style="margin-top:8px"><div class="small">—</div></div></details>
</div>

<div class="card"><h2>Энергопотоки</h2>
<div id="energyFlow" class="flow">
 <svg id="pzemHouseSvg" aria-hidden="true"><defs><marker id="pzemArrowMarker" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto"><path id="pzemHouseArrow" d="M0,0 L8,4 L0,8 z"/></marker></defs><path id="pzemHousePath" marker-end="url(#pzemArrowMarker)"/></svg>
 <div class="flow-node flow-input"><div class="ico">▤</div><small>Ввод · PZEM</small><b id="fInput">— W</b><small id="fInputSub">общий ввод</small></div><div id="inputDown" class="flow-v"><span></span></div>
 <div class="flow-node flow-pv"><div class="ico">☀️</div><small>PV</small><b id="fPv">— W</b><small id="fPvSub">—</small></div><div id="pvDown" class="flow-v"><span></span></div>
 <div class="flow-node flow-grid"><div class="ico">⚡</div><small>Сеть</small><b id="fGrid">— W</b><small id="fGridSub">—</small></div><div id="gridLine" class="flow-line"><span></span></div>
 <div class="flow-node flow-center"><div class="ico">▣</div><small>ANENJI</small><b id="fInv">—</b><small id="fInvSub">—</small></div><div id="loadLine" class="flow-line"><span></span></div>
 <div class="flow-node flow-load"><div class="ico">🏠</div><small>Нагрузка</small><b id="fLoad">— W</b><small id="fLoadSub">—</small></div>
 <div id="batVert" class="flow-v"><span></span></div><div class="flow-node flow-bat"><div class="ico">🔋</div><small>Батарея</small><b id="fBat">— V</b><small id="fBatSub">—</small></div>
</div>
<div class="small" style="margin:-5px 0 12px">Направление видно по движущейся точке и стрелке. Пунктир PZEM → Нагрузка показывает общий сетевой ввод дома; это не мощность выхода ANENJI reg213.</div>
<div class="info-grid">
 <div class="info-box"><h3>Батарея</h3><div id="infoBattery"></div></div>
 <div class="info-box"><h3>Солнечная энергия / PV</h3><div id="infoPv"></div></div>
 <div class="info-box"><h3>Инвертор / Нагрузка</h3><div id="infoInv"></div></div>
 <div class="info-box"><h3>PZEM-016 · общий ввод</h3><div id="infoPzem">—</div></div>
</div></div>

<div class="card"><h2>Полная телеметрия</h2><div id="tele" class="grid"></div><div class="toolbar" style="margin-top:12px"><button onclick="statusLoad(true)">Обновить сейчас</button><span id="age" class="small"></span></div></div>

<div class="card"><h2>Настройки инвертора</h2>
<div class="toolbar"><button onclick="settingsLoad(true)">Обновить настройки</button><span class="small">Обычно используются последние прочитанные значения; кнопка принудительно перечитывает ANENJI.</span></div>
<div id="settingsMsg" class="small" style="margin:10px 0"></div>
<div id="settings" class="settings-groups"></div>
</div>

<div class="card"><h2>Wi‑Fi / сеть ESP32</h2>
<div id="netState" class="small" style="margin-bottom:10px"></div>
<div class="toolbar"><select id="netSsid"></select><button id="wifiScanBtn" onclick="wifiScan()">Сканировать Wi‑Fi</button><span id="wifiScanState" class="small"></span></div>
<div class="netgrid" style="margin-top:10px">
<label>SSID вручную<input id="netSsidManual" placeholder="или выберите сеть выше"></label>
<label>Пароль Wi‑Fi<input id="netPass" type="password" placeholder="оставьте пустым, чтобы не менять"></label>
</div>
<label style="display:block;margin-top:10px"><input id="netDhcp" type="checkbox" onchange="netToggle()"> DHCP</label>
<div class="netgrid" style="margin-top:10px">
<label>IP адрес<input id="netIp" placeholder="192.168.88.158"></label>
<label>Gateway<input id="netGw" placeholder="192.168.88.1"></label>
<label>Mask<input id="netMask" placeholder="255.255.255.0"></label>
<label>DNS<input id="netDns" placeholder="192.168.88.1"></label>
</div>
<div class="toolbar" style="margin-top:10px">
<button class="danger" onclick="netSave()">Сохранить Wi‑Fi и перезагрузить</button>
<button onclick="enterSetup()">Переинициализация Wi‑Fi</button>
<button class="danger" onclick="forgetWifi()">Сбросить настройки Wi‑Fi</button>
</div>
<div class="small" style="margin-top:8px">«Переинициализация Wi‑Fi» перезагрузит ESP32 в режим настройки, сохранив текущие данные. «Сбросить настройки Wi‑Fi» удалит SSID/пароль/IP и запустит режим первичной настройки.</div>
</div>


<div class="card"><h2>Статистика аккумулятора</h2>
<div class="grid">
 <div class="metric"><small>Циклы LiFePO4 · EFC</small><b id="bsEfc">—</b></div>
 <div class="metric"><small>Принято батареей</small><b id="bsChargedAh">—</b></div>
 <div class="metric"><small>Отдано батареей</small><b id="bsDischargedAh">—</b></div>
 <div class="metric"><small>Заряжено всего</small><b id="bsCharged">—</b></div>
 <div class="metric"><small>Разряжено всего</small><b id="bsDischarged">—</b></div>
 <div class="metric"><small>Калиброванная ёмкость</small><b id="bsCapacity">—</b></div>
</div>
<div id="bsDetails" class="small" style="margin-top:10px">—</div>
<div class="toolbar" style="margin-top:10px">
 <button id="bsRefreshBtn" onclick="batteryStatsLoad(true)">Обновить</button>
</div>
<details style="margin-top:10px"><summary style="cursor:pointer;color:#9ca3af">Сервисные действия батареи</summary>
 <div class="small" style="margin:8px 0">Открывайте только для калибровки или сброса статистики. Действия требуют пароля администратора и дополнительного подтверждения.</div>
 <div class="toolbar">
  <button onclick="batteryCalibrationStart()">Запустить контрольный разряд</button>
  <button onclick="batteryCalibrationFinish()">Завершить и сохранить ёмкость</button>
  <button onclick="batteryCalibrationCancel()">Отменить</button>
 </div>
 <div class="toolbar" style="margin-top:8px">
  <label>Фактическая ёмкость, Ah <input id="bsManualAh" type="number" min="10" max="1000" step="0.1" style="width:100px"></label>
  <button onclick="batteryCalibrationManual()">Задать вручную</button>
  <button class="danger" onclick="batteryStatsReset(false)">Сбросить статистику</button>
  <button class="danger" onclick="batteryStatsReset(true)">Сбросить всё</button>
 </div>
</details>
<div id="bsMsg" class="small" style="margin-top:8px"></div>
<details style="margin-top:10px"><summary>История по дням</summary><div class="scroll"><table><thead><tr><th>Дата</th><th>Заряд</th><th>Разряд</th><th>ΔEFC</th></tr></thead><tbody id="bsHistory"></tbody></table></div></details>
<div class="small" style="margin-top:8px"><b>EFC для LiFePO4 считается по отданной ёмкости Ah:</b> суммарные разряженные Ah / опорная ёмкость Ah. Частичные разряды складываются: два разряда по 50% ≈ 1 EFC. Зарядные Ah ведутся отдельно и не удваивают число циклов. kWh сохраняются как энергетическая статистика. Контрольный разряд запускайте на SOC ≥90%: прошивка проверенно пишет RAW 2 (SBU) в регистр 301, переводя инвертор на приоритет Solar → Battery → Utility. При целевом SOC 20% прежнее значение регистра 301 восстанавливается автоматически; затем нажмите «Завершить и сохранить ёмкость». Статистика хранится в AT24C32 с CRC и кольцевой записью.</div>
</div>

<div class="card"><h2>RTC / Планировщик</h2>
<div class="grid">
 <div class="metric"><small>RTC DS1307</small><b id="rtcState">—</b></div>
 <div class="metric"><small>Время RTC</small><b id="rtcTime">—</b></div>
 <div class="metric"><small>Задачи</small><b id="rtcRuns">—</b></div>
</div>
<div class="netgrid" style="margin-top:10px">
 <label>NTP сервер<select id="ntpServerSelect" onchange="ntpServerChoice()"><option value="pool.ntp.org">pool.ntp.org</option><option value="time.google.com">time.google.com</option><option value="time.cloudflare.com">time.cloudflare.com</option><option value="custom">Пользовательский</option></select></label>
 <label>Свой NTP сервер<input id="ntpServerCustom" placeholder="ntp.example.org" disabled></label>
 <label>Часовой пояс, UTC<input id="ntpUtcOffset" type="number" min="-12" max="14" step="0.25" value="3" placeholder="3"></label>
</div>
<div class="toolbar" style="margin-top:8px"><button onclick="ntpSave()">Сохранить NTP / UTC</button></div>
<div class="toolbar" style="margin-top:10px">
 <button onclick="rtcSyncBrowser()">Синхронизировать с браузером</button>
 <button onclick="rtcSyncNtp()">Синхронизировать NTP</button>
 <button onclick="rtcLoad()">Обновить RTC</button>
 <span id="rtcMsg" class="small"></span>
</div>
<div class="small" style="margin-top:8px">DS1307/совместимый RTC: SDA GPIO21, SCL GPIO22. AT24C32 определяется автоматически на 0x50…0x57. Планировщик работает автономно без Wi‑Fi.</div>
<div class="scroll" style="margin-top:12px">
<table><thead><tr><th>#</th><th>Вкл</th><th>Дни</th><th>Время</th><th>Регистр</th><th>RAW</th><th></th></tr></thead><tbody id="schedBody"></tbody></table>
</div>
<div class="small" style="margin-top:8px">RAW — значение регистра до масштабирования. Разрешены только регистры из белого списка настроек; перед выполнением снова применяются проверки безопасности.</div>
</div>

<div class="card"><h2>PZEM-016 · ввод сети</h2>
<div class="grid">
 <div class="metric"><small>Напряжение</small><b id="pzV">— V</b></div><div class="metric"><small>Ток</small><b id="pzA">— A</b></div>
 <div class="metric"><small>Активная мощность</small><b id="pzW">— W</b></div><div class="metric"><small>Энергия</small><b id="pzKwh">— kWh</b></div>
 <div class="metric"><small>Частота</small><b id="pzHz">— Hz</b></div><div class="metric"><small>Power factor</small><b id="pzPf">—</b></div>
</div>
<div class="toolbar" style="margin-top:10px"><label><input id="pzemEnabled" type="checkbox"> опрашивать PZEM-016</label><label>Modbus адрес <input id="pzemAddr" type="number" min="1" max="247" value="1" style="width:80px"></label><button onclick="pzemSave()">Сохранить PZEM</button><span id="pzemState" class="small"></span></div>
<div class="netgrid" style="margin-top:12px"><label>Начальное значение T1, kWh<input id="pzemT1" type="number" min="0" step="0.001"></label><label>Начальное значение T2, kWh<input id="pzemT2" type="number" min="0" step="0.001"></label></div>
<div class="toolbar" style="margin-top:8px"><button onclick="pzemTariffSet()">Синхронизировать T1/T2 со счётчиком</button><span id="pzemTariffState" class="small"></span></div>
<details style="margin-top:10px"><summary>Сервис PZEM</summary><div class="toolbar" style="margin-top:8px"><button class="danger" onclick="pzemEnergyReset()">Обнулить энергию PZEM</button><span id="pzemResetState" class="small"></span></div><div class="small">Обнуляется только внутренний накопительный счётчик энергии PZEM. T1/T2 не изменяются. Требуются пароль администратора и дополнительное подтверждение.</div></details>
<div class="small" style="margin-top:8px">Тариф T1: 07:00–23:00. T2: 23:00–07:00. После ввода текущих показаний электросчётчика прошивка распределяет прирост энергии PZEM по тарифам и хранит T1/T2 в AT24C32. PZEM-016: 9600 8N1, FC04, регистры 0x0000…0x0009.</div>
</div>

<div class="card"><h2>Modbus / RS‑485</h2>
<div class="netgrid">
<label>Адрес устройства Modbus RTU<input id="modbusSlave" type="number" min="1" max="247" step="1"></label>
<label>Скорость<input id="modbusBaud" value="9600" disabled></label>
<label>Формат<input id="modbusFormat" value="8N1" disabled></label>
</div>
<div class="toolbar" style="margin-top:10px"><button onclick="modbusSave()">Сохранить адрес Modbus</button><span id="modbusState" class="small"></span></div>
<div class="small" style="margin-top:8px">Адрес 1…247 хранится в NVS и сохраняется при обычном обновлении прошивки.</div>
</div>

<div class="card"><h2>Обновление прошивки · OTA</h2>
<div class="small">Локальное обновление ESP32 по Wi‑Fi. Выберите скомпилированный файл <b>.bin</b>. Во время записи RS‑485 и планировщик временно приостанавливаются. После успешной записи ESP32 автоматически перезагрузится.</div>
<div class="toolbar" style="margin-top:10px"><input id="otaFile" type="file" accept=".bin,application/octet-stream"><button onclick="otaUpload()">Загрузить прошивку</button><span id="otaState" class="small">версия: …</span></div>
<div style="margin-top:8px"><progress id="otaProgress" max="100" value="0" style="width:100%;height:18px"></progress></div>
<div class="small" style="margin-top:8px">Требуется пароль администратора. Не отключайте питание во время записи. Настройки NVS и статистика AT24C32 при обычном OTA сохраняются.</div>
</div>

<div class="card"><h2>Datalogger</h2>
<div class="small">Экспериментальное управление штатным EyeBond / SmartESS Wi-Fi datalogger. ESP32 не выполняет никаких действий автоматически.</div>
<div class="grid" style="margin-top:10px"><label>IP datalogger<input id="dlIp" placeholder="например 192.168.1.50"></label><label>SSID точки datalogger<input id="dlApSsid" placeholder="Q00... / W00..."></label><label>Пароль точки datalogger<input id="dlApPass" type="password" placeholder="пароль AP"></label></div>
<div class="toolbar" style="margin-top:10px"><button onclick="dlServiceAp()">1. Сервисная AP ESP32</button><button onclick="dlConnectAp()">2. Подключиться к AP datalogger</button><button onclick="dlRestore()">Вернуть домашний Wi-Fi</button></div>
<div id="dlStatus" class="small" style="margin-top:8px">—</div>
<div class="toolbar" style="margin-top:10px"><button onclick="dlInfo()">Info</button><button onclick="dlPing()">Ping</button></div>
<details style="margin-top:10px"><summary>Изменение параметров datalogger</summary><div class="small" style="margin:8px 0">Команды подтверждены для SmartESS/EyeBond: STA SSID (41), STA password (43), restart (29). Это не пароль собственной AP datalogger.</div><div class="grid"><label>Новый STA SSID<input id="dlStaSsid"></label><label>Новый STA пароль<input id="dlStaPass" type="password"></label></div><div class="toolbar" style="margin-top:8px"><button onclick="dlSet('ssid')">Записать STA SSID</button><button onclick="dlSet('password')">Записать STA пароль</button><button onclick="dlRestart()">Restart datalogger</button></div></details>
<pre id="dlOut" style="white-space:pre-wrap;max-height:260px;overflow:auto"></pre></div>

<div class="card"><h2>Связь</h2>
<div class="toolbar" style="margin-bottom:10px">
  <span id="wifiBadge" class="badge offline">Wi‑Fi OFFLINE</span>
  <span id="invBadge" class="badge offline">Инвертор OFFLINE</span>
</div>
<div id="diag"></div>
<details style="margin-top:8px"><summary>Расширенная диагностика</summary><div id="diagExtra" class="small" style="margin-top:8px"></div></details>
</div>

<div class="card"><h2>Чтение регистров</h2>
<div class="toolbar"><input id="rawAddr" value="215" size="8"><input id="rawCount" value="1" size="5"><button onclick="rawRead()">Прочитать</button></div>
<pre id="rawOut"></pre></div>

<div class="card">
<details id="profilesDetails">
<summary style="cursor:pointer;font-weight:700;font-size:17px">Профили батареи · расширенные</summary>
<div class="small" style="margin:10px 0">Все известные профили собраны здесь. Прошивка разрешает запись только после автоматического подтверждения класса батарейной системы (24 V или 48 V) по двум независимым признакам.</div>
<div class="toolbar"><select id="profileSelect" onchange="renderProfilePreview()"></select><button id="profileApplyBtn" class="danger" onclick="applySelectedProfile()">Применить</button></div>
<div id="profileInfo" style="margin-top:10px"></div>
<div id="profilePreview" class="scroll" style="margin-top:10px"></div>
<div id="profileMsg" class="small" style="margin-top:8px"></div>
</details>
</div>

<div class="small">HTTP :80 · RTU 9600 8N1 · DS1307 RTC + AT24C32 · Modbus RTU gateway отключён</div>
</div><div id="linkLostOverlay"><div id="linkLostBox"><h2>⚠ НЕТ СВЯЗИ С ESP32</h2><p>Данные на странице могут быть устаревшими. Интерфейс не заблокирован.</p><p id="linkLostSince">Проверяю соединение…</p></div></div>
<script>
const $=id=>document.getElementById(id),f=(v,d=1)=>typeof v==='number'?v.toFixed(d):'—';
let currentLang=localStorage.getItem('anenji_lang')||'ru';
const UI_TEXT=[
 ['Язык','Language'],['Связь','Connection'],['Расширенная диагностика','Advanced diagnostics'],
 ['Энергопотоки','Energy flow'],['Сеть','Grid'],['Инвертор','Inverter'],['Нагрузка','Load'],['Батарея','Battery'],
 ['Солнечная энергия / PV','Solar / PV'],['Инвертор / Нагрузка','Inverter / Load'],
 ['Полная телеметрия','Full telemetry'],['Обновить сейчас','Refresh now'],['Настройки инвертора','Inverter settings'],
 ['Обновить настройки','Refresh settings'],['Wi‑Fi / сеть ESP32','Wi‑Fi / ESP32 network'],
 ['Сканировать Wi‑Fi','Scan Wi‑Fi'],['SSID вручную','SSID manually'],['Пароль Wi‑Fi','Wi‑Fi password'],
 ['Сохранить Wi‑Fi и перезагрузить','Save Wi‑Fi and reboot'],['Переинициализация Wi‑Fi','Reconfigure Wi‑Fi'],
 ['Сбросить настройки Wi‑Fi','Reset Wi‑Fi settings'],['Modbus / RS‑485','Modbus / RS‑485'],
 ['Адрес устройства Modbus RTU','Modbus RTU device address'],['Скорость','Baud rate'],['Формат','Format'],
 ['Сохранить адрес Modbus','Save Modbus address'],['Чтение регистров','Raw register explorer'],['Прочитать','Read'],
 ['Профили батареи · расширенные','Battery profiles · advanced'],['Применить','Apply'],['RTC / Планировщик','RTC / Scheduler'],['Время RTC','RTC time'],['Задачи','Tasks'],['Синхронизировать с браузером','Sync from browser'],['Обновить RTC','Refresh RTC'],['Дни','Days'],['Вкл','On'],['Регистр','Register']
];
const UI_LOOKUP=new Map();
UI_TEXT.forEach((p,i)=>{UI_LOOKUP.set(p[0],i);UI_LOOKUP.set(p[1],i)});
function trPair(text){const i=UI_LOOKUP.get(text);return i===undefined?text:UI_TEXT[i][currentLang==='ru'?0:1]}
function translateStatic(root=document.body){
 const w=document.createTreeWalker(root,NodeFilter.SHOW_TEXT);let n;
 while((n=w.nextNode())){const raw=n.nodeValue,trim=raw.trim();if(!trim)continue;const t=trPair(trim);if(t!==trim)n.nodeValue=raw.replace(trim,t)}
}
function setLang(lang){
 currentLang=lang==='en'?'en':'ru';
 localStorage.setItem('anenji_lang',currentLang);
 document.documentElement.lang=currentLang;
 translateStatic();
 $('langRu')?.classList.toggle('active',currentLang==='ru');
 $('langEn')?.classList.toggle('active',currentLang==='en');
 if(window.lastStatusTelemetry){renderEnergy(window.lastStatusTelemetry);if(window.lastStatusPacket){const x=window.lastStatusPacket,t=x.telemetry||{};$('tele').innerHTML=teleDefs.map(d=>`<div class=metric><small>${teleLabel(d[0],d[1])} · ${d[2]}</small><b>${f(t[d[0]],d[4])} ${d[3]}</b></div>`).join('')}}
 if(window.lastSettingsData)renderSettingsGroups(window.lastSettingsData);
 if(Object.keys(profileDefs||{}).length)renderProfilePreview();
}

const kv=(a,b)=>`<div class=kv><span>${a}</span><b>${b}</b></div>`;
let writeEnabled=false,activeProfile='ANJ_24V_8S_1P',profileDefs={};

function adminHeaders(){
 let p=sessionStorage.getItem('anenji_admin_pass')||'';
 if(!p){
  p=prompt(currentLang==='ru'
   ?'Введите пароль администратора ANENJI (по умолчанию совпадает с паролем Wi-Fi сети ANENJI-SETUP):'
   :'Enter ANENJI admin password (by default it is the ANENJI-SETUP Wi-Fi password):')||'';
  if(p)sessionStorage.setItem('anenji_admin_pass',p);
 }
 return {'Content-Type':'application/json','X-ANENJI-Admin':p};
}
function clearAdminPassword(){sessionStorage.removeItem('anenji_admin_pass');}

async function verifiedAdminHeaders(msgId='bsMsg'){
 const msg=$(msgId);
 let p=sessionStorage.getItem('anenji_admin_pass')||'';
 if(!p){
  p=prompt(currentLang==='ru'?'Введите пароль администратора ANENJI:':'Enter ANENJI admin password:')||'';
  if(!p){if(msg)msg.innerHTML='<span class=bad>Пароль не введён.</span>';return null;}
 }
 if(msg)msg.textContent='Проверка пароля администратора…';
 try{
  await api('/api/auth/check',{method:'POST',headers:{'Content-Type':'application/json','X-ANENJI-Admin':p},body:'{}',timeoutMs:3500});
  sessionStorage.setItem('anenji_admin_pass',p);
  if(msg)msg.innerHTML='<span class=ok>Пароль администратора принят.</span>';
  return {'Content-Type':'application/json','X-ANENJI-Admin':p};
 }catch(e){
  clearAdminPassword();
  const text=currentLang==='ru'?'Неверный пароль администратора. Действие отменено.':'Invalid admin password. Action cancelled.';
  if(msg)msg.innerHTML='<span class=bad>'+text+'</span>';
  alert(text);
  return null;
 }
}

async function api(url,opt={}){
 const timeoutMs=Number(opt.timeoutMs||2800);
 const reqOpt={...opt}; delete reqOpt.timeoutMs;
 const c=new AbortController(),tm=setTimeout(()=>c.abort(),timeoutMs);
 try{
  const o={...reqOpt,cache:'no-store',signal:c.signal};
  const r=await fetch(url,o);let j={};try{j=await r.json()}catch(e){}
  if(!r.ok){if(r.status===401)clearAdminPassword();throw new Error(j.error||('HTTP '+r.status))}return j
 }finally{clearTimeout(tm)}
}

const TELE_RU={
mode:'Режим',grid_voltage:'Напряжение сети',grid_frequency:'Частота сети',grid_power:'Мощность сети',
inverter_voltage:'Напряжение инвертора',inverter_current:'Ток инвертора',inverter_frequency:'Частота инвертора',
inverter_power:'Мощность инвертора',inverter_charging_power:'Мощность зарядки инвертора',
output_voltage:'Выходное напряжение',output_current:'Выходной ток',output_frequency:'Выходная частота',
output_active_power:'Активная мощность выхода',output_apparent_power:'Полная мощность выхода',
battery_voltage:'Напряжение АКБ',battery_current:'Ток АКБ',battery_power:'Мощность АКБ',
pv_voltage:'Напряжение PV',pv_current:'Ток PV',pv_power:'Мощность PV',pv_charging_power:'Мощность зарядки PV',
load_percent:'Нагрузка',dcdc_temperature:'Температура DC/DC',inverter_temperature:'Температура инвертора',
battery_soc:'SOC АКБ',net_battery_current:'Суммарный ток АКБ',
inverter_charge_current:'Ток зарядки от инвертора',pv_charge_current:'Ток зарядки от PV'
};
function teleLabel(key,en){return currentLang==='ru'?(TELE_RU[key]||en):en}

const teleDefs=[
 ['mode','Mode',201,'',0],['grid_voltage','Grid voltage',202,'V',1],['grid_frequency','Grid frequency',203,'Hz',2],
 ['grid_power','Grid power',204,'W',0],['inverter_voltage','Inverter voltage',205,'V',1],['inverter_current','Inverter current',206,'A',1],
 ['inverter_frequency','Inverter frequency',207,'Hz',2],['inverter_power','Inverter power',208,'W',0],['inverter_charging_power','Inverter charging power',209,'W',0],
 ['output_voltage','Output voltage',210,'V',1],['output_current','Output current',211,'A',1],['output_frequency','Output frequency',212,'Hz',2],
 ['output_active_power','Output active power',213,'W',0],['output_apparent_power','Output apparent power',214,'VA',0],
 ['battery_voltage','Battery voltage',215,'V',1],['battery_current','Battery current',216,'A',1],['battery_power','Battery power',217,'W',0],
 ['pv_voltage','PV voltage',219,'V',1],['pv_current','PV current',220,'A',1],['pv_power','PV power',223,'W',0],['pv_charging_power','PV charging power',224,'W',0],
 ['load_percent','Load',225,'%',0],['dcdc_temperature','DCDC temperature',226,'°C',0],['inverter_temperature','Inverter temperature',227,'°C',0],
 ['battery_soc','Battery SOC',229,'%',0],['net_battery_current','Net battery current',232,'A',1],
 ['inverter_charge_current','Inverter charge current',233,'A',1],['pv_charge_current','PV charge current',234,'A',1]
];

let lastEventLoadMs=0;
function renderAlerts(x){
 const a=x.alerts||{},faults=Array.isArray(a.faults)?a.faults:[],warnings=Array.isArray(a.warnings)?a.warnings:[];
 if($('alertMode'))$('alertMode').textContent=a.operation_mode_text||'—';
 if($('alertFaultCount'))$('alertFaultCount').textContent=String(faults.length);
 if($('alertWarningCount'))$('alertWarningCount').textContent=String(warnings.length);
 if($('activeAlerts')){const rows=[];faults.forEach(v=>rows.push(`<div class="alert-row critical"><b>FAULT · bit ${v.bit}</b><small>${v.text}</small></div>`));warnings.forEach(v=>rows.push(`<div class="alert-row warning"><b>WARNING · bit ${v.bit}</b><small>${v.text}</small></div>`));$('activeAlerts').innerHTML=rows.length?rows.join(''):'<div class="small ok">Активных аварий и предупреждений нет.</div>';}
}
async function eventsLoad(force=false){
 const now=Date.now();if(!force&&now-lastEventLoadMs<9000)return;lastEventLoadMs=now;
 try{const x=await api('/api/events',{timeoutMs:2200}),ev=Array.isArray(x.events)?x.events:[];if(!$('eventLog'))return;$('eventLog').innerHTML=ev.length?ev.slice(0,24).map(e=>{const cls=e.severity==='CRITICAL'?'critical':(e.severity==='WARNING'?'warning':'');const state=e.type==='mode'?'':(e.active?'RAISED':'CLEARED');return `<div class="alert-row ${cls} ${e.active?'':'clear'}"><b>${e.severity} · ${e.type.toUpperCase()} ${state}</b><small>${e.message} · uptime ${e.uptime_s}s</small></div>`}).join(''):'<div class="small">Событий пока нет.</div>';}catch(e){}
}
let autonomyAvgW=null,autonomyLastMs=0;

function formatRuntime(hours){
 if(!Number.isFinite(hours)||hours<0)return '—';
 const totalMin=Math.max(0,Math.round(hours*60));
 const d=Math.floor(totalMin/1440),h=Math.floor((totalMin%1440)/60),m=totalMin%60;
 if(d>0)return `${d} д ${h} ч ${m} мин`;
 if(h>0)return `${h} ч ${m} мин`;
 return `${m} мин`;
}

function autonomyEstimate(t,prof,bv,bc,soc){
 if(!prof||!Number.isFinite(soc)||soc<0||soc>100)return {text:'—',sub:'нет данных профиля/SOC'};
 const reserve=Number(prof.reserve_soc??15);
 const usableKwh=Math.max(0,Number(prof.energy_kwh||0)*(soc-reserve)/100);

 // In our mapped telemetry positive battery current = charge, negative = discharge.
 // Use V × net battery current because the sign of reg217 battery_power is not yet fully verified.
 const dischargeW=(bc < -0.2 && bv>1) ? bv*(-bc) : 0;
 const now=Date.now();

 if(dischargeW>5){
   if(autonomyAvgW==null || !Number.isFinite(autonomyAvgW)){
     autonomyAvgW=dischargeW;
   }else{
     // EMA: deliberately smooth load changes so ETA does not jump every 2 seconds.
     const dt=autonomyLastMs?Math.min(10,Math.max(0.2,(now-autonomyLastMs)/1000)):2;
     const tau=45; // seconds
     const alpha=1-Math.exp(-dt/tau);
     autonomyAvgW += alpha*(dischargeW-autonomyAvgW);
   }
   autonomyLastMs=now;

   if(soc<=reserve)return {text:'0 мин',sub:`SOC ≤ резерв ${reserve}%`};
   const hours=usableKwh/(autonomyAvgW/1000);
   return {
     text:'≈ '+formatRuntime(hours),
     sub:`до ${reserve}% · средний разряд ${Math.round(autonomyAvgW)} W · доступно ${usableKwh.toFixed(2)} kWh`
   };
 }

 // During charge/idle there is no meaningful discharge ETA. Slowly forget old load estimate.
 if(autonomyAvgW!=null && autonomyLastMs && now-autonomyLastMs>120000) autonomyAvgW=null;
 return {
   text:'—',
   sub:bc>0.2 ? `АКБ заряжается · резерв ${reserve}%` : `нет устойчивого разряда · резерв ${reserve}%`
 };
}

let pzemHouseFlowActive=false,pzemHouseFlowRaf=0;
function hidePzemHouseFlow(){
 const path=$('pzemHousePath'),arrow=$('pzemHouseArrow');
 if(path){path.removeAttribute('d');path.classList.remove('active');}
 if(arrow)arrow.classList.remove('active');
}
function layoutPzemHouseFlow(){
 pzemHouseFlowRaf=0;
 const host=$('energyFlow'),src=host&&host.querySelector('.flow-input'),dst=host&&host.querySelector('.flow-load'),path=$('pzemHousePath'),arrow=$('pzemHouseArrow');
 if(!host||!src||!dst||!path||!arrow)return;
 // Do not draw from zero/stale rectangles while the Overview tab is hidden or
 // while the browser is still laying out the grid after a page refresh.
 const h=host.getBoundingClientRect(),a=src.getBoundingClientRect(),b=dst.getBoundingClientRect();
 if(h.width<100||h.height<100||a.width<20||a.height<20||b.width<20||b.height<20){
  hidePzemHouseFlow(); return;
 }
 // PZEM common-input path: middle of PZEM right edge -> 90-degree route ->
 // vertical entry into the top centre of the common-input Load node.
 const x1=a.right-h.left,y1=a.top-h.top+a.height*0.50;
 const x2=b.left-h.left+b.width*0.50,y2=b.top-h.top;
 if(!Number.isFinite(x1)||!Number.isFinite(y1)||!Number.isFinite(x2)||!Number.isFinite(y2)||x2<=x1+10||y2<=0){
  hidePzemHouseFlow(); return;
 }
 path.setAttribute('d',`M ${x1} ${y1} H ${x2} V ${y2}`);
 path.classList.toggle('active',!!pzemHouseFlowActive);
 arrow.classList.toggle('active',!!pzemHouseFlowActive);
}
function schedulePzemHouseFlowLayout(){
 hidePzemHouseFlow();
 if(pzemHouseFlowRaf)cancelAnimationFrame(pzemHouseFlowRaf);
 // Two animation frames guarantee that tab/grid/card layout has been committed.
 pzemHouseFlowRaf=requestAnimationFrame(()=>requestAnimationFrame(layoutPzemHouseFlow));
}
function updatePzemHouseFlow(active){
 pzemHouseFlowActive=!!active;
 schedulePzemHouseFlowLayout();
}
window.addEventListener('resize',schedulePzemHouseFlowLayout,{passive:true});
window.addEventListener('load',schedulePzemHouseFlowLayout);

function renderEnergy(t){
 window.lastStatusTelemetry=t;
 // Port model. Do not combine AC, PV and battery registers into a synthetic
 // "loss" value: they are measured at different conversion stages.
 // reg208 remains diagnostic until its exact ANENJI semantics are confirmed.
 const pv=Number(t.pv_power??0),gp=Number(t.grid_power??0),ip=Number(t.inverter_power??0),load=Number(t.output_active_power??0);
 const bv=Number(t.battery_voltage??0),bc=Number(t.net_battery_current??t.battery_current??0),soc=Number(t.battery_soc??0),gv=Number(t.grid_voltage??0);
 const gridA=gv>1?Math.abs(gp)/gv:0, prof=profileDefs[activeProfile], energy=prof?prof.energy_kwh*(soc/100):null;
 const bpRaw=Number(t.battery_power);
 const battWAbs=Number.isFinite(bpRaw)&&Math.abs(bpRaw)>0.5?Math.abs(bpRaw):Math.abs(bv*bc);
 const gridImport=Math.max(0,gp),gridExport=Math.max(0,-gp),pvIn=Math.max(0,pv),loadOut=Math.max(0,load);
 const autonomy=autonomyEstimate(t,prof,bv,bc,soc);
 const pz=(window.lastStatusPacket&&window.lastStatusPacket.pzem)||{};
 const pzFresh=!!pz.online && Number.isFinite(Number(pz.power));
 const pzW=pzFresh?Number(pz.power):0;
 if($('fInput')) $('fInput').textContent=pzFresh?f(pzW,0)+' W':'— W';
 if($('fInputSub')) $('fInputSub').textContent=pzFresh?`${f(pz.voltage,1)} V · ${f(pz.current,3)} A · ${f(pz.frequency,1)} Hz`:(currentLang==='ru'?'PZEM нет данных':'PZEM no data');
 if($('inputDown')) $('inputDown').className='flow-v'+(pzFresh&&pzW>5?' active down':'');
 updatePzemHouseFlow(pzFresh&&pzW>5);
 $('fPv').textContent=f(pv,0)+' W'; $('fPvSub').textContent=`${f(t.pv_voltage)} V · ${f(t.pv_current)} A`;
 $('fGrid').textContent=f(Math.abs(gp),0)+' W'; $('fGridSub').textContent=`${gp>5?'→ ANENJI':(gp< -5?'ANENJI → сеть':'ожидание')} · ${f(gv)} V · ≈${f(gridA,1)} A`;
 $('fInv').textContent=(currentLang==='ru'?'Инвертор ':'Inverter ')+f(ip,0)+' W';
 $('fInvSub').textContent=`${f(t.inverter_voltage)} V · ${f(t.inverter_current)} A · ${f(t.inverter_frequency,2)} Hz`;
 $('fLoad').textContent=f(loadOut,0)+' W'; $('fLoadSub').textContent=`${f(t.load_percent,0)} % · ${f(t.output_current)} A · ${f(t.output_apparent_power,0)} VA`;
 $('fBat').textContent=f(bv)+' V'; $('fBatSub').textContent=`${f(soc,0)} % · ${f(bc)} A · ${f(battWAbs,0)} W · ${bc>0.2?(currentLang==='ru'?'заряд':'charge'):(bc< -0.2?(currentLang==='ru'?'разряд':'discharge'):(currentLang==='ru'?'ожидание':'idle'))}`;
 $('pvDown').className='flow-v'+(pvIn>5?' active down':'');
 $('gridLine').className='flow-line'+(gridImport>5?' active right':(gridExport>5?' active export left':''));
 $('loadLine').className='flow-line'+(loadOut>5?' active right':'');
 $('batVert').className='flow-v'+(bc>0.2?' active down':(bc< -0.2?' active discharge up':''));
 const L=currentLang==='ru';
 $('infoBattery').innerHTML=kv(L?'Напряжение':'Voltage',f(bv)+' V')+kv(L?'Ток':'Current',f(bc)+' A')+kv(L?'Мощность':'Power',f(battWAbs,0)+' W')+kv(L?'Направление':'Direction',bc>0.2?(L?'Заряд':'Charge'):(bc< -0.2?(L?'Разряд':'Discharge'):(L?'Ожидание':'Idle')))+kv('SOC',f(soc,0)+' %')+kv(L?'Остаток энергии':'Estimated energy',energy!=null?f(energy,2)+' kWh':'—')+(typeof autonomy!=='undefined'?kv(L?'Автономная работа':'Estimated runtime',autonomy.text)+kv(L?'Расчёт автономности':'Runtime basis',autonomy.sub):'')+kv(L?'Заряд от инвертора':'Charge from inverter',f(t.inverter_charge_current)+' A')+kv(L?'Заряд от PV':'Charge from PV',f(t.pv_charge_current)+' A');
 $('infoPv').innerHTML=kv(L?'Напряжение':'Voltage',f(t.pv_voltage)+' V')+kv(L?'Ток':'Current',f(t.pv_current)+' A')+kv(L?'Мощность PV':'PV power',f(pvIn,0)+' W')+kv(L?'Мощность зарядки PV (диагн.)':'PV charging power (diag.)',f(t.pv_charging_power,0)+' W');
 $('infoInv').innerHTML=kv(L?'Выход в нагрузку':'Load output',f(loadOut,0)+' W')+kv(L?'Мощность инвертора · reg208':'Inverter power · reg208',f(ip,0)+' W')+kv(L?'Ток силового тракта':'Power-stage current',f(t.inverter_current)+' A')+kv(L?'Напряжение выхода':'Output voltage',f(t.output_voltage)+' V')+kv(L?'Частота выхода':'Output frequency',f(t.output_frequency,2)+' Hz')+kv(L?'Полная мощность нагрузки':'Load apparent power',f(t.output_apparent_power,0)+' VA')+kv('DCDC temp',f(t.dcdc_temperature,0)+' °C')+kv('Inverter temp',f(t.inverter_temperature,0)+' °C');
}

let browserLinkFails=0,browserLinkLostAt=0,statusLoadBusy=false,settingsWriteBusy=false;
function browserLinkSet(ok){const ov=$('linkLostOverlay');if(ok){browserLinkFails=0;browserLinkLostAt=0;if(ov)ov.classList.remove('show');return}browserLinkFails++;if(browserLinkFails<2)return;if(!browserLinkLostAt)browserLinkLostAt=Date.now();if(ov){ov.classList.add('show');const sec=Math.max(0,Math.floor((Date.now()-browserLinkLostAt)/1000));$('linkLostSince').textContent=`Нет ответа ${sec} с. Автопроверка продолжается каждые 3 с…`;}}
async function statusLoad(force=false){
 if(statusLoadBusy&&!force)return;
 statusLoadBusy=true;
 try{
  const x=await api(force?'/api/refresh':'/api/status'),t=x.telemetry||{};window.lastStatusPacket=x;writeEnabled=!!x.write_enabled;activeProfile=x.active_profile||activeProfile;
  $('tele').innerHTML=teleDefs.map(d=>`<div class=metric><small>${teleLabel(d[0],d[1])} · ${d[2]}</small><b>${f(t[d[0]],d[4])} ${d[3]}</b></div>`).join('');
  renderAlerts(x); eventsLoad(force);
  const pz=x.pzem||{}; window.lastPzem=pz; if($('pzV')){ $('pzV').textContent=pz.online?f(pz.voltage,1)+' V':'— V'; $('pzA').textContent=pz.online?f(pz.current,3)+' A':'— A'; $('pzW').textContent=pz.online?f(pz.power,1)+' W':'— W'; $('pzKwh').textContent=pz.online?f(pz.energy_kwh,3)+' kWh':'— kWh'; $('pzHz').textContent=pz.online?f(pz.frequency,1)+' Hz':'— Hz'; $('pzPf').textContent=pz.online?f(pz.pf,2):'—'; $('pzemEnabled').checked=!!pz.enabled; $('pzemAddr').value=pz.address||1; $('pzemState').textContent=pz.enabled?(pz.online?('ONLINE · '+pz.age_ms+' ms'):'OFFLINE'):'выключен'; if(document.activeElement!==$('pzemT1'))$('pzemT1').value=Number(pz.t1_kwh||0).toFixed(3); if(document.activeElement!==$('pzemT2'))$('pzemT2').value=Number(pz.t2_kwh||0).toFixed(3); }
  if($('infoPzem')) $('infoPzem').innerHTML=kv('Мощность всего ввода',pz.online?f(pz.power,0)+' W':'—')+kv('Напряжение',pz.online?f(pz.voltage,1)+' V':'—')+kv('Ток',pz.online?f(pz.current,3)+' A':'—')+kv('Частота',pz.online?f(pz.frequency,1)+' Hz':'—')+kv('PF',pz.online?f(pz.pf,2):'—')+kv('PZEM энергия',pz.online?f(pz.energy_kwh,3)+' kWh':'—')+kv('T1 · 07–23',f(pz.t1_kwh,3)+' kWh')+kv('T2 · 23–07',f(pz.t2_kwh,3)+' kWh')+kv('Текущий тариф',pz.tariff===1?'T1':(pz.tariff===2?'T2':'—'));
  const wb=$('wifiBadge'),ib=$('invBadge'),swb=$('stateWifiBadge'),sib=$('stateInvBadge');
  const wifiClass='badge '+(x.wifi_online?'online':'offline');
  const wifiText=x.wifi_online?'Wi‑Fi ONLINE':'Wi‑Fi OFFLINE';
  const invClass='badge '+(x.inverter_online?'online':'offline');
  const invText=x.inverter_online?(currentLang==='ru'?'Инвертор ONLINE':'Inverter ONLINE'):(currentLang==='ru'?'Инвертор OFFLINE':'Inverter OFFLINE');
  if(wb){wb.className=wifiClass;wb.textContent=wifiText;}
  if(swb){swb.className=wifiClass;swb.textContent=wifiText;}
  if(ib){ib.className=invClass;ib.textContent=invText;}
  if(sib){sib.className=invClass;sib.textContent=invText;}
  $('energyFlow').className='flow'+(x.inverter_online?'':' offline');
  const lastOk=x.last_ok_age_ms==null?'ещё не было':(x.last_ok_age_ms+' ms назад');
  $('diag').innerHTML=`IP: <code>${x.wifi_ip}</code> · RSSI ${x.rssi} dBm · uptime ${x.uptime_s}s<br>Modbus slave ${x.modbus_slave} · RTU TX/RX: ${x.rtu_tx}/${x.rtu_rx} · errors ${x.rtu_errors} · timeouts ${x.rtu_timeouts} · last OK: ${lastOk}<br>Последняя ошибка: ${x.last_error||'нет'}`;
  $('diagExtra').innerHTML=`RTC: ${x.rtc_present?'OK':'—'} · Scheduler ${x.scheduler_runs}/${x.scheduler_errors}<br>Web writes/errors: ${x.web_writes}/${x.web_write_errors} · write busy: ${x.write_busy?'YES':'no'}<br>RTU worker: ${x.rtu_worker_busy?'BUSY':'idle'} · queue ${x.rtu_queue_depth} · hb ${x.rtu_worker_heartbeat} · age ${x.rtu_worker_age_ms} ms · settings ${x.settings_refreshing?'refreshing':'idle'}<br>WDT: Task ${x.task_wdt?'ON':'OFF'} · RTU recoveries ${x.rtu_uart_recoveries} · RTU stall ${x.rtu_worker_stall_events||0} · RTU reboot ${x.rtu_controlled_reboots||0} · Wi-Fi reboot attempts ${x.wifi_watchdog_reboots}<br>Heap: ${(x.free_heap/1024).toFixed(1)} KB · minimum ${(x.min_free_heap/1024).toFixed(1)} KB · reset: ${x.reset_reason}`;
  $('age').textContent='Возраст телеметрии: '+x.age_ms+' ms';renderEnergy(t);browserLinkSet(true);
 }catch(e){browserLinkSet(false);$('diag').innerHTML='<span class=bad>Нет связи с ESP32: '+e.message+'</span>'}
 finally{statusLoadBusy=false}
}

function selectOptions(reg,val){const maps={300:{0:'Single',1:'Parallel',2:'3P1 / L1',3:'3P2 / L2',4:'3P3 / L3'},301:{1:'SUB',2:'SBU',3:'SUF',4:'ZEC'},302:{0:'APL',1:'UPS',2:'GEN'},303:{0:'Mute',1:'Source/Warning/Fault',2:'Warning/Fault',3:'Fault only'},305:{0:'Off after 1 minute',1:'Always light'},306:{0:'Disabled',1:'After 1 minute'},307:{0:'Disabled',1:'Enable'},308:{0:'Prohibit',1:'Allow'},309:{0:'Prohibit',1:'Allow'},310:{0:'Prohibit',1:'Allow'},313:{0:'Disabled',1:'Enable'},320:{2200:'220 V',2300:'230 V',2400:'240 V'},321:{5000:'50 Hz',6000:'60 Hz'},322:{0:'AGM',1:'FLD',2:'USER / USE'},331:{1:'SOF — Solar first',2:'SNU — Solar + Utility',3:'OSO — Only Solar',4:'SOR — Solar residual'},406:{0:'Local or remote',1:'Local only',2:'Remote only'}};const m=maps[reg]||{};return Object.entries(m).map(([k,v])=>`<option value="${k}" ${Number(k)===Number(val)?'selected':''}>${v}</option>`).join('')}

const SETTING_RU={300:'Режим выхода AC',301:'Приоритет выхода',302:'Диапазон входного напряжения',303:'Зуммер',305:'Подсветка LCD',306:'Автовозврат LCD',307:'Энергосбережение',308:'Автоперезапуск после перегрузки',309:'Автоперезапуск после перегрева',310:'Обход при перегрузке',313:'Выравнивание АКБ',320:'Выходное напряжение',321:'Выходная частота',322:'Тип АКБ',323:'Защита АКБ от перенапряжения',324:'Bulk / CV',325:'Float',326:'Возврат к сети, напряжение',327:'Low DC в режиме сети',329:'Low DC вне сети',330:'CV → Float, время',331:'Приоритет зарядки',332:'Макс. ток зарядки',333:'Макс. ток зарядки от сети',334:'Напряжение выравнивания',335:'Время выравнивания',336:'Тайм-аут выравнивания',337:'Интервал выравнивания',341:'Low DC SOC в режиме сети',342:'SOC восстановления',343:'SOC отключения вне сети',344:'Макс. отдача в сеть',351:'Макс. ток разряда АКБ',406:'Режим включения'};
const SETTING_EN={300:'AC output mode',301:'Output priority',302:'Input voltage range',303:'Buzzer',305:'LCD backlight',306:'LCD auto return',307:'Power saving',308:'Overload auto restart',309:'Over-temperature auto restart',310:'Overload bypass',313:'Battery equalization',320:'Output voltage',321:'Output frequency',322:'Battery type',323:'Battery overvoltage protection',324:'Bulk / CV',325:'Float',326:'Back to grid voltage',327:'Low DC in grid mode',329:'Low DC off-grid',330:'CV → Float time',331:'Charging priority',332:'Max charging current',333:'Max utility charging current',334:'Equalization voltage',335:'Equalization time',336:'Equalization timeout',337:'Equalization interval',341:'Low DC SOC in grid mode',342:'Recovery SOC',343:'Off-grid cut-off SOC',344:'Max grid feed-in',351:'Max battery discharge current',406:'Turn-on mode'};
function settingLabel(x){return (currentLang==='ru'?SETTING_RU:SETTING_EN)[x.reg]||x.label}
function groupLabel(g){
 const ru={'Основные':'Основные','Прочие':'Прочие','АКБ':'АКБ','SOC':'SOC','Remote':'Удалённое управление'};
 const en={'Основные':'General','Прочие':'Other','АКБ':'Battery','SOC':'SOC','Remote':'Remote control'};
 return (currentLang==='ru'?ru:en)[g]||g;
}
function settingSelectLabel(reg,val){
 const maps={300:{0:'Single',1:'Parallel',2:'3 Phase',3:'Split Phase'},301:{1:'SUB',2:'SBU',3:'SUF',4:'ZEC'},302:{0:'APL — Appliance',1:'UPS'},303:{0:'Off',1:'On'},305:{0:'Off',1:'On'},306:{0:'Off',1:'On'},307:{0:'Off',1:'On'},308:{0:'Off',1:'On'},309:{0:'Off',1:'On'},310:{0:'Off',1:'On'},313:{0:'Off',1:'On'},320:{2200:'220 V',2300:'230 V',2400:'240 V'},321:{5000:'50 Hz',6000:'60 Hz'},322:{0:'AGM',1:'FLD',2:'USER / USE'},331:{1:'SOF — Solar first',2:'SNU — Solar + Utility',3:'OSO — Only Solar',4:'SOR — Solar residual'},406:{0:'Local or remote',1:'Local only',2:'Remote only'}};
 const m=maps[reg]||{};
 return m[val]??m[String(val)]??String(val);
}
function settingUnit(reg){
 if([320,323,324,325,326,327,329,334].includes(reg))return ' V';
 if([332,333,351].includes(reg))return ' A';
 if([321].includes(reg))return ' Hz';
 if([341,342,343].includes(reg))return ' %';
 if([344].includes(reg))return ' W';
 if([330,335,336,337].includes(reg))return '';
 return '';
}
function settingCurrentText(s){
 if(s.kind==='select')return settingSelectLabel(s.reg,s.raw);
 const unit=settingUnit(s.reg);
 return `${s.value}${unit}`;
}
function renderSettingsGroups(data){
 window.lastSettingsData=data;
 const groups={};
 (data||[]).forEach(x=>{(groups[x.group]||(groups[x.group]=[])).push(x)});
 const order=Object.keys(groups);
 $('settings').innerHTML=order.map(group=>{
   const rows=groups[group].map(s=>{
     if(!s.ok)return `<tr><td>${s.reg}</td><td>${settingLabel(s)}</td><td class=bad>${s.error||(currentLang==='ru'?'ошибка':'error')}</td><td>—</td><td></td></tr>`;
     const ctl=s.kind==='select'
       ?`<select id="s${s.reg}" ${writeEnabled?'':'disabled'}>${selectOptions(s.reg,s.raw)}</select>`
       :`<input class=value id="s${s.reg}" type=number step="${s.scale===10?'0.1':'1'}" value="${s.value}" ${writeEnabled?'':'disabled'}>`;
     const current=`<span class="settings-current">${settingCurrentText(s)}<span class="raw">raw ${s.raw}</span></span>`;
     return `<tr><td>${s.reg}</td><td>${settingLabel(s)}</td><td id="sc${s.reg}">${current}</td><td class="settings-new">${ctl}</td><td><button ${writeEnabled?'':'disabled'} onclick="settingWrite(${s.reg})">${currentLang==='ru'?'Записать':'Write'}</button></td></tr>`;
   }).join('');
   return `<section class="settings-group"><h3>${groupLabel(group)}</h3><div class="scroll"><table><thead><tr><th>${currentLang==='ru'?'Регистр':'Register'}</th><th>${currentLang==='ru'?'Параметр':'Parameter'}</th><th>${currentLang==='ru'?'Текущее значение':'Current value'}</th><th>${currentLang==='ru'?'Новое значение':'New value'}</th><th></th></tr></thead><tbody>${rows}</tbody></table></div></section>`;
 }).join('');
}


function updateSettingCurrentInPlace(reg,raw,value){
 const row=(window.lastSettingsData||[]).find(x=>Number(x.reg)===Number(reg));
 if(!row)return;
 row.raw=raw; row.value=value; row.ok=true;
 const cell=$('sc'+reg);
 if(cell){
  cell.innerHTML=`<span class="settings-current">${settingCurrentText(row)}<span class="raw">raw ${row.raw}</span></span>`;
 }
 const input=$('s'+reg);
 if(input)input.value=(row.kind==='select'?row.raw:row.value);
}

async function settingsLoad(force=false){
 $('settingsMsg').textContent=force?(currentLang==='ru'?'Запрос на перечитывание настроек отправлен…':'Settings refresh queued…'):(currentLang==='ru'?'Загрузка настроек…':'Loading settings…');
 try{
  let j=await api('/api/settings'+(force?'?refresh=1':''));
  if(j.refreshing){
   const deadline=Date.now()+9000, gen=j.refresh_generation;
   while(Date.now()<deadline){ await new Promise(r=>setTimeout(r,300)); j=await api('/api/settings'); if(!j.refreshing && j.refresh_generation!==gen)break; }
  }
  writeEnabled=!!j.write_enabled;renderSettingsGroups(j.data||[]);
  $('settingsMsg').textContent=j.refreshing?(currentLang==='ru'?'Перечитывание ещё выполняется в фоне.':'Refresh still running in background.'):(currentLang==='ru'?'Настройки загружены из кэша RTU worker.':'Settings loaded from RTU worker cache.');
 }catch(e){$('settingsMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}
async function settingWrite(reg){
 const el=$('s'+reg),v=Number(el.value);
 if(!confirm(currentLang==='ru'?`Записать новое значение в регистр ${reg}: ${v}?`:`Write new value to register ${reg}: ${v}?`))return;
 settingsWriteBusy=true;
 $('settingsMsg').textContent=currentLang==='ru'?`Команда записи регистра ${reg} отправляется…`:`Queuing register ${reg} write…`;
 try{
  // POST returns immediately (202). FC16 + read-back runs in a FreeRTOS worker.
  const q=await api('/api/write',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({register:reg,value:v}),timeoutMs:2500});
  if(!q.queued||!q.job_id)throw new Error(currentLang==='ru'?'Команда не поставлена в очередь':'Write was not queued');
  $('settingsMsg').textContent=currentLang==='ru'?`Регистр ${reg}: запись выполняется, ожидаю контрольное чтение…`:`Register ${reg}: writing, waiting for read-back…`;

  const deadline=Date.now()+12000;
  let j=null;
  while(Date.now()<deadline){
   await new Promise(r=>setTimeout(r,250));
   const st=await api('/api/write/status?id='+encodeURIComponent(q.job_id),{timeoutMs:1800});
   if(st.done){j=st;break}
  }
  if(!j)throw new Error(currentLang==='ru'?'Таймаут ожидания результата записи':'Timed out waiting for write result');
  if(!j.ok||!j.verified)throw new Error(j.error||(currentLang==='ru'?'Нет подтверждения контрольным чтением':'No read-back confirmation'));

  // Read-back already proved the value in the inverter. Update only this row;
  // rebuilding the entire settings DOM can trigger browser scroll anchoring and
  // move the page down to the Wi-Fi card.
  updateSettingCurrentInPlace(reg,j.new_raw,j.new_value);
  $('settingsMsg').innerHTML=`<span class=ok>${currentLang==='ru'?'Подтверждено чтением':'Verified by read-back'}: ${currentLang==='ru'?'регистр':'register'} ${reg}, RAW ${j.old_raw} → ${j.new_raw}</span>`;
 }catch(e){
  $('settingsMsg').innerHTML='<span class=bad>'+(currentLang==='ru'?'Запись не подтверждена: ':'Write not confirmed: ')+e.message+'</span>';
 }finally{
  settingsWriteBusy=false;
  setTimeout(()=>statusLoad(false),500);
 }
}

function fmtProfileValue(k,v){
 const units={bulk_v:'V',float_v:'V',low_mains_v:'V',cutoff_v:'V',capacity_ah:'Ah',energy_kwh:'kWh',max_charge_a:'A',max_utility_charge_a:'A',max_discharge_a:'A'};
 return `${v} ${units[k]||''}`;
}
function renderProfilePreview(){
 const id=$('profileSelect').value,p=profileDefs[id];
 if(!p){$('profileInfo').textContent='Нет профиля';$('profilePreview').innerHTML='';return}
 const compat=!!p.compatible;
 $('profileApplyBtn').disabled=!writeEnabled||!compat;
 const d=window.profileDetect||{};
 $('profileInfo').innerHTML=`<b>${p.label}</b> · <span class="badge ${compat?'online':'offline'}">${compat?'ДОСТУПЕН':'ЗАБЛОКИРОВАН'}</span><br><span class=small>${p.description||''}</span><br><span class=small>Активный профиль: <b>${activeProfile}</b> · запись: <b class="${writeEnabled?'ok':'bad'}">${writeEnabled?'разрешена':'запрещена'}</b></span><br><span class=small>Определение системы: <b>${d.system_verified?(d.detected_system_v+' V подтверждено'):'не подтверждено'}</b> · АКБ ${d.battery_v??'—'} V · Bulk ${d.bulk_v??'—'} V · Float ${d.float_v??'—'} V</span>`;
 const rows=[
  ['Система',p.system_v+' V'],['Ёмкость',fmtProfileValue('capacity_ah',p.capacity_ah)],['Энергия',fmtProfileValue('energy_kwh',p.energy_kwh)],
  ['Bulk/CV',fmtProfileValue('bulk_v',p.bulk_v)],['Float',fmtProfileValue('float_v',p.float_v)],
  ['Low DC mains',fmtProfileValue('low_mains_v',p.low_mains_v)],['Off-grid cut-off',fmtProfileValue('cutoff_v',p.cutoff_v)],
  ['Макс. заряд',fmtProfileValue('max_charge_a',p.max_charge_a)],['Заряд от сети',fmtProfileValue('max_utility_charge_a',p.max_utility_charge_a)],
  ['Макс. разряд',fmtProfileValue('max_discharge_a',p.max_discharge_a)]
 ];
 $('profilePreview').innerHTML='<table><tbody>'+rows.map(r=>`<tr><td>${r[0]}</td><td><b>${r[1]}</b></td></tr>`).join('')+'</tbody></table>';
 $('profileMsg').textContent=compat?'Система напряжения подтверждена. При применении значения записываются с read-back проверкой.':`Запись заблокирована до подтверждения подходящей системы напряжения. ${d.detect_reason||''}`;
}
async function profilesLoad(){
 try{
  const j=await api('/api/profiles');
  writeEnabled=!!j.write_enabled;activeProfile=j.active_profile||activeProfile;profileDefs=j.profiles||{};
  window.profileDetect=j;
  $('profileSelect').innerHTML=Object.entries(profileDefs).map(([id,p])=>`<option value="${id}" ${id===activeProfile?'selected':''}>${p.compatible?'':'[заблокирован] '}${p.label}</option>`).join('');
  renderProfilePreview();
 }catch(e){$('profileInfo').innerHTML='<span class=bad>'+e.message+'</span>'}
}
async function applySelectedProfile(){
 const id=$('profileSelect').value,p=profileDefs[id]; if(!p||!p.compatible)return; if(!confirm(`Применить профиль "${p.label}"?`))return;
 try{
  const q=await api('/api/profile/apply',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({profile:id})});
  if(!q.queued||!q.job_id)throw new Error('Profile was not queued');
  $('profileMsg').textContent=currentLang==='ru'?'Профиль записывается в RTU worker…':'Profile is being written by RTU worker…';
  const deadline=Date.now()+45000; let r=null;
  while(Date.now()<deadline){await new Promise(x=>setTimeout(x,350));const st=await api('/api/profile/status?id='+q.job_id);if(st.done){r=st;break}}
  if(!r)throw new Error(currentLang==='ru'?'Таймаут применения профиля':'Profile timeout'); if(!r.ok)throw new Error(r.error||'Profile failed');
  activeProfile=id;
  if(r.partial){
   const failed=(r.failures||[]).map(x=>`reg ${x.reg} (RAW ${x.raw}): ${x.error}`).join('<br>');
   $('profileMsg').innerHTML='<span class=bad>'+(currentLang==='ru'
     ?`Профиль выбран для расчётов. Записано ${r.applied}/${r.attempted}. Не записаны:<br>${failed}`
     :`Profile selected for calculations. Written ${r.applied}/${r.attempted}. Failed:<br>${failed}`)+'</span>';
  }else{
   $('profileMsg').innerHTML='<span class=ok>'+(currentLang==='ru'?'Профиль применён и проверен.':'Profile applied and verified.')+'</span>';
  }
  await settingsLoad(true);await profilesLoad();
 }catch(e){$('profileMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}

async function wifiScan(){
 const btn=$('wifiScanBtn'), state=$('wifiScanState'), sel=$('netSsid');
 if(!btn||!state||!sel)return;

 btn.disabled=true;
 btn.textContent='Сканирование…';
 state.textContent='Запуск сканирования…';
 sel.innerHTML='<option value="">Поиск сетей…</option>';

 try{
  await api('/api/wifi/scan?start=1');
  const deadline=Date.now()+20000;

  while(Date.now()<deadline){
   await new Promise(r=>setTimeout(r,500));
   const j=await api('/api/wifi/scan');

   if(j.scanning){
    state.textContent='Идёт поиск Wi‑Fi…';
    continue;
   }

   const nets=j.networks||[];
   if(j.scan_failed){
    state.innerHTML='<span class=bad>Ошибка сканирования</span>';
    sel.innerHTML='<option value="">— ошибка сканирования —</option>';
   }else if(!nets.length){
    state.textContent='Сети не найдены';
    sel.innerHTML='<option value="">— сети не найдены —</option>';
   }else{
    state.innerHTML=`<span class=ok>Найдено сетей: ${nets.length}</span>`;
    sel.innerHTML='<option value="">— выбрать сеть —</option>'+
      nets.map(n=>`<option value="${n.ssid.replaceAll('"','&quot;')}">${n.ssid} · ${n.rssi} dBm${n.open?' · OPEN':''}</option>`).join('');
   }
   return;
  }

  state.innerHTML='<span class=bad>Таймаут сканирования</span>';
  sel.innerHTML='<option value="">— таймаут —</option>';
 }catch(e){
  state.innerHTML='<span class=bad>'+e.message+'</span>';
  sel.innerHTML='<option value="">— ошибка —</option>';
 }finally{
  btn.disabled=false;
  btn.textContent='Сканировать Wi‑Fi';
 }
}

async function netLoad(){try{const j=await api('/api/network');setupMode=!!j.setup_mode;$('netDhcp').checked=!!j.dhcp;$('netIp').value=j.configured_ip||'';$('netGw').value=j.gateway||'';$('netMask').value=j.mask||'';$('netDns').value=j.dns||'';$('netSsidManual').value=j.ssid||'';$('netState').textContent=(setupMode?'SETUP MODE · ':'')+`SSID: ${j.ssid||'не настроен'} · сейчас IP ${j.current_ip||'—'} · сеть настройки: ${j.setup_ssid}`;netToggle();if(setupMode)wifiScan()}catch(e){$('netState').innerHTML='<span class=bad>'+e.message+'</span>'}}
function netToggle(){const dis=$('netDhcp').checked;['netIp','netGw','netMask','netDns'].forEach(x=>$(x).disabled=dis)}
function netSsidPicked(){const v=$('netSsid').value;if(v)$('netSsidManual').value=v}
function netSsidTyped(){const v=$('netSsidManual').value.trim();if(v&&$('netSsid').value!==v)$('netSsid').value=''}

async function netSave(){const dhcp=$('netDhcp').checked,manual=$('netSsidManual').value.trim(),selected=$('netSsid').value,ssid=manual||selected;if(!ssid){alert('Выберите или введите SSID');return}if(!confirm('Сохранить Wi‑Fi/сетевые настройки и перезагрузить ESP32?'))return;try{const headers=adminHeaders();await api('/api/network',{method:'POST',headers,body:JSON.stringify({ssid,password:$('netPass').value,dhcp,ip:$('netIp').value,gateway:$('netGw').value,mask:$('netMask').value,dns:$('netDns').value})});$('netState').textContent='Настройки сохранены. ESP32 перезагружается...'}catch(e){$('netState').innerHTML='<span class=bad>'+e.message+'</span>'}}
async function enterSetup(){if(!confirm('Перезагрузить ESP32 в режим переинициализации Wi‑Fi? Текущие настройки сохранятся.'))return;try{await api('/api/setup/enter',{method:'POST',headers:adminHeaders(),body:'{}'});$('netState').textContent='Перезагрузка в режим настройки Wi‑Fi...'}catch(e){alert(e.message)}}
async function forgetWifi(){if(!confirm('Удалить сохранённые SSID, пароль и IP-настройки? После перезагрузки ESP32 запустит сеть первичной настройки.'))return;try{await api('/api/wifi/forget',{method:'POST',headers:adminHeaders(),body:'{}'});$('netState').textContent='Wi‑Fi настройки удалены. Перезагрузка...'}catch(e){alert(e.message)}}

async function modbusLoad(){
 try{
  const j=await api('/api/modbus/config');
  $('modbusSlave').value=j.slave;
  $('modbusBaud').value=j.baud;
  $('modbusFormat').value=j.format;
  $('modbusState').textContent='Текущий адрес: '+j.slave;
 }catch(e){$('modbusState').innerHTML='<span class=bad>'+e.message+'</span>'}
}
async function modbusSave(){
 const slave=Number($('modbusSlave').value);
 if(!Number.isInteger(slave)||slave<1||slave>247){alert('Адрес Modbus должен быть 1…247');return}
 if(!confirm(`Сохранить Modbus RTU адрес ${slave}?`))return;
 try{
  const j=await api('/api/modbus/config',{method:'POST',headers:adminHeaders(),body:JSON.stringify({slave})});
  $('modbusState').innerHTML=`<span class=ok>Сохранено: адрес ${j.slave}</span>`;
  await statusLoad(true);
 }catch(e){$('modbusState').innerHTML='<span class=bad>'+e.message+'</span>'}
}



const TAB_STORAGE_KEY='anenji_active_tab';

async function dlReq(path,body={},admin=false){const h={'Content-Type':'application/json'};if(admin){const p=prompt(currentLang==='ru'?'Пароль администратора ESP32':'ESP32 admin password');if(p===null)throw new Error('cancelled');h['X-ANENJI-Admin']=p;}const r=await fetch(path,{method:'POST',headers:h,body:JSON.stringify(body)});let x={};try{x=await r.json();}catch(e){}if(!r.ok||x.ok===false)throw new Error(x.error||('HTTP '+r.status));return x;}
function dlTarget(){return ($('dlIp').value||'').trim();}
function dlShow(x){$('dlOut').textContent=JSON.stringify(x,null,2);}
async function dlStatus(){try{const r=await fetch('/api/datalogger/status',{cache:'no-store'}),x=await r.json();$('dlStatus').textContent=`STA: ${x.sta_ssid||'—'} ${x.sta_ip||''} | gateway ${x.gateway||'—'} | service AP ${x.service_ap_running?'ON':'OFF'}`;if(x.service_mode&&x.gateway&&!dlTarget())$('dlIp').value=x.gateway;}catch(e){}}
async function dlServiceAp(){try{const x=await dlReq('/api/datalogger/service-ap/start',{},true);dlShow(x);alert(`Подключитесь к Wi-Fi ${x.ssid}\nПароль: ${x.password}\nОткройте http://${x.ip}`);dlStatus();}catch(e){alert(e.message)}}
async function dlConnectAp(){try{const x=await dlReq('/api/datalogger/connect',{ssid:$('dlApSsid').value,password:$('dlApPass').value},true);dlShow(x);if(x.datalogger_ip)$('dlIp').value=x.datalogger_ip;dlStatus();}catch(e){alert(e.message)}}
async function dlRestore(){try{dlShow(await dlReq('/api/datalogger/restore',{},true));}catch(e){alert(e.message)}}
async function dlInfo(){try{dlShow(await dlReq('/api/datalogger/info',{ip:dlTarget()}));}catch(e){alert(e.message)}}
async function dlPing(){try{dlShow(await dlReq('/api/datalogger/ping',{ip:dlTarget()}));}catch(e){alert(e.message)}}
async function dlSet(name){const v=name==='ssid'?$('dlStaSsid').value:$('dlStaPass').value;if(!confirm('Записать параметр '+name+' в datalogger?'))return;try{dlShow(await dlReq('/api/datalogger/set',{ip:dlTarget(),name:name,value:v},true));}catch(e){alert(e.message)}}
async function dlRestart(){if(!confirm('Перезапустить datalogger?'))return;try{dlShow(await dlReq('/api/datalogger/set',{ip:dlTarget(),name:'restart',value:'1'},true));}catch(e){alert(e.message)}}
setInterval(dlStatus,5000);setTimeout(dlStatus,1200);

const TAB_RULES=[
 ['Состояние / аварии','overview'],['Энергопотоки','overview'],['Полная телеметрия','overview'],
 ['Связь','service'],['Datalogger','service'],
 ['Статистика аккумулятора','battery'],
 ['Настройки инвертора','settings'],
 ['RTC / Планировщик','scheduler'],
 ['Wi‑Fi / сеть ESP32','network'],['Modbus / RS‑485','network'],
 ['Чтение регистров','service']
];
function initTabs(){
 const host=$('tabHost'); if(!host)return;
 const pages={};
 for(const id of ['overview','battery','settings','scheduler','network','service']){const p=document.createElement('div');p.id='tab-'+id;p.className='tab-page';host.appendChild(p);pages[id]=p}
 const cards=[...document.querySelectorAll('.wrap>.card')];
 for(const c of cards){
   const h=c.querySelector('h2'); const title=h?h.textContent.trim():''; let page='service';
   const r=TAB_RULES.find(x=>title===x[0]); if(r)page=r[1];
   else if(c.querySelector('#profilesDetails'))page='battery';
   pages[page].appendChild(c);
 }
 showTab(localStorage.getItem(TAB_STORAGE_KEY)||'overview',false);
}
function showTab(id,remember=true){
 if(!document.getElementById('tab-'+id))id='overview';
 document.querySelectorAll('.tab-page').forEach(p=>p.classList.toggle('active',p.id==='tab-'+id));
 document.querySelectorAll('.app-tab').forEach(b=>b.classList.toggle('active',b.dataset.page===id));
 if(remember)localStorage.setItem(TAB_STORAGE_KEY,id);
 window.scrollTo({top:0,behavior:'instant'});
 if(id==='overview')schedulePzemHouseFlowLayout();
}

function daysOptions(mask){
 const opts=[[127,'Каждый день'],[31,'Пн–Пт'],[96,'Сб–Вс'],[1,'Пн'],[2,'Вт'],[4,'Ср'],[8,'Чт'],[16,'Пт'],[32,'Сб'],[64,'Вс']];
 return opts.map(x=>`<option value="${x[0]}" ${x[0]==mask?'selected':''}>${x[1]}</option>`).join('');
}
function renderSchedule(tasks){
 $('schedBody').innerHTML=(tasks||[]).map(t=>{
  const hh=String(t.hour).padStart(2,'0'),mm=String(t.minute).padStart(2,'0');
  return `<tr><td>${t.slot+1}</td><td><input id="se${t.slot}" type="checkbox" ${t.enabled?'checked':''}></td>
   <td><select id="sd${t.slot}">${daysOptions(t.days)}</select></td>
   <td><input id="st${t.slot}" type="time" value="${hh}:${mm}"></td>
   <td><input id="sr${t.slot}" type="number" min="0" max="65535" value="${t.register}" style="width:90px"></td>
   <td><input id="sv${t.slot}" type="number" min="0" max="65535" value="${t.raw}" style="width:90px"></td>
   <td><button onclick="scheduleSave(${t.slot})">${currentLang==='ru'?'Сохранить':'Save'}</button></td></tr>`;
 }).join('');
}
function fmtDateKey(v){const s=String(v||'');return s.length===8?`${s.slice(6,8)}.${s.slice(4,6)}.${s.slice(0,4)}`:'—'}
async function batteryStatsLoad(showFeedback=false){
 const btn=$('bsRefreshBtn');
 if(showFeedback){if(btn)btn.disabled=true;$('bsMsg').textContent='Обновление статистики…';}
 try{
  const x=await api('/api/battery/stats');
  $('bsEfc').textContent=Number(x.efc||0).toFixed(3);
  $('bsChargedAh').textContent=Number(x.charged_ah||0).toFixed(1)+' Ah';
  $('bsDischargedAh').textContent=Number(x.discharged_ah||0).toFixed(1)+' Ah';
  $('bsCharged').textContent=Number(x.charged_kwh||0).toFixed(2)+' kWh';
  $('bsDischarged').textContent=Number(x.discharged_kwh||0).toFixed(2)+' kWh';
  $('bsCapacity').textContent=x.calibrated_capacity_ah!=null?(Number(x.calibrated_capacity_ah).toFixed(1)+' Ah · '+(x.calibrated_capacity_valid?'калибровка':'НЕДОСТОВЕРНО')):'не задана';
  const c=x.calibration||{};
  $('bsDetails').innerHTML=`EEPROM: <b>${x.eeprom_present?'AT24C32 OK':'не найдена'}</b> · EFC считается по паспортной ёмкости профиля <b>${Number(x.reference_capacity_ah||0).toFixed(1)} Ah</b> · ${Number(x.reference_energy_kwh||0).toFixed(2)} kWh<br>`+
   `Сегодня: заряд ${Number(x.today_charged_kwh||0).toFixed(2)} kWh · разряд ${Number(x.today_discharged_kwh||0).toFixed(2)} kWh<br>`+
   `Макс. ток: заряд ${Number(x.max_charge_a||0).toFixed(1)} A · разряд ${Number(x.max_discharge_a||0).toFixed(1)} A<br>`+
   `Ток АКБ: reg232 <b>${x.raw_battery_current_a==null?'—':Number(x.raw_battery_current_a).toFixed(1)+' A'}</b> · для новой статистики <b>${x.stats_battery_current_a==null?'—':Number(x.stats_battery_current_a).toFixed(1)+' A'}</b> · коррекция +${Number(x.stats_current_offset_a||0).toFixed(1)} A<br>`+
   `Калибровка: <b>${c.active?'КОНТРОЛЬНЫЙ РАЗРЯД':'не активна'}</b>${c.active?` · ${c.start_soc}% → цель ${c.end_soc}% · сейчас ${c.current_soc??'—'}% · чистый разряд ${Number(c.net_discharge_ah||0).toFixed(1)} Ah / ${Number(c.net_discharge_kwh||0).toFixed(2)} kWh · ${Math.floor(Number(c.elapsed_sec||0)/3600)}:${String(Math.floor((Number(c.elapsed_sec||0)%3600)/60)).padStart(2,'0')}`:''}<br>${c.status||''}`;
  $('bsHistory').innerHTML=(x.history||[]).map(d=>`<tr><td>${fmtDateKey(d.date)}</td><td>${Number(d.charged_kwh).toFixed(2)} kWh</td><td>${Number(d.discharged_kwh).toFixed(2)} kWh</td><td>${Number(d.efc).toFixed(3)}</td></tr>`).join('');
  if(showFeedback)$('bsMsg').innerHTML='<span class=ok>Статистика обновлена · '+new Date().toLocaleTimeString()+'</span>';
 }catch(e){$('bsMsg').innerHTML='<span class=bad>Ошибка обновления: '+e.message+'</span>'}
 finally{if(showFeedback&&btn)btn.disabled=false;}
}
async function waitWriteJob(id,timeoutMs=12000){
 const deadline=Date.now()+timeoutMs;
 while(Date.now()<deadline){await new Promise(r=>setTimeout(r,300));const st=await api('/api/write/status?id='+encodeURIComponent(id),{timeoutMs:1800});if(st.done)return st}
 throw new Error('Тайм-аут подтверждения записи Modbus');
}
async function batteryCalibrationStart(){
 if(!confirm('Запустить контрольный разряд LiFePO4? Прошивка запишет RAW 2 (SBU) в регистр 301 и переведёт приоритет на Solar → Battery → Utility. Стартуйте только при SOC ≥90%.'))return;
 const headers=await verifiedAdminHeaders('bsMsg');if(!headers)return;
 try{
  $('bsMsg').innerHTML='<span class=ok>Пароль принят. Переключаю регистр 301 → 2 (SBU) и проверяю чтением…</span>';
  const q=await api('/api/battery/calibration/start',{method:'POST',headers,body:JSON.stringify({end_soc:20})});
  if(!q.queued||!q.job_id)throw new Error('Команда не поставлена в RTU очередь');
  const st=await waitWriteJob(q.job_id);
  if(!st.ok||!st.verified)throw new Error(st.error||'Регистр 301 не подтверждён чтением');
  $('bsMsg').innerHTML='<span class=ok>Контрольный разряд запущен: регистр 301 подтверждён как RAW 2 (SBU). Цель — 20% SOC.</span>';
  await batteryStatsLoad();
 }catch(e){$('bsMsg').innerHTML='<span class=bad>Калибровка не запущена: '+e.message+'</span>'}
}
async function batteryCalibrationFinish(){
 const headers=await verifiedAdminHeaders('bsMsg');if(!headers)return;
 if(!confirm('Завершить измерение и сохранить рассчитанную фактическую ёмкость?'))return;
 try{const r=await api('/api/battery/calibration/finish',{method:'POST',headers,body:'{}'});if(r.saved){$('bsMsg').innerHTML='<span class=ok>Калибровка сохранена: '+Number(r.capacity_ah).toFixed(1)+' Ah. Измерено '+Number(r.net_discharge_ah).toFixed(1)+' Ah, диапазон SOC '+r.soc_span+'%. '+(r.restore_queued?'Возврат режима 301 поставлен в очередь.':'')+'</span>'}else{$('bsMsg').innerHTML='<span class=bad>Измерение завершено: отдано '+Number(r.net_discharge_ah).toFixed(1)+' Ah, но ёмкость '+Number(r.capacity_ah).toFixed(1)+' Ah НЕ сохранена. '+(r.warning||'SOC недостаточно надёжен для экстраполяции')+'. EFC продолжает считаться по профилю '+Number(r.nominal_capacity_ah).toFixed(0)+' Ah.</span>'}await batteryStatsLoad()}catch(e){$('bsMsg').innerHTML='<span class=bad>Не удалось завершить: '+e.message+'</span>'}
}
async function batteryCalibrationCancel(){const headers=await verifiedAdminHeaders('bsMsg');if(!headers)return;try{const r=await api('/api/battery/calibration/cancel',{method:'POST',headers,body:'{}'});$('bsMsg').innerHTML='<span class=ok>Калибровка отменена. '+(r.restore_queued?'Прежний режим регистра 301 восстанавливается.':'')+'</span>';await batteryStatsLoad()}catch(e){$('bsMsg').innerHTML='<span class=bad>'+e.message+'</span>'}}
async function batteryCalibrationManual(){const ah=Number($('bsManualAh').value);if(!ah){$('bsMsg').innerHTML='<span class=bad>Введите ёмкость в Ah.</span>';return;}const headers=await verifiedAdminHeaders('bsMsg');if(!headers)return;try{await api('/api/battery/calibration/manual',{method:'POST',headers,body:JSON.stringify({capacity_ah:ah})});$('bsMsg').innerHTML='<span class=ok>Пароль принят. Фактическая ёмкость сохранена.</span>';await batteryStatsLoad()}catch(e){$('bsMsg').innerHTML='<span class=bad>'+e.message+'</span>'}}
async function batteryStatsReset(all){
 const headers=await verifiedAdminHeaders('bsMsg');if(!headers)return;
 const c=prompt(all?'Удалить ВСЮ статистику и калибровку? Введите RESET':'Обнулить EFC, Ah, kWh, токи и историю? Калибровка ёмкости сохранится. Введите RESET');
 if(c!=='RESET'){$('bsMsg').innerHTML='<span class=bad>Сброс отменён: необходимо точно ввести RESET.</span>';return;}
 $('bsMsg').textContent='Пароль принят. Стирание статистики…';
 try{
  const r=await api('/api/battery/stats/reset',{method:'POST',headers,body:JSON.stringify({confirm:'RESET',mode:all?'all':'stats'}),timeoutMs:12000});
  // Show the authoritative reset result immediately, before a follow-up GET.
  $('bsEfc').textContent='0.000';$('bsChargedAh').textContent='0.0 Ah';$('bsDischargedAh').textContent='0.0 Ah';
  $('bsCharged').textContent='0.00 kWh';$('bsDischarged').textContent='0.00 kWh';$('bsHistory').innerHTML='';
  if(all){$('bsManualAh').value='';$('bsCapacity').textContent='не задана';}
  const persist=r.persisted?'EEPROM: новая нулевая запись подтверждена':(r.eeprom_present?(r.storage_erased?'EEPROM очищена, но новая запись не подтверждена':'ОШИБКА очистки EEPROM'):'EEPROM не найдена — сброс только в RAM');
  $('bsMsg').innerHTML='<span class=ok>'+(all?'Сброс ВСЕЙ статистики выполнен.':'Сброс статистики выполнен; калибровка сохранена.')+'</span> · '+persist;
  setTimeout(()=>batteryStatsLoad(false),500);
 }catch(e){
  $('bsMsg').innerHTML='<span class=bad>Сброс не выполнен: '+e.message+'</span>';
 }
}

async function rtcLoad(){
 try{
  const r=await api('/api/rtc');
  $('rtcState').textContent=r.present?((r.type||'RTC')+' · '+(r.valid?'OK':'НЕТ ВРЕМЕНИ')):'НЕ НАЙДЕН';
  $('rtcTime').textContent=r.time||'—';
  $('rtcRuns').textContent=`${r.runs} / ${r.errors}`;
  $('rtcMsg').textContent=(r.ntp_synced?('NTP '+(r.ntp_server||'')+' · OK · коррекция '+r.ntp_last_correction_s+' с · '+r.ntp_sync_age_s+' с назад'):(r.last||''));
  const known=['pool.ntp.org','time.google.com','time.cloudflare.com'], ns=r.ntp_server||'pool.ntp.org';
  if(known.includes(ns)){$('ntpServerSelect').value=ns;$('ntpServerCustom').value='';$('ntpServerCustom').disabled=true;}else{$('ntpServerSelect').value='custom';$('ntpServerCustom').value=ns;$('ntpServerCustom').disabled=false;}
  $('ntpUtcOffset').value=((r.ntp_utc_offset_min??180)/60).toFixed(2).replace(/\.00$/,'');
  const s=await api('/api/schedule'); renderSchedule(s.tasks);
 }catch(e){$('rtcMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}
function ntpServerChoice(){ $('ntpServerCustom').disabled=$('ntpServerSelect').value!=='custom'; }
async function ntpSave(){
 const sel=$('ntpServerSelect').value, server=(sel==='custom'?$('ntpServerCustom').value.trim():sel);
 const utcHours=Number($('ntpUtcOffset').value), utc_offset_min=Math.round(utcHours*60);
 if(!server){$('rtcMsg').innerHTML='<span class=bad>Укажите NTP сервер</span>';return;}
 if(!Number.isFinite(utcHours)||utcHours < -12||utcHours > 14){$('rtcMsg').innerHTML='<span class=bad>UTC должен быть от -12 до +14</span>';return;}
 try{const h=await verifiedAdminHeaders('rtcMsg');if(!h)return;await api('/api/rtc/ntp/config',{method:'POST',headers:h,body:JSON.stringify({server,utc_offset_min})});$('rtcMsg').innerHTML='<span class=ok>NTP/UTC сохранены: '+server+' · UTC'+(utcHours>=0?'+':'')+utcHours+'</span>';await rtcLoad();}catch(e){$('rtcMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}
async function rtcSyncBrowser(){
 const d=new Date();
 const body={year:d.getFullYear(),month:d.getMonth()+1,day:d.getDate(),hour:d.getHours(),minute:d.getMinutes(),second:d.getSeconds()};
 try{
  await api('/api/rtc/set',{method:'POST',headers:adminHeaders(),body:JSON.stringify(body)});
  $('rtcMsg').innerHTML='<span class=ok>'+(currentLang==='ru'?'RTC синхронизирован':'RTC synchronized')+'</span>';
  await rtcLoad();
 }catch(e){$('rtcMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}
async function rtcSyncNtp(){
 try{
  const h=await verifiedAdminHeaders('rtcMsg'); if(!h)return;
  const r=await api('/api/rtc/ntp',{method:'POST',headers:h,body:'{}',timeoutMs:5000});
  $('rtcMsg').innerHTML='<span class=ok>'+(currentLang==='ru'?'NTP-синхронизация выполнена':'NTP synchronization completed')+'</span>';
  await rtcLoad();
 }catch(e){$('rtcMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}

async function pzemSave(){
 const address=Number($('pzemAddr').value), enabled=$('pzemEnabled').checked?1:0;
 try{const h=await verifiedAdminHeaders('pzemState');if(!h)return;await api('/api/pzem/config',{method:'POST',headers:h,body:JSON.stringify({address,enabled})});$('pzemState').innerHTML='<span class=ok>Сохранено</span>';setTimeout(()=>statusLoad(true),500);}catch(e){$('pzemState').innerHTML='<span class=bad>'+e.message+'</span>'}
}

async function pzemTariffSet(){
 const t1_kwh=Number($('pzemT1').value),t2_kwh=Number($('pzemT2').value);
 if(!Number.isFinite(t1_kwh)||!Number.isFinite(t2_kwh)||t1_kwh<0||t2_kwh<0){$('pzemTariffState').innerHTML='<span class=bad>Введите корректные T1/T2</span>';return;}
 if(!confirm(`Записать текущие показания счётчика?\nT1 = ${t1_kwh.toFixed(3)} kWh\nT2 = ${t2_kwh.toFixed(3)} kWh`))return;
 try{const h=await verifiedAdminHeaders('pzemTariffState');if(!h)return;const r=await api('/api/pzem/tariff',{method:'POST',headers:h,body:JSON.stringify({t1_kwh,t2_kwh})});$('pzemTariffState').innerHTML='<span class=ok>'+(r.persisted?'T1/T2 сохранены в EEPROM':'Задано, но EEPROM недоступна')+'</span>';setTimeout(()=>statusLoad(true),400);}catch(e){$('pzemTariffState').innerHTML='<span class=bad>'+e.message+'</span>'}
}

async function pzemEnergyReset(){
 if(!confirm('Обнулить ТОЛЬКО внутренний накопительный счётчик энергии PZEM-016? T1/T2 останутся без изменений.'))return;
 const typed=prompt('Для подтверждения введите RESET_PZEM');
 if(typed!=='RESET_PZEM'){$('pzemResetState').innerHTML='<span class=bad>Сброс отменён</span>';return;}
 try{
  const h=await verifiedAdminHeaders('pzemResetState');if(!h)return;
  $('pzemResetState').textContent='Сброс и проверка…';
  const q=await api('/api/pzem/energy/reset',{method:'POST',headers:h,body:JSON.stringify({confirm:'RESET_PZEM'})});
  if(!q.queued||!q.job_id)throw new Error('Команда не поставлена в RTU очередь');
  const st=await waitWriteJob(q.job_id,10000);
  if(!st.ok||!st.verified)throw new Error(st.error||'PZEM не подтвердил сброс');
  $('pzemResetState').innerHTML='<span class=ok>Энергия PZEM обнулена и проверена. T1/T2 сохранены.</span>';
  setTimeout(()=>statusLoad(true),400);
 }catch(e){$('pzemResetState').innerHTML='<span class=bad>'+e.message+'</span>'}
}

async function scheduleSave(slot){
 const tm=$('st'+slot).value.split(':');
 const body={slot,enabled:$('se'+slot).checked,days:Number($('sd'+slot).value),
             hour:Number(tm[0]||0),minute:Number(tm[1]||0),
             register:Number($('sr'+slot).value),raw:Number($('sv'+slot).value)};
 try{
  await api('/api/schedule',{method:'POST',headers:adminHeaders(),body:JSON.stringify(body)});
  $('rtcMsg').innerHTML='<span class=ok>'+(currentLang==='ru'?'Задача сохранена':'Task saved')+'</span>';
 }catch(e){$('rtcMsg').innerHTML='<span class=bad>'+e.message+'</span>'}
}

async function rawRead(){
 try{
  $('rawOut').textContent=currentLang==='ru'?'Чтение…':'Reading…';
  const q=await api('/api/raw?addr='+encodeURIComponent($('rawAddr').value)+'&count='+encodeURIComponent($('rawCount').value));
  if(q.ok && Array.isArray(q.values)){$('rawOut').textContent=JSON.stringify(q,null,2);return;}
  if(!q.queued||!q.job_id)throw new Error(q.error||'Raw read failed');
  const deadline=Date.now()+8000;let r=null;
  while(Date.now()<deadline){await new Promise(x=>setTimeout(x,200));const st=await api('/api/raw/status?id='+q.job_id);if(st.done){r=st;break}}
  if(!r)throw new Error(currentLang==='ru'?'Таймаут чтения':'Read timeout');if(!r.ok)throw new Error(r.error||'RTU read failed');
  $('rawOut').textContent=JSON.stringify(r,null,2);
 }catch(e){$('rawOut').textContent=e.message}
}

initTabs();
setLang(currentLang);
statusLoad();
$('settingsMsg').textContent=currentLang==='ru'?'Загрузка настроек…':'Loading settings…';
$('netSsid').addEventListener('change',netSsidPicked);
$('netSsidManual').addEventListener('input',netSsidTyped);
// WebServer is synchronous: stagger startup requests instead of opening a burst.
setTimeout(()=>netLoad(),150);
setTimeout(()=>modbusLoad(),350);
setTimeout(()=>rtcLoad(),600);
setTimeout(()=>batteryStatsLoad(),800);
setTimeout(()=>otaVersionLoad(),950);
setTimeout(()=>eventsLoad(true),1150);
async function otaVersionLoad(){
 try{
  const r=await fetch('/api/ota/status?ts='+Date.now(),{cache:'no-store'});
  if(!r.ok)return;
  const j=await r.json();
  if(j&&j.version&&!otaInProgressUi) $('otaState').textContent='v'+j.version;
 }catch(e){}
}
let otaInProgressUi=false;
async function waitForOtaReboot(previousVersion){
 const state=$('otaState');
 const deadline=Date.now()+60000;
 state.innerHTML='<span class=ok>Прошивка записана. Ожидание перезапуска ESP32…</span>';
 await new Promise(r=>setTimeout(r,2500));
 while(Date.now()<deadline){
  try{
   const c=new AbortController();const tm=setTimeout(()=>c.abort(),1800);
   const r=await fetch('/api/status?ota_check='+Date.now(),{cache:'no-store',signal:c.signal});clearTimeout(tm);
   if(r.ok){const j=await r.json();if(j.firmware_version && j.firmware_version!==previousVersion){
    $('otaProgress').value=100;
    state.innerHTML='<span class=ok>OTA успешно. Установлена версия v'+j.firmware_version+'</span>';
    setTimeout(()=>location.reload(),1800);return;
   }}
  }catch(e){}
  await new Promise(r=>setTimeout(r,1200));
 }
 state.innerHTML='<span class=bad>Не удалось подтвердить запуск новой версии. Проверьте /api/status.</span>';
}
async function otaUpload(){
 otaInProgressUi=true;
 let previousVersion='0.14.28';
 try{const vr=await fetch('/api/ota/status?pre='+Date.now(),{cache:'no-store'});if(vr.ok){const vj=await vr.json();if(vj.version)previousVersion=vj.version}}catch(e){}
 const f=$('otaFile').files&&$('otaFile').files[0];
 if(!f){alert('Выберите файл прошивки .bin');return}
 if(!f.name.toLowerCase().endsWith('.bin')){alert('Нужен скомпилированный файл .bin');return}
 if(!confirm('Записать новую прошивку ESP32? Во время обновления не отключайте питание.'))return;
 const h=await verifiedAdminHeaders('otaState');if(!h)return;
 try{
  const u=await fetch('/api/ota/unlock',{method:'POST',headers:h,cache:'no-store'});
  const uj=await u.json(); if(!u.ok||!uj.ok){$('otaState').innerHTML='<span class=bad>'+(uj.error||'OTA unlock failed')+'</span>';return}
 }catch(e){$('otaState').innerHTML='<span class=bad>Не удалось открыть OTA-сессию</span>';return}
 const fd=new FormData();fd.append('firmware',f,f.name);
 const x=new XMLHttpRequest();x.open('POST','/api/ota/update',true);x.timeout=120000;
 x.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded*100/e.total);$('otaProgress').value=p;$('otaState').textContent='Загрузка '+p+'%'}};
 x.onload=()=>{let j={};try{j=JSON.parse(x.responseText||'{}')}catch(e){};if(x.status>=200&&x.status<300&&j.ok){$('otaProgress').value=100;$('otaState').innerHTML='<span class=ok>Файл записан. Ожидаю перезапуск…</span>';waitForOtaReboot(previousVersion)}else{$('otaState').innerHTML='<span class=bad>'+(j.error||('HTTP '+x.status))+'</span>'}};
 x.onerror=()=>{$('otaState').innerHTML='<span class=warn>Соединение прервалось. Проверяю версию…</span>';waitForOtaReboot(previousVersion)};
 x.ontimeout=()=>{$('otaState').innerHTML='<span class=warn>Ответ OTA не получен. Проверяю версию…</span>';waitForOtaReboot(previousVersion)};
 x.send(fd);
}

setInterval(()=>{const p=document.getElementById('tab-battery');if(p&&p.classList.contains('active'))batteryStatsLoad(false)},5000);
setTimeout(()=>profilesLoad(),1000);
setTimeout(()=>settingsLoad(false),1600);
setInterval(()=>statusLoad(false),3000);
// Do not poll /api/rtc + /api/schedule every 30 s. Scheduler itself runs locally.
</script></body></html>
)HTML";
  web.send_P(200, "text/html; charset=utf-8", PAGE);
}

void handleOtaStatus() {
  const esp_partition_t* nextPart = esp_ota_get_next_update_partition(nullptr);
  size_t partSize = nextPart ? nextPart->size : 0;
  String j=F("{\"ok\":true,\"version\":\""); j+=FW_VERSION; j+=F("\",\"in_progress\":");
  j += otaInProgress ? F("true") : F("false");
  j += F(",\"update_supported\":"); j += nextPart ? F("true") : F("false");
  j += F(",\"upload_unlocked\":"); j += (otaUploadUnlocked && (int32_t)(otaUploadUnlockUntilMs-millis())>0) ? F("true") : F("false");
  j += F(",\"bytes_written\":"); j += String((uint32_t)otaBytesWritten);
  j += F(",\"ota_partition_bytes\":"); j += String((uint32_t)partSize);
  j += F(",\"update_error_code\":"); j += String(otaUpdateError);
  j += F(",\"last_error\":\""); j += jsonEscape(otaUploadError); j += F("\"}");
  sendJson(200,j);
}

// Authenticate in a normal, small HTTP request BEFORE multipart upload starts.
// The upload callback itself never reads headers, NVS, EEPROM or Strings for auth.
void handleOtaUnlock() {
  if(!adminAuthorized()){ sendJson(401,F("{\"ok\":false,\"error\":\"Invalid admin password\"}")); return; }
  otaUploadUnlocked=true;
  otaUploadUnlockUntilMs=millis()+60000UL;
  Serial.println(F("[OTA] UNLOCK OK (60 s)"));
  sendJson(200,F("{\"ok\":true,\"upload_window_s\":60}"));
}

void otaFail(const __FlashStringHelper* msg){
  otaUploadError=String(msg);
  otaUpdateError=Update.getError();
  Serial.print(F("[OTA] FAIL: ")); Serial.print(otaUploadError);
  Serial.print(F(" code=")); Serial.println(otaUpdateError);
}

void handleOtaUpload() {
  HTTPUpload& up=web.upload();
  if(up.status==UPLOAD_FILE_START){
    Serial.println(F("[OTA] START"));
    otaUploadAuthorized = otaUploadUnlocked && (int32_t)(otaUploadUnlockUntilMs-millis())>0;
    otaUploadUnlocked=false; // one-shot authorization
    otaUploadOk=false; otaUploadError=""; otaUpdateError=0; otaBytesWritten=0; otaPartitionSize=0; otaHeaderChecked=false;
    if(!otaUploadAuthorized){ otaUploadError=F("OTA upload is not unlocked"); Serial.println(F("[OTA] DENIED")); return; }
    const esp_partition_t* nextPart=esp_ota_get_next_update_partition(nullptr);
    if(!nextPart){ otaUploadError=F("No OTA update partition"); Serial.println(F("[OTA] NO PARTITION")); return; }
    otaPartitionSize=nextPart->size;
    otaInProgress=true; webWriteBusReserved=true;
    Serial.print(F("[OTA] PARTITION bytes=")); Serial.println((uint32_t)otaPartitionSize);
    if(!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)){
      otaFail(F("Update.begin failed")); otaInProgress=false; webWriteBusReserved=false; return;
    }
    Serial.println(F("[OTA] BEGIN OK"));
  } else if(up.status==UPLOAD_FILE_WRITE){
    if(!otaUploadAuthorized || otaUploadError.length()) return;
    // web.handleClient() executes this callback on loopTask. A large upload can keep
    // it inside the synchronous HTTP handler longer than the TWDT timeout, so feed
    // the subscribed loop task for every received firmware chunk.
    feedTaskWatchdog();
    if(!otaHeaderChecked){
      if(up.currentSize<1 || up.buf[0]!=0xE9){ otaUploadError=F("Bad ESP32 image header"); Serial.println(F("[OTA] BAD HEADER")); Update.abort(); otaInProgress=false; webWriteBusReserved=false; return; }
      otaHeaderChecked=true; Serial.println(F("[OTA] HEADER OK"));
    }
    if(otaBytesWritten + up.currentSize > otaPartitionSize){ otaUploadError=F("Image larger than OTA partition"); Serial.println(F("[OTA] TOO LARGE")); Update.abort(); otaInProgress=false; webWriteBusReserved=false; return; }
    size_t n=Update.write(up.buf,up.currentSize);
    feedTaskWatchdog();
    if(n!=up.currentSize){ otaFail(F("Update.write failed")); Update.abort(); otaInProgress=false; webWriteBusReserved=false; return; }
    otaBytesWritten += n;
    if((otaBytesWritten & 0xFFFFU) < n){ Serial.print(F("[OTA] WRITE bytes=")); Serial.println((uint32_t)otaBytesWritten); }
    yield();
    feedTaskWatchdog();
  } else if(up.status==UPLOAD_FILE_END){
    feedTaskWatchdog();
    Serial.print(F("[OTA] END bytes=")); Serial.println((uint32_t)otaBytesWritten);
    if(!otaUploadAuthorized || otaUploadError.length()) return;
    if(otaBytesWritten==0){ otaUploadError=F("Empty firmware upload"); Update.abort(); }
    else if(!Update.end(true)){ otaFail(F("Update.end failed")); }
    else { otaUploadOk=true; Serial.println(F("[OTA] END OK")); }
    otaInProgress=false; webWriteBusReserved=false;
  } else if(up.status==UPLOAD_FILE_ABORTED){
    Update.abort(); otaUploadError=F("Upload aborted by client");
    otaInProgress=false; webWriteBusReserved=false; Serial.println(F("[OTA] ABORTED"));
  }
}

void handleOtaUploadDone() {
  if(!otaUploadAuthorized){ sendJson(401,F("{\"ok\":false,\"error\":\"OTA upload is not unlocked\"}")); return; }
  if(!otaUploadOk){
    String j=F("{\"ok\":false,\"error\":\""); j+=jsonEscape(otaUploadError.length()?otaUploadError:String(F("OTA update failed")));
    j+=F("\",\"bytes_written\":"); j+=String((uint32_t)otaBytesWritten);
    j+=F(",\"ota_partition_bytes\":"); j+=String((uint32_t)otaPartitionSize);
    j+=F(",\"update_error_code\":"); j+=String(otaUpdateError); j+='}'; sendJson(500,j); return;
  }
  devicePrefs.putString("ota_pending_from", FW_VERSION);
  devicePrefs.putBool("ota_pending", true);
  String j=F("{\"ok\":true,\"message\":\"Firmware written; reboot scheduled\",\"from_version\":\""); j+=FW_VERSION; j+=F("\",\"bytes_written\":");
  j+=String((uint32_t)otaBytesWritten); j+=F(",\"ota_partition_bytes\":"); j+=String((uint32_t)otaPartitionSize); j+='}';
  sendJson(200,j);
  otaRestartPending=true; otaRestartAtMs=millis()+7000;
  Serial.println(F("[OTA] HTTP OK; REBOOT SCHEDULED"));
}

void setupWeb() {
  const char* headerKeys[] = {"X-ANENJI-Admin"};
  web.collectHeaders(headerKeys, 1);
  web.on("/", HTTP_GET, handleRoot);
  web.on("/api/status", HTTP_GET, sendJsonStatus);
  web.on("/api/datalogger/status", HTTP_GET, handleDataloggerStatus);
  web.on("/api/datalogger/service-ap/start", HTTP_POST, handleDataloggerServiceApStart);
  web.on("/api/datalogger/connect", HTTP_POST, handleDataloggerConnect);
  web.on("/api/datalogger/restore", HTTP_POST, handleDataloggerRestore);
  web.on("/api/datalogger/info", HTTP_POST, handleDataloggerInfo);
  web.on("/api/datalogger/ping", HTTP_POST, handleDataloggerPing);
  web.on("/api/datalogger/set", HTTP_POST, handleDataloggerSetParam);
  web.on("/api/events", HTTP_GET, handleEvents);
  web.on("/api/refresh", HTTP_GET, handleRefresh);
  web.on("/api/raw", HTTP_GET, handleRaw);
  web.on("/api/raw/status", HTTP_GET, handleRawStatus);
  web.on("/api/settings", HTTP_GET, handleSettings);
  web.on("/api/profiles", HTTP_GET, handleProfiles);
  web.on("/api/network", HTTP_GET, handleNetworkGet);
  web.on("/api/auth/check", HTTP_POST, handleAdminAuthCheck);
  web.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  web.on("/api/setup/enter", HTTP_POST, handleEnterSetup);
  web.on("/api/wifi/forget", HTTP_POST, handleWifiForget);
  web.on("/api/network", HTTP_POST, handleNetworkSet);
  web.on("/api/modbus/config", HTTP_GET, handleModbusConfigGet);
  web.on("/api/modbus/config", HTTP_POST, handleModbusConfigSet);
  web.on("/api/battery/stats", HTTP_GET, handleBatteryStatsGet);
  web.on("/api/battery/calibration/start", HTTP_POST, handleBatteryCalibrationStart);
  web.on("/api/battery/calibration/cancel", HTTP_POST, handleBatteryCalibrationCancel);
  web.on("/api/battery/calibration/finish", HTTP_POST, handleBatteryCalibrationFinish);
  web.on("/api/battery/calibration/manual", HTTP_POST, handleBatteryCalibrationManual);
  web.on("/api/battery/stats/reset", HTTP_POST, handleBatteryStatsReset);
  web.on("/api/rtc", HTTP_GET, handleRtcGet);
  web.on("/api/rtc/set", HTTP_POST, handleRtcSet);
  web.on("/api/rtc/ntp", HTTP_POST, handleRtcNtpSync);
  web.on("/api/rtc/ntp/config", HTTP_POST, handleNtpConfigSet);
  web.on("/api/pzem/config", HTTP_POST, handlePzemConfigSet);
  web.on("/api/pzem/tariff", HTTP_POST, handlePzemTariffSet);
  web.on("/api/pzem/energy/reset", HTTP_POST, handlePzemEnergyReset);
  web.on("/api/ota/status", HTTP_GET, handleOtaStatus);
  web.on("/api/ota/unlock", HTTP_POST, handleOtaUnlock);
  web.on("/api/ota/update", HTTP_POST, handleOtaUploadDone, handleOtaUpload);
  web.on("/api/schedule", HTTP_GET, handleScheduleGet);
  web.on("/api/schedule", HTTP_POST, handleScheduleSet);
  web.on("/api/write", HTTP_POST, handleWrite);
  web.on("/api/write/status", HTTP_GET, handleWriteStatus);
  web.on("/api/profile/apply", HTTP_POST, handleProfileApply);
  web.on("/api/profile/status", HTTP_GET, handleProfileStatus);
  web.on("/generate_204", HTTP_ANY, handleCaptiveRedirect);
  web.on("/hotspot-detect.html", HTTP_ANY, handleCaptiveRedirect);
  web.on("/connecttest.txt", HTTP_ANY, handleCaptiveRedirect);
  web.on("/ncsi.txt", HTTP_ANY, handleCaptiveRedirect);
  web.onNotFound(handleCaptiveRedirect);
  web.begin();
}

// ---------------- Wi-Fi / first-boot provisioning ----------------
uint32_t measureBootHold() {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(BOOT_BUTTON_PIN) != LOW) return 0;

  uint32_t start = millis();
  Serial.println(F("[BOOT] button held"));
  while (digitalRead(BOOT_BUTTON_PIN) == LOW && (uint32_t)(millis() - start) < 17000) {
    delay(20);
  }
  return millis() - start;
}

void startSetupAp() {
  setupMode = true;
  setupApRunning = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(SETUP_AP_IP, SETUP_AP_GW, SETUP_AP_MASK);
  WiFi.softAP(setupApSsid.c_str(), setupApPassword.c_str());
  dnsServer.start(53, "*", SETUP_AP_IP);

  Serial.println();
  Serial.println(F("========== ANENJI SETUP =========="));
  Serial.print(F("AP:       ")); Serial.println(setupApSsid);
  Serial.print(F("Password: ")); Serial.println(setupApPassword);
  Serial.print(F("Open:     http://")); Serial.println(SETUP_AP_IP);
  Serial.println(F("=================================="));
}

bool connectStoredWiFi(uint32_t timeoutMs = 20000) {
  if (wifiSsid.length() == 0) return false;

  // Our own wifi_cfg namespace is the single source of truth.
  // Prevent the ESP32 Wi-Fi driver from silently reconnecting to an older AP.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  if (netUseStatic) {
    IPAddress ip,gw,mask,dns;
    if (parseIp(netLocalIp,ip) && parseIp(netGateway,gw) &&
        parseIp(netMask,mask) && parseIp(netDns,dns)) {
      WiFi.config(ip,gw,mask,dns);
    } else {
      Serial.println(F("[WiFi] invalid static config; using DHCP"));
      WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
  } else {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  }

  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  Serial.print(F("[WiFi] connecting to ")); Serial.println(wifiSsid);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (uint32_t)(millis() - start) < timeoutMs) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WiFi] connected, IP=")); Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println(F("[WiFi] connection failed; AP is NOT opened automatically"));
  return false;
}

void initNetworkMode() {
  uint32_t held = measureBootHold();

  if (held >= BOOT_WIFI_RESET_MS) {
    Serial.println(F("[BOOT] 15s: erase Wi-Fi namespace only"));
    wifiPrefs.clear();
    wifiSsid = "";
    wifiPass = "";
    startSetupAp();
    return;
  }

  bool setupOnce = devicePrefs.getBool("setup_once", false);
  if (setupOnce) devicePrefs.remove("setup_once");

  if (held >= BOOT_SETUP_MS || setupOnce || wifiSsid.length() == 0) {
    setupRequestedByButton = held >= BOOT_SETUP_MS;
    startSetupAp();
    return;
  }

  // Important security behavior: failed home Wi-Fi does NOT expose Setup AP.
  connectStoredWiFi();
}

// ---------------- runtime BOOT button ----------------
void serviceBootButton() {
  static bool wasDown = false;
  static uint32_t pressedAt = 0;
  static uint32_t lastAnnouncedSec = 0;
  static bool resetCommitted = false;

  bool down = (digitalRead(BOOT_BUTTON_PIN) == LOW);
  uint32_t now = millis();

  if (down && !wasDown) {
    wasDown = true;
    resetCommitted = false;
    pressedAt = now;
    lastAnnouncedSec = 0;
    Serial.println(F("[BOOT] button pressed: hold 5 s for Setup AP, 15 s to erase Wi-Fi"));
    return;
  }

  if (down && wasDown) {
    uint32_t held = now - pressedAt;
    uint32_t sec = held / 1000U;
    if (sec != lastAnnouncedSec && sec > 0) {
      lastAnnouncedSec = sec;
      Serial.print(F("[BOOT] held ")); Serial.print(sec); Serial.println(F(" s"));
      if (sec == 5) Serial.println(F("[BOOT] release now -> Setup AP; keep holding to 15 s -> erase Wi-Fi"));
    }

    if (!resetCommitted && held >= BOOT_WIFI_RESET_MS) {
      resetCommitted = true;
      Serial.println(F("[BOOT] 15 s reached: erasing wifi_cfg and restarting into Setup AP"));
      wifiPrefs.clear();
      delay(100);
      ESP.restart();
    }
    return;
  }

  if (!down && wasDown) {
    uint32_t held = now - pressedAt;
    wasDown = false;
    if (held >= BOOT_SETUP_MS && held < BOOT_WIFI_RESET_MS) {
      Serial.println(F("[BOOT] 5 s action: restarting into Setup AP; saved Wi-Fi preserved"));
      devicePrefs.putBool("setup_once", true);
      delay(100);
      ESP.restart();
    } else if (held < BOOT_SETUP_MS) {
      Serial.println(F("[BOOT] released before 5 s: no action"));
    }
  }
}

// ---------------- setup / loop ----------------
void setup() {
  mainLoopTaskHandle = xTaskGetCurrentTaskHandle();
  Serial.begin(115200);
  delay(300);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  Serial.println();
  Serial.println(F("[FW] ESP32 ANENJI WiFi/RS485 v0.14.31 partial profile apply + no custom current/voltage limits + 4 output-priority modes + direct raw API + battery stats current offset + OTA stable + Web OTA + PZEM right-angle load flow + EEPROM-safe tariffs + fixed UTC offset + PZEM stable + T1/T2 EEPROM + NTP select + inverter panel + RTU watchdog + DS1307/AT24C32"));

  pinMode(RS485_DE_RE_PIN, OUTPUT);
  rs485ReceiveMode();
  RS485.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(50);  // prevent a stuck RTC/I2C bus from blocking the web loop
  RtcDateTime bootRtc;
  rtcPresent = rtcRead(bootRtc);
  if (rtcPresent) { rtcCached=bootRtc; rtcCachedAtMs=millis(); }
  Serial.print(F("[RTC] DS1307/compatible: "));
  Serial.println(rtcPresent ? (bootRtc.valid ? F("OK") : F("present, time invalid")) : F("not found"));
  detectExternalEeprom();
  Serial.print(F("[EEPROM] AT24C32: "));
  if(eepromPresent){Serial.print(F("OK @0x"));Serial.println(eepromI2cAddr,HEX);}else Serial.println(F("not found"));

  loadPersistentConfig();
  if(devicePrefs.getBool("ota_pending", false)){
    String from=devicePrefs.getString("ota_pending_from", "unknown");
    devicePrefs.putString("ota_last_from", from);
    devicePrefs.putString("ota_last_to", FW_VERSION);
    devicePrefs.putBool("ota_last_ok", true);
    devicePrefs.putBool("ota_pending", false);
    devicePrefs.remove("ota_pending_from");
    Serial.print(F("[OTA] boot confirmed: ")); Serial.print(from); Serial.print(F(" -> ")); Serial.println(FW_VERSION);
  }
  batteryStatsLoad();
  pzemTariffLoad();
  initNetworkMode();

  rtuJobQueue = xQueueCreate(6, sizeof(RtuJob));
  if (rtuJobQueue) {
    BaseType_t taskOk = xTaskCreatePinnedToCore(
      rtuWorkerTask, "anenji_rtu", 6144, nullptr, 1, &rtuWorkerHandle, 1);
    if (taskOk != pdPASS) {
      rtuWorkerHandle=nullptr; vQueueDelete(rtuJobQueue); rtuJobQueue=nullptr;
      Serial.println(F("[RTU] worker create failed"));
    } else Serial.println(F("[RTU] single-owner worker ready"));
  } else Serial.println(F("[RTU] queue create failed"));

  setupWeb();

  telemetryForceRequested = true;
  settingsRefreshRequested = true;
  lastPollMs = millis();

  // Enable after provisioning/connect code so a deliberate 15 s BOOT hold cannot trip it.
  initTaskWatchdog();
  feedTaskWatchdog();

  Serial.print(F("[BOOT] reset reason: "));
  Serial.println(resetReasonText(esp_reset_reason()));
}

void loop() {
  loopHeartbeat++;
  feedTaskWatchdog();
  if(otaRestartPending && (int32_t)(millis()-otaRestartAtMs)>=0){ delay(50); ESP.restart(); }
  serviceBootButton();
  serviceWifiWatchdog();
  serviceHeapWatchdog();
  serviceNtpRtcSync();

  if (setupMode) {
    dnsServer.processNextRequest();
  } else if (WiFi.status() != WL_CONNECTED && wifiSsid.length()) {
    static uint32_t lastRetry = 0;
    if ((uint32_t)(millis() - lastRetry) > 10000) {
      lastRetry = millis();
      Serial.println(F("[WiFi] reconnect stored network"));
      WiFi.disconnect();
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
  }

  web.handleClient();
  if(!otaInProgress && !otaRestartPending) serviceRtuWatchdog();

  // RTU polling is owned by the dedicated worker; loop() never waits for Modbus.

  if(!otaInProgress && !otaRestartPending){
    serviceScheduler();
    serviceBatteryStats();
    servicePzemTariff();
  }

  feedTaskWatchdog();
  delay(1);
}
