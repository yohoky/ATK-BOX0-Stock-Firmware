#include "ui_display.h"
#include "cfg_pins.h"
#include "font_render.h"
#include "wifi_mgr.h"
#include "radio_player.h"
#include "news_feed.h"
#include "gyro_imu.h"
#include <WiFi.h>
#include <esp_heap_caps.h>

// ---------- LovyanGFX 板级定义: ST7789 240x240 SPI ----------
class LGFX_BOX0 : public lgfx::LGFX_Device {
  lgfx::Bus_SPI       _bus;
  lgfx::Panel_ST7789  _panel;
  lgfx::Light_PWM     _light;
public:
  LGFX_BOX0() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 8000000;
      cfg.pin_sclk = PIN_LCD_SCLK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc   = PIN_LCD_DC;
      _bus.config(cfg);
    }
    _panel.setBus(&_bus);
    {
      auto cfg = _panel.config();
      cfg.pin_cs   = PIN_LCD_CS;
      cfg.pin_rst  = -1;
      cfg.pin_busy = -1;
      cfg.panel_width  = 240;
      cfg.panel_height = 240;
      cfg.invert = true;      // IPS 屏, 若颜色反相改 false
      cfg.rgb_order = false;  // 若红蓝互换改 true
      cfg.memory_width  = 240;
      cfg.memory_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
    }
    _panel.setLight(&_light);
    setPanel(&_panel);
  }
};

LGFX_BOX0 tft;
static LGFX_Sprite spr(&tft);     // 全帧精灵 240x240 (仅在 PSRAM 可用时使用)
static LGFX_Sprite sprRow(&tft);  // 单行精灵 240x34 (无 PSRAM 时逐行推送, 翻页不闪)
static bool        s_rowSpr = false;

// ============================================================
// 绘制目标自适应
//   整屏 RGB565 精灵 = 115KB。有 PSRAM 走精灵(整帧推送, 不闪);
//   没有 PSRAM 就直接画屏, 但列表模式改用 240x34 的单行精灵逐行推,
//   这样翻页时不会整片闪黑(整屏精灵会吃掉 115KB, WiFi 就没缓冲了)。
// ============================================================
static bool s_sprOk = false;
static inline LovyanGFX& disp() {
  return s_sprOk ? (LovyanGFX&)spr : (LovyanGFX&)tft;
}
static inline void frameClear() {
  if (s_sprOk) spr.fillSprite(C_BG);
  else         tft.fillScreen(C_BG);
}
static inline void frameCommit() {
  if (s_sprOk) { tft.startWrite(); spr.pushSprite(0, 0); tft.endWrite(); }
}
// 把当前行精灵推到屏幕的 y 位置
static inline void pushRow(int y) {
  if (!s_rowSpr) return;
  tft.startWrite();
  tft.pushImage(0, y, 240, UI_LIST_ROW_H, (uint16_t*)sprRow.getBuffer());
  tft.endWrite();
}

static bool s_dirty      = true;
static bool s_radioDirty = true;
static int  s_curStock   = -1;
static uint32_t s_volUntil = 0;
static int  s_lastMin    = -1;
static uint32_t s_lastTouch = 0;

// 涨跌幅提醒弹屏: 触发后强制单只显示该股, 字符跳动 + 边框闪烁
static volatile int      s_alertIdx    = -1;
static volatile uint32_t s_alertUntil  = 0;
static int      s_alertJitter = 0;

void uiShowAlert(int idx, uint32_t ms) {
  s_alertIdx   = idx;
  s_alertUntil = millis() + ms;
  uiSetScreen(true);          // 熄屏时也要点亮给用户看
  s_dirty = true;
}
bool uiAlertActive() { return s_alertIdx >= 0 && millis() < s_alertUntil; }
void uiClearAlert()  { s_alertIdx = -1; s_alertUntil = 0; s_alertJitter = 0; s_dirty = true; }

// 覆盖场景
volatile UiScene g_scene = SCENE_MAIN;
volatile int     g_menuSel = 0;
volatile uint8_t g_menuListN = MENU_N;
volatile int     g_stationSel = 0;
volatile int     g_newsSel = 0;   // 新闻子页光标

// 菜单: 一次最多显示几项(超过则滚动)
#define MENU_ROWS  6
#define MENU_ROW_H 23

// toast (跨任务: 用自旋锁保护写入)
static portMUX_TYPE s_tMux = portMUX_INITIALIZER_UNLOCKED;
static char     s_toastMsg[72] = "";
static uint16_t s_toastCol = 0xFFFF;
static volatile uint32_t s_toastUntil = 0;

// ---------------- 菜单项 ----------------
static const char* MENU_NAME[MENU_N] = {
  "显示模式", "轮播间隔", "刷新间隔", "涨跌提醒", "自动关机", "亮度",
  "音量", "自动熄屏", "测试提示音", "设备信息", "恢复出厂", "关机"
};

// GB2312 字库里有"涨"没有"跌"? 有的(GB2312 一级字库). 
// (电台停用后这两个函数无人调用, 用 ENABLE_RADIO 包起来避免 -Wunused-function)
#if ENABLE_RADIO
static const char* radioStateCn() {
  switch (g_radioState) {
    case RADIO_PLAYING:    return "播放";
    case RADIO_CONNECTING: return "连接";
    case RADIO_PAUSED:     return "暂停";
    case RADIO_ERROR:      return "失败";
    default:               return "停止";
  }
}

static uint16_t radioStateColor() {
  switch (g_radioState) {
    case RADIO_PLAYING:    return C_DOWN;
    case RADIO_CONNECTING: return C_WARN;
    case RADIO_PAUSED:     return C_ACCENT;
    case RADIO_ERROR:      return C_UP;
    default:               return C_FLAT;
  }
}
#endif

static const char* menuValue(int i, char* buf, int n) {
  switch (i) {
    case MENU_DISP:
      return g_displayMode == DISP_LIST ? "列表" :
             g_displayMode == DISP_ONE  ? "单只" : "定时";
    case MENU_JUMP:     snprintf(buf, n, "%d 秒", g_jumpSec);    return buf;
    case MENU_REFRESH:  snprintf(buf, n, "%d 秒", g_refreshSec); return buf;
    case MENU_ALERT:
      snprintf(buf, n, "%s %.0f%%", g_alertOn ? "开" : "关", g_alertPct); return buf;
    case MENU_APOW:     snprintf(buf, n, "%s", poffName(g_powerOffMin)); return buf;
    case MENU_BRIGHT:   snprintf(buf, n, "%d%%", g_bright);      return buf;
    case MENU_VOL:      snprintf(buf, n, "%d", g_volume);        return buf;
    case MENU_OFF:
      if (g_autoOffMin) snprintf(buf, n, "%d 分", g_autoOffMin);
      else              snprintf(buf, n, "常亮");
      return buf;
    default: buf[0] = 0; return buf;
  }
}

// ---------------- 常见代码 -> 名称 兜底表 ----------------
// 行情没抓到 / 名称解析失败时, 至少还能显示中国人看得懂的名字,
// 而不是 sh600519 这种代码(用户反馈"没有显示股票的名称"就是这里)。
struct NameEnt { const char* code; const char* name; };
static const NameEnt s_nameTbl[] = {
  {"sh000001", "上证指数"}, {"sz399001", "深证成指"}, {"sz399006", "创业板指"},
  {"sh000300", "沪深300"},  {"sh000688", "科创50"},   {"sz399905", "中证500"},
  {"sh600519", "贵州茅台"}, {"sz300750", "宁德时代"}, {"sh601318", "中国平安"},
  {"sz000858", "五粮液"},   {"sh600036", "招商银行"}, {"sz000001", "平安银行"},
  {"sh601899", "紫金矿业"}, {"sh600900", "长江电力"}, {"sz002594", "比亚迪"},
  {"sh688981", "中芯国际"}, {"sz300059", "东方财富"}, {"sh601012", "隆基绿能"},
  {"sz000333", "美的集团"}, {"sz000651", "格力电器"}, {"sz000725", "京东方"},
  {"sh601988", "中国银行"}, {"sh601857", "中国石油"}, {"sh600030", "中信证券"},
  {"sz002415", "海康威视"}, {"sh600276", "恒瑞医药"}, {"sh601166", "兴业银行"},
  {"sh600000", "浦发银行"}, {"sz002230", "科大讯飞"}, {"sh603259", "药明康德"},
  {"rt_hk00700", "腾讯控股"}, {"rt_hk09988", "阿里巴巴"}, {"rt_hk03690", "美团"},
  {"rt_hk01810", "小米集团"}, {"rt_hk00941", "中国移动"},
  {"gb_aapl", "苹果"}, {"gb_tsla", "特斯拉"}, {"gb_nvda", "英伟达"}, {"gb_msft", "微软"},
  // 当前默认自选股(用户 2026-11-06 指定)
  {"sh603993", "洛阳钼业"}, {"sz000783", "长江证券"}, {"sz000977", "浪潮信息"},
  {"sz000938", "紫光股份"}, {"sz002670", "国盛证券"}, {"sz159841", "证券ETF"},
  {"sh512980", "传媒ETF"}, {"sh563020", "红利低波ETF"}, {"sz002657", "中科金财"},
  {"sz300215", "电科院"}, {"sz002439", "启明星辰"},
};
static const char* nameFallback(const char* code) {
  for (size_t i = 0; i < sizeof(s_nameTbl) / sizeof(s_nameTbl[0]); i++)
    if (strcmp(s_nameTbl[i].code, code) == 0) return s_nameTbl[i].name;
  return nullptr;
}

// 行情快照(一次锁, 供整页使用)
static Quote s_snap[MAX_STOCKS];
static int   s_snapN = 0;

static void snapshotQuotes() {
  if (!quotesLock()) return;
  int n = g_nStock;
  if (n > MAX_STOCKS) n = MAX_STOCKS;
  if (n > 0) memcpy(s_snap, g_quotes, sizeof(Quote) * n);
  s_snapN = n;
  quotesUnlock();
}

// 名称 / 现价 / 涨跌幅 取用: 有行情优先行情, 否则兜底表, 再否则代码
struct RowView {
  char  name[48];
  char  code[16];
  bool  valid;
  float price, chg, pct, open, prev, hi, lo;
  double amt, volume, prevVolume, prevAmount;
};

static void rowOf(int i, RowView& v) {
  memset(&v, 0, sizeof(v));
  if (i < 0 || i >= g_nStock) return;
  strncpy(v.code, g_stocks[i].code, 15);

  const Quote* q = (i < s_snapN) ? &s_snap[i] : nullptr;
  bool valid = q && q->valid;
  v.valid = valid;
  if (valid) {
    v.price = q->price; v.chg = q->chg; v.pct = q->chgpct;
    v.open = q->open;   v.prev = q->prevClose;
    v.hi = q->high;     v.lo = q->low;  v.amt = q->amount;
    v.volume = q->volume; v.prevVolume = q->prevVolume;
    v.prevAmount = q->prevAmount;
  }
  // 名称里出现 '?' = GBK 解码失败(或接口给了非法字节), 这种情况宁可用本地
  // 兜底表, 也不要让用户看到 "????"(v4.0 屏幕上的实况)
  bool nameOk = valid && q->name[0] && (strchr(q->name, '?') == nullptr);
  if (nameOk) {
    strncpy(v.name, q->name, 47);
  } else {
    const char* fb = nameFallback(v.code);
    strncpy(v.name, fb ? fb : v.code, 47);
  }
}

static void fmtPrice(char* out, int n, float p) {
  if (p >= 10000.0f)     snprintf(out, n, "%.0f", p);
  else if (p >= 100.0f)  snprintf(out, n, "%.2f", p);
  else                   snprintf(out, n, "%.2f", p);
}

// ---------------- 通用: 顶部标题条 ----------------
static void drawTitle(const char* t, const char* right = nullptr) {
  disp().fillRect(0, 0, 240, 22, C_DIM);
  drawCN(disp(), 6, 3, t, C_FG, C_DIM);
  if (right) {
    int w = cnTextWidth(right);
    drawCN(disp(), 234 - w, 3, right, C_FLAT, C_DIM);
  }
}

int uiPageCount() {
  int rows = UI_LIST_ROWS;
  return (g_nStock + rows - 1) / rows;
}

// 当前实际渲染模式: DISP_AUTO 时按重力方向解析成单只/列表
// v7.1: 导出给主循环共用。板上实测无 IMU 时, DISP_AUTO 由 g_autoEffMode
//   (主循环按 g_autoSwitchSec 定时在列表/单只间切换)决定 —— 旧版这里永远返回
//   DISP_LIST, 所以"自动切换显示模式"一直是死的。
uint8_t uiEffectiveMode() {
  if (g_displayMode != DISP_AUTO) return g_displayMode;
  if (g_gyroOn && gyroPresent()) {
    GyroOrientation o = gyroRead();
    if (o != GYRO_UNKNOWN) return (o == GYRO_LANDSCAPE) ? DISP_LIST : DISP_ONE;
  }
  return g_autoEffMode;
}

// ---------------- 无网引导/失败页 ----------------
static bool hasAnyQuote() {
  bool any = false;
  if (quotesLock()) {
    for (int i = 0; i < g_nStock; i++) if (g_quotes[i].valid) { any = true; break; }
    quotesUnlock();
  }
  return any;
}

static void drawNetHint() {
  disp().fillRect(0, 19, 240, 173, C_BG);
  if (strlen(g_wifiSsid) == 0) {
    drawCN(disp(), 4, 26, "配网向导", C_ACCENT);
    drawCN(disp(), 4, 50, "1 手机连热点", C_FG);
    drawCN(disp(), 12, 68, "BOX0-Setup", C_WARN);
    drawCN(disp(), 4, 90, "2 浏览器打开", C_FG);
    drawCN(disp(), 12, 108, "192.168.4.1", C_WARN);
    drawCN(disp(), 4, 130, "3 选家里 WiFi", C_FG);
    drawCN(disp(), 4, 148, "  输密码保存", C_FG);
    char m[48];
    snprintf(m, sizeof(m), "内存%uK PSRAM%uM", (unsigned)(ESP.getFreeHeap() >> 10),
             (unsigned)(ESP.getPsramSize() >> 20));
    drawCN(disp(), 4, 172, m, C_FLAT);
  } else {
    drawCN(disp(), 4, 24, "未连上路由器", C_WARN);
    char l[96];
    snprintf(l, sizeof(l), "名称: %s", g_wifiSsid);
    drawCN(disp(), 4, 46, l, C_FG);
    if (g_wifiRc) {
      snprintf(l, sizeof(l), "原因: %s", wifiRcText(g_wifiRc));
      drawCNWrap(disp(), 4, 66, 232, l, C_WARN, C_BG, 18);
    } else {
      drawCN(disp(), 4, 66, "原因: 连接超时", C_FLAT);
    }
    drawCN(disp(), 4, 108, "左右同按 = 菜单", C_FG);
    drawCN(disp(), 4, 130, "手机连 BOX0-Setup", C_FG);
    drawCN(disp(), 4, 148, "打开 192.168.4.1", C_FG);
  }
}

// ---------------- 列表模式: 一屏 5 只 (对齐用户给的行情软件样式) ----------------
//   ┌──────────────────────────────┐
//   │▌ 上证指数        3686.17 +2.16%│
//   │▌ 深证成指       11336.00 +4.22%│
//   名称左对齐(16px) / 现价右对齐(12pt 粗, 红涨绿跌) / 涨跌幅最右(9pt)
//   行首 3px 色条 = 涨跌色, 与截图里的左侧竖线一致
static void drawStockRowTo(LovyanGFX& g, int i, int y) {
  RowView v;
  rowOf(i, v);

  uint16_t cc = (v.chg > 0) ? C_UP : (v.chg < 0 ? C_DOWN : C_FLAT);
  if (!v.valid) cc = C_FLAT;

  // 行首涨跌色条
  g.fillRect(0, y + 4, 3, 26, cc);

  // 名称: 固定 4 个字(每个 16px = 64px), 多数股票名就是 4 字, 超出截断
  //   ★ cnEllipsis 内部预留 16px 省略号, 传 64 会只剩 3 字; 传 80 才能放满 4 字
  char nm[48];
  cnEllipsis(nm, sizeof(nm), v.name, 80);
  drawCN(g, 5, y + 9, nm, C_FG, C_BG);

  // 现价: 靠左对齐, 从名字后约一个字符处开始(x=80), 与名字隔开
  char ps[20];
  if (v.valid) fmtPrice(ps, sizeof(ps), v.price);
  else         strcpy(ps, "--");
  g.setFont(&fonts::FreeSansBold12pt7b);
  g.setTextDatum(lgfx::middle_left);
  g.setTextColor(cc, C_BG);
  g.drawString(ps, 80, y + 17);

  // 涨跌幅: 靠右对齐到 x=236, 与现价之间留足空隙(现价最宽约 90px 到 x=170)
  char cs[20];
  if (v.valid) snprintf(cs, sizeof(cs), "%+.2f%%", v.pct);
  else         strcpy(cs, "--");
  g.setFont(&fonts::FreeSansBold9pt7b);
  g.setTextDatum(lgfx::middle_right);
  g.drawString(cs, 236, y + 17);
  g.setTextDatum(lgfx::top_left);

  // 分隔线: 只在行间, 浅到几乎看不见, 保持截图那种"整块清爽"的感觉
  if (i + 1 < g_nStock) g.fillRect(5, y + UI_LIST_ROW_H - 1, 230, 1, C_LINE);
}

static void drawStockRow(int i, int y) {
  if (s_rowSpr && !s_sprOk) {
    sprRow.fillSprite(C_BG);          // 整行重画, 不留残影
    drawStockRowTo((LovyanGFX&)sprRow, i, 0);
    pushRow(y);
  } else {
    drawStockRowTo(disp(), i, y);
  }
}

// 空行(该页股票不足 5 只时)整行刷黑
static void clearStockRow(int y) {
  if (s_rowSpr && !s_sprOk) {
    sprRow.fillSprite(C_BG);
    pushRow(y);
  } else {
    disp().fillRect(0, y, 240, UI_LIST_ROW_H, C_BG);
  }
}

static void drawStockList() {
  if (g_nStock == 0) {
    disp().fillRect(0, 19, 240, 173, C_BG);
    drawCN(disp(), 12, 60, "自选股为空", C_FLAT);
    drawCN(disp(), 12, 82, "请网页配置代码", C_FLAT);
    return;
  }
  snapshotQuotes();

  int page = g_curStock / UI_LIST_ROWS;
  int pages = uiPageCount();
  if (page >= pages) page = pages - 1;
  if (page < 0) page = 0;
  int first = page * UI_LIST_ROWS;

  for (int r = 0; r < UI_LIST_ROWS; r++) {
    int i = first + r;
    int y = UI_LIST_Y0 + r * UI_LIST_ROW_H;
    if (i < g_nStock) drawStockRow(i, y);
    else              clearStockRow(y);       // 翻到最后一页时清掉上一页的残留
  }
  // 列表区顶部 1px 缝隙(19..20) + 底部到屏幕底的缝隙(237..239)
  if (s_rowSpr && !s_sprOk) {
    disp().fillRect(0, 19, 240, 2, C_BG);
    disp().fillRect(0, 237, 240, 3, C_BG);
  }
}

// ---------------- 单只大屏 ----------------
static void drawStockMain(int idx) {
  disp().fillRect(0, 19, 240, 220, C_BG);   // 19..238 (底部不再有新闻条, 主区占满)
  if (g_nStock == 0) {
    drawCN(disp(), 12, 60, "自选股为空", C_FLAT);
    drawCN(disp(), 12, 82, "请网页配置代码", C_FLAT);
    return;
  }
  if (idx < 0) idx = 0;
  if (idx >= g_nStock) idx = g_nStock - 1;
  snapshotQuotes();

  RowView v;
  rowOf(idx, v);

  // 名称 + 序号
  char nm[48];
  cnEllipsis(nm, sizeof(nm), v.name, 150);
  drawCN(disp(), 8, 22 + s_alertJitter, nm, C_FG, C_BG);
  char pos[12];
  snprintf(pos, sizeof(pos), "%d/%d", idx + 1, g_nStock);
  drawCN(disp(), 236 - cnTextWidth(pos), 22, pos, C_FLAT, C_BG);
  drawCN(disp(), 8, 40, v.code, C_FLAT, C_BG);
  disp().fillRect(0, 58, 240, 1, C_LINE);

  if (!v.valid) {
    drawCN(disp(), 16, 90, "行情获取中...", C_WARN);
    if (g_wifiState != WIFI_CONNECTED) drawCN(disp(), 16, 114, "(等待网络)", C_FLAT);
    return;
  }

  uint16_t cc = (v.chg > 0) ? C_UP : (v.chg < 0 ? C_DOWN : C_FLAT);

  // 现价: 24pt 超大数字(单只大屏主打, 醒目)
  char ps[20];
  fmtPrice(ps, sizeof(ps), v.price);
  disp().setFont(&fonts::FreeSansBold24pt7b);
  disp().setTextDatum(lgfx::middle_left);
  disp().setTextColor(cc, C_BG);
  disp().drawString(ps, 8, 92 + s_alertJitter);
  disp().setTextDatum(lgfx::top_left);

  // 涨跌额 + 涨跌幅 (12pt 加粗, 亮色)
  char cs[32];
  snprintf(cs, sizeof(cs), "%+.2f  %+.2f%%", v.chg, v.pct);
  disp().setFont(&fonts::FreeSansBold12pt7b);
  disp().setTextColor(cc, C_BG);
  disp().drawString(cs, 8, 128);
  disp().setTextDatum(lgfx::top_left);

  // 开/高 一行, 低/昨收 一行: 用亮色(C_FG)
  // ★ 这些行含中文("开""高""低""昨收"), 必须用 drawCN(点阵), 不能用 drawString(英文字体)
  char b[48];
  snprintf(b, sizeof(b), "开 %.2f  高 %.2f", v.open, v.hi);
  drawCN(disp(), 8, 156, b, C_FG, C_BG);
  snprintf(b, sizeof(b), "低 %.2f  昨收 %.2f", v.lo, v.prev);
  drawCN(disp(), 8, 174, b, C_FG, C_BG);

  // 成交额 + 同时段放量对比 (只写"放"字, +正放量 -缩量, 单位亿)
  if (v.amt > 0) {
    char amt[32];
    snprintf(amt, sizeof(amt), "%.2f亿", v.amt / 1e8);
    // 放量差值 = 今日成交额 - 昨日同时刻成交额(单位亿)
    char volSign = ' ';
    double diffE = 0;
    if (v.prevAmount > 0) {
      diffE = (v.amt - v.prevAmount) / 1e8;
      if (diffE >= 0.01)      volSign = '+';   // 放量
      else if (diffE <= -0.01) volSign = '-';  // 缩量
      else                     volSign = ' ';  // 基本持平
    }
    uint16_t vc = (volSign == '+') ? C_UP : (volSign == '-' ? C_DOWN : C_FLAT);
    char line[48];
    if (volSign != ' ') {
      snprintf(line, sizeof(line), "额 %s  放%+.2f亿", amt, diffE);
    } else {
      snprintf(line, sizeof(line), "额 %s", amt);
    }
    drawCN(disp(), 8, 196, line, C_FG, C_BG);
  } else {
    drawCN(disp(), 8, 196, "额 --", C_FLAT, C_BG);
  }
}

// ---------------- 状态栏 ----------------
// v7.0: 固定 6 项 —— 星期 / 时间 / 第几页 / 音量 / 电量 / WiFi信号(模式)
static void drawStatusBar() {
  disp().fillRect(0, 0, 240, 19, C_BG);
  struct tm t;
  char tbuf[8]  = "--:--";
  char wbuf[12] = "周-";
  if (getLocalTime(&t, 50)) {
    static const char* WD[] = {"日", "一", "二", "三", "四", "五", "六"};
    int wd = (t.tm_wday < 0 || t.tm_wday > 6) ? 0 : t.tm_wday;
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", t.tm_hour, t.tm_min);
    snprintf(wbuf, sizeof(wbuf), "周%s", WD[wd]);
    if (t.tm_min != s_lastMin) s_lastMin = t.tm_min;
  }

  // 1 星期   2 时间
  drawCN(disp(), 3, 1, wbuf, C_FG);
  drawCN(disp(), 42, 1, tbuf, C_FG);

  // 3 第几页(列表=页/总页, 单只=第几只/总只数)
  {
    char pg[12];
    if (uiEffectiveMode() == DISP_LIST) {
      int pages = uiPageCount();
      int page = g_curStock / UI_LIST_ROWS;
      if (page >= pages) page = pages - 1;
      if (page < 0) page = 0;
      snprintf(pg, sizeof(pg), "%d/%d", page + 1, pages > 0 ? pages : 1);
    } else {
      snprintf(pg, sizeof(pg), "%d/%d", (int)g_curStock + 1, (int)g_nStock);
    }
    drawCN(disp(), 88, 1, pg, C_ACCENT);
  }

  // 4 音量: 喇叭图标 + 数字(替代原来的 V60)
  {
    int vx = 126;
    uint16_t vc = g_volume ? C_ACCENT : C_FLAT;
    disp().fillRect(vx,     7, 2, 6,  vc);
    disp().fillRect(vx + 2, 5, 2, 10, vc);
    disp().fillRect(vx + 4, 3, 2, 14, vc);
    char vb[8];
    snprintf(vb, sizeof(vb), "%d", g_volume);
    drawCN(disp(), vx + 9, 1, vb, vc);
  }

  // 5 电量: 百分比数字画在电池图标内部; 60-100 绿色, 20-59 橙, <20 红
  {
    static uint32_t s_batAt = 0;
    static int s_batPct = -1;
    if (s_batPct < 0 || millis() - s_batAt > 5000) {
      s_batAt = millis();
      s_batPct = batPctFromRaw(analogRead(PIN_BAT_VSEN));
    }
    bool charging = (digitalRead(PIN_CHG_STATUS) == LOW);
    uint16_t bc = (s_batPct >= 60) ? C_DOWN :
                  (s_batPct >= 20) ? C_WARN : C_UP;
    int bx = 166, by = 3, bw = 28, bh = 12;
    disp().drawRect(bx, by, bw, bh, bc);
    disp().fillRect(bx + bw, by + 3, 2, bh - 6, charging ? C_ACCENT : bc);  // 正极: 青=充电中
    disp().fillRect(bx + 2, by + 2, bw - 4, bh - 4, C_DIM);
    disp().fillRect(bx + 2, by + bh - 4, (bw - 4) * s_batPct / 100, 2, bc);
    char bb[8];
    snprintf(bb, sizeof(bb), "%d", s_batPct);
    disp().setFont(&fonts::Font0);
    disp().setTextDatum(lgfx::middle_center);
    disp().setTextColor(C_FG, C_DIM);
    disp().drawString(bb, bx + bw / 2, by + bh / 2 - 1);
    disp().setTextDatum(lgfx::top_left);
  }

  // 6 WiFi: 缩小的 3 格信号 + 模式字(连=已连路由 / 中=连接中 / 热=热点模式)
  {
    int wx = 206;
    bool ok = (g_wifiState == WIFI_CONNECTED);
    int rssi = ok ? WiFi.RSSI() : -100;
    uint16_t sc = ok ? (rssi > -70 ? C_ACCENT : C_WARN) : C_WARN;
    for (int i = 0; i < 3; i++) {
      int h = 3 + i * 4;
      uint16_t c = (ok && rssi > (-88 + i * 12)) ? sc : 0x2120;
      disp().fillRect(wx + i * 4, 16 - h, 3, h, c);
    }
    // v7.1: 正常连接只显示信号格图标, 不再画"连"字; 其它模式仍用文字提示
    if (!ok) {
      const char* mode = (g_wifiState == WIFI_CONNECTING) ? "中" : "热";
      drawCN(disp(), wx + 14, 1, mode, sc);
    }
  }
}

// ---------------- 底部新闻条 ----------------
// 取 src 前 maxW 像素, 写入 out, 返回消耗的字节数(按 utf8 边界, 不会切断汉字)
static int utf8Take(char* out, int outSz, const char* src, int maxW) {
  const uint8_t* p = (const uint8_t*)src;
  int w = 0, o = 0;
  while (*p && o < outSz - 5) {
    uint8_t c = *p;
    int n = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
    int cw = (c < 0x80) ? 8 : 16;
    if (w + cw > maxW) break;
    for (int k = 0; k < n && p[k]; k++) out[o++] = (char)p[k];
    w += cw; p += n;
  }
  out[o] = 0;
  return (int)((const char*)p - src);
}

static void drawBottomBar() {
  // 新闻条已停用: 底部只保留音量条预览(调音量时盖在列表最下一行之上)
  if (s_volUntil > millis()) {
    int w = g_volume * 2;
    disp().fillRect(10, 216, 200, 8, 0x2120);
    disp().fillRect(10, 216, w, 8, C_ACCENT);
    char vb[16]; snprintf(vb, sizeof(vb), "音量 %d", g_volume);
    drawCN(disp(), 10, 194, vb, C_FG, C_DIM, true);
  }
}

// ---------------- 覆盖场景 ----------------
static void drawMenu() {
  disp().fillRect(0, 0, 240, 240, C_BG);
  bool adj = (g_scene == SCENE_ADJ);
  drawTitle(adj ? "调节" : "菜单", adj ? "M键完成" : "M长按退出");

  if (adj) {
    int sel = g_menuSel;
    const char* nm = (sel >= 0 && sel < MENU_N) ? MENU_NAME[sel] : "";
    drawCN(disp(), 8, 44, nm, C_FLAT);
    if (sel == MENU_OFF) {
      // 自动熄屏分钟档(含中文, 一律用 drawCN 点阵字, 不能用英文字体)
      drawCN(disp(), 8, 70, g_autoOffMin ? "自动熄屏已开启" : "不自动熄屏",
             g_autoOffMin ? C_DOWN : C_FLAT);
      char b[32];
      if (g_autoOffMin) snprintf(b, sizeof(b), "%d 分钟", g_autoOffMin);
      else              snprintf(b, sizeof(b), "常亮");
      drawCN(disp(), 92, 104, b, C_ACCENT, C_BG);
      drawCN(disp(), 20, 148, "左/右键 切换档位", C_FLAT);
      drawCN(disp(), 20, 172, "0/1/3/5/10/30/60/120分", C_FLAT);
      drawCN(disp(), 20, 196, "M键完成 长按回主屏", C_FLAT);
      return;
    }
    if (sel == MENU_APOW) {
      // 自动关机: 关闭 / 息屏后 30分 / 1时 / 2时 / 4时, 当前档高亮
      drawCN(disp(), 8, 64, "息屏后多久自动关机", C_FLAT);
      int y = 88;
      for (int i = 0; i < POFF_N; i++) {
        bool on = (POFF_STEPS[i] == g_powerOffMin);
        if (on) disp().fillRect(4, y - 3, 232, 24, C_SEL);
        drawCN(disp(), 12, y, poffName(POFF_STEPS[i]),
               on ? C_BG : C_FG, on ? C_SEL : C_BG);
        y += 26;
      }
      drawCN(disp(), 12, 220, "左右选档 M完成 长按回主屏", C_FLAT);
      return;
    }
    if (sel == MENU_ALERT) {
      // 涨跌提醒: 显示开关状态 + 阈值进度条 + 大数字
      drawCN(disp(), 8, 70, g_alertOn ? "提醒已开启" : "提醒已关闭", g_alertOn ? C_DOWN : C_FLAT);
      int v = (int)(g_alertPct * 10);          // 0.1% 精度, 1.0%->10, 20%->200
      disp().fillRect(20, 96, 200, 22, 0x2120);
      disp().fillRect(20, 96, (int)((g_alertPct - 1.0f) * 200 / 19.0f), 22, C_SEL);
      char b[24];
      snprintf(b, sizeof(b), "%.1f%%", g_alertPct);
      disp().setFont(&fonts::FreeSansBold18pt7b);
      disp().setTextDatum(lgfx::middle_center);
      disp().setTextColor(C_FG, C_BG);
      disp().drawString(b, 120, 140);
      disp().setTextDatum(lgfx::top_left);
      drawCN(disp(), 20, 170, "左/右键 调全局阈值", C_FLAT);
      drawCN(disp(), 20, 194, "M键=开关 长按回主屏", C_FLAT);
      int perN = 0;
      for (int i = 0; i < g_nStock; i++) if (g_alertPer[i] != 0) perN++;
      char pb[32];
      snprintf(pb, sizeof(pb), "独立阈值%d只 网页可设", perN);
      drawCN(disp(), 20, 216, pb, C_ACCENT);
      return;
    }
    int v, vmax;
    if      (sel == MENU_BRIGHT)  { v = g_bright;     vmax = 100; }
    else if (sel == MENU_VOL)     { v = g_volume;     vmax = 100; }
    else if (sel == MENU_JUMP)    { v = g_jumpSec;    vmax = 60;  }
    else                          { v = g_refreshSec; vmax = 120; } // MENU_REFRESH
    disp().fillRect(20, 80, 200, 22, 0x2120);
    disp().fillRect(20, 80, v * 200 / (vmax ? vmax : 100), 22, C_SEL);
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    disp().setFont(&fonts::FreeSansBold18pt7b);
    disp().setTextDatum(lgfx::middle_center);
    disp().setTextColor(C_FG, C_BG);
    disp().drawString(b, 120, 124);
    disp().setTextDatum(lgfx::top_left);
    drawCN(disp(), 20, 160, "左键 -   右键 +", C_FLAT);
    drawCN(disp(), 20, 184,
           sel == MENU_REFRESH ? "3~120秒 M键完成" : "调好后按M完成", C_FLAT);
    return;
  }

  int top = 0;
  if (g_menuSel >= MENU_ROWS) top = g_menuSel - MENU_ROWS + 1;
  if (top > MENU_N - MENU_ROWS) top = MENU_N - MENU_ROWS;
  if (top < 0) top = 0;

  int y = 28;
  for (int k = 0; k < MENU_ROWS; k++) {
    int i = top + k;
    if (i >= MENU_N) break;
    bool sel = (i == g_menuSel);
    uint16_t bgc = sel ? C_SEL : C_BG;
    if (sel) disp().fillRect(4, y - 3, 232, MENU_ROW_H, C_SEL);
    drawCN(disp(), 10, y, MENU_NAME[i], sel ? C_BG : C_FG, bgc);
    char vb[16];
    const char* v = menuValue(i, vb, sizeof(vb));
    if (v[0]) {
      int w = cnTextWidth(v);
      drawCN(disp(), 230 - w, y, v, sel ? C_BG : C_ACCENT, bgc);
    }
    y += MENU_ROW_H;
  }

  if (MENU_N > MENU_ROWS) {
    int trackY = 26, trackH = MENU_ROWS * MENU_ROW_H;
    int barH = trackH * MENU_ROWS / MENU_N;
    int barY = trackY + (trackH - barH) * top / (MENU_N - MENU_ROWS);
    disp().fillRect(237, trackY, 2, trackH, 0x2120);
    disp().fillRect(237, barY, 2, barH, C_ACCENT);
  }

  drawCN(disp(), 6, 178, "左右选 M确定 双键返回", C_FLAT);
  drawCN(disp(), 6, 200, "M长按回主界面 设置自动保存", C_FLAT);
}

static void drawConfirm() {
  disp().fillRect(0, 0, 240, 240, C_BG);
  drawTitle("请确认");
  bool isReset = (g_menuSel == MENU_RESET);
  drawCN(disp(), 20, 44, isReset ? "恢复出厂设置?" : "关闭电源?", C_WARN);
  if (isReset) {
    drawCN(disp(), 20, 76, "将清除: WiFi密码", C_FG);
    drawCN(disp(), 20, 96, "自选股 / 电台", C_FG);
    drawCN(disp(), 20, 116, "管理密码(回默认)", C_FG);
    drawCN(disp(), 20, 146, "中键/双键 确认清除", C_UP);
  } else {
    drawCN(disp(), 20, 76, "电池供电将直接断电", C_FG);
    drawCN(disp(), 20, 96, "USB 供电进入深睡", C_FG);
    drawCN(disp(), 20, 116, "按电源键可再开机", C_FLAT);
    drawCN(disp(), 20, 146, "中键/双键 确认关机", C_UP);
  }
  drawCN(disp(), 20, 196, "左/右键取消", C_FLAT);
}

// ---------------- 新闻子页 ----------------
static void drawNewsScene() {
  disp().fillRect(0, 0, 240, 240, C_BG);
  drawTitle("新闻要闻", "中键返回");

  if (g_nNews == 0) {
    drawCN(disp(), 12, 70, "还没有新闻", C_FLAT);
    drawCN(disp(), 12, 92, "等联网后自动抓取", C_FLAT);
    drawCN(disp(), 12, 130, "网页可设 财经/科技", C_FLAT);
    return;
  }
  if (g_newsSel >= g_nNews) g_newsSel = g_nNews - 1;
  if (g_newsSel < 0) g_newsSel = 0;

  const int ROWS = 7, RH = 24, Y0 = 26;
  int top = 0;
  if (g_newsSel >= ROWS) top = g_newsSel - ROWS + 1;
  if (g_newsSel < top) top = g_newsSel;
  if (top > g_nNews - ROWS) top = g_nNews - ROWS;
  if (top < 0) top = 0;

  for (int k = 0; k < ROWS; k++) {
    int i = top + k;
    if (i >= g_nNews) break;
    int y = Y0 + k * RH;
    bool sel = (i == g_newsSel);
    uint16_t bgc = sel ? C_SEL : C_BG;
    if (sel) disp().fillRect(0, y - 3, 240, RH, C_SEL);

    bool fin = (g_news[i].src == 0);
    drawCN(disp(), 4, y + 3, fin ? "财" : "科",
           fin ? C_WARN : C_ACCENT, bgc, true);

    char t[96];
    cnEllipsis(t, sizeof(t), g_news[i].title, 206);
    drawCN(disp(), 24, y + 3, t, sel ? C_BG : C_FG, bgc, true);
  }

  drawCN(disp(), 4, 206, "左右翻页  中键=定位到这条", C_FLAT);
  char b[24];
  snprintf(b, sizeof(b), "%d/%d", g_newsSel + 1, (int)g_nNews);
  drawCN(disp(), 4, 224, b, C_DIM, C_BG, true);
}

static void drawInfo() {
  disp().fillRect(0, 0, 240, 240, C_BG);
  drawTitle("设备信息", "任意键返回");

  char b[72];
  int y = 26;
  drawCN(disp(), 6, y, "固件 " FW_VERSION, C_ACCENT); y += 18;

  snprintf(b, sizeof(b), "网络 %s", wifiStateText());
  drawCN(disp(), 6, y, b, g_wifiState == WIFI_CONNECTED ? C_FG : C_WARN); y += 18;

  if (g_wifiState == WIFI_CONNECTED) {
    snprintf(b, sizeof(b), "IP %s  %ddBm", g_wifiIp, WiFi.RSSI());
    drawCN(disp(), 6, y, b, C_FG); y += 18;
  } else if (strlen(g_wifiSsid)) {
    char raw[64], cut[64];
    snprintf(raw, sizeof(raw), "目标 %s", g_wifiSsid);
    cnEllipsis(cut, sizeof(cut), raw, 228);
    drawCN(disp(), 6, y, cut, C_FLAT); y += 18;
  }

#if ENABLE_RADIO
  snprintf(b, sizeof(b), "音频 %s%s", g_codecOk ? "ES8311" : "未应答",
           g_codecOk ? (g_radioHasAudio ? " 有数据" : " 无数据") : "(无声)");
  drawCN(disp(), 6, y, b, g_codecOk ? C_FLAT : C_WARN); y += 18;

  snprintf(b, sizeof(b), "码率 %u kbps 播%us",
           (unsigned)(g_radioBitrate / 1000), (unsigned)g_radioSec);
  drawCN(disp(), 6, y, b, C_FLAT); y += 18;
#else
  int perN = 0;
  for (int i = 0; i < g_nStock; i++) if (g_alertPer[i] != 0) perN++;
  snprintf(b, sizeof(b), "提醒%s %.1f%% 独立%d只",
           g_alertOn ? "开" : "关", g_alertPct, perN);
  drawCN(disp(), 6, y, b, C_FLAT); y += 18;
  // v7.1: 信息页直接显示两套时间范围, 便于不连网页也能核对
  snprintf(b, sizeof(b), "更新%u:%02u-%u:%02u/%u-%u",
           g_fetchWin.amS / 60, g_fetchWin.amS % 60,
           g_fetchWin.amE / 60, g_fetchWin.amE % 60,
           g_fetchWin.pmS / 60, g_fetchWin.pmE / 60);
  drawCN(disp(), 6, y, b, C_FG); y += 18;
  snprintf(b, sizeof(b), "预警%u:%02u-%u:%02u/%u-%u",
           g_alertWin.amS / 60, g_alertWin.amS % 60,
           g_alertWin.amE / 60, g_alertWin.amE % 60,
           g_alertWin.pmS / 60, g_alertWin.pmE / 60);
  drawCN(disp(), 6, y, b, C_FG); y += 18;
#endif

  snprintf(b, sizeof(b), "音量%d 亮度%d %s", g_volume, g_bright,
           g_displayMode == DISP_LIST ? "列表" : "单只");
  drawCN(disp(), 6, y, b, C_FLAT); y += 18;

  snprintf(b, sizeof(b), "内存%uK PSRAM%uM 屏%s",
           (unsigned)(ESP.getFreeHeap() >> 10), (unsigned)(ESP.getPsramSize() >> 20),
           s_sprOk ? "缓存" : "直画");
  drawCN(disp(), 6, y, b, C_FLAT); y += 18;

  uint32_t up = millis() / 1000;
  snprintf(b, sizeof(b), "运行 %luh%02lum  股票%u", up / 3600, (up / 60) % 60,
           (unsigned)g_nStock);
  drawCN(disp(), 6, y, b, C_FLAT); y += 18;

  drawCN(disp(), 6, 214, "左右同按=中键  长按=关机", C_FLAT);
}

// ---------------- toast ----------------
void uiToast(const char* msg, uint16_t color, uint16_t ms) {
  portENTER_CRITICAL(&s_tMux);
  strncpy(s_toastMsg, msg, sizeof(s_toastMsg) - 1);
  s_toastMsg[sizeof(s_toastMsg) - 1] = 0;
  s_toastCol = color;
  s_toastUntil = millis() + ms;
  portEXIT_CRITICAL(&s_tMux);
  s_radioDirty = true;
}

static void drawToast() {
  char msg[72];
  uint16_t col;
  uint32_t until;
  portENTER_CRITICAL(&s_tMux);
  memcpy(msg, s_toastMsg, sizeof(msg));
  col = s_toastCol;
  until = s_toastUntil;
  portEXIT_CRITICAL(&s_tMux);
  if (!msg[0] || millis() > until) return;

  char cut[72];
  cnEllipsis(cut, sizeof(cut), msg, 216);
  int w = cnTextWidth(cut) + 16;
  int x = (240 - w) / 2;
  if (x < 2) { x = 2; w = 236; }
  disp().fillRect(x, 62, w, 24, 0x18E3);
  disp().drawRect(x, 62, w, 24, col);
  drawCN(disp(), x + 8, 66, cut, col, 0x18E3);
}

// ---------------- 涨跌幅提醒叠加层 ----------------
static void drawAlertOverlay(int idx) {
  uint16_t c = ((millis() / 300) % 2) ? C_UP : C_WARN;
  for (int w = 0; w < 3; w++)
    disp().drawRect(w, w, 240 - 2 * w, 240 - 2 * w, c);
  RowView v; rowOf(idx, v);
  char b[64], cut[64];
  snprintf(b, sizeof(b), "涨跌提醒 %+.2f%% %s", v.pct, v.name);
  disp().fillRect(0, 218, 240, 20, c);
  cnEllipsis(cut, sizeof(cut), b, 230);
  drawCN(disp(), 5, 219, cut, C_BG, c, true);
}

// ---------------- 主绘制 ----------------
static void drawAll() {
  UiScene sc = g_scene;
  bool hint = false;
  if (sc == SCENE_MAIN)
    hint = (g_wifiState == WIFI_AP_ONLY && !hasAnyQuote());

  // 列表 + 单行精灵模式: 每行自己整行覆盖, 不需要(也不应该)整屏擦黑,
  // 否则翻页时先黑一下再出字, 看着像闪屏
  uint8_t eff = uiEffectiveMode();
  bool rowPath = (sc == SCENE_MAIN) && (eff == DISP_LIST) &&
                 !hint && s_rowSpr && !s_sprOk;
  if (!rowPath) frameClear();

  if (sc == SCENE_MENU || sc == SCENE_ADJ) {
    drawMenu();
  } else if (sc == SCENE_INFO) {
    drawInfo();
  } else if (sc == SCENE_CONFIRM) {
    drawConfirm();
  } else if (sc == SCENE_NEWS) {
    drawNewsScene();
  } else {
    drawStatusBar();
    // 只有"没连上路由器且一条行情都没有"才铺满配网向导;
    // 连上了但行情暂时抓不到, 仍然显示列表(代码/名称照常, 价格显示 --)
    if (hint) {
      drawNetHint();
    } else if (uiAlertActive()) {
      s_alertJitter = ((millis() / 170) % 2) ? 2 : -2;
      drawStockMain((int)s_alertIdx);
      drawAlertOverlay((int)s_alertIdx);
    } else {
      s_alertJitter = 0;
      if (eff == DISP_LIST) drawStockList();
      else                  drawStockMain(s_curStock);
    }
    drawBottomBar();
  }
  drawToast();
  frameCommit();
}

void uiInvalidate()      { s_dirty = true; }
void uiFlashRadioLine()  { s_radioDirty = true; }
void uiShowVolume()      { s_volUntil = millis() + 2000; s_radioDirty = true; }
void uiTouch()           { s_lastTouch = millis(); }
uint32_t uiLastTouchAt() { return s_lastTouch; }   // v7.1 自动关机兜底基准

// 旧滚动行情条已取消: 一屏多只已经能看到全部自选股, 不需要跑马灯
void uiRedrawTicker() { s_dirty = true; }

void uiApplyBrightness() {
  uint8_t b = g_bright;
  if (b < 5) b = 5;
  if (b > 100) b = 100;
  tft.setBrightness((uint8_t)(255 * b / 100));
}

static uint32_t s_screenOffAt = 0;            // 最近一次熄屏时刻(自动关机计时用)

void uiSetScreen(bool on) {
  g_screenOn = on;
  if (on) {
    s_screenOffAt = 0;
    uiApplyBrightness();
    uiTouch();
    s_dirty = true;          // 熄屏期间可能漏了刷新, 点亮时整页重画
  } else {
    s_screenOffAt = millis();
    tft.setBrightness(0);
  }
}

uint32_t uiScreenOffAt() { return s_screenOffAt; }

bool uiAutoOffCheck() {
  if (!g_screenOn || g_autoOffMin == 0) return false;
  if (g_scene != SCENE_MAIN) return false;      // 菜单/信息页不熄
  if (millis() - s_lastTouch < (uint32_t)g_autoOffMin * 60000UL) return false;
  uiSetScreen(false);
  return true;
}

void uiInit() {
  // 电池检测: BAT_VSEN=GPIO1 (ADC1_CH0), CHRG=GPIO48 (开漏, 低=充电中)
  pinMode(PIN_BAT_VSEN, INPUT);
  pinMode(PIN_CHG_STATUS, INPUT_PULLUP);
  analogReadResolution(12);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(C_BG);

  // 整屏 RGB565 精灵 = 115200 字节。没有 PSRAM 时直接画屏, 把内存留给 WiFi。
  s_sprOk = false;
  s_rowSpr = false;
  size_t psramBytes = ESP.getPsramSize();
  if (psramBytes >= 300 * 1024) {
    spr.setPsram(true);
    spr.setColorDepth(16);
    if (spr.createSprite(240, 240)) s_sprOk = true;
    else {
      spr.setPsram(false);
      if (spr.createSprite(240, 240)) s_sprOk = true;
    }
  }
  // 没有整帧精灵: 退一步用 240x34 的单行精灵(16KB), 列表翻页逐行推, 不闪
  if (!s_sprOk) {
    sprRow.setColorDepth(16);
    if (sprRow.createSprite(240, UI_LIST_ROW_H)) s_rowSpr = true;
  }
  frameClear();

  Serial.printf("[UI] sprite=%d rowspr=%d psram=%uK heap=%u\n",
                (int)s_sprOk, (int)s_rowSpr,
                (unsigned)(psramBytes >> 10), (unsigned)ESP.getFreeHeap());

  uiApplyBrightness();
  drawCN(disp(), 24, 80, "BOX0 收音机+行情", C_ACCENT);
  drawCN(disp(), 24, 104, "固件 " FW_VERSION, C_FG);
  drawCN(disp(), 24, 128, "启动中...", C_FLAT);
  frameCommit();
  s_lastTouch = millis();
}

void uiTask(void* arg) {
  (void)arg;
  uint32_t lastBar = 0;
  static uint32_t lastAlertDraw = 0;
  for (;;) {
    if (!g_screenOn) { vTaskDelay(pdMS_TO_TICKS(120)); continue; }

    uint32_t now = millis();
    UiScene sc = g_scene;

    // 提醒弹屏期间按 ~170ms 节奏重画, 形成字符跳动/边框闪烁
    if (uiAlertActive() && sc == SCENE_MAIN && now - lastAlertDraw > 170) {
      lastAlertDraw = now;
      s_dirty = true;
    } else if (!uiAlertActive() && s_alertIdx >= 0) {
      uiClearAlert();                 // 到点自动退出弹屏
    }

    if (s_dirty) {
      s_dirty = false;
      if (s_curStock != g_curStock) s_curStock = g_curStock;
      drawAll();
      continue;
    }
    if (s_radioDirty) {
      s_radioDirty = false;
      if (sc == SCENE_MAIN) {
        drawBottomBar();
        drawToast();
        frameCommit();
      } else if (sc == SCENE_NEWS) {
        s_dirty = true;
      }
      continue;
    }
    // toast 消失后重绘(否则提示会一直挂在屏上)
    if (s_toastUntil && now > s_toastUntil + 60) {
      s_toastUntil = 0;
      s_dirty = true;
      continue;
    }
    // 状态栏: 每 30 秒刷一次(时间/信号/页码), 不要等整页重绘
    if (sc == SCENE_MAIN && now - lastBar > 30000) {
      lastBar = now;
      drawStatusBar();
      frameCommit();
    }
    uiAutoOffCheck();
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}
