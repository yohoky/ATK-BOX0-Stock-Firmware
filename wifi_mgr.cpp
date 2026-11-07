#include "wifi_mgr.h"
#include "app_config.h"
#include "cfg_pins.h"
#include "ui_display.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <time.h>

volatile WifiState g_wifiState = WIFI_AP_ONLY;
volatile int       g_wifiRc    = 0;
char               g_wifiIp[16] = "0.0.0.0";
volatile bool      g_wifiEverOk = false;

static uint32_t s_t0      = 0;      // 本轮连接开始时间
static uint32_t s_retryAt = 0;      // 下次自动重连时间
static bool     s_staOn   = false;  // STA 是否处于已发起状态
static uint32_t s_apSince = 0;      // 热点接入第一个客户端的时刻
static bool     s_forceSta = false; // 网页刚保存了账号: 这一次优先连路由器
static uint32_t s_lastScanAt = 0;   // 上次为选网做扫描的时刻(节流, 别反复打断射频)
static uint32_t s_roamAt = 0;       // 上次漫游检查时刻
static int      s_trySeq = 0;       // 扫描无果时按顺序轮换的下一条 profile

// 未连上路由器时劫持 DNS: 手机连热点后自动弹出配置页(captive portal)
static DNSServer s_dns;
static bool      s_dnsOn = false;

const char* wifiStateText() {
  switch (g_wifiState) {
    case WIFI_CONNECTED:  return "已连接";
    case WIFI_CONNECTING: return "连接中";
    default:              return "热点模式";
  }
}

const char* wifiRcText(int rc) {
  switch (rc) {
    case WL_NO_SSID_AVAIL:   return "找不到该 WiFi(检查名称/是否为 2.4GHz)";
    case WL_CONNECT_FAILED:  return "密码错误或认证失败";
    case WL_CONNECTION_LOST: return "连接中丢失";
    case WL_DISCONNECTED:    return "连接超时(信号弱或路由器拒绝)";
    default:                 return "未知原因";
  }
}

// 扫描周边, 在"已保存的历史 WiFi"里挑信号最好的一条。
//   exclude: 要排除的索引(通常是刚认证失败的那条), -1 = 不排除
static int pickSavedByScan(int exclude) {
  if (millis() - s_lastScanAt < 15000 && s_lastScanAt) return -2;  // 节流: -2=本次不扫描
  s_lastScanAt = millis();
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) { WiFi.scanDelete(); return -1; }
  int best = -1, bestR = -1000;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    int r = WiFi.RSSI(i);
    for (int p = 0; p < g_wifiProfileN; p++) {
      if (p == exclude) continue;
      if (s == g_wifiProfiles[p].ssid && r > bestR) { bestR = r; best = p; }
    }
  }
  WiFi.scanDelete();
  return best;
}

// 用指定历史 WiFi 发起连接, 并把它置顶为"最近使用"
static bool beginProfile(int idx) {
  if (!cfgWifiUse(idx)) return false;
  Serial.printf("[WiFi] 连接已保存网络 \"%s\"\n", g_wifiSsid);
  WiFi.disconnect(false, false);
  delay(30);
  // 无密码(开放)网络必须走单参数 begin; 传空字符串在部分固件上会被当 WPA 而认证失败
  if (!g_wifiPass[0]) WiFi.begin(g_wifiSsid);
  else                WiFi.begin(g_wifiSsid, g_wifiPass);
  s_t0 = millis();
  s_staOn = true;
  g_wifiRc = 0;
  g_wifiState = WIFI_CONNECTING;
  return true;
}

static void startSTA() {
  if (!strlen(g_wifiSsid)) {
    g_wifiState = WIFI_AP_ONLY;
    s_staOn = false;
    Serial.println("[WiFi] 未配置 SSID, 保持热点模式");
    return;
  }
  Serial.printf("[WiFi] 连接 \"%s\" ...\n", g_wifiSsid);
  WiFi.disconnect(false, false);
  delay(30);
  // 无密码(开放)网络必须走单参数 begin; 传空字符串在部分固件上会被当 WPA 而认证失败
  if (!g_wifiPass[0]) WiFi.begin(g_wifiSsid);
  else                WiFi.begin(g_wifiSsid, g_wifiPass);
  s_t0    = millis();
  s_staOn = true;
  g_wifiRc = 0;
  g_wifiState = WIFI_CONNECTING;
}

void wifiInit() {
  WiFi.persistent(false);          // 不写 SDK 自己的 nvs, 我们用自己的 NVS
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);          // ★ 热点常开: 任何时候都能通过热点进去改配置
  // ★★ 顺序很重要: softAPConfig 必须在 softAP 之前!
  //    在热点已经启动之后再改 IP, 会重新拉起 netif 和 DHCP server,
  //    结果就是"能连上、能拿到一次 IP, 之后 ARP/Ping/HTTP 全不通"。
  IPAddress apIp(192, 168, 4, 1);
  WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
  // max_connection 同时决定 DHCP 租约池大小; 4 个足够, 且比 8 省下不少 WiFi 缓冲内存
  WiFi.softAP("BOX0-Setup", nullptr, 1, false, 4);
  Serial.printf("[WiFi] 热点 BOX0-Setup 已开启  http://%s  DHCP池=4\n",
                WiFi.softAPIP().toString().c_str());
  // 已保存多组 WiFi: 开机先按"当前地点信号最好"选一条, 家/公司来回搬免配网
  if (g_wifiProfileN > 1) {
    int p = pickSavedByScan(-1);
    if (p >= 0) cfgWifiUse(p);
  }
  startSTA();
}

// 带超时的扫描: 扫描会短暂占用射频(AP 可能断 1 秒), 因此只允许未连上时调用
int wifiScan(String* ssidOut, int* rssiOut, int* encOut, int maxN) {
  // ★ 允许"已连上路由器"时随时扫描(用户要能换 WiFi)。
  //   同步扫描约 2-3 秒会短暂占用射频, 期间行情可能少刷一次, 但不会断连。
  int n = WiFi.scanNetworks(false, true);         // 同步扫描, 含隐藏 SSID
  if (n <= 0) { WiFi.scanDelete(); return 0; }
  int out = 0;
  for (int i = 0; i < n && out < maxN; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (int k = 0; k < out; k++) if (ssidOut[k] == s) { dup = true; break; }
    if (dup) continue;
    ssidOut[out] = s;
    rssiOut[out] = WiFi.RSSI(i);
    encOut[out]  = (int)WiFi.encryptionType(i);
    out++;
  }
  WiFi.scanDelete();
  return out;
}

void wifiApplyCreds() {
  Serial.println("[WiFi] 应用新账号, 热切换(不重启)");
  g_wifiState = WIFI_CONNECTING;
  s_forceSta = true;      // 手机上刚点保存: 别因为"热点有人连着"把 STA 掐掉
  s_retryAt = 0;
  startSTA();
}

void wifiReconnect() {
  Serial.println("[WiFi] 手动重连");
  s_staOn = false;
  startSTA();
}

void wifiLoop() {
  // ---- captive portal: 未连上路由器时, 把手机的所有域名解析到本机 ----
  // 手机连上 BOX0-Setup 后会自己弹出"登录到网络"页面, 不用手打 IP。
  // 连上路由器后立刻关闭劫持, 避免影响手机正常上网。
  bool wantDns = (g_wifiState != WIFI_CONNECTED);
  if (wantDns && !s_dnsOn) {
    s_dnsOn = s_dns.start(53, "*", WiFi.softAPIP());
    if (s_dnsOn) Serial.println("[DNS] captive portal 已开启");
  } else if (!wantDns && s_dnsOn) {
    s_dns.stop();
    s_dnsOn = false;
    Serial.println("[DNS] 已连网, 关闭劫持");
  }
  if (s_dnsOn) s_dns.processNextRequest();

  // ★★ 关键: 有人正连着热点做配置时, 大幅降低连路由器的频率
  //    AP+STA 双模下 WiFi.begin() 会不停扫描, 每次射频被占用约 1 秒,
  //    DHCP 的 DISCOVER/OFFER 就在这个空档丢掉 -> "能连上热点但拿不到 IP"。
  //    但只保护"刚接入的 20 秒"(DHCP 阶段), 之后用户还在配置页时也要能连上路由器,
  //    否则现象就是"网页保存了账号, 手机不离开热点就永远连不上路由器"。
  int apN = WiFi.softAPgetStationNum();
  if (apN > 0) { if (!s_apSince) s_apSince = millis(); }
  else         { s_apSince = 0; }
  bool apBusy = (apN > 0) && !s_forceSta && s_apSince && (millis() - s_apSince < 20000);

  if (!s_staOn) {
    if (apBusy) return;                       // 有人在用热点: 不扫描, 不重连
    // 连接失败后 2 分钟自动重试一次(不强求, 省电)
    if (strlen(g_wifiSsid) && millis() > s_retryAt && s_retryAt) {
      s_retryAt = 0;
      startSTA();
    }
    return;
  }

  if (apBusy && g_wifiState != WIFI_CONNECTED) {
    // 热点有客户端, 而路由器还没连上 -> 停掉 STA, 别打断热点
    WiFi.disconnect(false, false);
    s_staOn     = false;
    g_wifiState = WIFI_AP_ONLY;
    s_retryAt   = millis() + 8000;            // 客户端走了 8 秒后再试
    Serial.println("[WiFi] 有设备接热点, 暂停连路由器(避免扫描打断DHCP)");
    return;
  }

  wl_status_t st = WiFi.status();

  if (st == WL_CONNECTED) {
    if (g_wifiState != WIFI_CONNECTED) {
      s_forceSta   = false;
      g_wifiState  = WIFI_CONNECTED;
      g_wifiEverOk = true;
      snprintf(g_wifiIp, sizeof(g_wifiIp), "%s", WiFi.localIP().toString().c_str());
      Serial.printf("[WiFi] 已连接 IP=%s RSSI=%d\n", g_wifiIp, WiFi.RSSI());
      configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
      if (MDNS.begin("box0")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[MDNS] http://box0.local");
      }
      s_roamAt = millis();
    }
    // 漫游: 当前信号很差时, 每 3 分钟看有没有更强的已保存网络(换地点自动切换)
    if (g_wifiProfileN > 1 && millis() - s_roamAt > 180000 && WiFi.RSSI() < -75) {
      s_roamAt = millis();
      int p = pickSavedByScan(-1);
      if (p >= 0 && strcmp(g_wifiProfiles[p].ssid, g_wifiSsid) != 0) {
        Serial.println("[WiFi] 发现更强的已保存网络, 自动切换");
        beginProfile(p);
      }
    }
    return;
  }

  // 曾经连上过, 现在掉线 -> 按已保存列表重新选网, 不再死等原 SSID
  if (g_wifiState == WIFI_CONNECTED) {
    Serial.println("[WiFi] 连接丢失, 重新选择已保存网络");
    g_wifiState = WIFI_CONNECTING;
    s_t0 = millis();
    if (g_wifiProfileN > 1) {
      int p = pickSavedByScan(-1);
      if (p >= 0 && beginProfile(p)) return;
    }
    WiFi.reconnect();
    return;
  }

  // 连接中: 密码错/找不到 SSID 立刻判失败, 不必等满 25 秒
  // (前 6 秒不判定: WiFi.begin() 刚发出时 status 还带着上一轮的值)
  if ((millis() - s_t0) > 6000 && (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL)) {
    g_wifiRc = (int)st;
    Serial.printf("[WiFi] 连接失败 rc=%d (%s)\n", g_wifiRc, wifiRcText(g_wifiRc));
    // ★ 立刻改试其它已保存网络, 不再干等 2 分钟(换地点后的关键路径)
    if (g_wifiProfileN > 1) {
      int cur = cfgWifiFind(g_wifiSsid);
      int p = pickSavedByScan(cur);
      if (p == -2) {   // 扫描节流中 -> 按顺序轮换下一条
        s_trySeq = (s_trySeq + 1) % g_wifiProfileN;
        p = (s_trySeq == cur) ? (s_trySeq + 1) % g_wifiProfileN : s_trySeq;
      }
      if (p == -1 && g_wifiProfileN > 1) {
        s_trySeq = (cur + 1) % g_wifiProfileN;
        p = s_trySeq;
      }
      if (p >= 0 && p != cur && beginProfile(p)) return;
    }
    g_wifiState = WIFI_AP_ONLY;
    s_staOn     = false;
    s_forceSta  = false;
    s_retryAt   = millis() + 30000;
    return;
  }

  // 连接中, 超时判定
  if (millis() - s_t0 > 25000) {
    g_wifiRc   = (int)st;
    g_wifiState = WIFI_AP_ONLY;
    s_staOn    = false;
    s_forceSta = false;
    s_retryAt  = millis() + 30000;
    Serial.printf("[WiFi] 连接超时 rc=%d (%s), 回到热点模式\n",
                  g_wifiRc, wifiRcText(g_wifiRc));
  }
}

// ---------------- 关机 ----------------
void powerOff() {
  // 注意: 这里不要用 Serial.flush(), HWCDC 在主机不收数据时可能一直等
  Serial.println("[SYS] 关机");

  // 先给用户"正在关机"的视觉反馈, 再断电(直接用背光 PWM, 不要 pinMode 覆盖)
  uiToast("正在关机...", 0xFD20, 3000);
  delay(400);
  uiSetScreen(false);
  if (s_dnsOn) { s_dns.stop(); s_dnsOn = false; }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);

  // 电池供电时: 松掉电源锁存 -> 整机真断电
  pinMode(PIN_SYS_POW, OUTPUT);
  digitalWrite(PIN_SYS_POW, LOW);
  delay(900);
  digitalWrite(PIN_SYS_POW, LOW);          // 再补一次(有些板子锁存要稳定拉低)
  delay(300);

  // 走到这里说明 USB 供电把电源兜住了 -> 退而求其次进深睡
  // 任意按键(L/M/R 低电平)唤醒
  Serial.println("[SYS] USB 供电, 进入深睡(按任意键唤醒)");
  esp_sleep_enable_ext1_wakeup((1ULL << PIN_BTN_L) | (1ULL << PIN_BTN_M) | (1ULL << PIN_BTN_R),
                               ESP_EXT1_WAKEUP_ANY_LOW);
  rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_L);
  rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_M);
  rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_R);
  esp_deep_sleep_start();
}
