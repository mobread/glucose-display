// Captive-portal WiFi setup: the device opens its own access point ("Glucose-XXXX"), answers
// every DNS query with its own address so phones pop up their sign-in sheet, and serves a page
// to pick a network and enter the password. Credentials go through net::request_connect(), so
// they are only saved (to NVS, alongside the other saved networks) once they actually connect.
//
// Starts on its own when there is no network configured, or when WiFi has been down for
// PORTAL_AFTER_MS; can also be started/stopped from a button (request_start/request_stop).
// Runs in its own task so the page stays responsive while the HA task blocks on HTTP.
// Enable per board with WIFI_PORTAL 1.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "wifi_mgr.h"

#ifndef PORTAL_AFTER_MS
#define PORTAL_AFTER_MS 45000   // > one connect attempt (20 s) + a retry
#endif

namespace portal {

enum Phase { OFF, WAITING, CONNECTING, FAILED, DONE };

static volatile bool start_req = false, stop_req = false;
static volatile Phase phase = OFF;
static char ap_name[24] = "";
static char submitted[33] = "";   // network chosen on the page
static bool auto_started = false;

static WebServer *server = nullptr;
static DNSServer *dns = nullptr;
static const IPAddress AP_IP(192, 168, 4, 1);

// ---- main loop side ----
static void request_start() { start_req = true; }
static void request_stop() { stop_req = true; }
static bool active() { return phase != OFF; }

// ---------- page ----------
static String html_escape(const char *s) {
  String o;
  for (; *s; s++) {
    switch (*s) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      case '\\': o += "&#92;"; break;   // also keeps /status valid JSON
      default: o += *s;
    }
  }
  return o;
}

static const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Glucose display WiFi</title>
<style>
body{font-family:-apple-system,system-ui,sans-serif;background:#0b0f14;color:#e6edf3;margin:0;padding:20px;max-width:440px}
h1{font-size:20px;margin:0 0 4px}p{color:#7d8590;margin:0 0 16px;font-size:14px}
.net{display:flex;justify-content:space-between;width:100%;padding:12px;margin:0 0 6px;background:#161c24;border:1px solid #232b36;border-radius:8px;color:#e6edf3;font-size:16px;text-align:left}
.net.sel{border-color:#58a6ff}.net small{color:#7d8590}
label{display:block;font-size:13px;color:#7d8590;margin:14px 0 4px}
input{width:100%;box-sizing:border-box;padding:12px;font-size:16px;background:#161c24;border:1px solid #232b36;border-radius:8px;color:#e6edf3}
.row{display:flex;gap:8px}.row input{flex:1}
button.go{width:100%;margin-top:18px;padding:14px;font-size:17px;background:#58a6ff;color:#0b0f14;border:0;border-radius:8px;font-weight:600}
button.sm{padding:0 14px;background:#161c24;color:#e6edf3;border:1px solid #232b36;border-radius:8px}
a{color:#58a6ff}#st{margin-top:16px;font-size:15px}
</style></head><body>
)HTML";

static void send_page() {
  String h = FPSTR(PAGE_HEAD);
  h += "<h1>Glucose display WiFi</h1><p>Pick the network the display should use.</p><div id=nets>";
  net::Network nets[net::MAX_NETWORKS];
  uint32_t gen = 0;
  int n = net::get_networks(nets, gen);
  if (n <= 0 && net::is_scanning()) h += "<p>Scanning&hellip;</p>";
  else if (n <= 0) h += "<p>No networks found.</p>";
  for (int i = 0; i < n; i++) {
    String e = html_escape(nets[i].ssid);
    int bars = nets[i].rssi > -60 ? 4 : nets[i].rssi > -70 ? 3 : nets[i].rssi > -80 ? 2 : 1;
    h += "<button class=net type=button data-s=\"" + e + "\" onclick=\"pick(this)\"><span>" + e +
         "</span><small>" + String(nets[i].secure ? "&#128274; " : "") + String("&#9602;&#9604;&#9606;&#9608;").substring(0, bars * 7) +
         "</small></button>";
  }
  h += R"HTML(</div><a href="/scan">Scan again</a>
<form method=post action="/connect" onsubmit="document.getElementById('st').textContent='Connecting...'">
<label>Network name</label><input name=ssid id=ssid autocapitalize=none autocorrect=off required maxlength=32>
<label>Password</label><div class=row><input name=pass id=pass type=password maxlength=64>
<button class=sm type=button onclick="var p=document.getElementById('pass');p.type=p.type=='password'?'text':'password'">Show</button></div>
<button class=go type=submit>Connect</button></form><div id=st></div>
<script>function pick(b){document.querySelectorAll('.net').forEach(x=>x.classList.remove('sel'));b.classList.add('sel');
document.getElementById('ssid').value=b.dataset.s;document.getElementById('pass').focus()}</script></body></html>)HTML";
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "text/html", h);
}

static void send_connecting() {
  String h = FPSTR(PAGE_HEAD);
  h += "<h1>Connecting&hellip;</h1><p id=msg>Joining <b>" + html_escape(submitted) + "</b>. This takes up to 20 seconds.</p>";
  h += R"HTML(<p><a href="/">Back</a></p><script>
function poll(){fetch('/status').then(r=>r.json()).then(s=>{var m=document.getElementById('msg');
if(s.state=='connected'){m.innerHTML='Connected to <b>'+s.ssid+'</b> ('+s.ip+'). The display will close this hotspot shortly - you can close this page.';return}
if(s.state=='failed'){m.innerHTML='Could not join <b>'+s.ssid+'</b>: '+s.reason+'. <a href="/">Try again</a>';return}
setTimeout(poll,1500)}).catch(()=>setTimeout(poll,2000))}setTimeout(poll,1500)</script></body></html>)HTML";
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "text/html", h);
}

static void handle_connect() {
  String ssid = server->arg("ssid"), pass = server->arg("pass");
  ssid.trim();
  if (!ssid.length()) { server->sendHeader("Location", "/"); server->send(302); return; }
  strlcpy(submitted, ssid.c_str(), sizeof submitted);
  Serial.printf("[portal] connect requested: '%s'\n", submitted);
  net::request_connect(submitted, pass.c_str());
  phase = CONNECTING;
  send_connecting();
}

static void handle_status() {
  char ssid[33], reason[32];
  net::State st = net::get_state(ssid, sizeof ssid, reason, sizeof reason);
  const char *s = st == net::CONNECTED ? "connected" : st == net::FAILED ? "failed" : "connecting";
  String j = String("{\"state\":\"") + s + "\",\"ssid\":\"" + html_escape(ssid) + "\",\"reason\":\"" + reason +
             "\",\"ip\":\"" + (st == net::CONNECTED ? WiFi.localIP().toString() : String("")) + "\"}";
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "application/json", j);
}

// Any other URL (phone OS connectivity checks, typed addresses) -> the setup page. A redirect
// instead of the expected "success" response is what makes iOS/Android show the sign-in sheet.
static void handle_redirect() {
  server->sendHeader("Location", String("http://") + AP_IP.toString() + "/");
  server->send(302, "text/plain", "");
}

// ---------- start / stop (portal task) ----------
static void start(bool automatic) {
  if (phase != OFF) return;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(ap_name, sizeof ap_name, "Glucose-%02X%02X", mac[4], mac[5]);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(ap_name);   // open network: only up while the display needs setting up
  if (!dns) dns = new DNSServer();
  dns->setErrorReplyCode(DNSReplyCode::NoError);
  dns->start(53, "*", AP_IP);
  if (!server) {
    server = new WebServer(80);
    server->on("/", HTTP_GET, send_page);
    server->on("/scan", HTTP_GET, [] { net::request_scan(); server->sendHeader("Location", "/"); server->send(302); });
    server->on("/connect", HTTP_POST, handle_connect);
    server->on("/status", HTTP_GET, handle_status);
    server->onNotFound(handle_redirect);
  }
  server->begin();
  net::hold_retries = true;   // don't scan / hop networks under the phone's feet
  net::request_scan();
  submitted[0] = 0;
  auto_started = automatic;
  phase = WAITING;
  Serial.printf("[portal] started: join '%s', then open http://%s (heap free %u, min %u)\n", ap_name,
                AP_IP.toString().c_str(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
}

static void stop() {
  if (phase == OFF) return;
  server->stop();
  dns->stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  net::hold_retries = false;
  phase = OFF;
  Serial.printf("[portal] stopped (heap free %u, min %u)\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
}

static void task(void *) {
  uint32_t offline_since = millis(), connected_since = 0;
  for (;;) {
    char ssid[33];
    net::State st = net::get_state(ssid, sizeof ssid);
    bool connected = st == net::CONNECTED;
    uint32_t now = millis();
    if (connected) { offline_since = 0; if (!connected_since) connected_since = now; }
    else { connected_since = 0; if (!offline_since) offline_since = now; }

    if (stop_req) { stop_req = false; stop(); }
    if (start_req) { start_req = false; start(false); }

    if (phase == OFF) {
      if (st == net::NO_CONFIG || (!connected && now - offline_since > PORTAL_AFTER_MS)) start(true);
    } else {
      server->handleClient();
      dns->processNextRequest();
      if (phase == CONNECTING && st == net::FAILED && !strcmp(ssid, submitted)) phase = FAILED;
      if (phase == FAILED && st == net::CONNECTING) phase = CONNECTING;
      if (connected && (submitted[0] ? !strcmp(ssid, submitted) : auto_started)) phase = DONE;
      // close 20 s after connecting, so the phone's page can show the result first
      if (phase == DONE && connected_since && now - connected_since > 20000) stop();
    }
    vTaskDelay(pdMS_TO_TICKS(phase == OFF ? 250 : 5));
  }
}

static void begin() {
  xTaskCreatePinnedToCore(task, "portal", 8192, nullptr, 1, nullptr, 0);
}

}  // namespace portal
