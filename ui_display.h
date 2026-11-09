#pragma once
#include <LovyanGFX.hpp>
#include "app_config.h"

// UI 布局 (240x240, 股票为主显示, 收音机只占底部一行)
//
//   列表模式 DISP_LIST (默认, 一屏 5 只, 每几秒自动翻页)
//     [状态栏   y0-18 ] 时间 / 交易角标 / 页码 1/2 / 信号
//     [列表     y20-190] 5 行, 行高 34:
//                        左上 名称(16px)  右上 现价(12pt 粗, 红涨绿跌)
//                        左下 代码(灰)    右下 涨跌幅(16px)
//                        行首 3px 涨跌色条
//     [收音机行 y192-239] 状态 + 电台名 + 歌名 / 音量条
//
//   单只大屏 DISP_ONE (菜单可切换)
//     [状态栏   y0-18 ]
//     [主区     y20-190] 名称 + 代码 + 现价(18pt) + 涨跌 + 开/高/低/额
//     [收音机行 y192-239]
//
// 覆盖场景 (菜单/电台/信息页) 全屏绘制
//
// 按键 (全部可用左右键完成, 不依赖中键):
//   左/右 短按   上一/下一页(列表) 或 上一/下一只(单只)
//   左/右 长按   音量 -/+
//   中 短按      播放/暂停        中 双击: 停止
//   中 长按      菜单            左右同时短按 = 中键短按(兜底)
//   左右同时长按 1.5 秒 = 关机

#define C_BG      0x0000  // 黑
#define C_FG      0xFFFF  // 白
#define C_UP      0xF800  // 红(涨)
#define C_DOWN    0x07E0  // 绿(跌)
#define C_FLAT    0x8410  // 灰(平/次要)
#define C_ACCENT  0x05BF  // 青(状态强调)
#define C_WARN    0xFD20  // 橙(警告/连接中)
#define C_DIM     0x1082  // 深灰底
#define C_SEL     0x1D9E  // 选中背景(绿)
#define C_ROW     0x18E3  // 列表选中行的深灰底
#define C_LINE    0x18E3  // 分隔线

// 列表模式版面 (v6: 去掉底部新闻条, 一屏 6 只)
//   一行一只:  名称(左)   现价(右, 红涨绿跌)   涨跌幅(最右)
//   6 行 * 36px = 216, 21..236; 底部不再留新闻条
#define UI_LIST_ROWS   6     // 一屏几只
#define UI_LIST_ROW_H  36    // 行高
#define UI_LIST_Y0     21    // 首行 y

// 底部新闻条已停用(新闻功能移除), 保留宏定义避免其它文件引用报错
#define UI_NEWS_Y0     237   // 新闻条起点(已停用, 占位)
#define UI_NEWS_H      3     // 新闻条高度(已停用, 占位)

enum UiScene {
  SCENE_MAIN = 0,   // 正常行情
  SCENE_MENU,       // 按键菜单
  SCENE_ADJ,        // 菜单内调节(亮度/音量)
  SCENE_INFO,       // 设备信息(诊断页)
  SCENE_CONFIRM,    // 关机/恢复出厂二次确认
  SCENE_NEWS,       // v7.2 实时快讯子页(标题 + 摘要)
  SCENE_RADIO       // v7.2 收音机全屏页
};

// 菜单项: 顺序必须与 ui_display.cpp 的 MENU_NAME 一致, 所以放在头文件里共用
enum MenuIdx {
  MENU_DISP = 0,    // 显示模式(单只/列表/自动)
  MENU_JUMP,        // 轮播间隔(秒)
  MENU_REFRESH,     // 行情刷新间隔(秒)  v6.2 新增
  MENU_ALERT,       // 涨跌提醒 开关 + 阈值(%)
  MENU_APOW,        // 自动关机(关/息屏后30分/1时/2时/4时) v6.2 改档位
  MENU_BRIGHT,      // 亮度
  MENU_VOL,         // 音量
  MENU_OFF,         // 自动熄屏档位
  MENU_BEEP,        // 测试提示音 v6.2 新增
  MENU_INFO,        // 设备信息
  MENU_RESET,       // 恢复出厂
  MENU_POWER,       // 关机
  MENU_MODE,        // v7.2 工作模式: 股票 / 收音机
  MENU_NEWS,        // v7.2 实时快讯(三连 M 的兜底入口)
  MENU_ALERT_REP,   // v7.3 提醒间隔: 5/15/30/60/120分钟/一天
  MENU_TICKER,      // v7.3 主界面底部滚动快讯条 开/关
  MENU_N
};

extern volatile UiScene g_scene;
extern volatile int     g_menuSel;
extern volatile uint8_t g_menuListN;
extern volatile int     g_stationSel;   // 电台子页光标(电台已停用, 保留)
extern volatile int     g_newsSel;      // 新闻子页光标
extern volatile uint8_t g_newsDetail;   // v7.2: 1=新闻页展开全文摘要, 0=列表+预览

void uiInit();
void uiInvalidate();          // 请求整页重绘
void uiRedrawTicker();        // 行情变化后触发重绘(旧滚动条已去掉, 保留接口)
void uiFlashRadioLine();      // 收音机状态变化时只刷底部
void uiTask(void* arg);       // FreeRTOS 任务 (core 0)

void uiShowVolume();                        // 显示音量条(2s)
void uiToast(const char* msg, uint16_t color = 0xFFFF, uint16_t ms = 1600);
void uiSetScreen(bool on);                  // 熄屏/点亮(背光)
void uiApplyBrightness();                   // 按 g_bright 重设背光
void uiTouch();                             // 记录用户操作(自动熄屏计时)
bool uiAutoOffCheck();                      // uiTask 调用: 到点则熄屏

int  uiPageCount();                         // 列表模式共几页

// 涨跌幅提醒弹屏(强制单只 + 跳动闪烁)
void uiShowAlert(int idx, uint32_t ms = 12000);
bool uiAlertActive();
void uiClearAlert();
// 最近一次熄屏时刻(0=亮屏), 供主循环做"熄屏后 N 分钟自动关机"
uint32_t uiScreenOffAt();
// 最后一次用户操作时刻(自动关机兜底基准, v7.1)
uint32_t uiLastTouchAt();
// DISP_AUTO 解析后的实际显示模式: 有 IMU 用重力, 否则用 g_autoEffMode(定时切换)
uint8_t  uiEffectiveMode();
