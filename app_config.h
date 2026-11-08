#pragma once
#include <Arduino.h>
#include <Preferences.h>

#define MAX_STATIONS  8
#define MAX_STOCKS    30
#define WIFI_PROFILE_N 8   // 可保存的历史 WiFi 组数(>=5), 按 SSID 去重, 最近使用排最前
#define STA_NAME_LEN  40   // utf8 字节 (≈13 汉字, 屏幕单行最多显示 15 汉字)
#define STA_URL_LEN   128
#define STK_CODE_LEN  14

// v7.1: 信息更新/预警提示两套独立时间范围 / 每只股票独立提醒阈值(互不影响)
//       / 修自动关机(M键常态低电平误唤醒) / 自动显示模式改为真正可用的定时切换
//       / 状态栏已连WiFi只留图标 / 完整 merged 固件
// v7.0: 多组 WiFi 凭据自动漫游 / 状态栏 6 项 / 三键操作重设计 / 网页全接口+升级菜单
//       / 出厂默认音量 60 / 网页 OTA 实测可用
// v6.2: 盒子菜单补全设置项 / 自动关机 4 档 / 音量默认30+状态栏显示 / 提示音温和化
// v6.1.1: 修 Web load() 访问已删除的电台控件导致设置项加载不全
// v6.1: 修 ES8311 0x32 音量(本机无声) / 去掉 beep 时 CODEC_PWR 断电毛刺(USB 重枚举)
//       / M 键高电平有效 / 电池官方标定 / I2C 假地址
#define FW_VERSION    "v7.1"

// 电台功能开关: 用户反馈在线电台在本网络下播不出声音, 已停用(2026-11)
//   代码与网页接口保留, 改成 1 重新编译即可恢复
#define ENABLE_RADIO  0

// 新闻条
#define MAX_NEWS      32
#define NEWS_TITLE    116   // utf8 字节(≈38 汉字), 底部两行最多显示 24 字
#define NEWS_SRC      3     // 新闻分类数: 0财经+科技 1财经 2科技

// 股票显示模式
#define DISP_ONE      0    // 单只大屏: 大字现价 + 涨跌 + 开高低额
#define DISP_LIST     1    // 列表模式(默认): 一屏 6 只, 每 g_jumpSec 秒自动翻页
#define DISP_AUTO     2    // 自动: 定时在列表/单只间切换(板上无IMU, 重力不可用)
// 配置结构版本: 结构/长度一改就 +1, cfgLoad 发现不匹配会重置列表,
// 避免旧 NVS 二进制按新结构解读成乱码(中文变"锟斤拷")
#define CFG_VER       8   // 8: 默认自选股对齐实测盒子 14 只(新增 sz399006 创业板指)

// ---- v7.1 时间范围 ----
// 每个窗口 = 上午起/上午止/下午起/下午止, 单位: 当日 0 点起的分钟数(0..1439)
// 默认 9:00-11:30 / 13:00-15:00
struct TimeWindow {
  uint16_t amS, amE, pmS, pmE;
};
#define TW_DEF_AM_S  540    // 9:00
#define TW_DEF_AM_E  690    // 11:30
#define TW_DEF_PM_S  780    // 13:00
#define TW_DEF_PM_E  900    // 15:00

struct Station {
  char name[STA_NAME_LEN];
  char url[STA_URL_LEN];
};

// 多组 WiFi 凭据: 换个地点(家/公司)自动连已存过的网络, 不用重新配网
struct WifiProfile {
  char ssid[33];
  char pass[65];
};

struct StockDef {
  char code[STK_CODE_LEN];   // 新浪格式: sh600000 / sz399001 / rt_hk00700 / gb_aapl
};

struct Quote {
  char     code[STK_CODE_LEN];
  char     name[48];          // utf8
  float    price  = 0;
  float    chg    = 0;       // 涨跌额
  float    chgpct = 0;       // 涨跌幅 %
  float    open = 0, prevClose = 0, high = 0, low = 0;
  double   amount = 0;        // 成交额(元)
  double   volume = 0;        // 成交量(手)
  double   prevVolume = 0;    // 昨日成交量(手), 用于放量/缩量判断
  double   prevAmount = 0;    // 昨日同时刻累计成交额(元), 用于同时段放量对比
  bool     valid = false;
  uint32_t ts = 0;
};

// ---------- 配置 ----------
extern Preferences prefs;
extern char     g_wifiSsid[33];
extern char     g_wifiPass[65];
extern char     g_adminPass[17];
extern WifiProfile g_wifiProfiles[WIFI_PROFILE_N];
extern uint8_t  g_wifiProfileN;

// 历史 WiFi 管理(NVS: wpn + ws0/wp0 ... ws7/wp7)
void cfgWifiLoad();                                  // cfgLoad 内调用, 含旧单组配置迁移
bool cfgWifiAdd(const char* ssid, const char* pass);// 新增/更新并置顶, 超出 8 组淘汰最旧
bool cfgWifiDel(int idx);                            // 删除某组
bool cfgWifiUse(int idx);                            // 选中某组为当前网络并置顶
int  cfgWifiFind(const char* ssid);                  // 按 SSID 查索引, -1=没有
extern uint8_t  g_volume;      // 0-100
extern uint8_t  g_bright;      // 背光亮度 10-100 (%)
extern uint8_t  g_autoOffMin;     // 自动熄屏分钟: 0=不熄屏, 1/3/5/10/30/60/120
extern uint8_t  g_autoPowerOff;   // 1=熄屏后无操作自动关机, 0=关(默认开)
extern uint8_t  g_powerOffMin;    // 熄屏后多少分钟关机: 0=关 / 30 / 60 / 120 / 240
// 自动关机档位(分钟), 盒子菜单与 Web 共用
static const uint16_t POFF_STEPS[] = {0, 30, 60, 120, 240};
#define POFF_N ((int)(sizeof(POFF_STEPS) / sizeof(POFF_STEPS[0])))
static inline const char* poffName(uint16_t m) {
  switch (m) {
    case 30:  return "息屏后30分";
    case 60:  return "息屏后1时";
    case 120: return "息屏后2时";
    case 240: return "息屏后4时";
    default:  return "关闭";
  }
}
extern uint8_t  g_refreshSec;  // 交易时段刷新间隔
extern uint8_t  g_jumpSec;     // 股票轮播间隔(秒)
extern uint8_t  g_displayMode; // DISP_ONE / DISP_LIST
// ---- 新闻条 (功能已停用, 保留符号仅为兼容旧代码) ----
extern uint8_t  g_newsOn;      // 1=底部显示新闻条, 0=不显示
extern uint8_t  g_newsSec;     // 每条新闻停留秒数(3-30)
extern uint16_t g_newsRefSec;  // 重新抓取的间隔秒数(60-3600)
extern uint8_t  g_newsSrc;     // 0=财经+科技 1=只看财经 2=只看科技
// ---- v7.1 两套独立时间范围 ----
extern TimeWindow g_fetchWin;   // 「信息更新」时段: 该时段内按 refresh 秒抓行情
extern TimeWindow g_alertWin;   // 「预警提示」时段: 该时段内才检查涨跌阈值并响铃
extern uint8_t  g_workdayOnly;  // 1=时间范围只在周一~周五生效(默认), 0=每天生效
extern uint8_t  g_fetchOutside; // 0=时段外完全不抓(默认), 1=时段外每 300 秒抓一次

// ---- 涨跌幅提醒 ----
extern uint8_t  g_alertOn;     // 总开关: 1=开启提醒, 0=关
extern float    g_alertPct;    // 全局触发阈值(百分比, 出厂 3.0 对齐实测盒子)
// 每只股票独立阈值: 0=跟随全局, >0=独立阈值(1..20), <0=该股不提醒
// 改全局阈值只重置"跟随全局"的股票去重位; 改某只股票只重置它自己的位, 互不影响。
extern float    g_alertPer[MAX_STOCKS];

// ---- 自动显示模式 ----
extern uint8_t  g_gyroOn;      // 保留: 若以后外接 IMU, 1=优先用重力方向
extern uint8_t  g_autoSwitchSec; // DISP_AUTO 时每 N 秒在列表/单只间切换; 0=不切换
extern uint8_t  g_autoEffMode;   // 运行时实际显示模式(不落盘)
extern Station  g_stations[MAX_STATIONS];
extern uint8_t  g_nStation;
extern int8_t   g_curStation;  // 当前电台索引
extern StockDef g_stocks[MAX_STOCKS];
extern uint8_t  g_nStock;

void cfgLoad();
void cfgSaveSettings();
void cfgSaveLists();
void cfgSaveVolume();          // 标记音量待保存(不立即写 NVS)
void cfgFlushVolume();         // 主循环调用: 音量稳定 1.5 秒后落盘
void cfgSaveCurStation();      // 记住当前电台(带锁, 可从任意任务调用)
void cfgFactoryReset();        // 恢复出厂: 清 NVS 并写回默认值

// ---------- 行情快照 ----------
extern Quote g_quotes[MAX_STOCKS];
extern SemaphoreHandle_t g_quoteMutex;
bool quotesLock(uint32_t ms = 200);
void quotesUnlock();

// ---------- 运行状态 ----------
enum RadioState { RADIO_IDLE, RADIO_CONNECTING, RADIO_PLAYING, RADIO_PAUSED, RADIO_ERROR };
extern volatile RadioState g_radioState;
extern char     g_streamTitle[96];   // ICY 歌曲信息(utf8)
extern volatile bool g_screenOn;
extern volatile int  g_curStock;     // 轮播当前索引
// 工具
String GBKtoUTF8(const uint8_t* gbk, int len);
bool utf8Valid(const uint8_t* s, int len);
// v7.1: 时间范围校验/钳位 + 两套窗口判断
void cfgSanitizeWindow(TimeWindow& w);
bool inTimeWindow(const TimeWindow& w);
bool isTradingTime();          // = inTimeWindow(g_fetchWin), 兼容旧调用点
bool isAlertTime();            // = inTimeWindow(g_alertWin)
float alertThresholdFor(int i);// 每股有效阈值; <0 表示该股禁用提醒
