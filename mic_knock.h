#pragma once
#include <Arduino.h>

// v7.3 实验功能: 用板上 ES8311 麦克风(I2S DIN = GPIO6)检测短促敲击声,
// 平替 M 键短按。只在「屏幕点亮 + 股票模式 + I2S 归提示音」时生效。
// 默认关闭(g_micKnock=0), 由菜单/网页打开; 打开后可通过 /api/diag 的
// micRxOk / micPeak / micKnocks 自检硬件是否真的读得到数据。
void     micKnockLoop();       // 主 loop 每轮调用, 非阻塞
bool     micRxOk();            // I2S RX 通道是否建立成功
uint16_t micPeak();            // 最近一次采样的峰值(0-32767)
uint32_t micKnockCount();      // 累计触发次数
void     micKnockResetRx();    // 配置/模式变化时释放 RX, 下次自动重建
