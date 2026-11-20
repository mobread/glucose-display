// WiFi connection manager with runtime-configurable credentials (Waveshare P4 build).
//
// Remembers up to MAX_KNOWN networks (SSID + password) in NVS, most recently used first; new ones
// are only saved once they actually connect, so a typo can't replace a working network.
// WIFI_SSID / WIFI_PASS from secrets.h are the fallback when nothing is saved.
// Boot joins the most recent saved network; if the current network can't be joined (and the user
// didn't just pick it), the strongest visible saved network is tried instead.
//
// All WiFi calls happen on the HA task (net::service()); the UI talks to it through
// request_scan() / request_connect() and reads state/scan results back, all under a mutex.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "secrets.h"

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

namespace net {

enum State { NO_CONFIG, CONNECTING, CONNECTED, FAILED };

struct Network { char ssid[33]; int8_t rssi; bool secure; };
static const int MAX_NETWORKS = 30;

static SemaphoreHandle_t mtx = nullptr;
static State state = CONNECTING;   // until begin() knows better (keeps the UI from auto-opening setup at boot)
static char cur_ssid[33] = "", cur_pass[65] = "";
static char fail_reason[32] = "";
static bool ever_connected = false;

// UI -> task requests
static bool scan_requested = false;
static bool connect_requested = false;
static char req_ssid[33], req_pass[65];

// task -> UI scan results
static Network networks[MAX_NETWORKS];
static int network_count = 0;
static uint32_t scan_generation = 0;
static bool scanning = false;

// saved networks (shared: UI reads passwords for prefill, task adds on connect)
struct Known { char ssid[33]; char pass[65]; };
static const int MAX_KNOWN = 8;
static Known known[MAX_KNOWN];
static int known_count = 0;

// task-private
static uint32_t attempt_start = 0;
static bool save_on_connect = false;   // user picked this network: remember it once it connects

struct Lock {
  Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mtx); }
};

// ---------- UI side ----------
static void request_scan() { Lock l; scan_requested = true; scanning = true; }

static void request_connect(const char *ssid, const char *pass) {
  Lock l;
  strlcpy(req_ssid, ssid, sizeof req_ssid);
  strlcpy(req_pass, pass, sizeof req_pass);
  connect_requested = true;
  // reflect the switch immediately, so the UI doesn't report the *old* network as connected
  // before the task has picked the request up (cur_ssid is only ever written by the task)
  state = CONNECTING;
  fail_reason[0] = 0;
}

static State get_state(char *ssid_out = nullptr, size_t n = 0, char *reason_out = nullptr, size_t rn = 0) {
  Lock l;
  if (ssid_out) strlcpy(ssid_out, connect_requested ? req_ssid : cur_ssid, n);
  if (reason_out) strlcpy(reason_out, fail_reason, rn);
  return state;
}

static bool has_ever_connected() { Lock l; return ever_connected; }
static bool is_scanning() { Lock l; return scanning; }

// copies results if they changed since `gen`; returns count or -1 if unchanged
static int get_networks(Network *out, uint32_t &gen) {
  Lock l;
  if (gen == scan_generation) return -1;
  gen = scan_generation;
  memcpy(out, networks, sizeof(Network) * network_count);
  return network_count;
}

// ---------- saved networks ----------
static void store_known() {   // caller holds the lock
  Preferences p;
  p.begin("wifi", false);
  p.putUChar("n", known_count);
  char k[4];
  for (int i = 0; i < known_count; i++) {
    snprintf(k, sizeof k, "s%d", i); p.putString(k, known[i].ssid);
    snprintf(k, sizeof k, "p%d", i); p.putString(k, known[i].pass);
  }
  p.end();
}

static void load_known() {
  Preferences p;
  p.begin("wifi", true);
  known_count = min((int)p.getUChar("n", 0), MAX_KNOWN);
  char k[4];
  for (int i = 0; i < known_count; i++) {
    snprintf(k, sizeof k, "s%d", i); strlcpy(known[i].ssid, p.getString(k, "").c_str(), sizeof known[i].ssid);
    snprintf(k, sizeof k, "p%d", i); strlcpy(known[i].pass, p.getString(k, "").c_str(), sizeof known[i].pass);
  }
  String legacy = known_count ? String() : p.getString("ssid", "");   // single-network format from before
  String legacy_pass = legacy.length() ? p.getString("pass", "") : String();
  p.end();
  if (legacy.length()) {
    strlcpy(known[0].ssid, legacy.c_str(), sizeof known[0].ssid);
    strlcpy(known[0].pass, legacy_pass.c_str(), sizeof known[0].pass);
    known_count = 1;
    store_known();
  }
}

static int find_known(const char *ssid) {   // caller holds the lock
  for (int i = 0; i < known_count; i++) if (!strcmp(known[i].ssid, ssid)) return i;
  return -1;
}
static int find_known_locked(const char *ssid) { Lock l; return find_known(ssid); }

// move/insert at the front (most recent); persists only when something changed
static void remember(const char *ssid, const char *pass) {
  Lock l;
  int i = find_known(ssid);
  if (i == 0 && !strcmp(known[0].pass, pass)) return;
  if (i < 0) i = known_count < MAX_KNOWN ? known_count++ : MAX_KNOWN - 1;   // full: drop the oldest
  for (; i > 0; i--) known[i] = known[i - 1];
  strlcpy(known[0].ssid, ssid, sizeof known[0].ssid);
  strlcpy(known[0].pass, pass, sizeof known[0].pass);
  store_known();
}

// UI side
static bool known_password(const char *ssid, char *out, size_t n) {
  Lock l;
  int i = find_known(ssid);
  if (i < 0) return false;
  strlcpy(out, known[i].pass, n);
  return true;
}

static bool forget(const char *ssid) {
  Lock l;
  int i = find_known(ssid);
  if (i < 0) return false;
  for (; i < known_count - 1; i++) known[i] = known[i + 1];
  known_count--;
  store_known();
  return true;
}

// ---------- task side ----------
static void set_state(State s, const char *reason = "") {
  Lock l;
  state = s;
  strlcpy(fail_reason, reason, sizeof fail_reason);
  if (s == CONNECTED) ever_connected = true;
}

static void start_connect() {
  WiFi.disconnect();
  vTaskDelay(pdMS_TO_TICKS(100));
  Serial.printf("[wifi] connecting to '%s'\n", cur_ssid);
  WiFi.begin(cur_ssid, cur_pass);
  attempt_start = millis();
  set_state(CONNECTING);
}

static int do_scan(bool reconnect = true) {
  // scanning can't run while a connect attempt is in flight; a connected STA can scan fine
  bool was_connected = WiFi.status() == WL_CONNECTED;
  if (!was_connected) { WiFi.disconnect(); vTaskDelay(pdMS_TO_TICKS(200)); }
  int n = WiFi.scanNetworks();
  Network tmp[MAX_NETWORKS];
  int count = 0;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;   // hidden networks: type the name manually
    int8_t rssi = WiFi.RSSI(i);
    int j = 0;
    for (; j < count; j++) if (s == tmp[j].ssid) break;   // same SSID on several APs/bands
    if (j < count) { if (rssi > tmp[j].rssi) tmp[j].rssi = rssi; continue; }
    if (count >= MAX_NETWORKS) continue;
    strlcpy(tmp[count].ssid, s.c_str(), sizeof tmp[count].ssid);
    tmp[count].rssi = rssi;
    tmp[count].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    count++;
  }
  WiFi.scanDelete();
  // strongest first
  for (int a = 1; a < count; a++)
    for (int b = a; b > 0 && tmp[b].rssi > tmp[b - 1].rssi; b--) { Network t = tmp[b]; tmp[b] = tmp[b - 1]; tmp[b - 1] = t; }
  {
    Lock l;
    memcpy(networks, tmp, sizeof(Network) * count);
    network_count = count;
    scan_generation++;
    scanning = false;
  }
  Serial.printf("[wifi] scan: %d networks\n", count);
  if (reconnect && !was_connected && cur_ssid[0]) start_connect();
  return count;
}

// Current network unreachable: switch to the strongest visible saved network (or the built-in
// fallback). Returns true if it picked a different network than the current one.
static bool pick_best_known() {
  do_scan(false);
  Known best = {};
  int best_rssi = -1000;
  {
    Lock l;
    for (int i = 0; i < network_count; i++) {
      int k = find_known(networks[i].ssid);
      const char *pass = k >= 0 ? known[k].pass
                       : (strlen(WIFI_SSID) && !strcmp(networks[i].ssid, WIFI_SSID)) ? WIFI_PASS : nullptr;
      if (pass && networks[i].rssi > best_rssi) {
        best_rssi = networks[i].rssi;
        strlcpy(best.ssid, networks[i].ssid, sizeof best.ssid);
        strlcpy(best.pass, pass, sizeof best.pass);
      }
    }
  }
  if (!best.ssid[0] || !strcmp(best.ssid, cur_ssid)) return false;
  Serial.printf("[wifi] '%s' unreachable; switching to saved network '%s'\n", cur_ssid, best.ssid);
  {
    Lock l;
    strlcpy(cur_ssid, best.ssid, sizeof cur_ssid);
    strlcpy(cur_pass, best.pass, sizeof cur_pass);
  }
  return true;
}

// call once before the UI or the task touch anything here
static void init() {
  if (mtx) return;
  mtx = xSemaphoreCreateMutex();
  load_known();
}

static void begin() {
  WiFi.persistent(false);   // we keep credentials in our own NVS namespace
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  if (known_count) {
    strlcpy(cur_ssid, known[0].ssid, sizeof cur_ssid);
    strlcpy(cur_pass, known[0].pass, sizeof cur_pass);
    Serial.printf("[wifi] using saved network '%s' (%d saved)\n", cur_ssid, known_count);
  } else if (strlen(WIFI_SSID)) {
    strlcpy(cur_ssid, WIFI_SSID, sizeof cur_ssid);
    strlcpy(cur_pass, WIFI_PASS, sizeof cur_pass);
    Serial.printf("[wifi] using built-in network '%s'\n", cur_ssid);
  }
  if (cur_ssid[0]) start_connect();
  else { set_state(NO_CONFIG); Serial.println("[wifi] no network configured"); }
}

// Call often from the HA task. Handles UI requests, reconnects, and saving. Returns true if connected.
static bool service() {
  bool do_scan_now = false, do_connect_now = false;
  {
    Lock l;
    if (scan_requested) { scan_requested = false; do_scan_now = true; }
    if (connect_requested) {
      connect_requested = false; do_connect_now = true;
      strlcpy(cur_ssid, req_ssid, sizeof cur_ssid);
      strlcpy(cur_pass, req_pass, sizeof cur_pass);
    }
  }
  if (do_connect_now) { save_on_connect = true; start_connect(); }
  if (do_scan_now) do_scan();

  if (!cur_ssid[0]) return false;
  wl_status_t st = WiFi.status();
  // right after a switch the STA can still briefly report the previous association
  if (st == WL_CONNECTED && WiFi.SSID() != cur_ssid) st = WL_DISCONNECTED;
  if (st == WL_CONNECTED) {
    if (state != CONNECTED) {
      set_state(CONNECTED);
      Serial.printf("[wifi] connected to '%s' ip=%s gw=%s dns=%s\n", cur_ssid, WiFi.localIP().toString().c_str(),
                    WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
    }
    if (save_on_connect) {
      save_on_connect = false;
      remember(cur_ssid, cur_pass);
      Serial.printf("[wifi] saved '%s'\n", cur_ssid);
    } else if (find_known_locked(cur_ssid) > 0) {
      remember(cur_ssid, cur_pass);   // auto-joined a saved network: make it the most recent
    }
    return true;
  }

  if (state == CONNECTED) start_connect();   // dropped: auto-reconnect is on, but restart the timer/state
  if (millis() - attempt_start > 20000) {
    const char *why = st == WL_NO_SSID_AVAIL ? "network not found"
                    : st == WL_CONNECT_FAILED ? "wrong password?"
                    : "couldn't connect";
    if (state != FAILED) Serial.printf("[wifi] '%s': %s (status %d)\n", cur_ssid, why, (int)st);
    set_state(FAILED, why);
    // A network the user just picked keeps being retried (they see the error on the setup screen).
    // Otherwise fall over to the strongest visible saved network, else retry the same one.
    // Either way keep the FAILED state (and reason) visible until something actually connects.
    if (!save_on_connect) pick_best_known();
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    Serial.printf("[wifi] retrying '%s'\n", cur_ssid);
    WiFi.begin(cur_ssid, cur_pass);
    attempt_start = millis();
  }
  return false;
}

}  // namespace net
