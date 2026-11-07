#pragma once
#include <Arduino.h>

// ── 按键事件 ────────────────────────────────────────────────
// 采样放到独立任务里(5ms), 主循环只从队列取事件。
// 原来在 loop() 里轮询: 音频/网页一旦卡住 200ms 以上, 短按(约 80ms)
// 整个被跳过 —— 表现就是"只有长按(音量)有用, 短按全无反应"。
enum BtnEvent {
  BTN_NONE = 0,
  BTN_ANY_DOWN,     // 任意键刚按下(熄屏唤醒用, 按下即亮)
  BTN_L_SHORT, BTN_L_LONG,
  BTN_R_SHORT, BTN_R_LONG,
  BTN_M_SHORT, BTN_M_LONG, BTN_M_DOUBLE,
  BTN_LR_SHORT,     // 左右同时短按 = 等效中键短按(中键不灵时的兜底)
  BTN_LR_LONG,      // 左右同时按住 1.5 秒 = 关机
  BTN_MR_LONG       // 中键+右键同时按住 3 秒 = 强制进下载模式
};

void     btnInit();        // 建立队列
void     btnTaskStart();   // 启动 5ms 采样任务(core1, 高优先级)
BtnEvent btnPoll();        // 主循环: 取一个事件(无事件返回 BTN_NONE)
// 丢弃队列 + 抑制当前按住的键(熄屏唤醒后用: 否则长按唤屏会立刻开始连调音量)
void     btnFlush();
// 诊断用: 返回三个键当前原始电平(L=0x01 M=0x02 R=0x04, 按下位=1)
uint8_t  btnRawState();
// 诊断: 指定键(0=左 1=中 2=右)开机以来电平变化次数。
//   按住/松开几次后看这个数: 不变=硬件/上拉没读到; 增加=采样正常, 问题在事件分发。
uint32_t btnEdgeCount(int i);
