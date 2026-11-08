/*
 * BOX0 网络收音机 + 股票显示器   固件 v4.0
 * 硬件: 正点原子 ESP32 AI BOX0 (atk-dnesp32s3-box0) 240x240
 * 框架: Arduino-ESP32 3.x
 *
 * ── 按键 (v7.0: 短按/长按/一直按/组合按, 三键全利用) ────────
 *   主界面
 *     左/右 短按       上一/下一页(列表) 或 上一/下一只(单只)
 *     左/右 长按+一直按 音量 -/+ (170ms 连发)
 *     M 短按           单只大屏 <-> 列表 切换
 *     M 双击           立即刷新行情
 *     M 三连(v7.2)     实时快讯(标题+摘要)
 *     M 四连(v7.2)     收音机模式(菜单第13项也可切)
 *     M 长按           进入设置菜单
 *     左右同按 短按    进入设置菜单(M 键兜底)
 *     左右同按 1.5 秒  关机      M+右 3 秒 = 强制下载模式
 *   菜单/调节/确认/信息页 (都支持显式返回, 不必等超时)
 *     左/右             移动 / 减值·加值(长按连发)
 *     M 短按            确定 / 完成返回菜单(提醒页=切开关)
 *     左右同按 短按     返回上一级
 *     M 长按            任何页面直接回主界面
 *   熄屏后: 任意键按下即亮(这一下不触发功能), 8 秒/20 秒无操作也会自动回主界面
 *
 * ── 显示 ──────────────────────────────────────────────────
 *   默认列表模式: 一屏 6 只, 每 g_jumpSec 秒自动翻页
 *   状态栏 6 项: 星期 / 时间 / 页码 / 音量 / 电量 / WiFi(模式)
 *
 * ── 配置 ──────────────────────────────────────────────────
 *   手机连热点 BOX0-Setup -> 192.168.4.1 (默认密码 123456)
 *   所有配置热生效, 不重启(本板重启会松开电源锁存直接断电)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <time.h>
#include "cfg_pins.h"
#include "app_config.h"
#include "ui_display.h"
#include "font_render.h"
#include "radio_player.h"
#include "news_feed.h"
#include "stock_feed.h"
#include "web_admin.h"
#include "buttons.h"
#include "wifi_mgr.h"
#include "gyro_imu.h"
#include "beep.h"

// 菜单项索引见 ui_display.h 的 MenuIdx (两个文件共用, 不再各写一份)

static uint32_t s_lastJump = 0;
static bool     s_wifiOk = false;
static uint32_t s_menuAt = 0;
#define MENU_IDLE_MS 8000

static void toast(const char* m, uint16_t c = 0xFFFF) { uiToast(m, c, 1500); }

// ---------------- 翻页/切只 ----------------
static void stepStock(int dir) {
  if (g_nStock <= 0) return;
  // v7.1: 统一走 uiEffectiveMode(), DISP_AUTO 由定时切换(或探测到 IMU 时重力)决定
  bool isList = (uiEffectiveMode() == DISP_LIST);
  if (isList) {
    int pages = uiPageCount();
    if (pages <= 1) return;
    int p = g_curStock / UI_LIST_ROWS + dir;
    if (p < 0) p = pages - 1;
    if (p >= pages) p = 0;
    g_curStock = p * UI_LIST_ROWS;
  } else {
    g_curStock = (g_curStock + dir + g_nStock) % g_nStock;
  }
  s_lastJump = millis();
  uiInvalidate();
}

// ---------------- v7.2 工作模式: 股票 / 收音机 ----------------
// 用户确认「提示音和 MP3 不会同时播放」, 因此 I2S_NUM_0 可以分时独占:
//   进收音机 -> beep 释放通道, radioEnsure() 创建 MP3 解码器接管;
//   回股票   -> radioRelease() 析构解码器, beep 重建通道(提示音恢复)。
static void setAppMode(uint8_t m) {
  if (m == g_appMode) {
    g_scene = (m == APP_RADIO) ? SCENE_RADIO : SCENE_MAIN;
    uiInvalidate();
    return;
  }
  radioPause();                       // 先停解码任务, 避免与 I2S 交接并发
  if (m == APP_RADIO) {
    radioSetStage(10);
    // IDLE 任务的 WDT 在 setup() 里已一次性关闭(见下方说明), 这里不再反复摘挂:
    // 实测「退出收音机时 enableCore*WDT()」会把 IDLE 挂回 TWDT 却恢复不了喂狗,
    // 约 5 秒后必定触发任务看门狗复位(resetReason=6)。
    beepReleaseI2S();
    radioEnsure();
    if (g_nStation > 0) radioPlay(g_curStation);
    g_appMode = APP_RADIO;
    g_scene = SCENE_RADIO;
    toast("收音机模式 三键可操作", 0x05BF);
    radioResumeTask();
  } else {
    radioRelease();
    beepResumeI2S();
    g_appMode = APP_STOCK;
    g_scene = SCENE_MAIN;
    // 不要在这里 enable*WDT(): 重新挂载 IDLE 到 TWDT 会立刻触发复位。
    radioResumeTask();
    toast("股票行情模式", 0x05BF);
  }
  s_menuAt = millis();
  uiTouch();
  uiInvalidate();
}

// 收音机页: 左右切台 / M 播放暂停 / 双击停止 / 双键返回股票模式
static void handleRadioScene(BtnEvent e) {
  switch (e) {
    case BTN_L_SHORT:
      if (g_nStation > 0) {
        int nx = (g_curStation - 1 + g_nStation) % g_nStation;
        radioPlay(nx);
        toast(stationDisplayName(nx));
      }
      break;
    case BTN_R_SHORT:
      radioNextStation();
      toast(stationDisplayName(g_curStation));
      break;
    case BTN_L_LONG: radioSetVolume(g_volume - 5); uiShowVolume(); break;
    case BTN_R_LONG: radioSetVolume(g_volume + 5); uiShowVolume(); break;
    case BTN_M_SHORT:  radioTogglePlay(); break;
    case BTN_M_DOUBLE: radioStop(); toast("已停止播放"); break;
    case BTN_LR_SHORT: setAppMode(APP_STOCK); break;
    default: break;
  }
  uiInvalidate();
}

// ---------------- 主界面按键 ----------------
static void handleMain(BtnEvent e) {
  switch (e) {
    case BTN_L_SHORT:
      stepStock(-1);
      break;
    case BTN_R_SHORT:
      stepStock(+1);
      break;
    case BTN_L_LONG:
      radioSetVolume(g_volume - 5);
      cfgSaveVolume();
      uiShowVolume();
      beepAlert();                        // 音量预览: 每次调节响一声
      break;
    case BTN_R_LONG:
      radioSetVolume(g_volume + 5);
      cfgSaveVolume();
      uiShowVolume();
      beepAlert();                        // 音量预览: 每次调节响一声
      break;
    case BTN_M_DOUBLE:                     // 双击: 立即刷新行情
      stockRequestRefresh();
      toast("刷新行情", 0x05BF);
      break;
    case BTN_M_TRIPLE:                     // v7.2: 三连 M -> 实时快讯
      g_scene = SCENE_NEWS;
      g_newsSel = 0;
      g_newsDetail = 0;
      s_menuAt = millis();
      if (!newsReady()) newsKick();
      newsTakeNew();                       // 进页面即视为已读, 清状态栏红点
      uiInvalidate();
      break;
    case BTN_M_QUAD:                       // v7.2: 四连 M -> 收音机
      setAppMode(APP_RADIO);
      break;
    // v7.0: 恢复上一版习惯 —— M 短按只在"单只/列表"之间切换
    case BTN_M_SHORT:
      {
        uint8_t nx = (g_displayMode == DISP_LIST) ? DISP_ONE : DISP_LIST;
        g_displayMode = nx;
        g_curStock = 0;
        cfgSaveSettings();
        uiInvalidate();
        toast(nx == DISP_LIST ? "列表 6只/屏" : "单只大屏");
      }
      break;
    case BTN_M_LONG:                       // 长按 M: 进设置菜单
    case BTN_LR_SHORT:                     // 左右同按: 进菜单(M 键兜底)
      g_scene = SCENE_MENU;
      g_menuSel = MENU_DISP;
      s_menuAt = millis();
      uiInvalidate();
      break;
    default: break;
  }
}

// ---------------- 菜单 ----------------
static void menuEnter() {
  int sel = g_menuSel;
  if (sel == MENU_DISP) {
    // 三档循环: 单只 -> 列表 -> 自动
    uint8_t nx = g_displayMode + 1;
    if (nx > DISP_AUTO) nx = DISP_ONE;
    g_displayMode = nx;
    cfgSaveSettings();
    uiInvalidate();
    const char* nm = (nx == DISP_LIST) ? "列表 6只/屏" :
                     (nx == DISP_ONE)  ? "单只大屏" : "定时自动切换";
    toast(nm, 0x05BF);
  } else if (sel == MENU_BEEP) {
    beepTest();
    toast("提示音测试", 0x05BF);
  } else if (sel == MENU_MODE) {            // v7.2: 菜单里切股票/收音机
    setAppMode(g_appMode == APP_STOCK ? APP_RADIO : APP_STOCK);
  } else if (sel == MENU_NEWS) {            // v7.2: 三连 M 的兜底入口
    g_scene = SCENE_NEWS;
    g_newsSel = 0;
    g_newsDetail = 0;
    s_menuAt = millis();
    if (!newsReady()) newsKick();
    newsTakeNew();
    uiInvalidate();
  } else if (sel == MENU_BRIGHT || sel == MENU_VOL || sel == MENU_JUMP ||
             sel == MENU_REFRESH || sel == MENU_APOW ||
             sel == MENU_ALERT || sel == MENU_OFF) {
    g_scene = SCENE_ADJ;
    uiInvalidate();
  } else if (sel == MENU_INFO) {
    g_scene = SCENE_INFO;
    uiInvalidate();
  } else if (sel == MENU_RESET || sel == MENU_POWER) {
    g_scene = SCENE_CONFIRM;
    uiInvalidate();
  }
}

static void handleMenu(BtnEvent e) {
  int n = (int)g_menuListN;
  switch (e) {
    case BTN_L_SHORT:
    case BTN_L_LONG:
      g_menuSel = (g_menuSel - 1 + n) % n;
      uiInvalidate();
      break;
    case BTN_R_SHORT:
    case BTN_R_LONG:
      g_menuSel = (g_menuSel + 1) % n;
      uiInvalidate();
      break;
    case BTN_M_SHORT:
      menuEnter();
      break;
    case BTN_LR_SHORT:                     // 组合键: 显式返回主界面
      g_scene = SCENE_MAIN;
      uiInvalidate();
      toast("返回主界面");
      break;
    case BTN_M_DOUBLE:                     // 双击也作为返回(不用等超时)
      g_scene = SCENE_MAIN;
      uiInvalidate();
      break;
    default: break;
  }
}

// ---------------- 新闻子页 ----------------
// 左右翻条; 中键 = 把底部新闻条定位到这一条并回主界面; 中键长按 = 回菜单
static void handleNewsScene(BtnEvent e) {
  int n = g_nNews;
  if (n <= 0) {
    if (e != BTN_NONE) { g_scene = SCENE_MAIN; uiInvalidate(); }
    return;
  }
  switch (e) {
    case BTN_L_SHORT:
    case BTN_L_LONG:
      g_newsSel = (g_newsSel - 1 + n) % n;
      uiInvalidate();
      break;
    case BTN_R_SHORT:
    case BTN_R_LONG:
      g_newsSel = (g_newsSel + 1) % n;
      uiInvalidate();
      break;
    case BTN_M_SHORT:                     // v7.2: 展开/收起全文摘要
      g_newsDetail = !g_newsDetail;
      uiInvalidate();
      break;
    case BTN_LR_SHORT:                    // 显式返回(回到当前模式的主页)
      newsSelect(g_newsSel);
      newsTakeNew();
      g_scene = (g_appMode == APP_RADIO) ? SCENE_RADIO : SCENE_MAIN;
      uiInvalidate();
      break;
    case BTN_M_DOUBLE:
      newsKick();                       // 重新抓一次
      toast("刷新新闻..", 0x05BF);
      break;
    case BTN_M_LONG:
      g_scene = SCENE_MENU;
      uiInvalidate();
      break;
    default: break;
  }
}

// v7.1: 按每只股票的有效阈值统计当前超限只数(全局阈值与独立阈值各自生效)
static int countAlertHits() {
  int hit = 0;
  if (quotesLock(50)) {
    for (int i = 0; i < g_nStock; i++) {
      const Quote& q = g_quotes[i];
      float th = alertThresholdFor(i);
      if (th > 0 && q.valid && q.chgpct != 0 &&
          (q.chgpct >= th || q.chgpct <= -th)) hit++;
    }
    quotesUnlock();
  }
  return hit;
}

// ---------------- 调节亮度/音量/轮播/涨跌提醒 ----------------
static void handleAdj(BtnEvent e) {
  bool up   = (e == BTN_R_SHORT || e == BTN_R_LONG);
  bool down = (e == BTN_L_SHORT || e == BTN_L_LONG);

  // M 短按: 涨跌提醒页用来切开关; 其它调节页用来"完成并返回菜单"
  if (e == BTN_M_SHORT) {
    if (g_menuSel == MENU_ALERT) {
      g_alertOn = !g_alertOn;               // M 键切换提醒开关
      cfgSaveSettings();
      beepAlert();                          // 切换时响一声确认
      uiInvalidate();
      // 即时反馈: 用当前行情扫一遍, 告诉用户开关已生效 + 命中情况
      int hit = countAlertHits();
      char m[40];
      if (g_alertOn) snprintf(m, sizeof(m), "提醒已开 当前%d只超限", hit);
      else           snprintf(m, sizeof(m), "提醒已关", hit);
      toast(m, g_alertOn ? 0x05BF : 0x8410);
      return;
    }
    g_scene = SCENE_MENU;
    uiInvalidate();
    return;
  }
  if (e == BTN_LR_SHORT) {                  // 组合键: 返回上一级菜单
    g_scene = SCENE_MENU;
    uiInvalidate();
    return;
  }
  // BTN_M_LONG 由 loop 统一拦截: 任何子页面都直接回主界面
  if (!up && !down) return;

  // 自动关机档位: 关 / 息屏后30分 / 1时 / 2时 / 4时
  if (g_menuSel == MENU_APOW) {
    int cur = 0;
    for (int i = 0; i < POFF_N; i++)
      if (POFF_STEPS[i] == g_powerOffMin) { cur = i; break; }
    cur = constrain(cur + (up ? 1 : -1), 0, POFF_N - 1);
    g_powerOffMin   = (uint8_t)POFF_STEPS[cur];
    g_autoPowerOff  = g_powerOffMin ? 1 : 0;   // 与 Web 的开关字段保持同步
    cfgSaveSettings();
    toast(poffName(g_powerOffMin), 0x05BF);
    uiInvalidate();
    return;
  }

  // 自动熄屏: 分钟档 0(不熄)/1/3/5/10/30/60/120
  static const uint8_t OFF_STEPS[] = {0, 1, 3, 5, 10, 30, 60, 120};
  if (g_menuSel == MENU_OFF) {
    int cur = 0;
    for (int i = 0; i < 8; i++) if (OFF_STEPS[i] == g_autoOffMin) { cur = i; break; }
    cur = constrain(cur + (up ? 1 : -1), 0, 7);
    g_autoOffMin = OFF_STEPS[cur];
    cfgSaveSettings();
    uiTouch();
    char m[40];
    if (g_autoOffMin) snprintf(m, sizeof(m), "%d分钟无操作熄屏", g_autoOffMin);
    else              strcpy(m, "不自动熄屏");
    toast(m, 0x05BF);
    uiInvalidate();
    return;
  }

  if (g_menuSel == MENU_BRIGHT) {
    int v = (int)g_bright + (up ? 10 : -10);
    if (v < 10) v = 10;
    if (v > 100) v = 100;
    g_bright = v;
    uiApplyBrightness();
  } else if (g_menuSel == MENU_JUMP) {
    // 轮播间隔: 2-60 秒, 短按 1 秒, 长按 5 秒
    int step = (e == BTN_R_LONG || e == BTN_L_LONG) ? 5 : 1;
    int v = (int)g_jumpSec + (up ? step : -step);
    if (v < 2) v = 2;
    if (v > 60) v = 60;
    g_jumpSec = (uint8_t)v;
    cfgSaveSettings();
  } else if (g_menuSel == MENU_ALERT) {
    // 涨跌提醒阈值: 1-20%, 短按 0.5%, 长按 1%
    float step = (e == BTN_R_LONG || e == BTN_L_LONG) ? 1.0f : 0.5f;
    float v = g_alertPct + (up ? step : -step);
    if (v < 1.0f) v = 1.0f;
    if (v > 20.0f) v = 20.0f;
    g_alertPct = v;
    cfgSaveSettings();
    // 即时反馈: 用当前行情扫一遍, 让用户看到阈值调节是否"真的生效"
    int hit = countAlertHits();
    char m[40];
    snprintf(m, sizeof(m), "阈值%.1f%% 当前%d只超限", g_alertPct, hit);
    toast(m, hit > 0 ? 0xFD20 : 0x05BF);
  } else if (g_menuSel == MENU_REFRESH) {
    // 行情刷新间隔: 3-120 秒, 短按 1 秒, 长按 5 秒
    int step = (e == BTN_R_LONG || e == BTN_L_LONG) ? 5 : 1;
    int v = (int)g_refreshSec + (up ? step : -step);
    if (v < 3) v = 3;
    if (v > 120) v = 120;
    g_refreshSec = (uint8_t)v;
    cfgSaveSettings();
    char m[24]; snprintf(m, sizeof(m), "%d秒刷新行情", g_refreshSec);
    toast(m, 0x05BF);
  } else if (g_menuSel == MENU_VOL) {
    // 调音量并播预览音(已温和化, 不会吵到同事)
    radioSetVolume(g_volume + (up ? 5 : -5));
    cfgSaveVolume();
    uiShowVolume();
    beepAlert();
  }
  uiInvalidate();
}

// ---------------- 二次确认 ----------------
static void handleConfirm(BtnEvent e) {
  // 只有 M 短按执行危险操作; M 长按/组合键一律取消(loop 会统一回主界面)
  if (e == BTN_M_SHORT) {
    if (g_menuSel == MENU_POWER) {
      Serial.println("[SYS] 菜单确认关机");
      powerOff();                          // 断电或深睡, 正常不返回
    } else {
      cfgFactoryReset();
      g_curStock = 0;
      g_scene = SCENE_MAIN;
      uiInvalidate();
      toast("已恢复出厂", 0xFD20);
    }
    return;
  }
  if (e != BTN_NONE) {                     // 任意其他键 = 取消
    g_scene = SCENE_MENU;
    uiInvalidate();
  }
}

// v7.2: MP3 解码循环放在独立任务。
//   ESP32-audioI2S 的 audio.loop() 在建立流/解码时会同步阻塞数秒, 放在
//   Arduino loopTask 里会触发任务看门狗(实测 resetReason=6 直接复位)。
//   独立任务不注册 TWDT, 主 loop 保持响应 -> 播放时按键/网页仍能退出模式。
static void radioDecodeTask(void* arg) {
  (void)arg;
  for (;;) {
    if (radioTaskCanRun()) {
      radioTaskBegin();
      radioLoopHandler();
      radioTaskEnd();
      vTaskDelay(pdMS_TO_TICKS(2));   // 留出 CPU 给 IDLE/UI, 库要求尽量高频即可
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

void setup() {
  // ★ 最先做: 系统电源保持脚拉高, 否则松手/延时后整机断电
  pinMode(PIN_SYS_POW, OUTPUT);
  digitalWrite(PIN_SYS_POW, HIGH);

  Serial.begin(115200);
  // ★★ 关键: 本机 USB 是 USB-Serial/JTAG(HWCDC), 主机没打开串口时 write() 会一直阻塞。
  //    设为 0 = 没人收就直接丢弃, 永不阻塞。
  Serial.setTxTimeoutMs(0);
  delay(80);
  Serial.printf("\n[SYS] BOOT %s  psram=%uMB heap=%u\n", FW_VERSION,
                (unsigned)(ESP.getPsramSize() >> 20), (unsigned)ESP.getFreeHeap());

  // v7.2 稳定性修复: ESP32-audioI2S 的 audio.loop() 在建立流/解码时会同步阻塞,
  // 其自带的优先级 2 PeriodicTask 也可能把某个核的 IDLE 任务饿死, 触发 TWDT 复位。
  // 因此开机一次性把两个核的 IDLE 移出任务看门狗, 全程不再 enable*WDT()。
  // 反复「摘除→挂回」会让 IDLE 重新入表却没人喂狗, 约 5s 后必定复位(实测)。
  // loopTask 的 WDT 保持原状(Arduino 3.3 默认为关), 主循环仍由 decodeTask 解耦保护。
  disableCore0WDT();
  disableCore1WDT();

  pinMode(PIN_CODEC_PWR, OUTPUT);
  digitalWrite(PIN_CODEC_PWR, HIGH);
  digitalWrite(PIN_SYS_POW, HIGH);         // 再确认一次(复位瞬间可能被拉低)

  cfgLoad();
  g_menuListN = MENU_N;
  Serial.printf("[SYS] cfg ok  heap=%u\n", (unsigned)ESP.getFreeHeap());

  uiInit();                                // 屏先亮起来
  Serial.printf("[SYS] ui ok  heap=%u\n", (unsigned)ESP.getFreeHeap());

  btnInit();
  btnTaskStart();                          // 按键 5ms 采样, 与主循环解耦
  Serial.printf("[SYS] btn ok  heap=%u\n", (unsigned)ESP.getFreeHeap());

#if ENABLE_RADIO
  radioInit();
  Serial.printf("[SYS] radio ok codec=%d heap=%u\n", (int)g_codecOk,
                (unsigned)ESP.getFreeHeap());
#else
  // 电台停用, 但 codec 仍要初始化: 涨跌幅提醒音(beep)靠它出声
  codecInit();
  Serial.printf("[SYS] codec ok=%d (供提醒音) heap=%u\n", (int)g_codecOk,
                (unsigned)ESP.getFreeHeap());
#endif

  // v7.2: 开机预热 beep 的 I2S 通道(MCLK 常供 ES8311), 股票模式提示音不丢首声。
  // 进收音机模式时 setAppMode() 会先 beepReleaseI2S() 把通道让给 MP3 解码器。
  beepResumeI2S();

  gyroInit();                              // 板上实测无 IMU, 保留未来外接兼容

  newsInit();                              // 快讯缓存(不联网; 连上 WiFi 后任务自己抓)

  wifiInit();                              // 常开热点 + 后台连路由器, 非阻塞

  xTaskCreatePinnedToCore(uiTask, "ui", 8192, nullptr, 1, nullptr, 0);
  stockTaskStart();
  // v7.2 实时快讯: 改用东方财富 7x24(HTTP 明文 6KB, 手写扫描解析),
  // 不再用旧版新浪滚动接口(40+字段/16KB/ArduinoJson filter 在 ESP32 上反复解析出 0 条)。
  xTaskCreatePinnedToCore(newsTask, "news", 6144, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(radioDecodeTask, "radio", 8192, nullptr, 1, nullptr, 1);
  webBegin();

  uiInvalidate();
  Serial.printf("[SYS] ready %s ssid=\"%s\" heap=%u\n", FW_VERSION, g_wifiSsid,
                (unsigned)ESP.getFreeHeap());
}

void loop() {
  // v7.2: 网页(async_tcp 任务)只置标志, 模式切换(含 I2S 交接/Audio 创建)
  // 一律在主 loop 执行, 避免在异步网络任务里操作 I2S 与动态分配大对象。
  if (g_pendingMode >= 0) {
    uint8_t m = (uint8_t)g_pendingMode;
    g_pendingMode = -1;
    setAppMode(m);
  }

  BtnEvent e = btnPoll();

  // 任何操作都要重置"自动熄屏"计时, 否则正看着也会突然黑屏
  if (e != BTN_NONE) uiTouch();

  // 熄屏状态下: 任意键"按下"只负责点亮, 这一下不执行任何功能(防误操作)
  if (e == BTN_ANY_DOWN) {
    if (!g_screenOn) {
      uiSetScreen(true);
      btnFlush();                  // 别让这次长按继续连调音量
    }
    e = BTN_NONE;
  }

  // ★ 兜底关机: 左右键同时长按 1.5 秒
  //   不直接断电, 先弹确认页(两个手指想开菜单时按久一点就会误关机, 太危险);
  //   在确认页里再长按一次 = 立即关机, 短按一下 = 确认关机。
  if (e == BTN_LR_LONG) {
    if (g_scene == SCENE_CONFIRM && g_menuSel == MENU_POWER) {
      powerOff();                  // 断电或深睡, 正常不返回
      return;
    }
    g_scene   = SCENE_CONFIRM;
    g_menuSel = MENU_POWER;
    s_menuAt  = millis();
    uiInvalidate();
    e = BTN_NONE;
  }

  // ★ 强制进下载模式: M + R 同时长按 3 秒(刷机用, 不经过确认, 直接复位)
  if (e == BTN_MR_LONG) {
    Serial.println("[SYS] M+R 长按 -> 强制进下载模式");
    // 拉低 GPIO0(BOOT) 后复位, 进入 ROM 下载模式
    pinMode(0, OUTPUT);
    digitalWrite(0, LOW);
    delay(50);
    ESP.restart();
    return;
  }

  // v7.2: 收音机模式的子页面按 M 长按 = 直接退回股票模式(同时释放 I2S)
  if (g_appMode == APP_RADIO && g_scene != SCENE_MAIN && e == BTN_M_LONG) {
    setAppMode(APP_STOCK);
    e = BTN_NONE;
  }

  // ★ 显式返回: 任何子页面按 M 长按 = 立刻回主界面, 不用等 8 秒超时
  if (g_scene != SCENE_MAIN && e == BTN_M_LONG) {
    g_scene = SCENE_MAIN;
    uiInvalidate();
    toast("返回主界面");
  } else {
    switch (g_scene) {
      case SCENE_MAIN:    handleMain(e); break;
      case SCENE_MENU:    handleMenu(e); break;
      case SCENE_ADJ:     handleAdj(e);  break;
      case SCENE_CONFIRM: handleConfirm(e); break;
      case SCENE_NEWS:    handleNewsScene(e); break;
      case SCENE_RADIO:   handleRadioScene(e); break;
      case SCENE_INFO:
        if (e != BTN_NONE) { g_scene = SCENE_MENU; uiInvalidate(); }
        break;
    }
  }

  // 覆盖场景无操作自动回主界面(菜单不会把机器"卡"在某个页面)
  if (g_scene != SCENE_MAIN && e != BTN_NONE) s_menuAt = millis();
  uint32_t idleMs = (g_scene == SCENE_NEWS) ? 20000u : MENU_IDLE_MS;
  if (g_scene != SCENE_MAIN && millis() - s_menuAt > idleMs) {
    g_scene = SCENE_MAIN;
    uiInvalidate();
  }

  // 自动轮播: 列表模式翻页 / 单只模式换只, 间隔 g_jumpSec 秒
  if (g_screenOn && g_scene == SCENE_MAIN && g_nStock > 0 &&
      millis() - s_lastJump > (uint32_t)g_jumpSec * 1000) {
    if (uiEffectiveMode() == DISP_LIST) {
      if (uiPageCount() > 1) stepStock(+1);
      else s_lastJump = millis();
    } else {
      stepStock(+1);
    }
  }

  // 新闻条轮换已停用(新闻功能移除, g_nNews 恒为 0)
  // static uint32_t s_lastNews = 0;
  // if (g_newsOn && g_screenOn && g_scene == SCENE_MAIN && g_nNews > 0 &&
  //     millis() - s_lastNews > (uint32_t)g_newsSec * 1000) {
  //   s_lastNews = millis();
  //   newsNext();
  //   uiFlashRadioLine();
  // }

  // v7.1 自动显示模式: 板上无 IMU(实测 I2C 只有 ES8311), 旧代码算了 want 却从不
  //   写回, 所以"自动"一直是死的。改为真正可用的定时切换:
  //   每 g_autoSwitchSec 秒在列表/单只之间切一次。若 /api/imu 真探到 QMI8658,
  //   gyroPresent() 变 true, 自动改为重力方向优先(横=列表, 竖=单只)。
  static uint32_t s_autoSwAt = 0;
  static uint32_t s_gyroAt   = 0;
  static GyroOrientation s_lastOrient = GYRO_UNKNOWN;
  if (g_displayMode == DISP_AUTO && g_screenOn) {
    if (g_gyroOn && gyroPresent()) {
      if (millis() - s_gyroAt > 500) {
        s_gyroAt = millis();
        GyroOrientation o = gyroRead();
        if (o != GYRO_UNKNOWN && o != s_lastOrient) {
          s_lastOrient = o;
          g_autoEffMode = (o == GYRO_LANDSCAPE) ? DISP_LIST : DISP_ONE;
          g_curStock = 0;
          uiInvalidate();
        }
      }
    } else if (g_autoSwitchSec > 0 &&
               millis() - s_autoSwAt > (uint32_t)g_autoSwitchSec * 1000UL) {
      s_autoSwAt = millis();
      g_autoEffMode = (g_autoEffMode == DISP_LIST) ? DISP_ONE : DISP_LIST;
      g_curStock = 0;
      uiInvalidate();
    }
  }

  // v7.1 涨跌幅提醒:
  //   * 只在「预警提示」时间范围 g_alertWin 内检查(与「信息更新」时段完全独立)
  //   * 全局阈值 g_alertPct + 每股独立阈值 g_alertPer[i]:
  //       0=跟随全局 / >0=独立阈值 / <0=该股禁用
  //   * 去重位按股票单独维护: 改全局阈值只影响"跟随全局"的股票,
  //     改某只股票的阈值只清它自己的位, 互不影响
  //   * 每只股票每天最多提醒一次(跨天自动清零), 不再"响一次就整天不响"
  static uint32_t s_alertAt = 0;
  static int      s_alertDay = -1;
  static uint32_t s_alertMask = 0;      // 30 只用 uint32 位图足够
  static float    s_alertLastVal[MAX_STOCKS];
  static bool     s_alertValInit = false;
  if (millis() - s_alertAt > 3000) {
    s_alertAt = millis();
    struct tm tt; int day = -1;
    if (getLocalTime(&tt, 50)) day = tt.tm_yday;
    if (day != s_alertDay) { s_alertDay = day; s_alertMask = 0; }

    if (!s_alertValInit) {
      for (int i = 0; i < MAX_STOCKS; i++) s_alertLastVal[i] = alertThresholdFor(i);
      s_alertValInit = true;
    }
    for (int i = 0; i < MAX_STOCKS; i++) {
      float v = alertThresholdFor(i);
      if (s_alertLastVal[i] != v) {
        s_alertLastVal[i] = v;
        s_alertMask &= ~(1UL << i);   // 只重置这一只, 其它股票不受影响
      }
    }

    if (g_alertOn && isAlertTime() && quotesLock(50)) {
      int hitIdx = -1;
      float hitTh = 0;
      for (int i = 0; i < g_nStock && i < 32; i++) {
        float th = alertThresholdFor(i);
        if (th <= 0) continue;         // 该股禁用提醒
        const Quote& q = g_quotes[i];
        if (q.valid && q.chgpct != 0 &&
            (q.chgpct >= th || q.chgpct <= -th) &&
            !(s_alertMask & (1UL << i))) {
          hitIdx = i; hitTh = th;
          s_alertMask |= (1UL << i);
          break;
        }
      }
      quotesUnlock();
      if (hitIdx >= 0) {
        uiShowAlert(hitIdx, 12000);    // 弹屏 12 秒, 自动退出
        beepAlert();
        char m[48];
        snprintf(m, sizeof(m), "涨跌超限 %.1f%%", hitTh);
        toast(m, 0xFD20);
      }
    }
  }

  // v7.1 自动关机(修 2 个实测缺陷):
  //   ① 旧版要求 !g_screenOn 且只依赖 uiScreenOffAt(); 但任何点亮屏幕的 toast/弹屏
  //      都会把该计时清零, 30 分钟永远攒不满 -> 表现为"息屏后不关机"。
  //      改为以「最后一次用户操作」为兜底基准, 熄屏只是省电步骤。
  //   ② powerOff() 的 ext1 唤醒掩码含常态为 LOW 的 M 键(GPIO4 高电平有效),
  //      ANY_LOW 会立刻唤醒深睡 -> 看起来像"没关机"(已在 wifi_mgr.cpp 修掉)。
  // v7.2: 听广播时不自动关机(否则正在播放会被切断)
  if (g_powerOffMin > 0 && g_autoPowerOff && g_appMode == APP_STOCK &&
      g_scene == SCENE_MAIN) {
    uint32_t base = uiScreenOffAt() ? uiScreenOffAt() : uiLastTouchAt();
    if (base && millis() - base > (uint32_t)g_powerOffMin * 60000UL) {
      Serial.println("[SYS] 无操作超时, 自动关机");
      powerOff();
    }
  }

  // WiFi 状态机(非阻塞) + 状态变化触发重绘
  bool wasOk = s_wifiOk;
  s_wifiOk = (g_wifiState == WIFI_CONNECTED);
  if (s_wifiOk != wasOk) {
    uiInvalidate();
    if (s_wifiOk) toast("网络已连接", 0x07E0);
    else          toast("网络已断开", 0xFD20);
  }
  // v7.2: 开机不自动开播(一律股票模式); MP3 解码循环在 radioDecodeTask 里跑,
  // 主 loop 只负责按键/UI/网络, 不会被 audio.loop() 阻塞触发任务看门狗。

  // v7.2 新快讯: 状态栏红点常驻到用户进新闻页; 首次到达给一次 toast。
  // 默认不响铃(深夜不吵), 网页可开「新消息提示音」且仍受预警时段约束。
  static uint8_t s_newsNotified = 0;
  if (g_newsNew == 0) s_newsNotified = 0;
  else if (g_newsNew > s_newsNotified) {
    s_newsNotified = g_newsNew;
    char m[32];
    snprintf(m, sizeof(m), "快讯 +%u 条 三连M查看", (unsigned)g_newsNew);
    toast(m, 0x05BF);
    if (g_newsBeep && isAlertTime() && g_appMode == APP_STOCK) beepAlert();
    uiInvalidate();
  }

  webLoopHandler();
  cfgFlushVolume();          // 音量稳定后落盘
  beepLoop();                // 提醒音播完自动关断

  delay(2);
}
