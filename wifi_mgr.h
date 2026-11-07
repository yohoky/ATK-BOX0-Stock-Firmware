#pragma once
#include <Arduino.h>

// WiFi 运行状态
enum WifiState {
  WIFI_AP_ONLY    = 0,   // 只有热点(未配置 或 连接失败)
  WIFI_CONNECTING = 1,   // 正在连接路由器
  WIFI_CONNECTED  = 2    // 已连上路由器
};

extern volatile WifiState g_wifiState;
extern volatile int       g_wifiRc;      // 失败原因码 (wl_status_t)
extern char               g_wifiIp[16];
extern volatile bool      g_wifiEverOk;

void        wifiInit();         // 上电: 常开热点 + 并发连接(非阻塞)
void        wifiLoop();         // 主循环里调用, 状态机推进
void        wifiApplyCreds();   // 网页保存新账号后立即热切换(不重启)
void        wifiReconnect();    // 手动重连
const char* wifiStateText();    // "已连接"/"连接中"/"热点模式"
const char* wifiRcText(int rc); // 失败原因中文说明
void        powerOff();         // 关断整机电源(电池) / 深睡(USB 供电)

// WiFi 扫描: 只在未连上路由器时可用(避免扫描打断现有连接/AP)
// 返回发现的网络数, ssid 出参需调用方提供 char[33] 数组
int         wifiScan(String* ssidOut, int* rssiOut, int* encOut, int maxN);
