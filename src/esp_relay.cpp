/*******************************************************************************
 * ESP-Agri — Node A: Relay / Multi-Device Node   (Phase 2)
 *
 * Hardware:
 *   - ESP32 DOIT DevKit V1
 *   - Device GPIOs: PUMP_01→13, VALVE_01→19, LIGHT_01→23, MOTOR_01→4
 *   - D2 LED mirrors PUMP_01 state
 *   - SSD1306 OLED 128×64 on I2C (SDA=21, SCL=22, addr 0x3C)
 *
 * Behaviour:
 *   1. Joins the ESP-MESH network
 *   2. Listens for device commands (DEV_ON / DEV_OFF / TOGGLE + legacy pump)
 *   3. Validates Farm ID before acting
 *   4. Routes command to correct GPIO via device map
 *   5. Sends ACK with device state back to sender
 *   6. Logs every action to the debug ring buffer
 *   7. Displays multi-device state table on OLED
 *
 * Application code NEVER calls mesh APIs directly — only the transport
 * abstraction layer (agri_send / agri_broadcast / agri_on_receive).
 ******************************************************************************/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <string.h>
#include "agri_config.h"
#include "agri_protocol.h"
#include "agri_transport.h"
#include "agri_mesh_wifi.h"
#include "agri_display.h"
#include "agri_devmap.h"
#include "agri_gridui.h"
#include "agri_log.h"
#include "agri_nvs.h"
#include "agri_range.h"
#include "agri_rssi.h"

// ============================================================================
// Globals
// ============================================================================
static AgriMeshWifi g_mesh;
static AgriDisplay  g_display;
static WebServer    g_web(80);
static WebSocketsServer g_ws(81);
static const char*  DEVICE_ID = "RELAY01";

// Peer tracking (cache the remote node ID for directed ACKs)
static uint32_t g_lastRemoteNode = 0;
static bool g_meshWasConnected   = false;

// Timing
static uint32_t g_lastDisplayRefresh = 0;
static uint32_t g_lastRssiPoll       = 0;
static uint32_t g_lastHeartbeat      = 0;   // Periodic heartbeat with device states
static uint32_t g_lastScheduleTick   = 0;
static uint32_t g_lastWsLivePush     = 0;
static RssiTracker g_rssiTracker;

struct RelayScheduleEntry {
    bool enabled;
    uint32_t delaySec;
    uint32_t runSec;
    uint32_t armedAtMs;
    uint32_t runStartMs;
    bool running;
};

static RelayScheduleEntry g_schedule[GRID_DEV_TILES] = {};

struct SensorAutomationConfig {
    bool enabled;
    uint8_t threshold;
    int8_t targetIdx;
    bool aboveOn;
    bool lastHigh;
    bool lastEvalValid;
};

static SensorAutomationConfig g_sensorAuto = {
    false, 50, 0, true, false, false
};
static uint8_t g_sensorLevel = 0;
static bool g_sensorLevelValid = false;
static uint32_t g_sensorLastSampleMs = 0;
static char g_sensorLastSender[AGRI_DEVICE_ID_LEN] = "";
static const uint32_t SENSOR_STALE_MS = 30000;
static const uint32_t WS_ACTIVE_PUSH_MS = 1000;
static const uint32_t WS_IDLE_PUSH_MS = 2000;
static const uint8_t SENSOR_HISTORY_SIZE = 12;

struct SensorReading {
    uint8_t level;
    uint32_t atMs;
};

static SensorReading g_sensorHistory[SENSOR_HISTORY_SIZE] = {};
static uint8_t g_sensorHistCount = 0;
static uint8_t g_sensorHistHead = 0;

// AOD (Always-On Display) — sleep/wake on relay
static bool       g_displaySleeping    = false;
static uint32_t   g_lastActivityTime   = 0;
static bool       g_aodPrevEnabled     = true;
static uint8_t    g_aodPrevSec         = AGRI_AOD_DEFAULT_SEC;
static GridScreen g_prevScreenForAod   = GRID_MAIN;

// 3-button local navigation (UP / SEL / DOWN)
struct BtnState {
    uint8_t  pin;
    bool     raw;
    bool     stable;
    uint32_t debounceT;
    bool     fired;
};

static BtnState g_btnUp   = { AGRI_BTN_UP,   HIGH, HIGH, 0, false };
static BtnState g_btnDown = { AGRI_BTN_DOWN, HIGH, HIGH, 0, false };
static BtnState g_btnSel  = { AGRI_BTN_SEL,  HIGH, HIGH, 0, false };

static bool     g_selPressed   = false;
static uint32_t g_selPressTime = 0;
static bool     g_selLongFired = false;

static void update_led();
static bool apply_local_command(const char* deviceId, uint8_t cmd,
                                const char* sourceTag, const char* farmId = nullptr);
static void sync_grid_from_devmap();
static void save_devcfg_blob();
static void load_devcfg_blob();
static void save_schedule_blob();
static void load_schedule_blob();
static void schedule_tick(uint32_t now);
static bool schedule_any_enabled();
static int schedule_index_for_device(const char* deviceId);
static void sync_grid_schedule_badges(uint32_t now);
static void ws_broadcast_status();
static void mesh_broadcast_device_list_snapshot();
static void ws_send_error(uint8_t client, const char* err);
static void ws_send_ok(uint8_t client);
static void ws_on_event(uint8_t client, WStype_t type, uint8_t* payload, size_t length);
static void handle_serial_cli();
static void sensor_automation_apply(uint8_t level);
static void sensor_automation_on_sample(uint8_t level, const char* senderId);

static const char* relay_farm_id() {
    return agri_get_runtime_farm_id();
}

static void serial_print_cfg() {
    String relayDevices;
    const AgriDevice* tbl = agri_devmap_table();
    uint8_t cnt = agri_devmap_count();
    for (uint8_t i = 0; i < cnt; i++) {
        if (!tbl[i].valid) continue;
        if (relayDevices.length()) relayDevices += ';';
        relayDevices += String(i);
        relayDevices += ',';
        relayDevices += tbl[i].id;
        relayDevices += ',';
        relayDevices += String(tbl[i].gpio);
        relayDevices += ',';
        relayDevices += (tbl[i].activeHigh ? "1" : "0");
    }

    String json = "{\"role\":\"relay\",\"farmId\":\"";
    json += relay_farm_id();
    json += "\",\"id\":\"";
    json += relay_farm_id();
    json += "\",\"relayId\":\"";
    json += DEVICE_ID;
    json += "\",\"relayDevices\":\"";
    json += relayDevices;
    json += "\",\"list\":\"";
    json += relayDevices;
    json += "\",\"relayAllowedRemotes\":\"";
    json += AGRI_RELAY_ALLOWED_REMOTES_CSV;
    json += "\",\"syncVer\":2";
    json += ",\"protoVer\":2";
    json += "}";
    Serial.println(json);
}

static bool serial_apply_cfg(const String& jsonText) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonText);
    if (err) return false;

    const char* farmId = doc["farmId"] | doc["id"] | "";
    if (farmId[0]) {
        agri_set_runtime_farm_id(farmId);
        agri_nvs_save_relay_farm(farmId);
        grid_set_farm_id(farmId);
        g_display.setFarmId(farmId);
        g_display.markDirty();
    }

    Serial.println("{\"setCfg\":\"ok\",\"role\":\"relay\",\"syncVer\":2,\"protoVer\":2}");
    serial_print_cfg();
    return true;
}

static void handle_serial_cli() {
    if (!Serial.available()) return;

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (!cmd.length()) return;

    if (cmd == "AGRI_GET_CFG" || cmd == "AGRI_GET_CFG?") {
        serial_print_cfg();
        return;
    }

    if (cmd.startsWith("AGRI_SET_CFG ")) {
        String body = cmd.substring(strlen("AGRI_SET_CFG "));
        if (!serial_apply_cfg(body)) {
            Serial.println("{\"setCfg\":\"error\",\"reason\":\"bad_json\",\"role\":\"relay\",\"syncVer\":2}");
        }
        return;
    }

    if (cmd == "AGRI_HELP") {
        Serial.println("AGRI_GET_CFG");
        Serial.println("AGRI_SET_CFG {json}");
    }
}

static bool is_reserved_gpio(uint8_t gpio) {
    return gpio == AGRI_BTN_UP || gpio == AGRI_BTN_SEL || gpio == AGRI_BTN_DOWN ||
           gpio == AGRI_OLED_SDA || gpio == AGRI_OLED_SCL || gpio == AGRI_LED_PIN;
}

static bool is_valid_device_id(const char* id) {
    if (!id || !id[0]) return false;
    size_t len = strnlen(id, AGRI_DEVICE_ID_LEN);
    if (len == 0 || len >= AGRI_DEVICE_ID_LEN) return false;

    for (size_t i = 0; i < len; i++) {
        char c = id[i];
        bool ok = (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static bool relay_is_remote_allowed(const char* senderId) {
    const char* csv = AGRI_RELAY_ALLOWED_REMOTES_CSV;
    if (!csv || !csv[0]) return false;

    if (strcmp(csv, "*") == 0) return true;
    if (!senderId || !senderId[0]) return false;

    char listBuf[256] = {0};
    strlcpy(listBuf, csv, sizeof(listBuf));

    char* saveptr = nullptr;
    char* token = strtok_r(listBuf, ",", &saveptr);
    while (token) {
        while (*token == ' ' || *token == '\t') token++;
        char* end = token + strlen(token);
        while (end > token && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }

        if (strcmp(token, senderId) == 0) {
            return true;
        }
        token = strtok_r(nullptr, ",", &saveptr);
    }
    return false;
}

// ============================================================================
// Web Dashboard
// ============================================================================
static String web_status_json() {
    String json = "{\"farm\":\"";
    json += relay_farm_id();
    json += "\",\"nodeId\":";
    json += String(agri_get_node_id());
    json += ",\"mesh\":";
    json += (agri_is_connected() ? "true" : "false");
    json += ",\"nodes\":";
    json += String(agri_get_node_count() + 1);
    json += ",\"devices\":[";

    const AgriDevice* tbl = agri_devmap_table();
    uint8_t cnt = agri_devmap_count();
    bool first = true;
    for (uint8_t i = 0; i < cnt; i++) {
        if (!first) json += ",";
        first = false;
        json += "{\"id\":\"";
        json += tbl[i].id;
        json += "\",\"state\":";
        json += (tbl[i].valid && tbl[i].state ? "true" : "false");
        json += ",\"idx\":";
        json += String(i);
        json += ",\"gpio\":";
        json += String(tbl[i].gpio);
        json += ",\"ah\":";
        json += (tbl[i].activeHigh ? "true" : "false");
        json += ",\"valid\":";
        json += (tbl[i].valid ? "true" : "false");
        json += "}";
    }
    json += "]";
    json += ",\"schedule\":[";
    for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
        if (i) json += ",";
        uint32_t leftSec = 0;
        if (g_schedule[i].enabled) {
            if (g_schedule[i].running) {
                uint32_t elapsed = (millis() - g_schedule[i].runStartMs) / 1000UL;
                leftSec = (elapsed >= g_schedule[i].runSec) ? 0 : (g_schedule[i].runSec - elapsed);
            } else {
                uint32_t elapsed = (millis() - g_schedule[i].armedAtMs) / 1000UL;
                leftSec = (elapsed >= g_schedule[i].delaySec) ? 0 : (g_schedule[i].delaySec - elapsed);
            }
        }
        json += "{\"idx\":";
        json += String(i);
        json += ",\"en\":";
        json += (g_schedule[i].enabled ? "true" : "false");
        json += ",\"delay\":";
        json += String(g_schedule[i].delaySec);
        json += ",\"dur\":";
        json += String(g_schedule[i].runSec);
        json += ",\"running\":";
        json += (g_schedule[i].running ? "true" : "false");
        json += ",\"left\":";
        json += String(leftSec);
        json += "}";
    }
    json += "]";
    bool sensorOnline = g_sensorLevelValid && ((millis() - g_sensorLastSampleMs) <= SENSOR_STALE_MS);
    json += ",\"sensor\":{";
    json += "\"has\":";
    json += (g_sensorLevelValid ? "true" : "false");
    json += ",\"online\":";
    json += (sensorOnline ? "true" : "false");
    json += ",\"sender\":\"";
    json += g_sensorLastSender;
    json += "\"";
    json += ",\"level\":";
    json += String(g_sensorLevel);
    json += ",\"age\":";
    uint32_t sensorAgeSec = g_sensorLevelValid ? ((millis() - g_sensorLastSampleMs) / 1000UL) : 0;
    json += String(sensorAgeSec);
    json += ",\"history\":[";
    for (uint8_t k = 0; k < g_sensorHistCount; k++) {
        if (k) json += ",";
        uint8_t idx = (uint8_t)((g_sensorHistHead + SENSOR_HISTORY_SIZE - 1 - k) % SENSOR_HISTORY_SIZE);
        uint32_t ageSec = (millis() - g_sensorHistory[idx].atMs) / 1000UL;
        json += "{\"v\":";
        json += String(g_sensorHistory[idx].level);
        json += ",\"age\":";
        json += String(ageSec);
        json += "}";
    }
    json += "]";
    json += "}";
    json += ",\"automation\":{";
    json += "\"en\":";
    json += (g_sensorAuto.enabled ? "true" : "false");
    json += ",\"th\":";
    json += String(g_sensorAuto.threshold);
    json += ",\"idx\":";
    json += String(g_sensorAuto.targetIdx);
    json += ",\"aboveOn\":";
    json += (g_sensorAuto.aboveOn ? "true" : "false");
    json += "}";
    json += "}";
    return json;
}

static void web_handle_root() {
        String html = R"HTML(
<!doctype html>
<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>ESP-Agri Relay</title>
<style>
*{box-sizing:border-box}
body{margin:0;padding:12px;background:#0f1115;color:#e8eaed;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;line-height:1.35}
.wrap{max-width:980px;margin:0 auto}
.header{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}
h1{margin:0;font-size:22px;letter-spacing:.2px}
.conn{display:flex;align-items:center;gap:8px;font-size:13px;padding:6px 10px;border-radius:999px;border:1px solid #2b3342;background:#151c27}
.dot{width:10px;height:10px;border-radius:50%;background:#7d8798}
.dot.ok{background:#33c267}.dot.bad{background:#de5a5a}.dot.wait{background:#c7a23b}

.banner{margin-top:10px;padding:10px 12px;border-radius:12px;border:1px solid #2f3b4d;background:#151e2a;font-size:14px}
.banner.ok{border-color:#2d8a43;background:#16351f}
.banner.err{border-color:#9a2c2c;background:#3a1a1a}

.section{margin-top:14px}
.title{margin:0 0 10px 0;font-size:17px;font-weight:700}
.muted{opacity:.82;font-size:13px}

.quick{display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap;padding:10px 12px;border:1px solid #2b3342;border-radius:12px;background:#141b26}
.switch{display:flex;align-items:center;gap:8px;font-size:13px}
.switch input{accent-color:#3ea869}
.btn{border:1px solid #4a5364;background:#1a2230;color:#e8eaed;border-radius:10px;padding:8px 12px;min-height:36px;cursor:pointer}
.btn:disabled{opacity:.55;cursor:not-allowed}
.btn:focus-visible,.dev-card:focus-visible,input:focus-visible,select:focus-visible,summary:focus-visible{outline:2px solid #7fb1ff;outline-offset:2px}

.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}
.dev-card{border:1px solid #3a404d;border-radius:14px;padding:12px;text-align:left;color:#e8eaed;background:#1a1f29;min-height:106px;cursor:pointer;transition:border-color .15s,opacity .15s}
.dev-card:hover{border-color:#6a748a}
.dev-card.on{background:linear-gradient(180deg,#1a3f24 0%,#16351f 100%);border-color:#2f9a49}
.dev-card.off{background:linear-gradient(180deg,#512323 0%,#3a1a1a 100%);border-color:#a43d3d}
.dev-card.idle{background:#141922;border:1px dashed #4f5a6f;color:#9ca8bd;cursor:default}
.dev-card.pending{opacity:.62;cursor:progress}
.dev-name{font-weight:700}
.dev-sub{margin-top:6px;font-size:13px;opacity:.9}
.dev-hint{margin-top:8px;font-size:12px;opacity:.82}

details.adv{margin-top:16px;border:1px solid #2c3340;background:#131923;border-radius:14px;padding:8px 10px}
summary{cursor:pointer;font-weight:700;padding:6px 2px}
.cfg-grid{margin-top:10px;display:grid;grid-template-columns:1fr;gap:10px}
.cfg-card{border:1px solid #394354;background:#182130;border-radius:12px;padding:12px}
.cfg-row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:8px}
label{font-size:13px;min-width:78px;opacity:.9}
input,select{border:1px solid #4d5667;background:#0f131a;color:#e8eaed;border-radius:8px;padding:7px 8px}
input[type='text']{width:180px;max-width:100%}
input[type='number']{width:90px}
.cfg-error{margin-top:8px;color:#ff9090;font-size:12px;min-height:16px}

.toast{position:fixed;left:50%;bottom:14px;transform:translateX(-50%);max-width:92vw;padding:10px 14px;border-radius:10px;border:1px solid #2e7b40;background:#153320;color:#dbffe5;display:none;z-index:12;font-size:13px}
.toast.show{display:block}

.overlay{position:fixed;inset:0;background:rgba(0,0,0,.56);display:none;align-items:center;justify-content:center;padding:12px;z-index:11}
.overlay.show{display:flex}
.modal{width:min(520px,100%);border:1px solid #445065;background:#171d28;border-radius:14px;padding:14px}
.modal h4{margin:0 0 8px 0}.modal p{margin:0 0 12px 0;opacity:.92}
.actions{display:flex;justify-content:flex-end;gap:8px}

@media (min-width:720px){
    body{padding:18px}
    .grid{grid-template-columns:repeat(3,minmax(0,1fr))}
    .cfg-grid{grid-template-columns:repeat(2,minmax(0,1fr))}
}
</style>
</head>
<body>
<div class='wrap'>
    <div class='header'>
        <h1>ESP-Agri Relay Dashboard</h1>
        <div class='conn'><span id='connDot' class='dot wait'></span><span id='connTxt'>Connecting</span></div>
    </div>

    <div id='systemBanner' class='banner'>System Ready. All relays operational.</div>

    <div class='section'>
        <div class='title'>Quick Toggle</div>
        <div class='quick'>
            <div class='switch'>
                <input id='masterSwitch' type='checkbox'>
                <label for='masterSwitch'>Master relay switch</label>
            </div>
            <button id='modeBtn' class='btn' type='button'>Confirm Mode</button>
        </div>
        <div class='muted' style='margin-top:6px'>Default safety uses confirmation. You can switch to one-tap mode.</div>
    </div>

    <div class='section'>
        <div class='title'>Devices</div>
        <div id='grid' class='grid'></div>
    </div>

    <div class='section'>
        <div class='title'>Schedule</div>
        <div class='quick'>
            <div class='cfg-row'>
                <label for='schDevice'>Device</label>
                <select id='schDevice'></select>
            </div>
            <div class='cfg-row'>
                <label for='schDelay'>Delay (s)</label>
                <input id='schDelay' type='number' min='0' max='86400' value='30'>
            </div>
            <div class='cfg-row'>
                <label for='schDuration'>Run (s)</label>
                <input id='schDuration' type='number' min='1' max='86400' value='60'>
            </div>
            <div class='cfg-row'>
                <label for='schEnable'>Enable</label>
                <input id='schEnable' type='checkbox' checked>
            </div>
            <button id='schApply' class='btn' type='button'>Apply Timer</button>
        </div>
        <div id='schSync' class='muted' style='margin-top:6px'>Timers run from relay uptime.</div>
        <div id='schList' class='muted' style='margin-top:6px'></div>
    </div>

    <div class='section'>
        <div class='title'>Sensor Automation</div>
        <div class='quick'>
            <div class='cfg-row'>
                <label for='autoDevice'>Device</label>
                <select id='autoDevice'></select>
            </div>
            <div class='cfg-row'>
                <label for='autoThreshold'>Threshold (%)</label>
                <input id='autoThreshold' type='number' min='0' max='100' value='50'>
            </div>
            <div class='cfg-row'>
                <label for='autoEnable'>Enable</label>
                <input id='autoEnable' type='checkbox'>
            </div>
            <div class='cfg-row'>
                <label for='autoAboveOn'>Action</label>
                <select id='autoAboveOn'>
                    <option value='1'>Above threshold = ON</option>
                    <option value='0'>Above threshold = OFF</option>
                </select>
            </div>
            <button id='autoApply' class='btn' type='button'>Apply Automation</button>
        </div>
        <div id='sensorNow' class='muted' style='margin-top:6px'>Sensor: waiting for data...</div>
        <div id='autoRule' class='muted' style='margin-top:6px'>Automation disabled.</div>
    </div>

    <details class='adv'>
        <summary>Advanced Configuration</summary>
        <div class='muted' style='margin-top:6px'>Reserved GPIO: BTN(16,17,18), OLED(21,22), LED(2)</div>
        <div id='cfg' class='cfg-grid'></div>
    </details>
</div>

<div id='toast' class='toast' role='status' aria-live='polite'></div>

<div id='overlay' class='overlay' aria-hidden='true'>
    <div class='modal'>
        <h4>Confirm Action</h4>
        <p id='confirmText'>Proceed?</p>
        <div class='actions'>
            <button id='cancelBtn' class='btn' type='button'>Cancel</button>
            <button id='confirmBtn' class='btn' type='button'>Confirm</button>
        </div>
    </div>
</div>

<script>
const MAX_CARDS=6;
const reservedPins=[16,17,18,21,22,2];
const validIdRe=/^[A-Za-z0-9_-]{1,15}$/;
let ws=null;
let dataState=null;
let confirmMode=(localStorage.getItem('relay.toggle.confirm')??'1')==='1';
let pendingDevices=new Set();
let pendingConfig=new Set();
let pendingAll=false;
let confirmTarget=null;
let pendingToast='';

const connDot=document.getElementById('connDot');
const connTxt=document.getElementById('connTxt');
const systemBanner=document.getElementById('systemBanner');
const modeBtn=document.getElementById('modeBtn');
const masterSwitch=document.getElementById('masterSwitch');
const grid=document.getElementById('grid');
const cfg=document.getElementById('cfg');
const toast=document.getElementById('toast');
const overlay=document.getElementById('overlay');
const confirmText=document.getElementById('confirmText');
const confirmBtn=document.getElementById('confirmBtn');
const cancelBtn=document.getElementById('cancelBtn');
const schDevice=document.getElementById('schDevice');
const schDelay=document.getElementById('schDelay');
const schDuration=document.getElementById('schDuration');
const schEnable=document.getElementById('schEnable');
const schApply=document.getElementById('schApply');
const schSync=document.getElementById('schSync');
const schList=document.getElementById('schList');
const autoDevice=document.getElementById('autoDevice');
const autoThreshold=document.getElementById('autoThreshold');
const autoEnable=document.getElementById('autoEnable');
const autoAboveOn=document.getElementById('autoAboveOn');
const autoApply=document.getElementById('autoApply');
const sensorNow=document.getElementById('sensorNow');
const autoRule=document.getElementById('autoRule');

let schSelectedIdx=0;

function esc(s){return String(s).replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[c]));}
function showToast(msg){toast.textContent=msg;toast.classList.add('show');setTimeout(()=>toast.classList.remove('show'),1800);}
function setConn(kind,msg){connDot.className='dot '+kind;connTxt.textContent=msg;}
function setBanner(kind,msg){systemBanner.className='banner '+kind;systemBanner.textContent=msg;}
function modeLabel(){modeBtn.textContent=confirmMode?'Confirm Mode':'One-tap Mode';}
function mapErr(err){
    if(err==='reserved gpio') return 'Error occurred: reserved GPIO.';
    if(err==='invalid args') return 'Error occurred: invalid values.';
    if(err==='conflict/invalid config') return 'Error occurred: duplicate GPIO or invalid config.';
    if(err==='unknown device') return 'Error occurred: unknown device.';
    return 'Error occurred';
}

function openConfirm(target){
    confirmTarget=target;
    confirmText.textContent=target.message;
    overlay.classList.add('show');
    overlay.setAttribute('aria-hidden','false');
    confirmBtn.focus();
}
function closeConfirm(){
    overlay.classList.remove('show');
    overlay.setAttribute('aria-hidden','true');
    confirmTarget=null;
    if(dataState) render(dataState);
}

function send(obj){
    if(ws&&ws.readyState===1){ws.send(JSON.stringify(obj));return true;}
    setConn('wait','Reconnecting');
    setBanner('err','Connection lost. Reconnecting...');
    return false;
}

function connect(){
    const proto=(location.protocol==='https:'?'wss':'ws');
    ws=new WebSocket(proto+'://'+location.hostname+':81/');
    ws.onopen=()=>{
        setConn('ok','Connected');
        setBanner('ok','System Ready. All relays operational.');
        send({type:'get_status'});
    };
    ws.onclose=()=>{
        setConn('wait','Reconnecting');
        setBanner('err','Disconnected. Attempting reconnect...');
        setTimeout(connect,1500);
    };
    ws.onmessage=(ev)=>{
        try{
            const m=JSON.parse(ev.data);
            if(m.type==='status'){
                dataState=m.data;
                pendingDevices.clear();
                pendingConfig.clear();
                pendingAll=false;
                render(m.data);
            }else if(m.type==='ok'){
                showToast(pendingToast||'Configuration saved');
                pendingToast='';
            }else if(m.type==='error'){
                pendingDevices.clear();
                pendingConfig.clear();
                pendingAll=false;
                if(dataState) render(dataState);
                const msg=mapErr(m.err);
                setBanner('err',msg);
                showToast(msg);
            }
        }catch(_){
            setBanner('err','Error occurred while reading response.');
            showToast('Error occurred');
        }
    };
}

function render(d){
    if(!d) return;
    const devices=(d.devices||[]).slice(0,MAX_CARDS);
    const activeDevices=devices.filter(v=>v.valid);
    const allOn=activeDevices.length>0&&activeDevices.every(v=>v.state);
    masterSwitch.checked=allOn;
    masterSwitch.disabled=pendingAll||activeDevices.length===0;
    setBanner('ok',d.mesh?'System Ready. All relays operational.':'Mesh link unstable. Controls remain local.');

    const frag=document.createDocumentFragment();
    devices.forEach(dev=>{
        if(!dev.valid){
            const idle=document.createElement('div');
            idle.className='dev-card idle';
            idle.innerHTML='<div class="dev-name">Slot '+dev.idx+'</div><div class="dev-sub">No device</div><div class="dev-hint">Configure in Advanced section</div>';
            frag.appendChild(idle);
            return;
        }

        const btn=document.createElement('button');
        const pending=pendingDevices.has(dev.id)||pendingAll;
        btn.type='button';
        btn.className='dev-card '+(dev.state?'on':'off')+(pending?' pending':'');
        btn.disabled=pending;
        btn.setAttribute('aria-label','Toggle '+dev.id);
        btn.innerHTML='<div class="dev-name">'+esc(dev.id)+'</div>'+
            '<div class="dev-sub">GPIO '+dev.gpio+'</div>'+
            '<div class="dev-sub">Status: '+(dev.state?'ON':'OFF')+'</div>'+
            '<div class="dev-hint">'+(pending?'Pending...':'Tap to toggle')+'</div>';
        btn.onclick=()=>requestDeviceToggle(dev.id);
        frag.appendChild(btn);
    });

    for(let i=devices.length;i<MAX_CARDS;i++){
        const idle=document.createElement('div');
        idle.className='dev-card idle';
        idle.innerHTML='<div class="dev-name">Slot '+i+'</div><div class="dev-sub">No device</div><div class="dev-hint">Idle</div>';
        frag.appendChild(idle);
    }
    grid.innerHTML='';
    grid.appendChild(frag);

    renderConfig(devices);
    renderSchedule(d);
    renderAutomation(d);
}

function renderAutomation(d){
    const devices=(d.devices||[]).filter(v=>v.valid);
    if(devices.length===0){
        autoDevice.innerHTML='';
        autoApply.disabled=true;
        autoRule.textContent='No devices configured for automation.';
    }else{
        autoApply.disabled=false;
        const next=devices.map(v=>String(v.idx)).join(',');
        const cur=[...autoDevice.options].map(o=>o.value).join(',');
        if(next!==cur){
            autoDevice.innerHTML=devices.map(v=>'<option value="'+v.idx+'">'+esc(v.id)+'</option>').join('');
        }
        const a=d.automation||{};
        let idx=Number(a.idx);
        if(!devices.some(v=>v.idx===idx)) idx=devices[0].idx;
        autoDevice.value=String(idx);
        autoThreshold.value=String(Math.max(0,Math.min(100,Number(a.th)||0)));
        autoEnable.checked=!!a.en;
        autoAboveOn.value=(a.aboveOn===false)?'0':'1';

        const dev=devices.find(v=>v.idx===idx);
        const devName=dev?dev.id:('Slot '+idx);
        const actionAbove=(autoAboveOn.value==='1')?'ON':'OFF';
        const actionBelow=(autoAboveOn.value==='1')?'OFF':'ON';
        autoRule.textContent=autoEnable.checked
            ?('Rule: if sensor ≥ '+autoThreshold.value+'% then '+devName+' '+actionAbove+', else '+actionBelow)
            :'Automation disabled.';
    }

    const s=d.sensor||{};
    if(s.has){
        const sender=s.sender||'SENSOR';
        const online=!!s.online;
        const stateText=online?'ONLINE':'OFFLINE';
        sensorNow.textContent='Sensor '+sender+' ['+stateText+']: '+Number(s.level)+'% (updated '+Number(s.age||0)+'s ago)';
    }else{
        sensorNow.textContent='Sensor: waiting for data...';
    }
}

function renderSchedule(d){
    const devices=(d.devices||[]).filter(v=>v.valid);
    if(devices.length===0){
        schDevice.innerHTML='';
        schApply.disabled=true;
        schSync.textContent='No configured devices for scheduling.';
        schList.textContent='';
        return;
    }
    schApply.disabled=false;

    const currentIds=[...schDevice.options].map(o=>o.value).join(',');
    const nextIds=devices.map(v=>String(v.idx)).join(',');
    if(currentIds!==nextIds){
        schDevice.innerHTML=devices.map(v=>'<option value="'+v.idx+'">'+esc(v.id)+'</option>').join('');
    }

    if(!devices.some(v=>v.idx===schSelectedIdx)) schSelectedIdx=devices[0].idx;
    schDevice.value=String(schSelectedIdx);

    const sch=(d.schedule||[]).find(s=>s.idx===schSelectedIdx);
    if(sch){
        schEnable.checked=!!sch.en;
        schDelay.value=String(Number(sch.delay)||0);
        schDuration.value=String(Math.max(1, Number(sch.dur)||60));
    }

    const active=(d.schedule||[])
        .filter(s=>s.en)
        .map(s=>{
            const dev=devices.find(v=>v.idx===s.idx);
            const name=dev?dev.id:('Slot '+s.idx);
            const left=Number(s.left)||0;
            const phase=s.running?'running':'waiting';
            return name+': after '+(Number(s.delay)||0)+'s for '+(Number(s.dur)||0)+'s ('+phase+', '+left+'s left)';
        });
    schList.textContent=active.length?('Active: '+active.join(' | ')):'Active: none';
    schSync.textContent='Timers run from relay uptime.';
}

function renderConfig(devices){
    const frag=document.createDocumentFragment();
    devices.forEach(dev=>{
        const card=document.createElement('div');
        card.className='cfg-card';
        const cfgPend=pendingConfig.has(dev.idx);
        const idValue=dev.id||'';
        card.innerHTML='<div><b>Slot '+dev.idx+'</b></div>'+
            '<div class="cfg-row"><label for="id_'+dev.idx+'">Device ID</label><input id="id_'+dev.idx+'" type="text" maxlength="15" value="'+esc(idValue)+'"></div>'+
            '<div class="cfg-row"><label for="gpio_'+dev.idx+'">GPIO</label><input data-gpio="1" id="gpio_'+dev.idx+'" type="number" min="0" max="39" value="'+dev.gpio+'"></div>'+
            '<div class="cfg-row"><label for="ah_'+dev.idx+'">Polarity</label><select id="ah_'+dev.idx+'"><option value="1" '+(dev.ah?'selected':'')+'>Active High</option><option value="0" '+(!dev.ah?'selected':'')+'>Active Low</option></select></div>'+
            '<div id="err_'+dev.idx+'" class="cfg-error"></div>'+
            '<div class="cfg-row"><button id="apply_'+dev.idx+'" class="btn" type="button" '+(cfgPend?'disabled':'')+'>'+(cfgPend?'Applying...':'Apply')+'</button></div>';
        frag.appendChild(card);
    });
    cfg.innerHTML='';
    cfg.appendChild(frag);

    devices.forEach(dev=>{
        document.getElementById('apply_'+dev.idx).onclick=()=>saveConfig(dev.idx);
        document.getElementById('id_'+dev.idx).addEventListener('input',validateAllConfig);
        document.getElementById('gpio_'+dev.idx).addEventListener('input',validateAllConfig);
        document.getElementById('ah_'+dev.idx).addEventListener('change',validateAllConfig);
    });
    validateAllConfig();
}

function gatherGpioUsage(){
    const map=new Map();
    document.querySelectorAll('input[data-gpio="1"]').forEach(input=>{
        const idx=input.id.split('_')[1];
        const value=parseInt(input.value,10);
        if(Number.isNaN(value)) return;
        if(!map.has(value)) map.set(value,[]);
        map.get(value).push(idx);
    });
    return map;
}

function validateAllConfig(){
    const usage=gatherGpioUsage();
    document.querySelectorAll('[id^="apply_"]').forEach(btn=>{
        const idx=btn.id.split('_')[1];
        const id=document.getElementById('id_'+idx).value.trim();
        const gpio=parseInt(document.getElementById('gpio_'+idx).value,10);
        const errEl=document.getElementById('err_'+idx);
        let err='';
        if(!validIdRe.test(id)) err='ID format invalid (A-Z, 0-9, _ or -, max 15).';
        else if(Number.isNaN(gpio)||gpio<0||gpio>39) err='GPIO must be 0..39.';
        else if(reservedPins.includes(gpio)) err='GPIO '+gpio+' is reserved.';
        else if((usage.get(gpio)||[]).length>1) err='Duplicate GPIO '+gpio+' detected.';
        errEl.textContent=err;
        btn.disabled=!!err||pendingConfig.has(parseInt(idx,10));
    });
}

function requestDeviceToggle(deviceId){
    if(pendingDevices.has(deviceId)||pendingAll) return;
    if(confirmMode){
        openConfirm({kind:'device',device:deviceId,message:'Toggle '+deviceId+'?'});
        return;
    }
    doDeviceToggle(deviceId);
}

function doDeviceToggle(deviceId){
    pendingDevices.add(deviceId);
    if(dataState) render(dataState);
    pendingToast='Device toggled';
    if(!send({type:'toggle',device:deviceId})){
        pendingDevices.delete(deviceId);
        if(dataState) render(dataState);
    }
}

function doSetAll(state){
    pendingAll=true;
    if(dataState) render(dataState);
    pendingToast='Device toggled';
    if(!send({type:'set_all',state:state})){
        pendingAll=false;
        if(dataState) render(dataState);
    }
}

function saveConfig(idx){
    validateAllConfig();
    const btn=document.getElementById('apply_'+idx);
    if(btn.disabled) return;
    const id=document.getElementById('id_'+idx).value.trim();
    const gpio=parseInt(document.getElementById('gpio_'+idx).value,10);
    const ah=document.getElementById('ah_'+idx).value==='1';
    pendingConfig.add(idx);
    if(dataState) render(dataState);
    pendingToast='Configuration saved';
    if(!send({type:'config',idx:idx,id:id,gpio:gpio,ah:ah})){
        pendingConfig.delete(idx);
        if(dataState) render(dataState);
    }
}

schDevice.onchange=()=>{
    schSelectedIdx=parseInt(schDevice.value,10);
    if(dataState) renderSchedule(dataState);
};

schApply.onclick=()=>{
    const idx=parseInt(schDevice.value,10);
    const delay=parseInt(schDelay.value,10);
    const duration=parseInt(schDuration.value,10);
    if(Number.isNaN(idx)||Number.isNaN(delay)||Number.isNaN(duration)||delay<0||duration<1){
        showToast('Invalid schedule input');
        return;
    }
    pendingToast='Schedule saved';
    if(!send({type:'schedule',idx:idx,en:schEnable.checked,delay:delay,dur:duration})){
        setBanner('err','Connection lost. Reconnecting...');
    }
};

autoApply.onclick=()=>{
    const idx=parseInt(autoDevice.value,10);
    const th=parseInt(autoThreshold.value,10);
    const aboveOn=(autoAboveOn.value==='1');
    if(Number.isNaN(idx)||Number.isNaN(th)||th<0||th>100){
        showToast('Invalid automation input');
        return;
    }
    pendingToast='Automation saved';
    if(!send({type:'automation',idx:idx,en:autoEnable.checked,th:th,aboveOn:aboveOn})){
        setBanner('err','Connection lost. Reconnecting...');
    }
};

modeBtn.onclick=()=>{
    confirmMode=!confirmMode;
    localStorage.setItem('relay.toggle.confirm',confirmMode?'1':'0');
    modeLabel();
    showToast(confirmMode?'Confirm mode enabled':'One-tap mode enabled');
};

masterSwitch.onchange=()=>{
    if(!dataState) return;
    const targetOn=masterSwitch.checked;
    openConfirm({kind:'all',state:targetOn,message:targetOn?'Turn all relays ON?':'Turn all relays OFF?'});
};

confirmBtn.onclick=()=>{
    if(!confirmTarget){closeConfirm();return;}
    if(confirmTarget.kind==='device') doDeviceToggle(confirmTarget.device);
    else if(confirmTarget.kind==='all') doSetAll(confirmTarget.state);
    closeConfirm();
};
cancelBtn.onclick=()=>closeConfirm();
overlay.onclick=(e)=>{if(e.target===overlay)closeConfirm();};
window.addEventListener('keydown',e=>{if(e.key==='Escape')closeConfirm();});

modeLabel();
connect();
</script>
</body></html>
)HTML";

    g_web.send(200, "text/html", html);
}

static void ws_send_error(uint8_t client, const char* err) {
    String msg = "{\"type\":\"error\",\"err\":\"";
    msg += err;
    msg += "\"}";
    g_ws.sendTXT(client, msg);
}

static void ws_send_ok(uint8_t client) {
    g_ws.sendTXT(client, "{\"type\":\"ok\"}");
}

static bool apply_all_devices_state(bool on) {
    const AgriDevice* tbl = agri_devmap_table();
    uint8_t cnt = agri_devmap_count();
    bool appliedAny = false;
    uint8_t cmd = on ? CMD_DEV_ON : CMD_DEV_OFF;
    for (uint8_t i = 0; i < cnt; i++) {
        if (!tbl[i].valid) continue;
        if (apply_local_command(tbl[i].id, cmd, "WEB")) {
            appliedAny = true;
        }
    }
    return appliedAny;
}

static void ws_broadcast_status() {
    String payload = "{\"type\":\"status\",\"data\":";
    payload += web_status_json();
    payload += "}";
    g_ws.broadcastTXT(payload);
}

static void mesh_broadcast_device_list_snapshot() {
    const AgriDevice* tbl = agri_devmap_table();
    uint8_t cnt = agri_devmap_count();
    uint16_t snapshotMsgId = agri_next_message_id();

    for (uint8_t i = 0; i < cnt; i++) {
        AgriMessage rsp;
        rsp.clear();
        strlcpy(rsp.farmId, relay_farm_id(), sizeof(rsp.farmId));

        if (tbl[i].valid) {
            strlcpy(rsp.deviceId, tbl[i].id, sizeof(rsp.deviceId));
            rsp.devState = tbl[i].state ? 1 : 0;
        } else {
            snprintf(rsp.deviceId, sizeof(rsp.deviceId), "SLOT_%u", i + 1);
            rsp.devState = 0;
        }

        rsp.command      = CMD_DEVLIST_RSP;
        rsp.messageId    = snapshotMsgId;
        rsp.timestamp    = millis();
        rsp.nonce        = i;
        rsp.sourceNodeId = agri_get_node_id();

        agri_broadcast(rsp);
    }
}

static void ws_on_event(uint8_t client, WStype_t type, uint8_t* payload, size_t length) {
    if (type == WStype_CONNECTED) {
        ws_broadcast_status();
        return;
    }

    if (type != WStype_TEXT) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        ws_send_error(client, "bad json");
        return;
    }

    const char* msgType = doc["type"] | "";

    if (strcmp(msgType, "get_status") == 0) {
        ws_broadcast_status();
        return;
    }

    if (strcmp(msgType, "toggle") == 0) {
        const char* dev = doc["device"] | "";
        if (!dev[0]) {
            ws_send_error(client, "missing device");
            return;
        }
        bool ok = apply_local_command(dev, CMD_TOGGLE, "WEB");
        if (!ok) {
            ws_send_error(client, "unknown device");
            return;
        }
        ws_send_ok(client);
        ws_broadcast_status();
        return;
    }

    if (strcmp(msgType, "set") == 0) {
        const char* dev = doc["device"] | "";
        bool stateOn = doc["state"] | false;
        if (!dev[0]) {
            ws_send_error(client, "missing device");
            return;
        }

        bool ok = apply_local_command(dev, stateOn ? CMD_DEV_ON : CMD_DEV_OFF, "WEB");
        if (!ok) {
            ws_send_error(client, "unknown device");
            return;
        }
        ws_send_ok(client);
        ws_broadcast_status();
        return;
    }

    if (strcmp(msgType, "set_all") == 0) {
        bool stateOn = doc["state"] | false;
        bool ok = apply_all_devices_state(stateOn);
        if (!ok) {
            ws_send_error(client, "unknown device");
            return;
        }
        ws_send_ok(client);
        ws_broadcast_status();
        return;
    }

    if (strcmp(msgType, "config") == 0) {
        int idx = doc["idx"] | -1;
        const char* id = doc["id"] | "";
        int gpio = doc["gpio"] | -1;
        bool activeHigh = doc["ah"] | true;

        if (idx < 0 || idx >= agri_devmap_count() || !is_valid_device_id(id) || gpio < 0 || gpio > 39) {
            ws_send_error(client, "invalid args");
            return;
        }
        if (is_reserved_gpio((uint8_t)gpio)) {
            ws_send_error(client, "reserved gpio");
            return;
        }

        bool ok = agri_devmap_reconfigure((uint8_t)idx, id, (uint8_t)gpio, activeHigh);
        if (!ok) {
            ws_send_error(client, "conflict/invalid config");
            return;
        }

        sync_grid_from_devmap();
        update_led();
        save_devcfg_blob();
        g_display.markDirty();

        ws_send_ok(client);
        ws_broadcast_status();
        mesh_broadcast_device_list_snapshot();
        return;
    }

    if (strcmp(msgType, "schedule") == 0) {
        int idx = doc["idx"] | -1;
        bool en = doc["en"] | false;
        int delaySec = doc["delay"] | -1;
        int runSec = doc["dur"] | -1;

        if (idx < 0 || idx >= GRID_DEV_TILES || delaySec < 0 || runSec < 1 || runSec > 86400) {
            ws_send_error(client, "invalid args");
            return;
        }

        g_schedule[idx].enabled = en;
        g_schedule[idx].delaySec = (uint32_t)delaySec;
        g_schedule[idx].runSec = (uint32_t)runSec;
        g_schedule[idx].armedAtMs = millis();
        g_schedule[idx].runStartMs = 0;
        g_schedule[idx].running = false;
        save_schedule_blob();
        sync_grid_schedule_badges(millis());

        g_display.setAckStatus(schedule_any_enabled() ? "SCH ON" : "SCH OFF");
        g_display.markDirty();

        ws_send_ok(client);
        ws_broadcast_status();
        return;
    }

    if (strcmp(msgType, "automation") == 0) {
        int idx = doc["idx"] | -1;
        bool en = doc["en"] | false;
        int threshold = doc["th"] | -1;
        bool aboveOn = doc["aboveOn"] | true;

        if (idx < 0 || idx >= GRID_DEV_TILES || threshold < 0 || threshold > 100) {
            ws_send_error(client, "invalid args");
            return;
        }

        g_sensorAuto.enabled = en;
        g_sensorAuto.threshold = (uint8_t)threshold;
        g_sensorAuto.targetIdx = (int8_t)idx;
        g_sensorAuto.aboveOn = aboveOn;
        g_sensorAuto.lastEvalValid = false;

        if (g_sensorLevelValid) {
            sensor_automation_apply(g_sensorLevel);
        }

        ws_send_ok(client);
        ws_broadcast_status();
        return;
    }

    ws_send_error(client, "unknown message type");
}

static void web_init() {
    g_web.on("/", HTTP_GET, web_handle_root);
    g_web.onNotFound([]() {
        g_web.send(404, "application/json", "{\"ok\":false,\"err\":\"not found\"}");
    });
    g_web.begin();

    g_ws.begin();
    g_ws.onEvent(ws_on_event);

    bool mdnsOk = MDNS.begin(AGRI_RELAY_HOSTNAME);
    if (mdnsOk) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("ws", "tcp", 81);
        Serial.printf("[WEB] Dashboard ready on http://%s.local/\n", AGRI_RELAY_HOSTNAME);
    } else {
        Serial.println("[WEB] mDNS start failed (hostname unavailable)");
    }
    Serial.printf("[WEB] Dashboard IP: http://%s/\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WEB] WS endpoint: ws://%s:81/\n", WiFi.localIP().toString().c_str());
}

// ============================================================================
// Local grid toggle callback (relay-side direct control)
// ============================================================================
static void on_grid_toggle(const char* farmId, const char* deviceId, uint8_t cmd) {
    apply_local_command(deviceId, cmd, "LOCAL", farmId);
}

static bool apply_local_command(const char* deviceId, uint8_t cmd,
                                const char* sourceTag, const char* farmId) {
    const char* resolvedFarmId = (farmId && farmId[0]) ? farmId : relay_farm_id();
    AgriDevice* dev = agri_devmap_find(deviceId);
    if (!dev) {
        agri_log(resolvedFarmId, deviceId, agri_command_name(cmd), "NO_DEV");
        return false;
    }

    switch (cmd) {
        case CMD_PUMP_ON:
        case CMD_DEV_ON:
            agri_devmap_set(deviceId, true);
            break;
        case CMD_PUMP_OFF:
        case CMD_DEV_OFF:
            agri_devmap_set(deviceId, false);
            break;
        case CMD_TOGGLE:
            agri_devmap_toggle(deviceId);
            break;
        default:
            return false;
    }

    update_led();

    int st = agri_devmap_state(deviceId);
    bool on = (st == 1);
    grid_set_device_state(deviceId, on);

    uint16_t localMsgId = agri_next_message_id();
    g_display.setLastCommand(agri_command_name(cmd), localMsgId);
    g_display.setAckStatus("LOCAL");
    g_display.markDirty();

    agri_log(resolvedFarmId, deviceId, agri_command_name(cmd), on ? "ON" : "OFF");
    Serial.printf("[%s] %s -> %s\n", sourceTag, deviceId, on ? "ON" : "OFF");
    return true;
}

static void sync_grid_from_devmap() {
    const AgriDevice* tbl = agri_devmap_table();
    uint8_t cnt = agri_devmap_count();
    for (uint8_t i = 0; i < cnt && i < GRID_DEV_TILES; i++) {
        if (tbl[i].valid) {
            grid_set_device_binding(i, tbl[i].id, tbl[i].state);
        } else {
            char slotId[AGRI_DEVICE_ID_LEN];
            snprintf(slotId, sizeof(slotId), "SLOT_%u", i + 1);
            grid_set_device_binding(i, slotId, false);
        }
    }
}

static void save_devcfg_blob() {
    const AgriDevice* tbl = agri_devmap_table();
    uint8_t cnt = agri_devmap_count();
    String blob;
    for (uint8_t i = 0; i < cnt; i++) {
        if (!tbl[i].valid) continue;
        if (blob.length()) blob += ';';
        blob += String(i);
        blob += ',';
        blob += tbl[i].id;
        blob += ',';
        blob += String(tbl[i].gpio);
        blob += ',';
        blob += (tbl[i].activeHigh ? "1" : "0");
    }
    agri_nvs_save_devcfg(blob.c_str());
}

static void load_devcfg_blob() {
    char buf[256] = {0};
    if (!agri_nvs_load_devcfg(buf, sizeof(buf))) return;

    char* saveptr = nullptr;
    char* token = strtok_r(buf, ";", &saveptr);
    uint8_t seqIdx = 0;
    while (token) {
        int idx = -1;
        char id[AGRI_DEVICE_ID_LEN] = {0};
        int gpio = -1;
        int ah = 1;

        if (sscanf(token, "%d,%15[^,],%d,%d", &idx, id, &gpio, &ah) == 4) {
            if (idx >= 0 && idx < agri_devmap_count()) {
                agri_devmap_reconfigure((uint8_t)idx, id, (uint8_t)gpio, ah != 0);
            }
        } else if (sscanf(token, "%15[^,],%d,%d", id, &gpio, &ah) == 3) {
            if (seqIdx < agri_devmap_count()) {
                agri_devmap_reconfigure(seqIdx, id, (uint8_t)gpio, ah != 0);
                seqIdx++;
            }
        }

        token = strtok_r(nullptr, ";", &saveptr);
    }
}

static bool schedule_any_enabled() {
    for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
        if (g_schedule[i].enabled) return true;
    }
    return false;
}

static int schedule_index_for_device(const char* deviceId) {
    if (!deviceId || !deviceId[0]) return -1;
    const AgriDevice* tbl = agri_devmap_table();
    for (uint8_t i = 0; i < agri_devmap_count() && i < GRID_DEV_TILES; i++) {
        if (!tbl[i].valid) continue;
        if (strcmp(tbl[i].id, deviceId) == 0) return (int)i;
    }
    return -1;
}

static void sync_grid_schedule_badges(uint32_t now) {
    const AgriDevice* tbl = agri_devmap_table();
    for (uint8_t i = 0; i < agri_devmap_count() && i < GRID_DEV_TILES; i++) {
        if (!tbl[i].valid) continue;
        uint32_t leftSec = 0;
        bool enabled = g_schedule[i].enabled;
        bool running = g_schedule[i].running;
        if (enabled) {
            if (running) {
                uint32_t elapsed = (now - g_schedule[i].runStartMs) / 1000UL;
                leftSec = (elapsed >= g_schedule[i].runSec) ? 0 : (g_schedule[i].runSec - elapsed);
            } else {
                uint32_t elapsed = (now - g_schedule[i].armedAtMs) / 1000UL;
                leftSec = (elapsed >= g_schedule[i].delaySec) ? 0 : (g_schedule[i].delaySec - elapsed);
            }
        }
        grid_set_device_schedule(tbl[i].id, enabled, running, leftSec, g_schedule[i].delaySec, g_schedule[i].runSec);
    }
}

static void save_schedule_blob() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["idx"] = i;
        o["en"] = g_schedule[i].enabled;
        o["delay"] = g_schedule[i].delaySec;
        o["dur"] = g_schedule[i].runSec;
    }
    String out;
    serializeJson(doc, out);
    agri_nvs_save_schedule(out.c_str());
}

static void load_schedule_blob() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
        g_schedule[i].enabled = false;
        g_schedule[i].delaySec = 0;
        g_schedule[i].runSec = 0;
        g_schedule[i].armedAtMs = now;
        g_schedule[i].runStartMs = 0;
        g_schedule[i].running = false;
    }

    char buf[512] = {0};
    if (!agri_nvs_load_schedule(buf, sizeof(buf))) return;

    JsonDocument doc;
    if (deserializeJson(doc, buf)) return;
    JsonArray arr = doc.as<JsonArray>();
    for (JsonVariant v : arr) {
        int idx = v["idx"] | -1;
        uint32_t delaySec = v["delay"] | 0;
        uint32_t runSec = v["dur"] | 0;
        if (idx < 0 || idx >= GRID_DEV_TILES) continue;
        g_schedule[idx].enabled = v["en"] | false;
        g_schedule[idx].delaySec = delaySec;
        g_schedule[idx].runSec = runSec;
        g_schedule[idx].armedAtMs = now;
        g_schedule[idx].runStartMs = 0;
        g_schedule[idx].running = false;
    }
}

static void schedule_tick(uint32_t now) {
    if ((now - g_lastScheduleTick) < AGRI_SCHEDULE_TICK_MS) return;
    g_lastScheduleTick = now;

    const AgriDevice* tbl = agri_devmap_table();
    bool changed = false;
    bool persistNeeded = false;
    for (uint8_t i = 0; i < agri_devmap_count() && i < GRID_DEV_TILES; i++) {
        if (!tbl[i].valid) continue;
        if (!g_schedule[i].enabled) continue;

        if (!g_schedule[i].running) {
            uint32_t elapsedWaitSec = (now - g_schedule[i].armedAtMs) / 1000UL;
            if (elapsedWaitSec >= g_schedule[i].delaySec) {
                if (apply_local_command(tbl[i].id, CMD_DEV_ON, "SCHED")) {
                    g_schedule[i].running = true;
                    g_schedule[i].runStartMs = now;
                    changed = true;
                }
            }
            continue;
        }

        uint32_t elapsedRunSec = (now - g_schedule[i].runStartMs) / 1000UL;
        if (elapsedRunSec >= g_schedule[i].runSec) {
            if (apply_local_command(tbl[i].id, CMD_DEV_OFF, "SCHED")) {
                changed = true;
            }
            g_schedule[i].enabled = false;
            g_schedule[i].running = false;
            g_schedule[i].runStartMs = 0;
            persistNeeded = true;
        }
    }

    if (persistNeeded) {
        save_schedule_blob();
    }

    sync_grid_schedule_badges(now);
    if (schedule_any_enabled()) {
        g_display.markDirty();
    }

    if (changed) {
        g_display.setAckStatus(schedule_any_enabled() ? "SCH ON" : "SCH OFF");
        g_display.markDirty();
        ws_broadcast_status();
    }
}

static void sensor_automation_apply(uint8_t level) {
    if (!g_sensorAuto.enabled) return;

    if (g_sensorAuto.targetIdx < 0 || g_sensorAuto.targetIdx >= (int8_t)agri_devmap_count()) {
        return;
    }

    const AgriDevice* tbl = agri_devmap_table();
    const AgriDevice& dev = tbl[g_sensorAuto.targetIdx];
    if (!dev.valid) return;

    bool high = level >= g_sensorAuto.threshold;
    if (g_sensorAuto.lastEvalValid && g_sensorAuto.lastHigh == high) {
        return;
    }

    g_sensorAuto.lastEvalValid = true;
    g_sensorAuto.lastHigh = high;

    bool desiredOn = high ? g_sensorAuto.aboveOn : !g_sensorAuto.aboveOn;
    uint8_t cmd = desiredOn ? CMD_DEV_ON : CMD_DEV_OFF;
    if (apply_local_command(dev.id, cmd, "AUTO")) {
        g_display.setAckStatus("AUTO");
        g_display.markDirty();
    }
}

static void sensor_automation_on_sample(uint8_t level, const char* senderId) {
    g_sensorLevel = level;
    g_sensorLevelValid = true;
    g_sensorLastSampleMs = millis();
    g_sensorHistory[g_sensorHistHead].level = level;
    g_sensorHistory[g_sensorHistHead].atMs = g_sensorLastSampleMs;
    g_sensorHistHead = (uint8_t)((g_sensorHistHead + 1) % SENSOR_HISTORY_SIZE);
    if (g_sensorHistCount < SENSOR_HISTORY_SIZE) {
        g_sensorHistCount++;
    }
    if (senderId && senderId[0]) {
        strlcpy(g_sensorLastSender, senderId, sizeof(g_sensorLastSender));
    }
    sensor_automation_apply(level);
}

// ============================================================================
// D2 LED — mirrors PUMP_01 state
// ============================================================================
static void update_led() {
    int ps = agri_devmap_state("PUMP_01");
    digitalWrite(AGRI_LED_PIN, (ps == 1) ? HIGH : LOW);
}

// ============================================================================
// Build & Send ACK
// ============================================================================
static void send_ack(uint32_t destNode, uint16_t origMsgId, const char* devId) {
    AgriMessage ack;
    strlcpy(ack.farmId,   relay_farm_id(), sizeof(ack.farmId));
    strlcpy(ack.deviceId, devId,         sizeof(ack.deviceId));
    ack.command      = CMD_ACK;
    ack.messageId    = origMsgId;
    ack.timestamp    = millis();
    ack.nonce        = agri_random_nonce();
    ack.sourceNodeId = agri_get_node_id();

    // Attach the current state of the target device
    int st = agri_devmap_state(devId);
    ack.devState = (st == 1) ? 1 : 0;

    if (destNode != 0) {
        if (!agri_send(destNode, ack)) {
            Serial.println("[APP] Directed ACK failed, broadcasting...");
            agri_broadcast(ack);
        }
    } else {
        agri_broadcast(ack);
    }
}

// ============================================================================
// Handle a device command (ON / OFF / TOGGLE)
// Returns true if the device was found and acted upon.
// ============================================================================
static bool handle_device_cmd(const AgriMessage& msg) {
    const char* devId = msg.deviceId;
    AgriDevice* dev = agri_devmap_find(devId);

    // If the message targets our relay generic ID, default to PUMP_01
    if (!dev && strcmp(devId, DEVICE_ID) == 0) {
        devId = "PUMP_01";
        dev = agri_devmap_find(devId);
    }

    if (!dev) {
        Serial.printf("[APP] Unknown device '%s'\n", msg.deviceId);
        agri_log(msg.farmId, msg.deviceId,
                 agri_command_name(msg.command), "NO_DEV");
        return false;
    }

    switch (msg.command) {
        case CMD_PUMP_ON:
        case CMD_DEV_ON:
            agri_devmap_set(devId, true);
            break;
        case CMD_PUMP_OFF:
        case CMD_DEV_OFF:
            agri_devmap_set(devId, false);
            break;
        case CMD_TOGGLE:
            agri_devmap_toggle(devId);
            break;
        default:
            return false;
    }

    update_led();

    int st = agri_devmap_state(devId);
    grid_set_device_state(devId, st == 1);
    Serial.printf("[APP] %s → %s\n", devId, st ? "ON" : "OFF");
    agri_log(msg.farmId, devId,
             agri_command_name(msg.command), st ? "ON" : "OFF");

    return true;
}

// ============================================================================
// Message Handler  (called by transport layer)
// ============================================================================
static void on_message_received(uint32_t from, const AgriMessage& msg) {
    Serial.printf("[APP] Msg from %u  cmd=%s  farm=%s  dev=%s  mid=%u\n",
                  from, agri_command_name(msg.command), msg.farmId,
                  msg.deviceId, msg.messageId);

    // --- Farm ID validation ---
    if (!agri_validate_farm_id(msg.farmId)) {
        Serial.printf("[APP] REJECTED: Farm ID mismatch (got '%s')\n", msg.farmId);
        agri_log(msg.farmId, msg.deviceId,
                 agri_command_name(msg.command), "BAD_FARM");
        return;
    }

    bool senderAllowed = relay_is_remote_allowed(msg.senderId);
    bool sensorHeartbeat = (msg.command == CMD_HEARTBEAT) &&
                           (strncmp(msg.senderId, "SENSOR_", 7) == 0);
    if (!senderAllowed && !sensorHeartbeat) {
        Serial.printf("[APP] REJECTED: Unauthorized sender '%s'\n", msg.senderId);
        agri_log(msg.farmId, msg.deviceId,
                 agri_command_name(msg.command), "BAD_REMOTE");
        return;
    }

    // Valid traffic from remote indicates link is alive
    range_on_heartbeat(from);

    // --- Duplicate detection ---
    if (agri_is_duplicate(msg.deviceId, msg.messageId)) {
        Serial.printf("[APP] DUPLICATE: %s #%u — ignored\n",
                      msg.deviceId, msg.messageId);
        if (msg.command != CMD_HEARTBEAT) {
            send_ack(from, msg.messageId, msg.deviceId);
        }
        return;
    }
    agri_record_message(msg.deviceId, msg.messageId);

    // --- Cache peer node ID ---
    g_lastRemoteNode = from;

    // --- Process command ---
    switch (msg.command) {
        case CMD_PUMP_ON:
        case CMD_PUMP_OFF:
        case CMD_DEV_ON:
        case CMD_DEV_OFF:
        case CMD_TOGGLE:
            if (handle_device_cmd(msg)) {
                send_ack(from, msg.messageId, msg.deviceId);
                g_display.setLastCommand(agri_command_name(msg.command), msg.messageId);
                g_display.setLastSource(from);
                g_display.markDirty();
            }
            break;

        case CMD_SCHED_SET: {
            int idx = schedule_index_for_device(msg.deviceId);
            if (idx >= 0 && idx < GRID_DEV_TILES) {
                g_schedule[idx].enabled = true;
                g_schedule[idx].delaySec = msg.nonce;
                g_schedule[idx].runSec = (msg.devState == 0) ? 1 : msg.devState;
                g_schedule[idx].armedAtMs = millis();
                g_schedule[idx].runStartMs = 0;
                g_schedule[idx].running = false;
                save_schedule_blob();
                sync_grid_schedule_badges(millis());
                g_display.setAckStatus("SCH ON");
                g_display.markDirty();
                ws_broadcast_status();
                send_ack(from, msg.messageId, msg.deviceId);
            }
            return;
        }

        case CMD_SCHED_CLR: {
            int idx = schedule_index_for_device(msg.deviceId);
            if (idx >= 0 && idx < GRID_DEV_TILES) {
                g_schedule[idx].enabled = false;
                g_schedule[idx].running = false;
                g_schedule[idx].runStartMs = 0;
                save_schedule_blob();
                sync_grid_schedule_badges(millis());
                g_display.setAckStatus(schedule_any_enabled() ? "SCH ON" : "SCH OFF");
                g_display.markDirty();
                ws_broadcast_status();
                send_ack(from, msg.messageId, msg.deviceId);
            }
            return;
        }

        case CMD_STATUS_REQ: {
            if (strcmp(msg.deviceId, "*") == 0) {
                // Wildcard: reply once per registered device
                const AgriDevice* tbl = agri_devmap_table();
                uint8_t cnt = agri_devmap_count();
                for (uint8_t i = 0; i < cnt; i++) {
                    if (!tbl[i].valid) continue;
                    AgriMessage rsp;
                    strlcpy(rsp.farmId,   relay_farm_id(), sizeof(rsp.farmId));
                    strlcpy(rsp.deviceId, tbl[i].id,    sizeof(rsp.deviceId));
                    rsp.command      = CMD_STATUS_RSP;
                    rsp.messageId    = msg.messageId;
                    rsp.timestamp    = millis();
                    rsp.nonce        = agri_random_nonce();
                    rsp.sourceNodeId = agri_get_node_id();
                    rsp.devState     = tbl[i].state ? 1 : 0;
                    agri_send(from, rsp);
                }
            } else {
                // Single-device status request
                AgriMessage rsp;
                strlcpy(rsp.farmId,   relay_farm_id(), sizeof(rsp.farmId));
                strlcpy(rsp.deviceId, msg.deviceId,  sizeof(rsp.deviceId));
                rsp.command      = CMD_STATUS_RSP;
                rsp.messageId    = msg.messageId;
                rsp.timestamp    = millis();
                rsp.nonce        = agri_random_nonce();
                rsp.sourceNodeId = agri_get_node_id();

                int st = agri_devmap_state(msg.deviceId);
                rsp.devState = (st == 1) ? 1 : 0;
                agri_send(from, rsp);
            }
            return;
        }

        case CMD_DEVLIST_REQ: {
            const AgriDevice* tbl = agri_devmap_table();
            uint8_t cnt = agri_devmap_count();

            for (uint8_t i = 0; i < cnt; i++) {
                AgriMessage rsp;
                rsp.clear();
                strlcpy(rsp.farmId, relay_farm_id(), sizeof(rsp.farmId));

                if (tbl[i].valid) {
                    strlcpy(rsp.deviceId, tbl[i].id, sizeof(rsp.deviceId));
                    rsp.devState = (tbl[i].state ? 1 : 0)
                                 | (g_schedule[i].running ? 0x40 : 0x00)
                                 | (g_schedule[i].enabled ? 0x80 : 0x00);
                } else {
                    snprintf(rsp.deviceId, sizeof(rsp.deviceId), "SLOT_%u", i + 1);
                    rsp.devState = 0;
                }

                rsp.command      = CMD_DEVLIST_RSP;
                rsp.messageId    = msg.messageId;
                rsp.timestamp    = millis();
                rsp.nonce        = i; // slot index
                rsp.sourceNodeId = agri_get_node_id();

                agri_send(from, rsp);
            }
            return;
        }

        case CMD_HEARTBEAT:
            Serial.printf("[APP] Heartbeat from %s sender=%s level=%u\n",
                          msg.deviceId, msg.senderId, msg.devState);
            if (strncmp(msg.senderId, "SENSOR_", 7) == 0) {
                sensor_automation_on_sample(msg.devState, msg.senderId);
                ws_broadcast_status();
            }
            return;

        case CMD_ACK:
        case CMD_NACK:
        case CMD_STATUS_RSP:
        case CMD_DEVLIST_RSP:
            return;

        default:
            Serial.printf("[APP] Unknown command: %d\n", msg.command);
            return;
    }
}

// ============================================================================
// Debounce helpers
// ============================================================================
static bool debounce(BtnState& b) {
    bool reading = digitalRead(b.pin);

    if (reading != b.raw) {
        b.debounceT = millis();
    }
    b.raw = reading;

    if ((millis() - b.debounceT) < AGRI_DEBOUNCE_MS) return false;

    if (reading != b.stable) {
        b.stable = reading;
        if (b.stable == LOW) {
            b.fired = false;
        }
    }

    if (b.stable == LOW && !b.fired) {
        b.fired = true;
        return true;
    }
    return false;
}

static void debounce_sel() {
    bool reading = digitalRead(g_btnSel.pin);

    if (reading != g_btnSel.raw) {
        g_btnSel.debounceT = millis();
    }
    g_btnSel.raw = reading;

    if ((millis() - g_btnSel.debounceT) < AGRI_DEBOUNCE_MS) return;

    if (reading != g_btnSel.stable) {
        g_btnSel.stable = reading;
    }
}

// ============================================================================
// Process 3-button local UI input
// ============================================================================
static void process_buttons(uint32_t now) {
    bool anyPress = false;

    bool upEdge  = debounce(g_btnUp);
    bool dnEdge  = debounce(g_btnDown);
    debounce_sel();

    bool selDown       = (g_btnSel.stable == LOW);
    bool selJustPressed = (selDown && !g_selPressed);

    if (upEdge || dnEdge || selJustPressed) anyPress = true;

    if (anyPress && g_displaySleeping) {
        g_displaySleeping  = false;
        g_lastActivityTime = now;
        g_display.markDirty();
        if (selJustPressed) {
            g_selPressed   = true;
            g_selPressTime = now;
            g_selLongFired = true;
        }
        return;
    }

    if (upEdge) {
        grid_on_up();
        g_display.markDirty();
    }

    if (dnEdge) {
        grid_on_down();
        g_display.markDirty();
    }

    if (selJustPressed) {
        g_selPressed   = true;
        g_selPressTime = now;
        g_selLongFired = false;
    }

    if (selDown && g_selPressed && !g_selLongFired) {
        if ((now - g_selPressTime) >= AGRI_LONG_PRESS_MS) {
            grid_on_back();
            g_display.markDirty();
            g_selLongFired = true;
        }
    }

    if (!selDown && g_selPressed) {
        if (!g_selLongFired) {
            grid_on_ok();
            g_display.markDirty();
        }
        g_selPressed = false;
    }

    if (anyPress) {
        g_lastActivityTime = now;
    }
}

// ============================================================================
// Connection Change Handler
// ============================================================================
static void on_connection_change(bool connected, uint16_t nodeCount) {
    g_display.setMeshStatus(connected, nodeCount);
    g_display.markDirty();

    if (connected && !g_meshWasConnected) {
        mesh_broadcast_device_list_snapshot();
    }
    g_meshWasConnected = connected;

    Serial.printf("[APP] Mesh %s — %d peer(s)\n",
                  connected ? "connected" : "no peers", nodeCount);
}

// ============================================================================
// Setup
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    {
        char savedFarmId[AGRI_FARM_ID_LEN] = {0};
        agri_nvs_init();
        if (agri_nvs_load_relay_farm(savedFarmId, sizeof(savedFarmId))) {
            agri_set_runtime_farm_id(savedFarmId);
        }
    }

    Serial.println("\n========================================");
    Serial.println("  ESP-Agri Relay Node (Multi-Device)");
    Serial.printf("  Farm: %s\n", relay_farm_id());
    Serial.println("========================================\n");

    // --- Device map (fail-safe: all OFF at boot) ---
    agri_devmap_init();

    // --- 3 Buttons (local control on relay) ---
    pinMode(AGRI_BTN_UP,   INPUT_PULLUP);
    pinMode(AGRI_BTN_DOWN, INPUT_PULLUP);
    pinMode(AGRI_BTN_SEL,  INPUT_PULLUP);

    pinMode(AGRI_LED_PIN, OUTPUT);
    digitalWrite(AGRI_LED_PIN, LOW);
    Serial.println("[APP] Device map initialised");

    // --- OLED ---
    g_display.init();
    g_display.showSplash("ESP-Agri RELAY", "Multi-Device v2");
    delay(1000);

    // --- Load persisted device map config ---
    load_devcfg_blob();
    load_schedule_blob();

    // --- Grid UI for local relay control ---
    grid_init(on_grid_toggle, relay_farm_id());

    // --- Load AOD settings from NVS ---
    {
        bool aodEn = true;
        uint8_t aodSec = AGRI_AOD_DEFAULT_SEC;
        if (agri_nvs_load_aod(&aodEn, &aodSec)) {
            grid_set_aod(aodEn);
            grid_set_aod_timeout(aodSec);
        }
        g_aodPrevEnabled   = grid_aod_enabled();
        g_aodPrevSec       = grid_aod_timeout_sec();
        g_lastActivityTime = millis();
    }

    // Seed grid device states from relay device map
    sync_grid_from_devmap();
    sync_grid_schedule_badges(millis());

    // --- Range indicator ---
    range_init();

    // --- Transport (radio-agnostic API) ---
    agri_transport_init(&g_mesh);
    g_mesh.init();

    agri_on_receive(on_message_received);
    agri_on_connection_change(on_connection_change);

    // --- Local web dashboard ---
    web_init();

    // --- Seed random for nonces ---
    randomSeed(analogRead(0) ^ millis());

    // --- Display initial state ---
    g_display.setRole("RELAY");
    g_display.setNodeId(agri_get_node_id());
    g_display.setFarmId(relay_farm_id());
    g_display.setDeviceState(false);
    g_display.setMeshStatus(false, 0);
    g_display.setAckStatus(schedule_any_enabled() ? "SCH ON" : "SCH OFF");

    Serial.printf("[APP] Setup complete — Node ID: %u\n", agri_get_node_id());
    serial_print_cfg();
}

// ============================================================================
// Loop
// ============================================================================
void loop() {
    // --- Drive mesh radio ---
    agri_update();
    handle_serial_cli();
    g_web.handleClient();
    g_ws.loop();

    uint32_t now = millis();
    schedule_tick(now);

    // --- Live websocket push for schedule/automation dashboard use cases ---
    uint32_t wsPushInterval = schedule_any_enabled() ? WS_ACTIVE_PUSH_MS : WS_IDLE_PUSH_MS;
    if ((now - g_lastWsLivePush) >= wsPushInterval) {
        g_lastWsLivePush = now;
        if (schedule_any_enabled() || g_sensorLevelValid) {
            ws_broadcast_status();
        }
    }

    // --- Button handling (UP / SEL / DOWN) ---
    process_buttons(now);

    // --- OLED schedule actions from setup/device confirm ---
    {
        char reqDev[AGRI_DEVICE_ID_LEN] = {0};
        uint32_t reqDelay = 0;
        uint32_t reqRun = 0;
        if (grid_take_schedule_apply_request(reqDev, sizeof(reqDev), &reqDelay, &reqRun)) {
            int idx = schedule_index_for_device(reqDev);
            if (idx >= 0 && idx < GRID_DEV_TILES) {
                g_schedule[idx].enabled = true;
                g_schedule[idx].delaySec = reqDelay;
                g_schedule[idx].runSec = reqRun;
                g_schedule[idx].armedAtMs = now;
                g_schedule[idx].runStartMs = 0;
                g_schedule[idx].running = false;
                save_schedule_blob();
                sync_grid_schedule_badges(now);
                g_display.setAckStatus("SCH ON");
                g_display.markDirty();
                ws_broadcast_status();
            }
        }

        char disableDev[AGRI_DEVICE_ID_LEN] = {0};
        if (grid_take_schedule_disable_request(disableDev, sizeof(disableDev))) {
            int idx = schedule_index_for_device(disableDev);
            if (idx >= 0 && idx < GRID_DEV_TILES) {
                g_schedule[idx].enabled = false;
                g_schedule[idx].running = false;
                g_schedule[idx].runStartMs = 0;
                save_schedule_blob();
                sync_grid_schedule_badges(now);
                g_display.setAckStatus(schedule_any_enabled() ? "SCH ON" : "SCH OFF");
                g_display.markDirty();
                ws_broadcast_status();
            }
        }
    }

    // --- Grid UI timeout updates (fail flash clear, menu timeout, timestamps) ---
    if (grid_check_timeout()) {
        g_display.markDirty();
    }

    // --- AOD: reset idle timer when screen transitions TO GRID_MAIN ---
    {
        GridScreen cur = grid_current_screen();
        if (cur == GRID_MAIN && g_prevScreenForAod != GRID_MAIN) {
            g_lastActivityTime = now;
        }
        g_prevScreenForAod = cur;
    }

    // --- Range state timeout update ---
    if (range_update()) {
        g_display.markDirty();
    }

    // --- Periodic display refresh ---
    if (now - g_lastDisplayRefresh >= AGRI_DISPLAY_REFRESH_MS) {
        g_lastDisplayRefresh = now;
        if (!g_displaySleeping) {
            g_display.refresh();
        }
    }

    // --- AOD: auto-sleep display when inactive (only on main grid) ---
    if (!grid_aod_enabled() && !g_displaySleeping
        && grid_current_screen() == GRID_MAIN)
    {
        uint32_t timeout = (uint32_t)grid_aod_timeout_sec() * 1000UL;
        if ((now - g_lastActivityTime) >= timeout) {
            g_displaySleeping = true;
            g_display.showSplash("", "");
        }
    }

    // --- RSSI polling ---
    if ((now - g_lastRssiPoll) >= AGRI_RSSI_POLL_MS) {
        g_lastRssiPoll = now;
        g_rssiTracker.feed(agri_get_rssi());
        g_display.setRSSI(g_rssiTracker.smoothed(),
                          g_rssiTracker.bars(),
                          g_rssiTracker.trend(),
                          g_rssiTracker.rawMin(),
                          g_rssiTracker.rawMax(),
                          g_rssiTracker.qualityLabel());
    }

    // --- Periodic heartbeat broadcast with device state bitmask ---
    if ((now - g_lastHeartbeat) >= AGRI_RANGE_PING_MS) {
        g_lastHeartbeat = now;

        // Build a bitmask of device states (bit i = device i ON)
        const AgriDevice* tbl = agri_devmap_table();
        uint8_t cnt = agri_devmap_count();
        uint8_t bitmask = 0;
        for (uint8_t i = 0; i < cnt && i < 7; i++) {
            if (tbl[i].valid && tbl[i].state)
                bitmask |= (1 << i);
        }
        if (schedule_any_enabled()) {
            bitmask |= 0x80;
        }

        AgriMessage hb;
        hb.clear();
        strlcpy(hb.farmId,   relay_farm_id(), sizeof(hb.farmId));
        strlcpy(hb.deviceId, "*",           sizeof(hb.deviceId));
        hb.command      = CMD_HEARTBEAT;
        hb.messageId    = agri_next_message_id();
        hb.timestamp    = millis();
        hb.nonce        = agri_random_nonce();
        hb.sourceNodeId = agri_get_node_id();
        hb.devState     = bitmask;

        agri_broadcast(hb);
    }

    // --- AOD: persist settings when changed ---
    {
        bool curEn = grid_aod_enabled();
        uint8_t curS = grid_aod_timeout_sec();
        if (curEn != g_aodPrevEnabled || curS != g_aodPrevSec) {
            agri_nvs_save_aod(curEn, curS);
            g_aodPrevEnabled = curEn;
            g_aodPrevSec     = curS;
            g_lastActivityTime = now;
        }
    }
}
