#include "app_config.h"
#include "cfg_pins.h"

Preferences prefs;

char    g_wifiSsid[33] = "";
char    g_wifiPass[65] = "";
// v7.3: 锁定扫描到的 2.4G AP(双频合一网络同名双 BSSID 时避免挑错)
uint8_t g_wifiBssid[6] = {0, 0, 0, 0, 0, 0};
uint8_t g_wifiChannel  = 0;
bool    g_wifiHasBssid = false;
char    g_adminPass[17] = "123456";
uint8_t g_volume = 60;   // v7.0 出厂默认 60(提示音已温和化, 30 偏小)
uint8_t g_bright = 30;       // 出厂默认 30%(用户 2026-11-07 指定, 省电不刺眼)
uint8_t g_autoOffMin = 1;    // v7.1 对齐实测盒子: 1 分钟无操作熄屏
uint8_t g_autoPowerOff = 1;  // 默认开启自动关机
uint8_t g_powerOffMin = 30;  // 熄屏后 30 分钟无操作自动关机
uint8_t g_refreshSec = 5;    // 对齐实测盒子
uint8_t g_jumpSec = 5;
uint8_t g_displayMode = DISP_LIST;
// v7.2 实时快讯默认: 开, 60 秒轮询一次, 显示摘要, 新消息只角标不响铃
uint8_t  g_newsOn      = 1;
uint8_t  g_newsSec     = 8;
uint16_t g_newsRefSec  = 60;
uint8_t  g_newsSrc     = 0;
uint8_t  g_newsBeep    = 0;
uint8_t  g_newsSummary = 1;
volatile uint8_t g_appMode = APP_STOCK;   // 不落盘, 开机一律回股票模式
volatile int8_t  g_pendingMode = -1;     // 网页请求由主 loop 消费
// v7.3 新增配置
uint16_t g_alertRepeatMin = 30;   // 30 分钟内同一只股票只提醒一次
uint8_t  g_tickerNews     = 0;    // 底部快讯条默认关, 菜单/网页可开
uint8_t  g_newsAuto       = 0;    // 新闻页默认手动切换
uint8_t  g_newsAutoSec    = 8;
uint8_t  g_micKnock       = 0;    // 敲击检测默认关(实验功能)
uint16_t g_micThreshold   = 9000;

// ---- 版本更新说明(设备信息页 / 网页设备页 /api/diag 共用, 每条 1-100 字) ----
const FwNote FW_HISTORY[] = {
  { "v7.3", FW_NOTES },
  { "v7.2", "实时财经快讯(标题+摘要,60秒自动更新)、三连M进新闻、四连M进收音机、菜单可切股票/收音机模式;提示音与MP3互斥,并修复模式切换时的看门狗复位。" },
  { "v7.1", "两套独立时间窗口(信息更新/预警提示)、全局与每股独立提醒阈值、修复自动关机误唤醒、定时自动切换显示、0x0完整固件首发。" },
  { "v7.0", "8组历史WiFi自动漫游(含无密码)、状态栏六项重做、三键操作重设计与显式返回、网页升级tab与全接口面板、默认音量60。" },
};
const int FW_HISTORY_N = (int)(sizeof(FW_HISTORY) / sizeof(FW_HISTORY[0]));
// ---- v7.1 两套独立时间范围: 默认 9:00-11:30 / 13:00-15:00 ----
TimeWindow g_fetchWin = { TW_DEF_AM_S, TW_DEF_AM_E, TW_DEF_PM_S, TW_DEF_PM_E };
TimeWindow g_alertWin = { TW_DEF_AM_S, TW_DEF_AM_E, TW_DEF_PM_S, TW_DEF_PM_E };
uint8_t  g_workdayOnly  = 1;   // 仅周一~周五生效
uint8_t  g_fetchOutside = 0;   // 时段外不抓行情
// 涨跌提醒: 默认开, 全局阈值 3%(对齐实测盒子)
uint8_t  g_alertOn    = 1;
float    g_alertPct   = 3.0f;
float    g_alertPer[MAX_STOCKS] = {0};  // 0=跟随全局, >0=独立, <0=该股禁用
// 自动显示模式(板上无 IMU, 用定时切换; 若探测到 IMU 则重力优先)
uint8_t  g_gyroOn        = 1;
uint8_t  g_autoSwitchSec = 30;
uint8_t  g_autoEffMode   = DISP_LIST;

Station g_stations[MAX_STATIONS];
uint8_t g_nStation = 0;
int8_t  g_curStation = 0;

StockDef g_stocks[MAX_STOCKS];
uint8_t  g_nStock = 0;

Quote g_quotes[MAX_STOCKS];
SemaphoreHandle_t g_quoteMutex = nullptr;

volatile RadioState g_radioState = RADIO_IDLE;
char g_streamTitle[96] = "";
volatile bool g_screenOn = true;
volatile int  g_curStock = 0;

// 音量待落盘标记(见 cfgFlushVolume)
static volatile bool s_volDirty = false;
static uint32_t s_volChangeAt = 0;

// v7.1: 时间窗口钳位。分钟数 0..1439, 起必须早于止(至少 15 分钟), 非法回落默认。
void cfgSanitizeWindow(TimeWindow& w) {
  TimeWindow def = { TW_DEF_AM_S, TW_DEF_AM_E, TW_DEF_PM_S, TW_DEF_PM_E };
  auto fix = [](uint16_t& s, uint16_t& e, uint16_t ds, uint16_t de) {
    if (s > 1439 || e > 1439 || s >= e || e - s < 15) { s = ds; e = de; }
  };
  fix(w.amS, w.amE, def.amS, def.amE);
  fix(w.pmS, w.pmE, def.pmS, def.pmE);
  // 下午窗口不能早于上午窗口(允许重叠但不允许倒挂太多, 保持简单: 不强制)
}

// ★ Preferences 不是可重入的: 网页(async_tcp 任务)和主循环会同时写,
//   不加锁偶发把 NVS 写坏 -> 重启后配置变乱码/丢电台。所有写操作走这把锁。
static SemaphoreHandle_t s_cfgMux = nullptr;
static inline void cfgLock()   { if (s_cfgMux) xSemaphoreTake(s_cfgMux, portMAX_DELAY); }
static inline void cfgUnlock() { if (s_cfgMux) xSemaphoreGive(s_cfgMux); }

// ---------- 默认预设 ----------
static const char* DEF_STOCKS[] = {
  "sh000001",    // 上证指数
  "sz399001",    // 深证成指
  "sh603993",    // 洛阳钼业
  "sz000783",    // 长江证券
  "sz000977",    // 浪潮信息
  "sz000938",    // 紫光股份
  "sz002670",    // 国盛证券
  "sz159841",    // 证券ETF
  "sh512980",    // 传媒ETF广发
  "sh563020",    // 红利低波ETF易方达
  "sz002657",    // 中科金财
  "sz300215",    // 电科院
  "sz002439",    // 启明星辰
  "sz399006",    // 创业板指 (v7.1: 对齐实测盒子 14 只)
};
// ── 内置电台(2026-11 实测可播, 全部走 http 明文, ESP32 上省内存/出音快) ──
//    来源: radio-browser 中国区点播榜 + 逐个 range 请求验证(HTTP 200, audio/mpeg 或 aacp)
static const char* DEF_STA_NAME[] = {
  "中国之声", "北京新闻广播", "广东音乐之声", "上海动感101",
  "上海经典947", "AsiaFM高清音乐", "听云相声台", "听云评书台"
};
static const char* DEF_STA_URL[] = {
  "http://lhttp.qtfm.cn/live/15318317/64k.mp3",
  "http://lhttp.qtfm.cn/live/339/64k.mp3",
  "http://lhttp.qtfm.cn/live/1260/64k.mp3",
  "http://lhttp.qingting.fm/live/274/64k.mp3",
  "http://lhttp.qingting.fm/live/267/64k.mp3",
  "http://asiafm.hk:8000/asiahd",
  "http://m.tyradio.uk/tyxs.mp3",
  "http://m.tyradio.uk/typs.mp3",
};
#define DEF_NSTA ((int)(sizeof(DEF_STA_NAME) / sizeof(DEF_STA_NAME[0])))

// ---------------- 多组 WiFi 凭据 ----------------
WifiProfile g_wifiProfiles[WIFI_PROFILE_N];
uint8_t     g_wifiProfileN = 0;

static void cfgWifiSave() {
  prefs.putUChar("wpn", g_wifiProfileN);
  for (uint8_t i = 0; i < WIFI_PROFILE_N; i++) {
    char ks[8], kp[8], kb[8], kc[8];
    snprintf(ks, sizeof(ks), "ws%u", (unsigned)i);
    snprintf(kp, sizeof(kp), "wp%u", (unsigned)i);
    snprintf(kb, sizeof(kb), "wb%u", (unsigned)i);
    snprintf(kc, sizeof(kc), "wc%u", (unsigned)i);
    if (i < g_wifiProfileN) {
      prefs.putString(ks, g_wifiProfiles[i].ssid);
      prefs.putString(kp, g_wifiProfiles[i].pass);
      prefs.putBytes(kb, g_wifiProfiles[i].bssid, 6);
      prefs.putUChar(kc, g_wifiProfiles[i].channel);
    } else if (prefs.isKey(ks)) {
      prefs.remove(ks); prefs.remove(kp);
      prefs.remove(kb); prefs.remove(kc);
    }
  }
}

void cfgWifiLoad() {
  memset(g_wifiProfiles, 0, sizeof(g_wifiProfiles));
  g_wifiProfileN = prefs.getUChar("wpn", 0);
  if (g_wifiProfileN > WIFI_PROFILE_N) g_wifiProfileN = WIFI_PROFILE_N;
  for (uint8_t i = 0; i < g_wifiProfileN; i++) {
    char ks[8], kp[8], kb[8], kc[8];
    snprintf(ks, sizeof(ks), "ws%u", (unsigned)i);
    snprintf(kp, sizeof(kp), "wp%u", (unsigned)i);
    snprintf(kb, sizeof(kb), "wb%u", (unsigned)i);
    snprintf(kc, sizeof(kc), "wc%u", (unsigned)i);
    prefs.getString(ks, g_wifiProfiles[i].ssid, sizeof(g_wifiProfiles[i].ssid));
    prefs.getString(kp, g_wifiProfiles[i].pass, sizeof(g_wifiProfiles[i].pass));
    g_wifiProfiles[i].ssid[32] = 0;
    g_wifiProfiles[i].pass[64] = 0;
    if (prefs.isKey(kb)) prefs.getBytes(kb, g_wifiProfiles[i].bssid, 6);
    g_wifiProfiles[i].channel = prefs.getUChar(kc, 0);
  }
  // 旧固件只有单组 ssid/wpass: 自动迁移为第 1 组, 升级后不会掉网
  if (g_wifiSsid[0] && !g_wifiProfileN) {
    strncpy(g_wifiProfiles[0].ssid, g_wifiSsid, 32);
    strncpy(g_wifiProfiles[0].pass, g_wifiPass, 64);
    g_wifiProfileN = 1;
    cfgWifiSave();
  }
  Serial.printf("[Cfg] 已保存 %u 组 WiFi\n", (unsigned)g_wifiProfileN);
}

int cfgWifiFind(const char* ssid) {
  if (!ssid || !*ssid) return -1;
  for (int i = 0; i < g_wifiProfileN; i++)
    if (strncmp(g_wifiProfiles[i].ssid, ssid, 32) == 0) return i;
  return -1;
}

bool cfgWifiAdd(const char* ssid, const char* pass,
                const uint8_t* bssid, uint8_t channel) {
  if (!ssid || !*ssid || strlen(ssid) > 32) return false;
  cfgLock();
  int found = cfgWifiFind(ssid);
  WifiProfile p; memset(&p, 0, sizeof(p));
  strncpy(p.ssid, ssid, 32); p.ssid[32] = 0;
  strncpy(p.pass, pass ? pass : "", 64); p.pass[64] = 0;
  if (bssid && channel) { memcpy(p.bssid, bssid, 6); p.channel = channel; }
  if (found >= 0) {
    for (int i = found; i > 0; i--) g_wifiProfiles[i] = g_wifiProfiles[i - 1];
  } else {
    if (g_wifiProfileN < WIFI_PROFILE_N) g_wifiProfileN++;
    for (int i = g_wifiProfileN - 1; i > 0; i--)
      g_wifiProfiles[i] = g_wifiProfiles[i - 1];
  }
  g_wifiProfiles[0] = p;
  cfgWifiSave();
  cfgUnlock();
  return true;
}

bool cfgWifiDel(int idx) {
  if (idx < 0 || idx >= g_wifiProfileN) return false;
  cfgLock();
  for (int i = idx; i < g_wifiProfileN - 1; i++)
    g_wifiProfiles[i] = g_wifiProfiles[i + 1];
  g_wifiProfileN--;
  memset(&g_wifiProfiles[g_wifiProfileN], 0, sizeof(WifiProfile));
  cfgWifiSave();
  cfgUnlock();
  return true;
}

bool cfgWifiUse(int idx) {
  if (idx < 0 || idx >= g_wifiProfileN) return false;
  cfgLock();
  WifiProfile p = g_wifiProfiles[idx];
  for (int i = idx; i > 0; i--) g_wifiProfiles[i] = g_wifiProfiles[i - 1];
  g_wifiProfiles[0] = p;
  cfgWifiSave();
  strncpy(g_wifiSsid, p.ssid, 32); g_wifiSsid[32] = 0;
  strncpy(g_wifiPass, p.pass, 64); g_wifiPass[64] = 0;
  prefs.putString("ssid", g_wifiSsid);
  prefs.putString("wpass", g_wifiPass);
  memcpy(g_wifiBssid, p.bssid, 6);
  g_wifiChannel  = p.channel;
  g_wifiHasBssid = (p.channel != 0 && p.channel <= 14);
  prefs.putBytes("wbssid", g_wifiBssid, 6);
  prefs.putUChar("wchan", g_wifiHasBssid ? g_wifiChannel : 0);
  cfgUnlock();
  return true;
}

void cfgLoad() {
  prefs.begin("box0", false);
  if (!s_cfgMux) s_cfgMux = xSemaphoreCreateMutex();
  g_quoteMutex = xSemaphoreCreateMutex();

  prefs.getString("ssid", g_wifiSsid, sizeof(g_wifiSsid));
  prefs.getString("wpass", g_wifiPass, sizeof(g_wifiPass));
  // v7.3: 读回上次锁定的 2.4G AP(旧配置没有这两个 key -> 不锁定, 行为同旧版)
  if (prefs.isKey("wbssid")) prefs.getBytes("wbssid", g_wifiBssid, 6);
  g_wifiChannel  = prefs.getUChar("wchan", 0);
  g_wifiHasBssid = (g_wifiChannel > 0 && g_wifiChannel <= 14);
  prefs.getString("apass", g_adminPass, sizeof(g_adminPass));

  // 开源版本不预置任何真实 WiFi 凭据(旧版曾写死过一组, 已移除以防泄露)。
  // 首次使用: 设备会自动开热点 BOX0-Setup, 手机/电脑连上后打开 192.168.4.1 配网。
  // 已配置过的设备 NVS 里保有 SSID/密码, 这里不做任何覆盖。

  if (g_adminPass[0] == 0) strcpy(g_adminPass, "123456");
  g_volume = prefs.getUChar("vol", 60);
  g_bright = prefs.getUChar("brt", 30);
  // 自动熄屏改"分钟档"(旧 aoff 键是秒, 已弃用; 用新 aoffm 避免旧值串味)
  g_autoOffMin   = prefs.getUChar("aoffm", 0);
  g_autoPowerOff = prefs.getUChar("apoff", 1);
  g_powerOffMin  = prefs.getUChar("poffm", 30);
  // 一次性把老设备的亮度改成新出厂默认 30%(只执行一次, 之后用户自调的值不再被覆盖)
  if (prefs.getUChar("brtv", 0) < 2) {
    prefs.putUChar("brtv", 2);
    g_bright = 30;
    prefs.putUChar("brt", 30);
  }
  // v7.0: 出厂默认值改 60。只影响"没有 vol 键"的新设备/清过 NVS 的设备，
  //   不再做一次性覆盖 —— 用户自己调过的音量必须保留。
  // 自动关机档位以 g_powerOffMin 为唯一数据源, 旧值非法时回落到默认 30 分
  bool poffOk = false;
  for (int i = 0; i < POFF_N; i++)
    if (POFF_STEPS[i] == g_powerOffMin) { poffOk = true; break; }
  if (!poffOk) { g_powerOffMin = 30; prefs.putUChar("poffm", 30); }
  g_autoPowerOff = g_powerOffMin ? 1 : 0;
  g_refreshSec = prefs.getUChar("ref", 6);
  g_jumpSec = prefs.getUChar("jmp", 5);
  // ---- 新闻条 ----
  g_newsOn     = prefs.getUChar("nson", 1);
  g_newsSec    = prefs.getUChar("nssec", 8);
  g_newsRefSec = prefs.getUShort("nsref", 60);   // v7.2: 实时快讯 60 秒
  g_newsSrc    = prefs.getUChar("nssrc", 0);
  g_newsBeep    = prefs.getUChar("nsbeep", 0) ? 1 : 0;
  g_newsSummary = prefs.getUChar("nssum",  1) ? 1 : 0;
  // v7.3 新增: 提醒节流 / 底部快讯条 / 新闻自动切换 / 敲击检测
  g_alertRepeatMin = prefs.getUShort("altrep", 30);
  g_tickerNews     = prefs.getUChar("tknews",  0) ? 1 : 0;
  g_newsAuto       = prefs.getUChar("nsauto",  0) ? 1 : 0;
  g_newsAutoSec    = prefs.getUChar("nsautos", 8);
  g_micKnock       = prefs.getUChar("mick",    0) ? 1 : 0;
  g_micThreshold   = prefs.getUShort("mict",   9000);
  g_appMode = APP_STOCK;                        // 开机一律股票模式(不落盘)
  // ---- v7.1: 两套独立时间范围 ----
  if (prefs.getBytes("fwin", &g_fetchWin, sizeof(g_fetchWin)) != sizeof(g_fetchWin))
    g_fetchWin = { TW_DEF_AM_S, TW_DEF_AM_E, TW_DEF_PM_S, TW_DEF_PM_E };
  if (prefs.getBytes("awin", &g_alertWin, sizeof(g_alertWin)) != sizeof(g_alertWin))
    g_alertWin = { TW_DEF_AM_S, TW_DEF_AM_E, TW_DEF_PM_S, TW_DEF_PM_E };
  cfgSanitizeWindow(g_fetchWin);
  cfgSanitizeWindow(g_alertWin);
  g_workdayOnly  = prefs.getUChar("twkd",  1) ? 1 : 0;
  g_fetchOutside = prefs.getUChar("ftout", 0) ? 1 : 0;
  // ---- 涨跌提醒 ----
  g_alertOn  = prefs.getUChar("alton", 1);
  g_alertPct = prefs.getFloat("altpct", 3.0f);
  // 每股阈值: 旧设备没有这个键 -> 全 0(全部跟随全局), 行为与旧版一致
  if (prefs.getBytes("altper", g_alertPer, sizeof(g_alertPer)) != sizeof(g_alertPer))
    memset(g_alertPer, 0, sizeof(g_alertPer));
  for (int i = 0; i < MAX_STOCKS; i++) {
    if (g_alertPer[i] > 20.0f || g_alertPer[i] < -1.0f || isnan(g_alertPer[i]))
      g_alertPer[i] = 0;
  }
  // ---- 自动显示模式 ----
  g_gyroOn        = prefs.getUChar("gyro", 1);
  g_autoSwitchSec = prefs.getUChar("aswsec", 30);
  if (g_autoSwitchSec > 120) g_autoSwitchSec = 30;
  g_autoEffMode = DISP_LIST;
  // 默认"列表模式"; v7.1 不再把老设备强制改成 DISP_AUTO(板上无IMU, 那个选项曾无效)
  g_displayMode = prefs.getUChar("dsp", DISP_LIST);
  if (g_displayMode > DISP_AUTO) g_displayMode = DISP_LIST;
  if (prefs.getUChar("uiver", 0) < 7) prefs.putUChar("uiver", 7);

  // ---- 脏数据保护: NVS 写坏/旧版本残留时钳到合法区间, 否则会黑屏或秒切台 ----
  g_wifiSsid[32] = 0;  g_wifiPass[64] = 0;  g_adminPass[16] = 0;
  cfgWifiLoad();      // 读历史 WiFi 列表(旧版单组配置自动迁移成第 1 组)
  if (g_bright < 5)   g_bright = 30;      // 0 会让屏幕全黑, 用户以为变砖
  if (g_bright > 100) g_bright = 100;
  // 自动熄屏只允许 0/1/3/5/10/30/60/120 分钟, 其它值归零
  switch (g_autoOffMin) {
    case 0: case 1: case 3: case 5: case 10: case 30: case 60: case 120: break;
    default: g_autoOffMin = 0;
  }
  if (g_autoPowerOff > 1) g_autoPowerOff = 1;
  // 0 = 用户主动「关闭自动关机」, 必须保留; 旧版这里把 0 钳成 30,
  // 导致网页里关掉关机后一重启又自动打开(v7.2 修)。
  // v7.3: 档位表新增 15 分钟, 不能再写死 "<30 就归 30", 否则 15 分档保存不住
  if (g_powerOffMin != 0) {
    bool poffOk = false;
    for (int i = 0; i < POFF_N; i++) if (POFF_STEPS[i] == g_powerOffMin) { poffOk = true; break; }
    if (!poffOk) g_powerOffMin = 30;
  }
  if (g_refreshSec < 3)  g_refreshSec = 6;
  if (g_refreshSec > 120) g_refreshSec = 120;
  if (g_jumpSec < 2)   g_jumpSec = 5;
  if (g_jumpSec > 60)  g_jumpSec = 60;
  if (g_newsOn > 1)    g_newsOn = 1;
  if (g_newsSec < 3)   g_newsSec = 8;
  if (g_newsSec > 30)  g_newsSec = 30;
  if (g_newsRefSec < 30)   g_newsRefSec = 60;    // v7.2 允许 30 秒级准实时
  if (g_newsRefSec > 3600) g_newsRefSec = 3600;
  if (g_newsSrc > NEWS_SRC - 1) g_newsSrc = 0;
  if (g_newsBeep > 1)    g_newsBeep = 0;
  if (g_newsSummary > 1) g_newsSummary = 1;
  bool repOk = false;                      // v7.3 提醒间隔只允许 6 个标准档
  for (int i = 0; i < ALERT_REPEAT_N; i++)
    if (ALERT_REPEAT_STEPS[i] == g_alertRepeatMin) repOk = true;
  if (!repOk) g_alertRepeatMin = 30;
  if (g_newsAutoSec < 3 || g_newsAutoSec > 60) g_newsAutoSec = 8;
  if (g_micThreshold < 500 || g_micThreshold > 30000) g_micThreshold = 9000;
  if (g_alertOn > 1)   g_alertOn = 1;
  if (g_alertPct < 1.0f || g_alertPct > 20.0f) g_alertPct = 3.0f;
  if (g_workdayOnly > 1) g_workdayOnly = 1;
  if (g_fetchOutside > 1) g_fetchOutside = 1;
  if (g_gyroOn > 1)    g_gyroOn = 1;
  if (g_volume > 100)  g_volume = 60;   // v7.0 出厂默认 60(提示音已温和化, 30 偏小)

  g_nStation = prefs.getUChar("nsta", 0);
  g_curStation = prefs.getChar("cursta", 0);
  if (g_nStation > MAX_STATIONS) g_nStation = MAX_STATIONS;

  // 结构版本不匹配(升级过固件) -> 列表按默认重建, 但保留 WiFi/密码/亮度
  uint8_t ver = prefs.getUChar("cfgver", 1);
  bool layoutChanged = (ver != CFG_VER);

  if (layoutChanged || g_nStation == 0) {  // 首次上电/结构变更: 写默认电台
    g_nStation = DEF_NSTA;
    for (int i = 0; i < g_nStation; i++) {
      memset(&g_stations[i], 0, sizeof(Station));
      strncpy(g_stations[i].name, DEF_STA_NAME[i], STA_NAME_LEN - 1);
      strncpy(g_stations[i].url, DEF_STA_URL[i], STA_URL_LEN - 1);
    }
    g_curStation = 0;
    cfgSaveLists();
  } else {
    prefs.getBytes("sta", g_stations, sizeof(g_stations));
  }

  g_nStock = prefs.getUChar("nstk", 0);
  if (g_nStock > MAX_STOCKS) g_nStock = MAX_STOCKS;
  if (layoutChanged || g_nStock == 0) {
    g_nStock = sizeof(DEF_STOCKS) / sizeof(DEF_STOCKS[0]);
    for (int i = 0; i < g_nStock; i++) {
      memset(&g_stocks[i], 0, sizeof(StockDef));
      strncpy(g_stocks[i].code, DEF_STOCKS[i], STK_CODE_LEN - 1);
    }
    g_curStock = 0;
    cfgSaveLists();
  } else {
    prefs.getBytes("stk", g_stocks, sizeof(g_stocks));
  }
  if (layoutChanged) prefs.putUChar("cfgver", CFG_VER);

  if (g_curStation >= g_nStation) g_curStation = 0;
  if (g_curStation < 0) g_curStation = 0;
}

void cfgSaveSettings() {
  cfgLock();
  s_volDirty = false;
  prefs.putString("ssid", g_wifiSsid);
  prefs.putString("wpass", g_wifiPass);
  prefs.putBytes("wbssid", g_wifiBssid, 6);
  prefs.putUChar("wchan", g_wifiHasBssid ? g_wifiChannel : 0);
  prefs.putString("apass", g_adminPass);
  prefs.putUChar("vol", g_volume);
  prefs.putUChar("brt", g_bright);
  prefs.putUChar("aoffm", g_autoOffMin);
  prefs.putUChar("apoff", g_autoPowerOff);
  prefs.putUChar("poffm", g_powerOffMin);
  prefs.putUChar("ref", g_refreshSec);
  prefs.putUChar("jmp", g_jumpSec);
  prefs.putUChar("dsp", g_displayMode);
  prefs.putUChar("nson", g_newsOn);
  prefs.putUChar("nssec", g_newsSec);
  prefs.putUShort("nsref", g_newsRefSec);
  prefs.putUChar("nssrc", g_newsSrc);
  prefs.putUChar("nsbeep", g_newsBeep);
  prefs.putUChar("nssum",  g_newsSummary);
  prefs.putUShort("altrep", g_alertRepeatMin);
  prefs.putUChar("tknews",  g_tickerNews);
  prefs.putUChar("nsauto",  g_newsAuto);
  prefs.putUChar("nsautos", g_newsAutoSec);
  prefs.putUChar("mick",    g_micKnock);
  prefs.putUShort("mict",   g_micThreshold);
  prefs.putUChar("alton", g_alertOn);
  prefs.putFloat("altpct", g_alertPct);
  prefs.putBytes("altper", g_alertPer, sizeof(g_alertPer));
  prefs.putUChar("gyro", g_gyroOn);
  prefs.putUChar("aswsec", g_autoSwitchSec);
  prefs.putBytes("fwin", &g_fetchWin, sizeof(g_fetchWin));
  prefs.putBytes("awin", &g_alertWin, sizeof(g_alertWin));
  prefs.putUChar("twkd",  g_workdayOnly);
  prefs.putUChar("ftout", g_fetchOutside);
  prefs.putChar("cursta", g_curStation);
  cfgUnlock();
}

// 音量连调时每 160ms 一次, 直接写 NVS 会反复擦写同一扇区;
// 改为"标记脏 + 稳定 1.5 秒后落盘", 长按调完整段只写一次
void cfgSaveVolume() {
  s_volDirty = true;
  s_volChangeAt = millis();
}

void cfgFlushVolume() {
  if (!s_volDirty) return;
  if (millis() - s_volChangeAt < 1500) return;
  s_volDirty = false;
  cfgLock();
  prefs.putUChar("vol", g_volume);
  cfgUnlock();
}

void cfgSaveCurStation() {
  cfgLock();
  prefs.putChar("cursta", g_curStation);
  cfgUnlock();
}

void cfgFactoryReset() {
  cfgLock();
  prefs.clear();
  cfgUnlock();
  strcpy(g_wifiSsid, "");
  strcpy(g_wifiPass, "");
  memset(g_wifiBssid, 0, sizeof(g_wifiBssid));
  g_wifiChannel = 0;
  g_wifiHasBssid = false;
  memset(g_wifiProfiles, 0, sizeof(g_wifiProfiles));
  g_wifiProfileN = 0;
  strcpy(g_adminPass, "123456");
  g_volume = 60;   // 用户明确指定(提示音温和化后 30 偏小)
  g_bright = 30;   // 用户明确指定
  g_autoOffMin = 1;
  g_autoPowerOff = 1;
  g_powerOffMin = 30;
  g_refreshSec = 5;
  g_jumpSec = 5;
  g_displayMode = DISP_LIST;
  g_newsOn = 1;
  g_newsSec = 8;
  g_newsRefSec = 60;
  g_newsSrc = 0;
  g_newsBeep = 0;
  g_newsSummary = 1;
  g_alertRepeatMin = 30;
  g_tickerNews = 0;
  g_newsAuto = 0;
  g_newsAutoSec = 8;
  g_micKnock = 0;
  g_micThreshold = 9000;
  g_appMode = APP_STOCK;
  g_fetchWin = { TW_DEF_AM_S, TW_DEF_AM_E, TW_DEF_PM_S, TW_DEF_PM_E };
  g_alertWin = { TW_DEF_AM_S, TW_DEF_AM_E, TW_DEF_PM_S, TW_DEF_PM_E };
  g_workdayOnly = 1;
  g_fetchOutside = 0;
  g_alertOn = 1;
  g_alertPct = 3.0f;
  memset(g_alertPer, 0, sizeof(g_alertPer));
  g_gyroOn = 1;
  g_autoSwitchSec = 30;
  g_autoEffMode = DISP_LIST;

  g_nStation = DEF_NSTA;
  for (int i = 0; i < g_nStation; i++) {
    strncpy(g_stations[i].name, DEF_STA_NAME[i], STA_NAME_LEN - 1);
    strncpy(g_stations[i].url, DEF_STA_URL[i], STA_URL_LEN - 1);
  }
  g_curStation = 0;

  g_nStock = sizeof(DEF_STOCKS) / sizeof(DEF_STOCKS[0]);
  for (int i = 0; i < g_nStock; i++)
    strncpy(g_stocks[i].code, DEF_STOCKS[i], STK_CODE_LEN - 1);
  g_curStock = 0;

  cfgSaveLists();
  cfgSaveSettings();
}

void cfgSaveLists() {
  cfgLock();
  prefs.putUChar("cfgver", CFG_VER);
  prefs.putUChar("nsta", g_nStation);
  prefs.putBytes("sta", g_stations, sizeof(g_stations));
  prefs.putUChar("nstk", g_nStock);
  prefs.putBytes("stk", g_stocks, sizeof(g_stocks));
  cfgUnlock();
}

bool quotesLock(uint32_t ms) {
  return xSemaphoreTake(g_quoteMutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}
void quotesUnlock() { xSemaphoreGive(g_quoteMutex); }

// ---------- 编码工具 ----------
#include "font_render.h"

String GBKtoUTF8(const uint8_t* gbk, int len) {
  String out;
  out.reserve(len * 2);
  for (int i = 0; i < len; ) {
    uint8_t c = gbk[i];
    if (c < 0x80) { out += (char)c; i++; continue; }
    if (i + 1 >= len) break;
    uint16_t uni = gbkToUni(((uint16_t)c << 8) | gbk[i + 1]);
    i += 2;
    if (uni == 0) { out += '?'; continue; }
    if (uni < 0x80) out += (char)uni;
    else if (uni < 0x800) {
      out += (char)(0xC0 | (uni >> 6));
      out += (char)(0x80 | (uni & 0x3F));
    } else {
      out += (char)(0xE0 | (uni >> 12));
      out += (char)(0x80 | ((uni >> 6) & 0x3F));
      out += (char)(0x80 | (uni & 0x3F));
    }
  }
  return out;
}

bool utf8Valid(const uint8_t* s, int len) {
  int i = 0;
  while (i < len) {
    uint8_t c = s[i];
    int n = 0;
    if (c < 0x80) n = 1;
    else if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else return false;
    if (i + n > len) return false;
    for (int k = 1; k < n; k++)
      if ((s[i + k] & 0xC0) != 0x80) return false;
    i += n;
  }
  return true;
}

// v7.1: 两套可配置时间范围的统一判断 (闭区间 [起, 止])
//   g_workdayOnly=1 时周六/周日不生效; 时间未同步(NTP还没拿到)按"在时段内"处理,
//   保证首次开机或断网时仍能抓到一次行情, 不至于永远空白。
bool inTimeWindow(const TimeWindow& w) {
  struct tm t;
  if (!getLocalTime(&t, 50)) return true;
  if (g_workdayOnly && (t.tm_wday == 0 || t.tm_wday == 6)) return false;
  int m = t.tm_hour * 60 + t.tm_min;
  return (m >= (int)w.amS && m <= (int)w.amE) ||
         (m >= (int)w.pmS && m <= (int)w.pmE);
}
bool isTradingTime() { return inTimeWindow(g_fetchWin); }
bool isAlertTime()   { return inTimeWindow(g_alertWin); }

// 每只股票的有效阈值: 独立值优先, 0=跟随全局, -1=该股不提醒。
// 全局与每股互不干扰的关键: 触发去重位按股票单独维护(见主循环 s_alertLastVal[])。
float alertThresholdFor(int i) {
  if (i < 0 || i >= MAX_STOCKS) return g_alertPct;
  float v = g_alertPer[i];
  if (v < 0) return -1.0f;
  return (v > 0) ? v : g_alertPct;
}
