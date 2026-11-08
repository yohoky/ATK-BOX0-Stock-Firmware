#pragma once
#include <Arduino.h>
#include "app_config.h"

// 电台操作 (均可在任意任务调用, 实际动作在主 loop 中执行)
extern bool g_codecOk;                   // ES8311 是否在 I2C 上应答(无声时先看这个)
extern volatile bool g_radioHasAudio;    // 真的有音频流在跑(不是"连上了")
extern volatile uint32_t g_radioBitrate; // 当前码率 bps, 0 = 没收到数据
extern volatile uint32_t g_radioSec;     // 已播秒数

void radioInit();               // 开机只初始化 ES8311 codec(不占 I2S, 留给提示音)
void codecInit();               // 只初始化 ES8311 codec(供 beep 提醒音用)
// v7.2: I2S 运行时归属切换。提示音与 MP3 不会同时播放, 所以分时独占 I2S_NUM_0。
//   进收音机模式: beepReleaseI2S() -> radioEnsure()(new Audio + setPinout)
//   退回股票模式: radioRelease()(delete Audio, 析构会 i2s_del_channel) -> beepResumeI2S()
void radioEnsure();             // 创建解码器并接管 I2S(幂等)
void radioRelease();            // 停止并释放解码器与 I2S
bool radioActive();             // 解码器是否已接管 I2S
// 调试: 阶段标记放 RTC_NOINIT, panic/看门狗重启后仍可读, /api/diag 里返回
void radioSetStage(int s);
int  radioStage();
// v7.2: 解码循环放独立 FreeRTOS 任务(不注册任务看门狗), 主 loop 只做 UI/按键。
// 模式切换前先暂停该任务, 避免 radioRelease() 与 audio.loop() 并发 use-after-free。
void radioPause();
void radioResumeTask();
bool radioTaskCanRun();
void radioTaskBegin();
void radioTaskEnd();
void radioPlay(int idx);        // 请求播放指定电台(异步, loop 中生效)
void radioTogglePlay();         // 播放/暂停
void radioStop();               // 完全停止(释放流 + 关功放, 不会再自动重连)
void radioNextStation();        // 下一台
void radioSetVolume(int v);     // 0-100
int  radioGetVolume();

// ES8311 音量 / 自检(电台停用时 beep 也要用):
//   codecSetVolume 会按官方映射写 0x32; codecVolumeReg 回读 0x32(0xFF=codec 不在线)
void    codecSetVolume(int vol);
uint8_t codecVolumeReg();
uint8_t codecI2CAddr();
const char* stationDisplayName(int idx);  // 电台名(utf8), 越界返回空串
void radioLoopHandler();        // 主 loop 调用: 处理切台/重连 + audio.loop()
