// WiFi connection manager with runtime-configurable credentials (Waveshare P4 build).
//
// Credentials come from NVS (set on-device via the WiFi setup screen) and fall back to
// WIFI_SSID / WIFI_PASS from secrets.h. New credentials are only saved once they actually
// connect, so a typo can't replace a working network.
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

// task-private
static uint32_t attempt_start = 0;
static bool save_on_connect = false;

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

static void do_scan() {
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
  if (!was_connected && cur_ssid[0]) start_connect();
}

// call once before the UI or the task touch anything here
static void init() { if (!mtx) mtx = xSemaphoreCreateMutex(); }

static void begin() {
  WiFi.persistent(false);   // we keep credentials in our own NVS namespace
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  Preferences p;
  p.begin("wifi", true);
  String s = p.getString("ssid", ""), pw = p.getString("pass", "");
  p.end();
  if (s.length()) {
    strlcpy(cur_ssid, s.c_str(), sizeof cur_ssid);
    strlcpy(cur_pass, pw.c_str(), sizeof cur_pass);
    Serial.printf("[wifi] using saved network '%s'\n", cur_ssid);
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
      Preferences p;
      p.begin("wifi", false);
      p.putString("ssid", cur_ssid);
      p.putString("pass", cur_pass);
      p.end();
      Serial.printf("[wifi] saved '%s'\n", cur_ssid);
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
    // retry, but keep the FAILED state (and reason) visible until it actually connects
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    WiFi.begin(cur_ssid, cur_pass);
    attempt_start = millis();
  }
  return false;
}

}  // namespace net
