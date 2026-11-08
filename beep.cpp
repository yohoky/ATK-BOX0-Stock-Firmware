#include "beep.h"
#include "cfg_pins.h"
#include "radio_player.h"
#include <driver/i2s_std.h>
#include <math.h>

// 用新版 I2S 驱动(i2s_std)输出正弦提示音。
// v7.2: I2S_NUM_0 由「提示音」与「MP3 解码」分时独占(用户确认两者不会同时播放)。
//   进收音机模式 -> beepReleaseI2S() 删通道, Audio 库接管;
//   退回股票模式 -> Audio 析构释放, beepResumeI2S() 重建通道。
static volatile bool s_beepAudioEnabled = true;
static bool     s_beepBusy = false;
static uint32_t s_beepStart = 0;
static i2s_chan_handle_t s_tx = nullptr;
static uint32_t s_beepCalls = 0;
static int      s_beepErr   = 0;
static uint32_t s_beepWritten = 0;
static int      s_beepRet     = 0;

// v6.2 温和化: 700Hz 正弦 + 120ms×2 + 160ms 间隔 + 25ms 渐入渐出 + 低幅度
#define BEEP_SR      16000
#define BEEP_FREQ    700
#define BEEP_BURST   120
#define BEEP_GAP     160
#define BEEP_FADE    25
#define BEEP_AMP     5200
#define BEEP_TOTAL   (BEEP_BURST*2 + BEEP_GAP)  // 400ms

// ★ 通道常驻: 旧实现每次 beep 都 new_channel/del_channel, MCLK 跟着停启,
//   ES8311 的 PLL 来不及重新锁定, 短促滴声往往整段丢失。
//   v7.2 把它抽成独立函数, 模式切回股票时可以「只建通道不发声」预热 MCLK。
static bool ensureChannel() {
  if (s_tx) return true;
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chan_cfg, &s_tx, nullptr) != ESP_OK) {
    s_tx = nullptr; s_beepErr = 1; return false;
  }
  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BEEP_SR),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = (gpio_num_t)PIN_I2S_MCLK,
      .bclk = (gpio_num_t)PIN_I2S_BCLK,
      .ws   = (gpio_num_t)PIN_I2S_WS,
      .dout = (gpio_num_t)PIN_I2S_DOUT,
      .din  = I2S_GPIO_UNUSED,
      .invert_flags = {
        .mclk_inv = false, .bclk_inv = false, .ws_inv = false,
      },
    },
  };
  if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK) {
    i2s_del_channel(s_tx); s_tx = nullptr; s_beepErr = 2; return false;
  }
  if (i2s_channel_enable(s_tx) != ESP_OK) {
    i2s_del_channel(s_tx); s_tx = nullptr; s_beepErr = 3; return false;
  }
  s_beepErr = 0;
  return true;
}

// 生成一整段"滴 滴"并写入 I2S(阻塞写入, 数据量小很快完成)
static bool i2sBeepStart() {
  if (!s_beepAudioEnabled) return false;   // 收音机模式: I2S 归 MP3
  if (s_beepBusy) return true;
  if (!ensureChannel()) return false;

  int totalSamples = BEEP_TOTAL * BEEP_SR / 1000;
  int16_t* buf = (int16_t*)malloc(totalSamples * 2);
  if (!buf) { s_beepErr = 4; return false; }

  int burstSamples = BEEP_BURST * BEEP_SR / 1000;
  int gapSamples   = BEEP_GAP   * BEEP_SR / 1000;
  int fadeSamples  = BEEP_FADE  * BEEP_SR / 1000;
  const float w = 2.0f * 3.14159265f * BEEP_FREQ / BEEP_SR;
  for (int i = 0; i < totalSamples; i++) {
    int local = -1;
    if (i < burstSamples) local = i;
    else if (i >= burstSamples + gapSamples) local = i - burstSamples - gapSamples;
    int16_t s = 0;
    if (local >= 0 && local < burstSamples) {
      float env = 1.0f;
      if (local < fadeSamples)                  env = (float)local / fadeSamples;
      else if (local > burstSamples - fadeSamples) env = (float)(burstSamples - local) / fadeSamples;
      s = (int16_t)(sinf(w * i) * BEEP_AMP * env);
    }
    buf[i] = s;
  }

  size_t written = 0;
  esp_err_t wr = i2s_channel_write(s_tx, buf, totalSamples * 2, &written, portMAX_DELAY);
  free(buf);
  s_beepRet     = (int)wr;
  s_beepWritten = (uint32_t)written;
  if (wr != ESP_OK || written == 0) { s_beepErr = 5; return false; }

  s_beepBusy = true;
  s_beepStart = millis();
  return true;
}

static bool s_pinsReady = false;

static void beepPinsInit() {
  if (s_pinsReady) return;
  // ⚠ 绝不能每次 beep 都 pinMode(PIN_CODEC_PWR, OUTPUT):
  //   会先输出低电平 -> ES8311 掉电复位、0x32 回 0(静音) + USB 电源毛刺。
  pinMode(PIN_SPK_EN, OUTPUT);
  digitalWrite(PIN_SPK_EN, LOW);
  s_pinsReady = true;
}

static void beepAlertEx(int vol) {
  if (!s_beepAudioEnabled) return;  // 收音机模式: 提示音静默, 屏幕弹屏提醒照常
  if (s_beepBusy) return;
  s_beepCalls++;

  beepPinsInit();
  if (!g_codecOk) {
    pinMode(PIN_CODEC_PWR, OUTPUT);
    digitalWrite(PIN_CODEC_PWR, HIGH);
    delay(30);
  }

  // 音量必须放在所有电源操作之后写, 否则会被 codec 复位冲掉。
  codecSetVolume(vol);
  digitalWrite(PIN_SPK_EN, HIGH);
  delay(20);
  i2sBeepStart();
}

void beepAlert() { beepAlertEx(g_volume); }
void beepTest()  { beepAlertEx(g_volume > 0 ? g_volume : 30); }

bool beepActive() { return s_beepBusy; }
uint32_t beepCallCount() { return s_beepCalls; }
int      beepLastErr()   { return s_beepErr; }
uint32_t beepWrittenBytes() { return s_beepWritten; }
int      beepWriteRet()     { return s_beepRet; }

// ---- v7.2: 把 I2S_NUM_0 交给 MP3 解码器 / 收回来 ----
void beepReleaseI2S() {
  s_beepAudioEnabled = false;
  s_beepBusy = false;
  if (s_tx) {
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = nullptr;
  }
  // 不动功放脚: 由 radioEnsure() 接管
}

void beepResumeI2S() {
  s_beepAudioEnabled = true;
  ensureChannel();      // 预热 MCLK, 避免下一次提醒音首声丢失
}

bool beepAudioOwnsI2S() { return s_beepAudioEnabled; }

void beepLoop() {
  if (!s_beepAudioEnabled) return;   // 收音机播放中, 绝不能把功放拉低
  if (!s_beepBusy) return;
  if (millis() - s_beepStart > (uint32_t)(BEEP_TOTAL + 200)) {
    digitalWrite(PIN_SPK_EN, LOW);
    s_beepBusy = false;
  }
}
