// rover_web.h - manual control app for Rover-Bot.
// The robot serves a small web page on port 8080. Open it from a phone or PC browser
// on the same Wi-Fi: http://<robot-ip>:8080  (the robot shows the address on its screen
// a few seconds after Wi-Fi connects; you can also ask "what is your control address?").
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "config.h"
#include "rover_eyes.h"
#include "rover_robot.h"

#ifndef ROVER_WEB_PIN
#define ROVER_WEB_PIN "1234"  // change in config.h; sent with every request
#endif
#ifndef ROVER_WEB_PORT
#define ROVER_WEB_PORT 8080
#endif

static const char kRoverWebPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Rover-Bot Control</title>
<style>
:root{--bg:#10141a;--card:#1a212b;--fg:#e8eef6;--mut:#8b97a6;--acc:#3b9cff;--red:#e5484d;--ok:#3ecf8e}
*{box-sizing:border-box;touch-action:manipulation;-webkit-user-select:none;user-select:none}
body{margin:0;background:var(--bg);color:var(--fg);font:15px system-ui,sans-serif;padding:10px;max-width:560px;margin:auto}
h1{font-size:18px;margin:6px 0 10px;display:flex;justify-content:space-between;align-items:center}
.dot{width:12px;height:12px;border-radius:50%;background:var(--red)} .dot.on{background:var(--ok)}
.card{background:var(--card);border-radius:14px;padding:12px;margin-bottom:10px}
.card h2{font-size:13px;color:var(--mut);margin:0 0 8px;text-transform:uppercase;letter-spacing:.06em}
.pad{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
button{background:#263040;color:var(--fg);border:0;border-radius:12px;padding:14px 6px;font-size:16px}
button:active,button.hold{background:var(--acc)}
.stop{background:var(--red);font-weight:700}
.pad button,.tank input{touch-action:none}
.row{display:flex;gap:8px;flex-wrap:wrap} .row button{flex:1;min-width:30%;padding:11px 4px;font-size:14px}
label{display:flex;justify-content:space-between;color:var(--mut);font-size:13px;margin-top:6px}
input[type=range]{width:100%;height:34px}
.tank{display:flex;justify-content:space-around;gap:16px}
.tank div{text-align:center;flex:1}
.tank input{writing-mode:vertical-lr;direction:rtl;height:150px;width:44px}
input[type=text],select,input[type=number]{background:#0d1117;color:var(--fg);border:1px solid #2c3746;border-radius:8px;padding:8px;width:100%}
small{color:var(--mut)}
</style></head><body>
<h1>Rover-Bot <span id="st" class="dot"></span></h1>

<div class="card"><h2>Emergency</h2><button class="stop" style="width:100%" onclick="stopAll()">STOP</button></div>

<div class="card"><h2>Drive (hold a button)</h2>
<div class="pad">
<button data-d="1,0.4">&#8598;</button><button data-d="1,1">&#9650;</button><button data-d="0.4,1">&#8599;</button>
<button data-d="-1,1">&#9664;</button><button class="stop" onclick="stopAll()">&#9632;</button><button data-d="1,-1">&#9654;</button>
<button data-d="-1,-0.4">&#8601;</button><button data-d="-1,-1">&#9660;</button><button data-d="-0.4,-1">&#8600;</button>
</div>
<label>Speed <b id="spv">70</b>%</label><input id="sp" type="range" min="30" max="100" value="70">
<small>Hold = keeps driving, release = stops. Keyboard: W A S D / arrows, space = stop</small></div>

<div class="card"><h2>Motors separately (tank sliders, spring back to 0)</h2>
<div class="tank">
<div><b>Left motor</b><br><input id="ml" type="range" min="-100" max="100" value="0" orient="vertical"><br><span id="mlv">0</span></div>
<div><b>Right motor</b><br><input id="mr" type="range" min="-100" max="100" value="0" orient="vertical"><br><span id="mrv">0</span></div>
</div></div>

<div class="card"><h2>Servos</h2>
<label>Head <b id="hv">90</b>&deg; <small>45-135</small></label><input id="h" type="range" min="45" max="135" value="90">
<label>Left arm <b id="lav">10</b>&deg; <small>0 down - 180 up</small></label><input id="la" type="range" min="0" max="180" value="10">
<label>Right arm <b id="rav">10</b>&deg;</label><input id="ra" type="range" min="0" max="180" value="10">
<div class="row" style="margin-top:8px">
<button onclick="pose(90,10,10)">Rest</button><button onclick="pose(90,90,90)">Arms forward</button><button onclick="pose(90,170,170)">Arms up</button>
</div></div>

<div class="card"><h2>Actions</h2>
<div class="row">
<button onclick="g('wave')">Wave</button><button onclick="g('nod')">Nod</button><button onclick="g('shake_head')">Shake</button>
<button onclick="g('happy')">Happy</button><button onclick="g('cheer')">Cheer</button><button onclick="g('dance')">Dance</button>
<button onclick="g('hug')">Hug</button><button onclick="g('think')">Think</button><button onclick="g('sad')">Sad</button>
<button onclick="api('pickup')">Pick up</button><button onclick="api('putdown')">Put down</button><button onclick="api('rest')">Rest pose</button>
</div></div>

<div class="card"><h2>Eyes / screen</h2>
<div class="row">
<button onclick="f('happy')">Happy</button><button onclick="f('sad')">Sad</button><button onclick="f('angry')">Angry</button>
<button onclick="f('surprised')">Surprised</button><button onclick="f('sleepy')">Sleepy</button><button onclick="f('loving')">Love</button>
<button onclick="f('winking')">Wink</button><button onclick="f('thinking')">Think</button><button onclick="f('neutral')">Normal</button>
<button onclick="f('cam')">Camera view</button><button onclick="f('camoff')">Eyes</button><button onclick="f('clock')">Clock</button><button onclick="f('ip')">Show IP</button>
</div></div>

<div class="card"><h2>Setup</h2>
<label>Setting</label>
<select id="cs"><option>motor_left_invert</option><option>motor_right_invert</option><option>swap_motors</option>
<option>left_arm_invert</option><option>right_arm_invert</option><option>head_trim</option><option>left_arm_trim</option>
<option>right_arm_trim</option><option>turn_ms_per_90deg</option><option>cm_per_sec</option></select>
<label>Value (0/1 for invert and swap, degrees for trim)</label><input id="cv" type="number" value="1">
<div class="row" style="margin-top:8px"><button onclick="calib()">Save setting</button></div>
<label style="margin-top:12px">PIN</label><input id="pin" type="text" inputmode="numeric" placeholder="PIN">
<small id="msg"></small></div>

<script>
const $=id=>document.getElementById(id);
let pin=localStorage.getItem('rpin')||'1234'; $('pin').value=pin;
$('pin').onchange=()=>{pin=$('pin').value;localStorage.setItem('rpin',pin);poll()};
function setOk(ok){$('st').className='dot'+(ok?' on':'')}
async function api(p){
  try{const r=await fetch('/api/'+p+(p.includes('?')?'&':'?')+'k='+encodeURIComponent(pin),{cache:'no-store'});
    if(r.status==403){$('msg').textContent='Wrong PIN';setOk(false);return null}
    setOk(true);$('msg').textContent='';return await r.text()}
  catch(e){setOk(false);return null}
}
let spd=70; $('sp').oninput=e=>{spd=+e.target.value;$('spv').textContent=spd};
let tm=null,cur=[0,0];
function send(){api(`drive?l=${cur[0]}&r=${cur[1]}&ms=350`)}
function hold(l,r){cur=[Math.round(l*spd),Math.round(r*spd)];send();if(!tm)tm=setInterval(send,150)}
function release(){clearInterval(tm);tm=null;cur=[0,0];api('stop')}
function stopAll(){release();$('ml').value=0;$('mr').value=0;$('mlv').textContent=0;$('mrv').textContent=0;api('stop')}
document.querySelectorAll('[data-d]').forEach(b=>{
  const [l,r]=b.dataset.d.split(',').map(Number);
  b.addEventListener('pointerdown',e=>{e.preventDefault();try{b.setPointerCapture(e.pointerId)}catch(_){ }b.classList.add('hold');hold(l,r)});
  ['pointerup','pointercancel','lostpointercapture'].forEach(ev=>b.addEventListener(ev,()=>{if(b.classList.contains('hold')){b.classList.remove('hold');release()}}));
});
const keys={w:[1,1],arrowup:[1,1],s:[-1,-1],arrowdown:[-1,-1],a:[-1,1],arrowleft:[-1,1],d:[1,-1],arrowright:[1,-1]};
let kd=null;
addEventListener('keydown',e=>{const k=e.key.toLowerCase();if(k==' '){stopAll();return}
  if(keys[k]&&kd!==k){kd=k;hold(...keys[k])}});
addEventListener('keyup',e=>{if(kd===e.key.toLowerCase()){kd=null;release()}});
function tank(){const l=+$('ml').value,r=+$('mr').value;$('mlv').textContent=l;$('mrv').textContent=r;
  cur=[l,r];if((l||r)&&!tm)tm=setInterval(send,150);if(!l&&!r){clearInterval(tm);tm=null;api('stop')}else send()}
['ml','mr'].forEach(id=>{const el=$(id);el.oninput=tank;
  ['pointerup','touchend','mouseup'].forEach(ev=>el.addEventListener(ev,()=>{el.value=0;tank()}))});
let last=0;
function servo(id,key){const el=$(id);el.oninput=()=>{$(id+'v').textContent=el.value;const n=Date.now();
  if(n-last>60){last=n;api(`servo?${key}=${el.value}&sp=300`)}};
  el.onchange=()=>api(`servo?${key}=${el.value}&sp=300`)}
servo('h','h');servo('la','la');servo('ra','ra');
function pose(h,l,r){[['h',h],['la',l],['ra',r]].forEach(([i,v])=>{$(i).value=v;$(i+'v').textContent=v});api(`servo?h=${h}&la=${l}&ra=${r}&sp=150`)}
function g(n){api('gesture?n='+n)}
function f(m){api('face?m='+m)}
async function calib(){const t=await api(`calib?s=${$('cs').value}&v=${$('cv').value}`);$('msg').textContent=t||'failed'}
async function poll(){const t=await api('status');if(t){try{const s=JSON.parse(t);
  if(!document.activeElement||!['h','la','ra'].includes(document.activeElement.id)){
   [['h',s.head],['la',s.left_arm],['ra',s.right_arm]].forEach(([i,v])=>{$(i).value=v;$(i+'v').textContent=v})}}catch(e){}}}
poll();setInterval(poll,2500);
addEventListener('blur',stopAll);document.addEventListener('visibilitychange',()=>{if(document.hidden)stopAll()});
</script></body></html>)HTML";

class RoverWeb {
public:
    RoverWeb(RoverRobot* robot, RoverEyes* eyes) : robot_(robot), eyes_(eyes) {}

    // Starts the web server as soon as the robot has a Wi-Fi IP address.
    void Start() { xTaskCreate(&RoverWeb::WaitThunk, "rover_web", 4096, this, 3, nullptr); }

    // "http://192.168.x.y:8080", or "" when Wi-Fi is not connected yet.
    static std::string CurrentUrl() {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!netif) return "";
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) return "";
        char buf[40];
        snprintf(buf, sizeof(buf), "http://" IPSTR ":%d", IP2STR(&ip.ip), ROVER_WEB_PORT);
        return buf;
    }

private:
    RoverRobot* robot_;
    RoverEyes* eyes_;
    httpd_handle_t server_ = nullptr;
    int64_t last_servo_ms_ = 0;

    static void WaitThunk(void* arg) {
        auto* self = static_cast<RoverWeb*>(arg);
        std::string url;
        while ((url = CurrentUrl()).empty()) vTaskDelay(pdMS_TO_TICKS(2000));
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (self->StartServer() && self->eyes_) {
            self->eyes_->SetAddress(url);
            self->eyes_->ShowAddress(url, 15000);  // centred on the screen
        }
        vTaskDelete(nullptr);
    }

    bool StartServer() {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = ROVER_WEB_PORT;
        config.ctrl_port = 32770;
        config.max_uri_handlers = 12;
        config.stack_size = 6144;
        config.lru_purge_enable = true;
        if (httpd_start(&server_, &config) != ESP_OK) {
            ESP_LOGE("RoverWeb", "web server failed to start");
            return false;
        }
        Add("/", &RoverWeb::HandleRoot);
        Add("/api/drive", &RoverWeb::HandleDrive);
        Add("/api/stop", &RoverWeb::HandleStop);
        Add("/api/servo", &RoverWeb::HandleServo);
        Add("/api/gesture", &RoverWeb::HandleGesture);
        Add("/api/pickup", &RoverWeb::HandlePickup);
        Add("/api/putdown", &RoverWeb::HandlePutDown);
        Add("/api/rest", &RoverWeb::HandleRest);
        Add("/api/status", &RoverWeb::HandleStatus);
        Add("/api/calib", &RoverWeb::HandleCalib);
        Add("/api/face", &RoverWeb::HandleFace);
        ESP_LOGI("RoverWeb", "control page on port %d", ROVER_WEB_PORT);
        return true;
    }

    void Add(const char* uri, esp_err_t (*fn)(httpd_req_t*)) {
        httpd_uri_t u = {};
        u.uri = uri;
        u.method = HTTP_GET;
        u.handler = fn;
        u.user_ctx = this;
        httpd_register_uri_handler(server_, &u);
    }

    // ---- helpers
    static bool Query(httpd_req_t* req, const char* key, char* out, size_t n) {
        char q[192];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
        return httpd_query_key_value(q, key, out, n) == ESP_OK;
    }
    static int QueryInt(httpd_req_t* req, const char* key, int def) {
        char v[16];
        return Query(req, key, v, sizeof(v)) ? atoi(v) : def;
    }
    static bool Auth(httpd_req_t* req) {
        char k[16];
        if (Query(req, "k", k, sizeof(k)) && strcmp(k, ROVER_WEB_PIN) == 0) return true;
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "wrong PIN");
        return false;
    }
    static esp_err_t Ok(httpd_req_t* req, const char* body = "ok") {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    }
    static RoverWeb* Self(httpd_req_t* req) { return static_cast<RoverWeb*>(req->user_ctx); }

    // ---- handlers
    static esp_err_t HandleRoot(httpd_req_t* req) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, kRoverWebPage, HTTPD_RESP_USE_STRLEN);
    }

    static esp_err_t HandleDrive(httpd_req_t* req) {
        if (!Auth(req)) return ESP_OK;
        int l = QueryInt(req, "l", 0), r = QueryInt(req, "r", 0), ms = QueryInt(req, "ms", 300);
        Self(req)->robot_->Drive(l, r, std::min(ms, 600));  // dead-man: keeps going only while the page keeps sending
        return Ok(req);
    }

    static esp_err_t HandleStop(httpd_req_t* req) {
        if (!Auth(req)) return ESP_OK;
        Self(req)->robot_->Stop();
        return Ok(req);
    }

    static esp_err_t HandleServo(httpd_req_t* req) {
        if (!Auth(req)) return ESP_OK;
        auto* s = Self(req);
        s->robot_->NoteManualControl();
        s->robot_->SetPose(QueryInt(req, "h", -1), QueryInt(req, "la", -1), QueryInt(req, "ra", -1),
                           QueryInt(req, "sp", 200), false);
        return Ok(req);
    }

    static esp_err_t HandleGesture(httpd_req_t* req) {
        if (!Auth(req)) return ESP_OK;
        char n[24];
        if (!Query(req, "n", n, sizeof(n))) return Ok(req, "no name");
        int g = RoverRobot::GestureFromName(n);
        if (g < 0) return Ok(req, "unknown");
        Self(req)->robot_->NoteManualControl();
        Self(req)->robot_->PlayGesture(g, false);
        return Ok(req);
    }

    static void PickThunk(void* arg) {
        auto* robot = static_cast<RoverRobot*>(arg);
        robot->PickUp();
        vTaskDelete(nullptr);
    }
    static void PutThunk(void* arg) {
        auto* robot = static_cast<RoverRobot*>(arg);
        robot->PutDown();
        vTaskDelete(nullptr);
    }

    static esp_err_t HandlePickup(httpd_req_t* req) {
        if (!Auth(req)) return ESP_OK;
        Self(req)->robot_->NoteManualControl();
        xTaskCreate(&RoverWeb::PickThunk, "web_pick", 4096, Self(req)->robot_, 3, nullptr);
        return Ok(req);
    }

    static esp_err_t HandlePutDown(httpd_req_t* req) {
        if (!Auth(req)) return ESP_OK;
        Self(req)->robot_->NoteManualControl();
        xTaskCreate(&RoverWeb::PutThunk, "web_put", 4096, Self(req)->robot_, 3, nullptr);
        return Ok(req);
    }

    static esp_err_t HandleRest(httpd_req_t* req) {
        if (!Auth(req)) return ESP_OK;
        Self(req)->robot_->NoteManualControl();
        Self(req)->robot_->GoRest();
        return Ok(req);
    }

    static esp_err_t HandleStatus(httpd_req_t* req) {
        if (!Auth(req)) return ESP_OK;
        httpd_resp_set_type(req, "application/json");
        std::string j = Self(req)->robot_->StatusJson();
        return httpd_resp_send(req, j.c_str(), HTTPD_RESP_USE_STRLEN);
    }

    static esp_err_t HandleFace(httpd_req_t* req) {
        if (!Auth(req)) return ESP_OK;
        char m[24];
        auto* s = Self(req);
        if (!s->eyes_ || !Query(req, "m", m, sizeof(m))) return Ok(req, "no screen");
        if (strcmp(m, "cam") == 0) s->eyes_->ShowCamera(0);
        else if (strcmp(m, "camoff") == 0) s->eyes_->HideCamera();
        else if (strcmp(m, "clock") == 0) s->eyes_->ShowClock(8000);
        else if (strcmp(m, "ip") == 0) s->eyes_->ShowAddress(CurrentUrl(), 8000);
        else s->eyes_->SetEmotionByName(m, 5000);
        return Ok(req);
    }

    static esp_err_t HandleCalib(httpd_req_t* req) {
        if (!Auth(req)) return ESP_OK;
        char s[32];
        if (!Query(req, "s", s, sizeof(s))) return Ok(req, "no setting");
        bool ok = Self(req)->robot_->SetCalibration(s, QueryInt(req, "v", 0));
        return Ok(req, ok ? "saved" : "unknown setting");
    }
};
