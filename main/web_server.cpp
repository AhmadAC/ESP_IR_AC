#include "web_server.h"
#include "config.h"
#include "automation.h"
#include "dht_sensor.h"
#include "ir_uart.h"
#include "wifi_manager.h"
#include "system_utils.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_wifi.h"

// Standard libraries required for time, memory, and string parsing
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WEB_SERVER";
httpd_handle_t server = NULL;
bool server_disabled_by_timer = false;

/* ==============================================
   HTML UI PAYLOAD
   ============================================== */
const char HTML_UI[] = R"raw_html(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>ESP32 A/C Controller</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
    :root { --primary: #0ea5e9; --bg: #0f172a; --card: #1e293b; --text: #f1f5f9; }
    body { font-family: -apple-system, sans-serif; background: var(--bg); color: var(--text); padding: 15px; display: flex; flex-direction: column; align-items: center; margin:0; }
    .container { width: 100%; max-width: 500px; }
    .card { background: var(--card); padding: 25px; border-radius: 20px; border: 1px solid #334155; text-align: center; margin-bottom: 20px; }
    h2 { color: var(--primary); margin-top: 0; }
    input, select { width: 100%; padding: 12px; margin: 8px 0 20px; border-radius: 10px; border: 1px solid #475569; background: #0f172a; color: white; box-sizing: border-box; }
    button { width: 100%; padding: 15px; border: none; border-radius: 12px; font-weight: bold; cursor: pointer; color: white; margin-top:10px; transition: transform 0.1s; }
    button:active { transform: scale(0.96); opacity: 0.9; }
    .btn-green { background: #10b981; }
    .btn-red { background: #ef4444; }
    .btn-blue { background: #3b82f6; }
    .btn-gray { background: #475569; }
    .btn-add { background: #0ea5e9; padding: 10px; border-radius: 8px; width: auto; font-size: 0.9rem; margin-top: 5px; }
    .status-bar { padding: 12px; border-radius: 10px; font-weight: bold; text-align: center; font-size: 0.85rem; border: 1px solid; text-transform: uppercase; background: #172554; color: #93c5fd; border-color: #3b82f6; margin-bottom:15px; }
    .text-sm { font-size: 0.85rem; color: #94a3b8; margin-bottom:10px; }
    .sensor-val { font-size: 1.5rem; color: #38bdf8; font-weight: bold; }
    table { border-collapse: collapse; width: 100%; margin-bottom: 20px; font-size: 0.9rem; }
    th, td { border: 1px solid #475569; padding: 10px; text-align: center; }
    th { background: #0f172a; color: #0ea5e9; }
    .del-btn { background: #ef4444; padding: 5px; margin: 0; border-radius: 6px; }
    .flex-row { display: flex; gap: 10px; align-items: center; justify-content: center;}
</style>
</head>
<body>
    <div class="container">
        <div class="status-bar">A/C CONTROLLER DASHBOARD</div>
        
        <!-- Sensor Data Card -->
        <div class="card" style="display:flex; justify-content:space-around;">
            <div>
                <div class="text-sm">Temperature</div>
                <div class="sensor-val"><span id="curtemp">--</span> &deg;C</div>
            </div>
            <div>
                <div class="text-sm">Humidity</div>
                <div class="sensor-val"><span id="curhum">--</span> %</div>
            </div>
        </div>

        <!-- Controls Card -->
        <div class="card">
            <h2>Control A/C</h2>
            <div class="text-sm"><b>Time:</b> <span id="curtime">Loading...</span></div>
            <div class="text-sm"><b>Last Command:</b> <span id="lastcmd" style="color:#3b82f6;">None</span></div>
            
            <div style="display:flex; gap:10px;">
                <button class="btn-gray" onclick="c(0)">Turn OFF</button>
                <button class="btn-green" onclick="c(1)">Turn ON</button>
            </div>
            
            <div style="display:grid; grid-template-columns: repeat(3, 1fr); gap:8px; margin-top:10px;">
                <button class="btn-blue" onclick="c(2)">19&deg;C</button>
                <button class="btn-blue" onclick="c(3)">20&deg;C</button>
                <button class="btn-blue" onclick="c(4)">21&deg;C</button>
                <button class="btn-blue" onclick="c(5)">22&deg;C</button>
                <button class="btn-blue" onclick="c(6)">23&deg;C</button>
                <button class="btn-blue" onclick="c(7)">24&deg;C</button>
                <button class="btn-blue" onclick="c(8)">25&deg;C</button>
                <button class="btn-blue" onclick="c(9)">26&deg;C</button>
                <button class="btn-blue" onclick="c(10)">27&deg;C</button>
            </div>
            
            <div style="display:flex; gap:10px; margin-top:8px;">
                <button class="btn-red" onclick="c(11)">HOT Mode</button>
                <button class="btn-blue" style="background:#0ea5e9;" onclick="c(12)">COLD Mode</button>
            </div>
            <p id="cstat" class="text-sm" style="margin-top:15px;"></p>
        </div>
        
        <!-- Temp Automation Card -->
        <div class="card">
            <h2>Auto-Trigger Actions</h2>
            <div class="text-sm">Trigger IR commands based on temperature.</div>
            
            <table id="autoTbl">
                <thead><tr><th>Condition</th><th>Threshold</th><th>Action</th><th></th></tr></thead>
                <tbody id="autoBody"></tbody>
            </table>

            <div class="flex-row" style="margin-top: 15px;">
                <select id="a_cond" style="margin:0; flex:1;">
                    <option value="1">Temp &ge;</option>
                    <option value="0">Temp &le;</option>
                </select>
                <input type="number" id="a_thresh" step="0.5" style="margin:0; flex:1;" placeholder="25.0">
                <div class="text-sm" style="margin:0;">&deg;C</div>
            </div>
            
            <div class="flex-row" style="margin-top: 15px;">
                <select id="a_cmd" style="margin:0; flex:1;">
                    <option value="0">AC OFF</option>
                    <option value="1">AC ON</option>
                    <option value="2">Set 19C</option>
                    <option value="3">Set 20C</option>
                    <option value="4">Set 21C</option>
                    <option value="5">Set 22C</option>
                    <option value="6">Set 23C</option>
                    <option value="7">Set 24C</option>
                    <option value="8">Set 25C</option>
                    <option value="9">Set 26C</option>
                    <option value="10">Set 27C</option>
                    <option value="11">Mode HOT</option>
                    <option value="12">Mode COLD</option>
                </select>
                <button class="btn-add" onclick="addAuto()">Add +</button>
            </div>
            
            <button class="btn-green" style="margin-top:20px;" onclick="saveAutos()">Save Automations to Device</button>
            <p id="astat" class="text-sm" style="margin-top:10px;"></p>
        </div>

        <!-- Timers Card -->
        <div class="card">
            <h2>Timers</h2>
            <table id="timersTbl">
                <thead><tr><th>Day</th><th>Time</th><th>Action</th><th></th></tr></thead>
                <tbody id="timersBody"></tbody>
            </table>
            
            <select id="t_day">
                <option value="-1">Everyday</option><option value="0">Sunday</option><option value="1">Monday</option>
                <option value="2">Tuesday</option><option value="3">Wednesday</option><option value="4">Thursday</option>
                <option value="5">Friday</option><option value="6">Saturday</option>
            </select>
            <input type="time" id="t_time">
            
            <div class="flex-row">
                <select id="t_cmd" style="margin:0; flex:1;">
                    <option value="0">AC OFF</option>
                    <option value="1">AC ON</option>
                    <option value="2">Set 19C</option>
                    <option value="3">Set 20C</option>
                    <option value="4">Set 21C</option>
                    <option value="5">Set 22C</option>
                    <option value="6">Set 23C</option>
                    <option value="7">Set 24C</option>
                    <option value="8">Set 25C</option>
                    <option value="9">Set 26C</option>
                    <option value="10">Set 27C</option>
                    <option value="11">Mode HOT</option>
                    <option value="12">Mode COLD</option>
                    <option value="13">Server OFF</option>
                    <option value="14">Server ON</option>
                </select>
                <button class="btn-add" onclick="addTimer()">Add +</button>
            </div>
            
            <button class="btn-green" style="margin-top:20px;" onclick="saveTimers()">Save Timers to Device</button>
            <p id="tstat" class="text-sm" style="margin-top:10px;"></p>
        </div>

        <!-- Wi-Fi Setup Card -->
        <div class="card">
            <h2>Wi-Fi Settings</h2>
            <div id="status" class="text-sm">Ready to Scan</div>
            <button class="btn-gray" onclick="scan()">Scan Networks</button>
            
            <select id="ssid" style="margin-top:15px;"><option value="">-- Select --</option></select>
            <input type="password" id="pass" placeholder="Password">
            
            <button class="btn-green" onclick="save()">Save and Connect</button>
            
            <hr style="border-color:#334155; margin: 25px 0;">
            <div class="text-sm">Quick Boot Mode Switch</div>
            <button style="background:#f59e0b;" onclick="forceAP()">Force AP Mode</button>
            <button style="background:#10b981;" onclick="forceWiFi()">Use Saved Wi-Fi</button>
        </div>
    </div>

    <script>
        let timers = [];
        let autos = [];
        const days = {'-1':'Everyday','0':'Sun','1':'Mon','2':'Tue','3':'Wed','4':'Thu','5':'Fri','6':'Sat'};
        const cmds = {'0':'OFF','1':'ON','2':'19C','3':'20C','4':'21C','5':'22C','6':'23C','7':'24C','8':'25C','9':'26C','10':'27C','11':'HOT','12':'COLD','13':'SRV OFF','14':'SRV ON'};
        const conds = {'0':'&le;', '1':'&ge;'};
        
        function c(cmd){
            document.getElementById('cstat').innerText = 'Sending...';
            fetch('/ir?cmd='+cmd).then(r=>r.text()).then(t=>{
                document.getElementById('cstat').innerText = 'Status: ' + t;
                setTimeout(loadStatus, 600);
            });
        }
        
        function renderAutos(){
            let b = document.getElementById('autoBody');
            b.innerHTML = '';
            autos.forEach((a, i)=>{
                let tr = document.createElement('tr');
                tr.innerHTML = `<td>Temp ${conds[a.c]}</td><td>${a.t}&deg;C</td><td>${cmds[a.cmd]}</td><td><button class="del-btn" onclick="delAuto(${i})">X</button></td>`;
                b.appendChild(tr);
            });
        }

        function addAuto(){
            let cStr = document.getElementById('a_cond').value;
            let tStr = document.getElementById('a_thresh').value;
            let cmdStr = document.getElementById('a_cmd').value;
            if(!tStr) return alert('Enter a valid temperature threshold.');
            
            autos.push({ c: parseInt(cStr), t: parseFloat(tStr), cmd: parseInt(cmdStr) });
            renderAutos();
        }

        function delAuto(i){ autos.splice(i, 1); renderAutos(); }

        function saveAutos(){
            if (autos.length === 0) {
                let tStr = document.getElementById('a_thresh').value;
                if (tStr) {
                    if (confirm("Your automation list is empty. Would you like to automatically add the currently entered rule before saving?")) {
                        addAuto();
                    } else {
                        return;
                    }
                } else {
                    alert("Please add at least one automation rule to save.");
                    return;
                }
            }
            document.getElementById('astat').innerText = 'Saving...';
            fetch('/auto', {method:'POST', body:JSON.stringify(autos)}).then(()=>{
                document.getElementById('astat').innerText = 'Saved!';
                setTimeout(() => document.getElementById('astat').innerText='', 2000);
            });
        }

        function renderTimers(){
            let b = document.getElementById('timersBody');
            b.innerHTML = '';
            timers.forEach((t, i)=>{
                let tr = document.createElement('tr');
                tr.innerHTML = `<td>${days[t.d]}</td><td>${t.h.toString().padStart(2,'0')}:${t.m.toString().padStart(2,'0')}</td><td>${cmds[t.c]}</td><td><button class="del-btn" onclick="delTimer(${i})">X</button></td>`;
                b.appendChild(tr);
            });
        }

        function addTimer(){
            let d = parseInt(document.getElementById('t_day').value);
            let timeStr = document.getElementById('t_time').value;
            let c = parseInt(document.getElementById('t_cmd').value);
            if(!timeStr) return alert('Select a valid time.');
            let parts = timeStr.split(':');
            timers.push({ d:d, h:parseInt(parts[0]), m:parseInt(parts[1]), c:c });
            renderTimers();
        }

        function delTimer(i){ timers.splice(i, 1); renderTimers(); }

        function saveTimers(){
            document.getElementById('tstat').innerText = 'Saving...';
            fetch('/timers', { method:'POST', body:JSON.stringify(timers) }).then(()=>{
                document.getElementById('tstat').innerText = 'Saved!';
                setTimeout(() => document.getElementById('tstat').innerText='', 2000);
            });
        }

        function scan(){
            document.getElementById('status').innerText = 'Scanning...';
            fetch('/scan').then(r=>r.json()).then(d=>{
                let s = document.getElementById('ssid');
                s.innerHTML = '<option value="">-- Select --</option>';
                d.forEach(n=>{ s.innerHTML += '<option value="'+n+'">'+n+'</option>'; });
                document.getElementById('status').innerText = 'Found ' + d.length + ' networks';
            }).catch(() => document.getElementById('status').innerText = 'Scan Error');
        }
        
        function save(){
            let s = document.getElementById('ssid').value, p = document.getElementById('pass').value;
            if(!s) return alert('Select SSID');
            fetch('/save', { method:'POST', body:JSON.stringify({ssid:s, pass:p}) }).then(()=>{
                alert('Credentials saved! Rebooting...');
            });
        }
        
        function forceAP(){
            if(confirm("Switch to AP Mode and reboot?")) fetch('/switch_to_ap', { method: 'POST' }).then(() => alert('Rebooting...'));
        }
        
        function forceWiFi(){
            if(confirm("Switch to Wi-Fi Mode and reboot?")) fetch('/switch_to_wifi', { method: 'POST' }).then(() => alert('Rebooting...'));
        }

        let isInitialLoad = true;
        function loadStatus(){
            fetch('/status').then(r=>r.json()).then(d=>{
                document.getElementById('curtime').innerText = d.time;
                document.getElementById('lastcmd').innerText = d.last_cmd;
                document.getElementById('curtemp').innerText = d.temp.toFixed(1);
                document.getElementById('curhum').innerText = d.hum.toFixed(1);
                
                if (isInitialLoad) {
                    timers = d.timers || [];
                    autos = d.autos || [];
                    renderTimers();
                    renderAutos();
                    isInitialLoad = false;
                }
            });
        }

        setInterval(loadStatus, 5000);
        window.onload = loadStatus;
    </script>
</body></html>
)raw_html";

static const httpd_uri_t uri_index  = { .uri = "/",       .method = HTTP_GET,  .handler = index_get_handler,  .user_ctx = NULL };
static const httpd_uri_t uri_scan   = { .uri = "/scan",   .method = HTTP_GET,  .handler = scan_get_handler,   .user_ctx = NULL };
static const httpd_uri_t uri_save   = { .uri = "/save",   .method = HTTP_POST, .handler = save_post_handler,  .user_ctx = NULL };
static const httpd_uri_t uri_ir     = { .uri = "/ir",     .method = HTTP_GET,  .handler = ir_get_handler,     .user_ctx = NULL };
static const httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET,  .handler = status_get_handler, .user_ctx = NULL };
static const httpd_uri_t uri_timers = { .uri = "/timers", .method = HTTP_POST, .handler = timers_post_handler,.user_ctx = NULL };
static const httpd_uri_t uri_auto   = { .uri = "/auto",   .method = HTTP_POST, .handler = auto_post_handler,  .user_ctx = NULL };
static const httpd_uri_t uri_switch_ap   = { .uri = "/switch_to_ap",   .method = HTTP_POST, .handler = switch_ap_post_handler,   .user_ctx = NULL };
static const httpd_uri_t uri_switch_wifi = { .uri = "/switch_to_wifi", .method = HTTP_POST, .handler = switch_wifi_post_handler, .user_ctx = NULL };

static const httpd_uri_t uri_cp1 = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_portal_redirect, .user_ctx = NULL };
static const httpd_uri_t uri_cp2 = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_portal_redirect, .user_ctx = NULL };
static const httpd_uri_t uri_cp3 = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_portal_redirect, .user_ctx = NULL };
static const httpd_uri_t uri_fallback = { .uri = "/*", .method = HTTP_GET, .handler = captive_portal_redirect, .user_ctx = NULL };

static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_UI, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t captive_portal_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t ir_get_handler(httpd_req_t *req) {
    char buf[50];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[10];
        if (httpd_query_key_value(buf, "cmd", param, sizeof(param)) == ESP_OK) {
            int cmd = atoi(param);
            if (cmd >= 0 && cmd <= 12) {
                execute_command(cmd);
                httpd_resp_sendstr(req, "Command Sent");
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Direct webserver on/off modification unsupported via GET IR.");
            }
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing cmd parameter");
    return ESP_FAIL;
}

static esp_err_t auto_post_handler(httpd_req_t *req) {
    char buf[1024];
    int ret, remaining = req->content_len;
    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }
    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    cJSON *arr = cJSON_Parse(buf);
    if (arr && cJSON_IsArray(arr)) {
        if (xSemaphoreTake(auto_mutex, portMAX_DELAY)) {
            num_autos = 0;
            int size = cJSON_GetArraySize(arr);
            for(int i = 0; i < size && i < MAX_AUTOS; i++) {
                cJSON *item = cJSON_GetArrayItem(arr, i);
                cJSON *c = cJSON_GetObjectItem(item, "c");
                cJSON *t = cJSON_GetObjectItem(item, "t");
                cJSON *cmd = cJSON_GetObjectItem(item, "cmd");
                if(c && t && cmd) {
                    auto_rules[num_autos].condition = c->valueint;
                    auto_rules[num_autos].threshold = t->valuedouble;
                    auto_rules[num_autos].command = cmd->valueint;
                    auto_triggered[num_autos] = false; 
                    num_autos++;
                }
            }
            xSemaphoreGive(auto_mutex);
        }
        
        cJSON_Delete(arr);
        nvs_handle_t my_handle;
        if(nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
            nvs_set_blob(my_handle, "auto_rules", auto_rules, sizeof(AutoEntry) * num_autos);
            nvs_set_u32(my_handle, "num_autos", num_autos);
            nvs_commit(my_handle);
            nvs_close(my_handle);
        }
        httpd_resp_sendstr(req, "OK");
    } else {
        if (arr) cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON parameters");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    time_t now = 0;
    time(&now);
    struct tm timeinfo;
    memset(&timeinfo, 0, sizeof(timeinfo));
    localtime_r(&now, &timeinfo);
    
    char time_str[64];
    if (timeinfo.tm_year < (2020 - 1900)) {
        strcpy(time_str, "Time not set");
    } else {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "time", time_str);
    cJSON_AddStringToObject(root, "last_cmd", last_command_str);
    cJSON_AddNumberToObject(root, "temp", current_temp);
    cJSON_AddNumberToObject(root, "hum", current_hum);
    
    cJSON *autos_arr = cJSON_CreateArray();
    if (xSemaphoreTake(auto_mutex, portMAX_DELAY)) {
        for(int i = 0; i < num_autos; i++) {
            cJSON *a = cJSON_CreateObject();
            cJSON_AddNumberToObject(a, "c", auto_rules[i].condition);
            cJSON_AddNumberToObject(a, "t", auto_rules[i].threshold);
            cJSON_AddNumberToObject(a, "cmd", auto_rules[i].command);
            cJSON_AddItemToArray(autos_arr, a);
        }
        xSemaphoreGive(auto_mutex);
    }
    cJSON_AddItemToObject(root, "autos", autos_arr);
    
    cJSON *timers_arr = cJSON_CreateArray();
    if (xSemaphoreTake(timer_mutex, portMAX_DELAY)) {
        for(int i = 0; i < num_timers; i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddNumberToObject(t, "d", timers[i].day);
            cJSON_AddNumberToObject(t, "h", timers[i].hour);
            cJSON_AddNumberToObject(t, "m", timers[i].minute);
            cJSON_AddNumberToObject(t, "c", timers[i].command);
            cJSON_AddItemToArray(timers_arr, t);
        }
        xSemaphoreGive(timer_mutex);
    }
    cJSON_AddItemToObject(root, "timers", timers_arr);
    
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t timers_post_handler(httpd_req_t *req) {
    char buf[1024];
    int ret, remaining = req->content_len;
    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }
    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    
    cJSON *arr = cJSON_Parse(buf);
    if (arr && cJSON_IsArray(arr)) {
        if (xSemaphoreTake(timer_mutex, portMAX_DELAY)) {
            num_timers = 0;
            int size = cJSON_GetArraySize(arr);
            for(int i = 0; i < size && i < MAX_TIMERS; i++) {
                cJSON *item = cJSON_GetArrayItem(arr, i);
                cJSON *d = cJSON_GetObjectItem(item, "d");
                cJSON *h = cJSON_GetObjectItem(item, "h");
                cJSON *m = cJSON_GetObjectItem(item, "m");
                cJSON *c = cJSON_GetObjectItem(item, "c");
                if(d && h && m && c) {
                    timers[num_timers].day = d->valueint;
                    timers[num_timers].hour = h->valueint;
                    timers[num_timers].minute = m->valueint;
                    timers[num_timers].command = c->valueint;
                    num_timers++;
                }
            }
            xSemaphoreGive(timer_mutex);
        }
        cJSON_Delete(arr);
        
        nvs_handle_t my_handle;
        if(nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
            nvs_set_blob(my_handle, "timers", timers, sizeof(TimerEntry) * num_timers);
            nvs_set_u32(my_handle, "num_timers", num_timers);
            nvs_commit(my_handle);
            nvs_close(my_handle);
        }
        httpd_resp_sendstr(req, "OK");
    } else {
        if (arr) cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    wifi_scan_config_t scan_config = {};
    esp_wifi_scan_start(&scan_config, true);
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    wifi_ap_record_t *ap_info = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    esp_wifi_scan_get_ap_records(&ap_count, ap_info);
    cJSON *root = cJSON_CreateArray();
    for(int i = 0; i < ap_count; i++) cJSON_AddItemToArray(root, cJSON_CreateString((char*)ap_info[i].ssid));
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(ap_info); cJSON_Delete(root); free(json_str);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[200];
    int ret, remaining = req->content_len;
    size_t recv_len = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
    if ((ret = httpd_req_recv(req, buf, recv_len)) <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    
    cJSON *json = cJSON_Parse(buf);
    if(json) {
        cJSON *s = cJSON_GetObjectItem(json, "ssid");
        cJSON *p = cJSON_GetObjectItem(json, "pass");
        if(s && p) {
            nvs_handle_t my_handle;
            nvs_open("storage", NVS_READWRITE, &my_handle);
            nvs_set_str(my_handle, "wifi_ssid", s->valuestring);
            nvs_set_str(my_handle, "wifi_pass", p->valuestring);
            nvs_set_u8(my_handle, "force_ap", 0);
            nvs_commit(my_handle);
            nvs_close(my_handle);
        }
        cJSON_Delete(json);
        httpd_resp_sendstr(req, "OK");
        xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    }
    return ESP_OK;
}

static esp_err_t switch_ap_post_handler(httpd_req_t *req) {
    nvs_handle_t my_handle;
    nvs_open("storage", NVS_READWRITE, &my_handle);
    nvs_set_u8(my_handle, "force_ap", 1);
    nvs_commit(my_handle);
    nvs_close(my_handle);
    httpd_resp_sendstr(req, "OK");
    xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t switch_wifi_post_handler(httpd_req_t *req) {
    nvs_handle_t my_handle;
    nvs_open("storage", NVS_READWRITE, &my_handle);
    nvs_set_u8(my_handle, "force_ap", 0);
    nvs_commit(my_handle);
    nvs_close(my_handle);
    httpd_resp_sendstr(req, "OK");
    xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void start_web_server() {
    if (server == NULL) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.max_uri_handlers = 20;
        config.uri_match_fn = httpd_uri_match_wildcard;
        
        if (httpd_start(&server, &config) == ESP_OK) {
            httpd_register_uri_handler(server, &uri_index);
            httpd_register_uri_handler(server, &uri_scan);
            httpd_register_uri_handler(server, &uri_save);
            httpd_register_uri_handler(server, &uri_ir);
            httpd_register_uri_handler(server, &uri_status);
            httpd_register_uri_handler(server, &uri_timers);
            httpd_register_uri_handler(server, &uri_auto);
            httpd_register_uri_handler(server, &uri_switch_ap);
            httpd_register_uri_handler(server, &uri_switch_wifi);
            httpd_register_uri_handler(server, &uri_cp1);
            httpd_register_uri_handler(server, &uri_cp2);
            httpd_register_uri_handler(server, &uri_cp3);
            httpd_register_uri_handler(server, &uri_fallback);
            
            ESP_LOGI(TAG, "Web Server started successfully on port %d", config.server_port);
        }
    }
}

void stop_web_server() {
    if (server != NULL) {
        ESP_LOGI(TAG, "Stopping Web Server...");
        httpd_stop(server);
        server = NULL;
    }
}