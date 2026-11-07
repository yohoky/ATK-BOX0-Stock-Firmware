#pragma once
#include <Arduino.h>

// ============================================================
// 涨跌幅提醒音: 短促"滴滴"声
//   板上没有独立蜂鸣器, 用 ES8311 codec + 功放 + 喇叭输出。
//   由 I2S 生成 1kHz 方波, 播 2 声短滴(每次约 60ms)。
//   codec 由 radioInit() 初始化(即使电台停用也初始化, 供 beep 用)。
// ============================================================

// 播放一次提醒音(两短声"滴 滴")。非阻塞, 内部用 I2S DMA, 播完自动停。
// 声音从盒子板载 ES8311 + 功放 + 喇叭出, 与电脑/浏览器无关。
void beepAlert();
// 网页自检: 固定 90% 音量播一次, 排除用户把音量调到 0 的干扰
void beepTest();

// 内部: 主循环调用, 推进播放(其实 I2S 是 DMA 自动的, 这里用于超时关断)
void beepLoop();

// 是否正在响
bool beepActive();

// 诊断(/api/diag 用): 累计触发次数 / 最近一次初始化错误码
//   0=成功 1=通道创建失败 2=std模式初始化失败 3=使能失败 4=缓冲分配失败 5=I2S写失败
uint32_t beepCallCount();
int      beepLastErr();
uint32_t beepWrittenBytes();   // 最近一次 I2S 写入字节数(0=没写出去)
int      beepWriteRet();       // i2s_channel_write 返回码(0=ESP_OK)
