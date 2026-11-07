#include "beep.h"
#include "cfg_pins.h"
#include "radio_player.h"
#include <driver/i2s_std.h>
#include <math.h>

// 用新版 I2S 驱动(i2s_std)输出方波。注意: 不能用 legacy i2s_driver_install,
// 否则会和 ESP32-audioI2S 库(也是新版驱动)冲突 -> 启动即 abort 黑屏。
// 电台停用(ENABLE_RADIO=0)时 I2S_NUM_0 空闲, 这里独占使用没问题。
static bool     s_beepBusy = false;
static uint32_t s_beepStart = 0;
static i2s_chan_handle_t s_tx = nullptr;
static uint32_t s_beepCalls = 0;
static int      s_beepErr   = 0;
static uint32_t s_beepWritten = 0;   // 最近一次 I2S 实际写入字节数
static int      s_beepRet     = 0;   // 最近一次 i2s_channel_write 返回码

// v6.2 温和化(用户: 不要太急促, 影响上班同事):
//   1kHz 方波 -> 700Hz 正弦(去掉刺耳高次谐波)
//   60ms 急促两声 -> 120ms + 160ms 间隔, 节奏放缓
//   每声 25ms 渐入渐出包络, 消除方波起停的"咔哒"爆音
//   幅度 10000 -> 5200(约满量程 16%), 叠加默认音量 30 后明显更轻
#define BEEP_SR      16000
#define BEEP_FREQ    700
#define BEEP_BURST   120     // 每声 120ms
#define BEEP_GAP     160     // 两声间隔 160ms
#define BEEP_FADE    25      // 渐入/渐出 25ms
#define BEEP_AMP     5200
#define BEEP_TOTAL   (BEEP_BURST*2 + BEEP_GAP)  // 400ms

// 生成一整段"滴 滴"并写入 I2S(阻塞写入, 数据量小很快完成)
static bool i2sBeepStart() {
  if (s_beepBusy) return true;

  // ★ 通道常驻: 旧实现每次 beep 都 new_channel / del_channel, MCLK 跟着停启,
  //   ES8311 的 PLL 来不及重新锁定, 短促 210ms 的滴声往往整段丢失(完全无声)。
  //   改为首次创建后一直保留, 让 MCLK 持续供给 codec。
  if (!s_tx) {
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
  }

  // 2) 生成 PCM: 两声柔和正弦"叮…叮", 每声独立渐入渐出
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

  // 3) 写入(阻塞, 数据量约 12.8KB, 瞬时完成)
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
  //   Arduino 的 pinMode(OUTPUT) 会先输出低电平 -> CODEC_PWR 出现一个断电毛刺:
  //   a) ES8311 寄存器全部复位, 0x32 回到 0(静音), 刚写好的音量被冲掉 -> 盒子无声
  //   b) 电源轨被拉扯, USB 重枚举 -> Windows 响"设备插入"提示音(用户听到电脑叮咚)
  //   codec 供电由 codecInit() 开机时配好并常高, 这里只配一次功放脚。
  pinMode(PIN_SPK_EN, OUTPUT);
  digitalWrite(PIN_SPK_EN, LOW);
  s_pinsReady = true;
}

static void beepAlertEx(int vol) {
  if (s_beepBusy) return;      // 已经在响, 不叠加
  s_beepCalls++;

  beepPinsInit();
  // 只有 codec 不在线才尝试补电; 正常路径绝不动 CODEC_PWR 引脚。
  if (!g_codecOk) {
    pinMode(PIN_CODEC_PWR, OUTPUT);
    digitalWrite(PIN_CODEC_PWR, HIGH);
    delay(30);
  }

  // ★ 音量必须放在所有电源操作之后写, 否则会被 codec 复位冲掉。
  //   0x32 上电默认 0(≈-95dB 静音), 这是"完全无声"的主因。
  codecSetVolume(vol);
  digitalWrite(PIN_SPK_EN, HIGH);
  delay(20);                         // 等功放退出关断 + DAC 软斜坡起来
  i2sBeepStart();
}

void beepAlert() { beepAlertEx(g_volume); }
// Web 自检按钮: 沿用当前音量(状态栏 V 值可见), 不再固定 90% 以免突然过响
void beepTest()  { beepAlertEx(g_volume > 0 ? g_volume : 30); }

bool beepActive() { return s_beepBusy; }
uint32_t beepCallCount() { return s_beepCalls; }
int      beepLastErr()   { return s_beepErr; }
uint32_t beepWrittenBytes() { return s_beepWritten; }
int      beepWriteRet()     { return s_beepRet; }

void beepLoop() {
  if (!s_beepBusy) return;
  // 播完只关功放, ★ 不删 I2S 通道(MCLK 持续供给 ES8311, 见 i2sBeepStart 注释)
  if (millis() - s_beepStart > (uint32_t)(BEEP_TOTAL + 200)) {  // 留足 DMA 播完时间再关功放
    digitalWrite(PIN_SPK_EN, LOW);   // 关功放省电
    s_beepBusy = false;
  }
}
