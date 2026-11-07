#include "radio_player.h"
#include "cfg_pins.h"
#include "ui_display.h"
#include <Wire.h>
#include <WiFi.h>
#include "Audio.h"   // ESP32-audioI2S (github.com/schreibfaul1/ESP32-audioI2S)
#include <driver/gpio.h>

bool              g_codecOk      = false;
volatile bool     g_radioHasAudio = false;
volatile uint32_t g_radioBitrate  = 0;
volatile uint32_t g_radioSec      = 0;

static uint8_t g_codecAddr = ES8311_I2C_ADDR;
static uint8_t g_codecIdH  = 0, g_codecIdL = 0;

// ---------------- ES8311 最小驱动 ----------------
static void es8311Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(g_codecAddr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static uint8_t es8311Read(uint8_t reg) {
  Wire.beginTransmission(g_codecAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((int)g_codecAddr, 1) != 1) return 0;
  return Wire.available() ? Wire.read() : 0;
}

// 0x18 / 0x19 两个地址都试: 不同批次的 BOX0 上 ES8311 的 ADDR 脚电平不一致
static bool es8311Probe() {
  const uint8_t cand[2] = {ES8311_I2C_ADDR, 0x19};
  for (int i = 0; i < 2; i++) {
    Wire.beginTransmission(cand[i]);
    if (Wire.endTransmission() == 0) {
      g_codecAddr = cand[i];
      return true;
    }
  }
  return false;
}

// ES8311 DAC 音量寄存器 0x32: 0 = 约 -95.5dB(等同静音), 255 = +32dB(最大)。
// 官方 es8311_voice_volume_set(): reg32 = volume * 256 / 100 - 1。
// ★ 之前的写法 "255 - vol*255/100" 完全反了: 音量 100 时写 0 -> 永远静音。
// ★ 更致命的是电台停用(ENABLE_RADIO=0)时走 codecInit(), 从没调用过本函数,
//   0x32 一直是上电默认值 0 -> I2S 写得再成功也一点声音都没有。
static void es8311SetVolume(int vol) {
  if (vol < 0) vol = 0;
  if (vol > 100) vol = 100;
  uint8_t reg = (vol == 0) ? 0 : (uint8_t)((vol * 256 / 100) - 1);
  es8311Write(0x32, reg);
}

static bool es8311Init() {
  if (!es8311Probe()) {
    Serial.println("[ES8311] no ACK on 0x18/0x19");
    return false;
  }
  // ---- 第一阶段: 上电 ----
  es8311Write(0x44, 0x08);   // I2C 字长 / 7bit
  delay(20);
  es8311Write(0x44, 0x08);
  es8311Write(0x01, 0x30);   // MCLK 分频
  es8311Write(0x02, 0x00);
  es8311Write(0x03, 0x10);
  es8311Write(0x16, 0x24);
  es8311Write(0x04, 0x10);
  es8311Write(0x05, 0x00);
  es8311Write(0x0B, 0x00);
  es8311Write(0x0C, 0x1F);
  es8311Write(0x10, 0x1F);
  es8311Write(0x11, 0x7F);
  es8311Write(0x00, 0x80);   // 上电(从机模式)
  es8311Write(0x0D, 0x01);   // 数字接口: I2S 16bit
  // ---- 第二阶段: MCLK 来自引脚, 打开时钟 ----
  es8311Write(0x01, 0x3F);
  es8311Write(0x13, 0x10);   // 输出驱动使能
  es8311Write(0x1B, 0x0A);
  es8311Write(0x1C, 0x6A);
  es8311Write(0x44, 0x58);
  // ---- 第三阶段: 官方 ES8311 启动序列里标注 "NOT default" 的寄存器 ----
  //   (缺这些会出现"状态在播但完全没声音")
  es8311Write(0x0E, 0x02);   // 模拟 PGA / ADC 调制器上电
  es8311Write(0x12, 0x00);   // DAC 模拟电路上电
  es8311Write(0x13, 0x10);   // 输出驱动到 HP
  es8311Write(0x1C, 0x6A);   // ADC EQ bypass / 消直流
  es8311Write(0x31, 0x00);   // DAC 解除静音
  es8311Write(0x37, 0x08);   // DAC EQ bypass
  es8311Write(0x45, 0x00);   // 关闭 ADC2DAC 旁路, 走正常 DAC 输出
  es8311SetVolume(g_volume); // ★ 0x32 上电默认 0 = 静音, 必须显式写

  g_codecIdH = es8311Read(0xFD);
  g_codecIdL = es8311Read(0xFE);
  uint8_t pwr = es8311Read(0x00);
  Serial.printf("[ES8311] addr=0x%02X id=%02X%02X reg00=0x%02X\n",
                g_codecAddr, g_codecIdH, g_codecIdL, pwr);
  return (pwr == 0x80) || (g_codecIdH == 0x83);
}

// ---- 对外: 供 beep 在出声前重刷音量, 以及 /api/diag 自检 ----
void codecSetVolume(int vol) { if (g_codecOk) es8311SetVolume(vol); }
uint8_t codecVolumeReg()     { return g_codecOk ? es8311Read(0x32) : 0xFF; }
uint8_t codecI2CAddr()       { return g_codecAddr; }

// ---------------- 播放器 ----------------
static Audio audio;   // 默认: 外部DAC, I2S_NUM_0
static volatile int  s_pendingPlay   = -1;   // >=0: 请求切台
static volatile bool s_pendingToggle = false;
static volatile bool s_pendingStop   = false;
static volatile bool s_userStopped   = false; // 用户主动停止: 禁止断流自动重连
static uint32_t s_retryAt  = 0;
static uint8_t  s_retryCnt = 0;
static uint32_t s_connectAt = 0;
static uint32_t s_lastAudioAt = 0;
static uint32_t s_lastSec = 0xFFFFFFFF;

static void applyVolume() {
#if ENABLE_RADIO
  audio.setVolume(g_volume * 21 / 100);   // audioI2S: 0-21
#endif
  // 电台停用(ENABLE_RADIO=0)时 Audio 对象从未 setPinout/初始化,
  // 不能再去动它的 I2S; 音量只需要写 ES8311 的 0x32。
  es8311SetVolume(g_volume);
}

// 只初始化 ES8311 codec(供电 + I2C 配置 + DAC 解除静音)。
// 电台停用时也需要它: 涨跌幅提醒音(beep)要靠 codec 出声。
void codecInit() {
  pinMode(PIN_CODEC_PWR, OUTPUT);
  digitalWrite(PIN_CODEC_PWR, HIGH);
  delay(30);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  // 官方 board.cc 的 i2c_master_bus 用 enable_internal_pullup=1。板子外部上拉偏弱,
  // 不补内部上拉时总线会冒出大量幽灵 ACK(诊断里曾一次扫出 32 个假地址),
  // 也会让 ES8311 的长寄存器序列不稳定。只加上拉, 不改引脚复用。
  gpio_pullup_en((gpio_num_t)PIN_I2C_SDA);
  gpio_pullup_en((gpio_num_t)PIN_I2C_SCL);
  delay(10);
  g_codecOk = es8311Init();
  if (!g_codecOk) {              // 上电时序偶尔没跟上: 断电重来一次
    digitalWrite(PIN_CODEC_PWR, LOW);
    delay(80);
    digitalWrite(PIN_CODEC_PWR, HIGH);
    delay(60);
    g_codecOk = es8311Init();
  }
  pinMode(PIN_SPK_EN, OUTPUT);
  digitalWrite(PIN_SPK_EN, LOW);   // 初始静音
}

void radioInit() {
  codecInit();

  if (!audio.setPinout(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_MCLK))
    Serial.println("[Radio] setPinout 失败(I2S 引脚被占用?)");
  audio.setConnectionTimeout(6000, 6000);
  audio.forceMono(true);
  applyVolume();

  // 4.x 的回调改成了 std::function 事件表, 老的 audio_showstreamtitle() 不会被调用,
  // 所以之前"歌名一直不刷新"。
  audio.audio_info_callback = [](Audio::msg_t m) {
    if (m.e == Audio::evt_streamtitle || m.e == Audio::evt_name) {
      if (m.s && m.s[0]) {
        strncpy(g_streamTitle, m.s, sizeof(g_streamTitle) - 1);
        g_streamTitle[sizeof(g_streamTitle) - 1] = 0;
        uiFlashRadioLine();
      }
    } else if (m.e == Audio::evt_bitrate) {
      if (m.arg1 > 0) g_radioBitrate = (uint32_t)m.arg1;
    } else if (m.e == Audio::evt_eof) {
      if (!s_userStopped) {
        g_radioHasAudio = false;
        g_radioState = RADIO_ERROR;
        s_retryAt = millis() + 2000;
        uiFlashRadioLine();
      }
    }
  };
}

void radioSetVolume(int v) {
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  g_volume = v;
  applyVolume();
  cfgSaveVolume();     // 只标记, 由 cfgFlushVolume() 延迟落盘
}

int radioGetVolume() { return g_volume; }

void radioPlay(int idx)  { s_pendingPlay = idx; s_userStopped = false; }
void radioTogglePlay()   { s_pendingToggle = true; }
void radioStop()         { s_pendingStop = true; }

void radioNextStation() {
  if (g_nStation > 0) {
    int nx = (g_curStation + 1) % g_nStation;
    g_curStation = nx;
    s_pendingPlay = nx;
  }
}

const char* stationDisplayName(int idx) {
  if (idx < 0 || idx >= g_nStation) return "";
  return g_stations[idx].name;
}

static void failAndRetry(const char* why) {
  g_radioState = RADIO_ERROR;
  g_radioHasAudio = false;
  s_retryCnt++;
  s_retryAt = millis() + (s_retryCnt < 5 ? 4000 : 30000);
  Serial.printf("[Radio] %s (retry #%u)\n", why, (unsigned)s_retryCnt);
  uiFlashRadioLine();
}

static void doConnect(int idx) {
  if (idx < 0 || idx >= g_nStation) return;
  if (WiFi.status() != WL_CONNECTED) {
    // 没网不是"失败": 等网络回来再试, 否则一次没网就再也不播了
    g_radioState = RADIO_ERROR;
    g_radioHasAudio = false;
    s_retryAt = millis() + 8000;
    uiFlashRadioLine();
    return;
  }
  s_userStopped = false;
  digitalWrite(PIN_SPK_EN, HIGH);
  g_streamTitle[0] = 0;
  g_radioBitrate = 0;
  g_radioSec = 0;
  g_radioHasAudio = false;
  s_lastSec = 0xFFFFFFFF;

  // 注意: connecttohost() 只表示"URL 解析/HTTP 建连成功", 真正出音要等
  // loop() 里解到数据。以前直接把状态置成"播放", 于是出现"显示播放中但没声音"。
  g_radioState = RADIO_CONNECTING;
  s_connectAt = millis();
  s_lastAudioAt = millis();
  uiFlashRadioLine();

  bool ok = audio.connecttohost(g_stations[idx].url);
  Serial.printf("[Radio] connect %s -> %s\n", g_stations[idx].url, ok ? "ok" : "fail");
  if (!ok) {
    failAndRetry("connect fail");
    return;
  }
  s_retryCnt = 0;
  cfgSaveCurStation();
}

// 判定"真的在出音": 有码率 / 输入缓冲有数据 / 播放时间在走
static bool audioFlowing() {
  if (audio.getBitRate() > 0) return true;
  if (audio.getAudioCurrentTime() > 0) return true;
  if (audio.inBufferFilled() > 0) return true;
  return false;
}

void radioLoopHandler() {
  if (s_pendingStop) {
    s_pendingStop = false;
    s_pendingPlay = -1;
    s_pendingToggle = false;
    s_userStopped = true;
    audio.stopSong();
    digitalWrite(PIN_SPK_EN, LOW);
    g_radioState = RADIO_IDLE;
    g_radioHasAudio = false;
    g_radioBitrate = 0;
    g_radioSec = 0;
    g_streamTitle[0] = 0;
    s_retryAt = 0;
    s_retryCnt = 0;
    Serial.println("[Radio] stop");
    uiFlashRadioLine();
  }
  if (s_pendingPlay >= 0) {
    int idx = s_pendingPlay;
    s_pendingPlay = -1;
    g_curStation = idx;
    doConnect(idx);
  }
  if (s_pendingToggle) {
    s_pendingToggle = false;
    if (g_radioState == RADIO_PLAYING || g_radioState == RADIO_CONNECTING) {
      audio.pauseResume();
      g_radioState = (g_radioState == RADIO_PLAYING) ? RADIO_PAUSED : RADIO_PLAYING;
      g_radioHasAudio = (g_radioState == RADIO_PLAYING);
      digitalWrite(PIN_SPK_EN, g_radioState == RADIO_PAUSED ? LOW : HIGH);
      uiFlashRadioLine();
    } else if (g_radioState == RADIO_IDLE || g_radioState == RADIO_ERROR) {
      doConnect(g_curStation);
    }
  }

  audio.loop();

  // ---- 出音看门狗 ----
  uint32_t now = millis();
  if (g_radioState == RADIO_PLAYING || g_radioState == RADIO_CONNECTING) {
    bool flow = audioFlowing();
    if (flow) {
      s_lastAudioAt = now;
      uint32_t sec = audio.getAudioCurrentTime();
      if (sec != s_lastSec) {
        s_lastSec = sec;
        g_radioSec = sec;
        uiFlashRadioLine();
      }
      g_radioBitrate = audio.getBitRate();
      if (g_radioState == RADIO_CONNECTING) {
        g_radioState = RADIO_PLAYING;
        Serial.printf("[Radio] 出音 bitrate=%u\n", (unsigned)g_radioBitrate);
        uiFlashRadioLine();
      }
    }
    // 6 秒容差: 网络抖动时缓冲瞬时为空不应该立刻报"无音频"
    g_radioHasAudio = flow || (now - s_lastAudioAt < 6000);

    if (g_radioState == RADIO_CONNECTING && now - s_connectAt > 15000) {
      audio.stopSong();
      failAndRetry("no audio in 15s");
    } else if (g_radioState == RADIO_PLAYING && now - s_lastAudioAt > 20000) {
      audio.stopSong();
      failAndRetry("audio stalled 20s");
    }
  }

  // 断流重连(用户主动停止过就不重连)
  // s_retryAt==0 说明"上次连的时候没网", 也要继续等, 不能永远停在失败态
  if (!s_userStopped && g_radioState == RADIO_ERROR) {
    if (s_retryAt == 0) s_retryAt = now + 8000;
    if (now > s_retryAt) {
      s_retryAt = 0;
      doConnect(g_curStation);
    }
  }
}
