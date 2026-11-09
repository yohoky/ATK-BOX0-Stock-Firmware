#include "mic_knock.h"
#include "app_config.h"
#include "cfg_pins.h"
#include "buttons.h"
#include "beep.h"
#include <driver/i2s_std.h>

// 设计说明(隔离性优先):
//   beep / 收音机 MP3 都要独占 I2S_NUM_0 的 TX。为了完全不碰那条已验证稳定的
//   音频链路, 这里用 ESP32-S3 的第二个控制器 I2S_NUM_1, 配成 SLAVE **只读**:
//     从同样的 BCLK / WS / DIN 引脚取数据, 自己不驱动任何时钟线。
//   ESP32 的 GPIO 输入矩阵允许同一根输入信号送到多个外设, 所以不会和 TX 抢引脚。
//   若 ES8311 的 ADC 没在上电序列里使能, RX 会读到全 0 -> 功能不生效,
//   但播放/提示音完全不受影响(这是刻意选择的降级路径)。
static i2s_chan_handle_t s_rx = nullptr;
static volatile uint16_t s_peak = 0;
static volatile uint32_t s_knocks = 0;
static uint32_t s_lastKnock = 0;
static uint32_t s_lastTry = 0;
static uint8_t  s_enabled = 255;   // 255 = 首次, 用于检测开关变化

static void releaseRx() {
  if (s_rx) {
    i2s_channel_disable(s_rx);
    i2s_del_channel(s_rx);
    s_rx = nullptr;
  }
}

static bool ensureRx() {
  if (s_rx) return true;
  i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
  if (i2s_new_channel(&cfg, nullptr, &s_rx) != ESP_OK) {
    s_rx = nullptr;
    return false;
  }
  i2s_std_config_t std = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)PIN_I2S_BCLK,
      .ws   = (gpio_num_t)PIN_I2S_WS,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)PIN_I2S_DIN,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  if (i2s_channel_init_std_mode(s_rx, &std) != ESP_OK ||
      i2s_channel_enable(s_rx) != ESP_OK) {
    releaseRx();
    return false;
  }
  Serial.println("[MIC] I2S1 slave RX ready (GPIO6 DIN)");
  return true;
}

void micKnockLoop() {
  // 仅屏幕激活时生效: 息屏立刻释放 RX, 省电也避免误触发
  bool active = g_micKnock && g_screenOn &&
                g_appMode == APP_STOCK && beepAudioOwnsI2S();
  if ((uint8_t)active != s_enabled) {
    s_enabled = (uint8_t)active;
    releaseRx();
    s_lastTry = 0;
  }
  if (!active) { s_peak = 0; return; }

  if (!s_rx) {
    if (millis() - s_lastTry < 2000) return;   // 建通道失败时限速重试
    s_lastTry = millis();
    if (!ensureRx()) return;
  }

  int16_t buf[256];
  size_t got = 0;
  if (i2s_channel_read(s_rx, buf, sizeof(buf), &got, 0) != ESP_OK) return;
  int n = (int)(got / sizeof(int16_t));
  uint16_t peak = 0;
  for (int i = 0; i < n; i++) {
    uint16_t v = (uint16_t)abs((int)buf[i]);
    if (v > peak) peak = v;
  }
  if (peak) s_peak = peak;
  // 敲击 = 短促的高能量峰值; 500ms 冷却避免一次敲击触发多次
  if (peak >= g_micThreshold && millis() - s_lastKnock > 500) {
    s_lastKnock = millis();
    s_knocks++;
    btnInject(BTN_M_SHORT);
    Serial.printf("[MIC] knock peak=%u -> BTN_M_SHORT\n", (unsigned)peak);
  }
}

bool     micRxOk()       { return s_rx != nullptr; }
uint16_t micPeak()       { return s_peak; }
uint32_t micKnockCount() { return s_knocks; }
void     micKnockResetRx() { releaseRx(); s_lastTry = 0; }
