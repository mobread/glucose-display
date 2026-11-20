// Optional WireGuard tunnel back to the home network, so the panel can reach Home Assistant's
// plain-HTTP LAN address from anywhere. Enabled by defining WG_ENABLE 1 in secrets.h.
//
// Sequence (driven from the HA task): WiFi up -> NTP synced (handshakes need correct time) ->
// wg::start() -> tunnel becomes the default route -> HA requests go through it.
// The WireGuard UDP socket itself is bound to the WiFi netif, so it never loops into the tunnel.
//
// lwIP in Arduino-ESP32 is built without core locking, so every call into the WireGuard/lwIP
// API is marshalled onto the tcpip thread with tcpip_api_call().
#pragma once

#include "secrets.h"

// WireGuard is only wired up for the Waveshare P4 build (library + RAM); other boards ignore WG_ENABLE.
#if !defined(WG_ENABLE) || !WG_ENABLE || !defined(BOARD_WAVESHARE_P4_5)
namespace wg {
static constexpr bool enabled = false;
static bool start() { return true; }
static bool up() { return true; }
static volatile bool last_up = false;
static void maintain() {}
static void on_network_change() {}
}  // namespace wg
#else

#include <Arduino.h>
#include <esp_wireguard.h>
#include <lwip/priv/tcpip_priv.h>
#include <lwip/dns.h>

#ifndef WG_PORT
#define WG_PORT 51820
#endif
#ifndef WG_NETMASK
#define WG_NETMASK "255.255.255.255"
#endif
#ifndef WG_PRESHARED_KEY
#define WG_PRESHARED_KEY ""
#endif
// DNS server reached through the tunnel (e.g. the home router), so home-only names like
// ha.home.lan resolve. "" = keep using the local network's DNS.
#ifndef WG_DNS
#define WG_DNS ""
#endif
#ifndef WG_KEEPALIVE
#define WG_KEEPALIVE 25
#endif

namespace wg {

static constexpr bool enabled = true;
static wireguard_config_t cfg = ESP_WIREGUARD_CONFIG_DEFAULT();
static wireguard_ctx_t ctx = ESP_WIREGUARD_CONTEXT_DEFAULT();
static bool initialized = false, connected = false;
static uint32_t down_since = 0;
static ip_addr_t local_dns, tunnel_dns;   // local_dns = whatever DHCP on the WiFi gave us
static bool use_tunnel_dns = false;

// ---- run a WireGuard call on the tcpip thread ----
struct Call {
  struct tcpip_api_call_data base;   // must be first
  int op;
  esp_err_t result;
};
enum { OP_INIT, OP_CONNECT, OP_ALLOW_ALL, OP_SET_DEFAULT, OP_IS_UP, OP_DISCONNECT, OP_DNS_TUNNEL, OP_DNS_LOCAL, OP_DNS_CAPTURE };

static err_t on_tcpip(struct tcpip_api_call_data *d) {
  Call *c = (Call *)d;
  switch (c->op) {
    case OP_INIT:        c->result = esp_wireguard_init(&cfg, &ctx); break;
    case OP_CONNECT:     c->result = esp_wireguard_connect(&ctx); break;
    case OP_ALLOW_ALL:   c->result = esp_wireguard_add_allowed_ip(&ctx, "0.0.0.0", "0.0.0.0"); break;
    case OP_SET_DEFAULT:
      // a WiFi (re)connect makes esp-netif put the WiFi interface back as the default route
      if (ctx.netif && netif_default != ctx.netif) {
        c->result = esp_wireguard_set_default(&ctx);
        if (c->result == ESP_OK) c->result = ESP_ERR_NOT_FINISHED;   // tell the caller it changed
      } else {
        c->result = ESP_OK;
      }
      break;
    case OP_IS_UP:       c->result = esp_wireguard_peer_is_up(&ctx); break;
    case OP_DISCONNECT:  c->result = esp_wireguard_disconnect(&ctx); break;
    case OP_DNS_TUNNEL: {
      // (re)assert the tunnel DNS server; DHCP renewals on the WiFi can overwrite it
      const ip_addr_t *cur = dns_getserver(0);
      c->result = ESP_OK;
      if (!ip_addr_cmp(cur, &tunnel_dns)) {
        if (!ip_addr_isany(cur)) local_dns = *cur;
        dns_setserver(0, &tunnel_dns);
        c->result = ESP_ERR_NOT_FINISHED;   // changed
      }
      break;
    }
    case OP_DNS_CAPTURE: {
      // remember the DNS server DHCP just handed out on the (new) local network
      const ip_addr_t *cur = dns_getserver(0);
      if (!ip_addr_isany(cur) && !ip_addr_cmp(cur, &tunnel_dns)) local_dns = *cur;
      c->result = ESP_OK;
      break;
    }
    case OP_DNS_LOCAL:
      if (!ip_addr_isany(&local_dns)) dns_setserver(0, &local_dns);
      c->result = ESP_OK;
      break;
  }
  return ERR_OK;
}

static esp_err_t call(int op) {
  Call c = {};
  c.op = op;
  c.result = ESP_FAIL;
  tcpip_api_call(on_tcpip, &c.base);
  return c.result;
}

static volatile bool last_up = false;   // last result of up()/start(), for the status icons (any thread)
static bool up() { last_up = connected && call(OP_IS_UP) == ESP_OK; return last_up; }

// Bring the tunnel up. Blocks up to ~30 s waiting for the first handshake. Returns true when up.
static bool start() {
  if (!initialized) {
    cfg.private_key = WG_PRIVATE_KEY;
    cfg.public_key = WG_PEER_PUBLIC_KEY;
    cfg.preshared_key = strlen(WG_PRESHARED_KEY) ? WG_PRESHARED_KEY : nullptr;
    cfg.address = WG_ADDRESS;
    cfg.netmask = WG_NETMASK;
    cfg.endpoint = WG_ENDPOINT;
    cfg.port = WG_PORT;
    cfg.persistent_keepalive = WG_KEEPALIVE;   // keeps NAT mappings on the remote network open
    use_tunnel_dns = strlen(WG_DNS) && ipaddr_aton(WG_DNS, &tunnel_dns);
#ifdef WG_DEBUG
    esp_log_level_set("esp_wireguard", ESP_LOG_DEBUG);
    esp_log_level_set("wireguardif", ESP_LOG_DEBUG);
    esp_log_level_set("wireguard", ESP_LOG_DEBUG);
#endif
    esp_err_t e = call(OP_INIT);
    if (e != ESP_OK) { Serial.printf("[wg] init failed: %s\n", esp_err_to_name(e)); return false; }
    initialized = true;
  }

  if (!connected) {
    // connect resolves the endpoint hostname asynchronously; retry while DNS is in flight
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 40; i++) {
      e = call(OP_CONNECT);
      if (e != ESP_ERR_RETRY) break;
      vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (e != ESP_OK) { Serial.printf("[wg] connect failed: %s\n", esp_err_to_name(e)); return false; }
    // accept replies from any address behind the peer (HA lives on the home LAN, not the tunnel subnet)
    if (call(OP_ALLOW_ALL) != ESP_OK) Serial.println("[wg] add allowed ip failed");
    call(OP_SET_DEFAULT);   // route everything (i.e. HA traffic) through the tunnel
    connected = true;
    Serial.printf("[wg] connecting to %s:%d as %s\n", WG_ENDPOINT, WG_PORT, WG_ADDRESS);
  }

  for (int i = 0; i < 60; i++) {
    if (call(OP_IS_UP) == ESP_OK) {
      Serial.println("[wg] tunnel up");
      last_up = true;
      if (use_tunnel_dns) { call(OP_DNS_TUNNEL); Serial.printf("[wg] dns -> %s\n", WG_DNS); }
      down_since = 0;
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  Serial.printf("[wg] no handshake yet (endpoint resolved to %s)\n", ipaddr_ntoa(&cfg.endpoint_ip));
  last_up = false;
  return false;
}

// Tear down and rebuild. disconnect restores the WiFi netif as default route, so the endpoint
// hostname is re-resolved over the local network, not through the dead tunnel.
static void restart() {
  Serial.println("[wg] restarting tunnel");
  if (use_tunnel_dns) call(OP_DNS_LOCAL);   // resolve the endpoint over the local network
  if (connected) call(OP_DISCONNECT);
  connected = false;
  start();
}

// Call periodically. If the peer has been down for 2 min (keepalive is 25 s, rekey 2 min), rebuild.
static void maintain() {
  if (up()) {
    down_since = 0;
    // re-apply after WiFi reconnects / network changes (DHCP resets the default route and DNS)
    if (call(OP_SET_DEFAULT) == ESP_ERR_NOT_FINISHED) Serial.println("[wg] default route -> tunnel (re-applied)");
    if (use_tunnel_dns && call(OP_DNS_TUNNEL) == ESP_ERR_NOT_FINISHED) Serial.printf("[wg] dns -> %s (re-applied)\n", WG_DNS);
    return;
  }
  if (!down_since) { down_since = millis(); return; }
  if (millis() - down_since > 120000) { down_since = 0; restart(); }
}

// WiFi came back, possibly on a different network (new public IP, NAT, DNS). Rather than hope the
// server notices the old session roaming, rebuild the tunnel: fresh handshake from the new address,
// endpoint resolved with the new network's DNS. Takes about a second when the server is reachable.
static void on_network_change() {
  if (!initialized) return;          // first connect: start() runs after NTP
  call(OP_DNS_CAPTURE);
  down_since = 0;
  restart();
}

}  // namespace wg
#endif
