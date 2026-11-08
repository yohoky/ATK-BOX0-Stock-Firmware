#include "radio_player.h"
#include "cfg_pins.h"
#include "ui_display.h"
#include <Wire.h>
#include <WiFi.h>
#include "Audio.h"   // ESP32-audioI2S (github.com/schreibfaul1/ESP32-audioI2S)
#include <driver/gpio.h>

bool              g_codecOk       = false;
volatile bool     g_radioHasAudio = false;
volatile uint32_t g_radioBitrate  = 0;
volatile uint32_t g_radioSec      = 0;

static uint8_t g_codecAddr = ES8311_I2C_ADDR;
static uint8_t g_codecIdH  = 0, g_codecIdL = 0;

// ---------------- ES8311 最小驱动 ----------------
// 播放链路沿用原厂 BOX0 固件的 ES8311 上电序列 + I2S 引脚(cfg_pins.h)。
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

// ES8311 DAC 音量寄存器 0x32: 0 = 约 -95.5dB(等同静音), 255 = +32dB。
// 官方 es8311_voice_volume_set(): reg32 = volume * 256 / 100 - 1。
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
  es8311Write(0x44, 0x08);   // I2C 字长 / 7bit
  delay(20);
  es8311Write(0x44, 0x08);
  es8311Write(0x01, 0x30);
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
  es8311Write(0x01, 0x3F);
  es8311Write(0x13, 0x10);
  es8311Write(0x1B, 0x0A);
  es8311Write(0x1C, 0x6A);
  es8311Write(0x44, 0x58);
  es8311Write(0x0E, 0x02);
  es8311Write(0x12, 0x00);
  es8311Write(0x13, 0x10);
  es8311Write(0x1C, 0x6A);
  es8311Write(0x31, 0x00);   // DAC 解除静音
  es8311Write(0x37, 0x08);
  es8311Write(0x45, 0x00);   // 关闭 ADC2DAC 旁路
  es8311SetVolume(g_volume); // 0x32 上电默认 0 = 静音, 必须显式写

  g_codecIdH = es8311Read(0xFD);
  g_codecIdL = es8311Read(0xFE);
  uint8_t pwr = es8311Read(0x00);
  Serial.printf("[ES8311] addr=0x%02X id=%02X%02X reg00=0x%02X\n",
                g_codecAddr, g_codecIdH, g_codecIdL, pwr);
  return (pwr == 0x80) || (g_codecIdH == 0x83);
}

void codecSetVolume(int vol) { if (g_codecOk) es8311SetVolume(vol); }
uint8_t codecVolumeReg()     { return g_codecOk ? es8311Read(0x32) : 0xFF; }
uint8_t codecI2CAddr()       { return g_codecAddr; }

// ---------------- 播放器 ----------------
// v7.2: Audio 对象改为动态创建。开机不创建 -> I2S 归 beep 提示音;
//   只有进入收音机模式才 new Audio 接管, 退出时 delete 释放 I2S(库析构里
//   会 i2s_channel_disable + i2s_del_channel), 再还给 beep。
// 0x7200 魔数段: 冷启动时 RTC_NOINIT 是随机值, 只有落在这个区间才认为是有效阶段标记
#define RS_BASE 0x7200
RTC_NOINIT_ATTR static volatile int s_radioStage;  // 不能赋初值(noinit 段), 用魔数判有效
void radioSetStage(int s) { s_radioStage = RS_BASE + s; }
int  radioStage() {
  int v = s_radioStage;
  return (v >= RS_BASE && v < RS_BASE + 256) ? (v - RS_BASE) : -1;
}

static Audio* s_audio = nullptr;

// 解码任务与主 loop 的互斥(轻量级标志, 不用锁: audio.loop() 持锁会卡住按键)
static volatile bool s_radioPaused = false;
static volatile bool s_radioBusy   = false;
void radioPause() {
  s_radioPaused = true;
  uint32_t t = millis();
  while (s_radioBusy && millis() - t < 3000) vTaskDelay(pdMS_TO_TICKS(2));
}
void radioResumeTask() { s_radioPaused = false; }
bool radioTaskCanRun() {
  return !s_radioPaused && s_audio != nullptr && g_appMode == APP_RADIO;
}
void radioTaskBegin() { s_radioBusy = true; }
void radioTaskEnd()   { s_radioBusy = false; }
static volatile int  s_pendingPlay   = -1;
static volatile bool s_pendingToggle = false;
static volatile bool s_pendingStop   = false;
static volatile bool s_userStopped   = false;
static uint32_t s_retryAt  = 0;
static uint8_t  s_retryCnt = 0;
static uint32_t s_connectAt = 0;
static uint32_t s_lastAudioAt = 0;
static uint32_t s_lastSec = 0xFFFFFFFF;

static void applyVolume() {
  if (s_audio) s_audio->setVolume(g_volume * 21 / 100);   // audioI2S: 0-21
  // 无论解码器在不在, ES8311 的 0x32 都要写(供提示音/MP3 共用)
  es8311SetVolume(g_volume);
}

void codecInit() {
  pinMode(PIN_CODEC_PWR, OUTPUT);
  digitalWrite(PIN_CODEC_PWR, HIGH);
  delay(30);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  gpio_pullup_en((gpio_num_t)PIN_I2C_SDA);
  gpio_pullup_en((gpio_num_t)PIN_I2C_SCL);
  delay(10);
  g_codecOk = es8311Init();
  if (!g_codecOk) {
    digitalWrite(PIN_CODEC_PWR, LOW);
    delay(80);
    digitalWrite(PIN_CODEC_PWR, HIGH);
    delay(60);
    g_codecOk = es8311Init();
  }
  pinMode(PIN_SPK_EN, OUTPUT);
  digitalWrite(PIN_SPK_EN, LOW);
}

// 开机: 只初始化 codec, 不碰 I2S(留给提示音)。
void radioInit() { codecInit(); }

bool radioActive() { return s_audio != nullptr; }

void radioEnsure() {
  radioSetStage(11);
  if (!g_codecOk) codecInit();
  if (s_audio) return;
  radioSetStage(12);
  Audio* a = new Audio();                    // 默认 I2S_NUM_0
  radioSetStage(13);
  if (!a->setPinout(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_MCLK))
    Serial.println("[Radio] setPinout 失败(I2S 仍被占用?)");
  radioSetStage(14);
  a->setConnectionTimeout(6000, 6000);
  a->forceMono(true);
  // 4.x 的回调改成了 std::function 事件表
  a->audio_info_callback = [](Audio::msg_t m) {
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
  s_audio = a;
  applyVolume();
  radioSetStage(15);
  Serial.println("[Radio] I2S 已交给 MP3 解码器");
}

void radioRelease() {
  s_pendingStop   = false;
  s_pendingPlay   = -1;
  s_pendingToggle = false;
  s_userStopped   = true;
  s_retryAt       = 0;
  s_retryCnt      = 0;
  if (s_audio) {
    s_audio->stopSong();
    delete s_audio;              // 析构里释放 I2S 通道
    s_audio = nullptr;
  }
  digitalWrite(PIN_SPK_EN, LOW);
  g_radioState    = RADIO_IDLE;
  g_radioHasAudio = false;
  g_radioBitrate  = 0;
  g_radioSec      = 0;
  g_streamTitle[0] = 0;
  uiFlashRadioLine();
  Serial.println("[Radio] I2S 已还给提示音");
}

void radioSetVolume(int v) {
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  g_volume = v;
  applyVolume();
  cfgSaveVolume();
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
  if (!s_audio || idx < 0 || idx >= g_nStation) return;
  radioSetStage(20);
  if (WiFi.status() != WL_CONNECTED) {
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
  g_radioState = RADIO_CONNECTING;
  s_connectAt = millis();
  s_lastAudioAt = millis();
  uiFlashRadioLine();

  bool ok = s_audio->connecttohost(g_stations[idx].url);
  radioSetStage(21);
  Serial.printf("[Radio] connect %s -> %s\n", g_stations[idx].url, ok ? "ok" : "fail");
  if (!ok) { failAndRetry("connect fail"); return; }
  s_retryCnt = 0;
  cfgSaveCurStation();
}

static bool audioFlowing() {
  if (!s_audio) return false;
  if (s_audio->getBitRate() > 0) return true;
  if (s_audio->getAudioCurrentTime() > 0) return true;
  if (s_audio->inBufferFilled() > 0) return true;
  return false;
}

void radioLoopHandler() {
  // 只有收音机模式才会创建解码器; 没创建时把待办请求清掉, 避免堆积
  if (!s_audio) {
    s_pendingStop = false;
    s_pendingToggle = false;
    s_pendingPlay = -1;
    return;
  }
  if (s_pendingStop) {
    s_pendingStop = false;
    s_pendingPlay = -1;
    s_pendingToggle = false;
    s_userStopped = true;
    s_audio->stopSong();
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
      s_audio->pauseResume();
      g_radioState = (g_radioState == RADIO_PLAYING) ? RADIO_PAUSED : RADIO_PLAYING;
      g_radioHasAudio = (g_radioState == RADIO_PLAYING);
      digitalWrite(PIN_SPK_EN, g_radioState == RADIO_PAUSED ? LOW : HIGH);
      uiFlashRadioLine();
    } else if (g_radioState == RADIO_IDLE || g_radioState == RADIO_ERROR) {
      doConnect(g_curStation);
    }
  }

  radioSetStage(30);
  s_audio->loop();
  radioSetStage(31);

  uint32_t now = millis();
  if (g_radioState == RADIO_PLAYING || g_radioState == RADIO_CONNECTING) {
    bool flow = audioFlowing();
    if (flow) {
      s_lastAudioAt = now;
      uint32_t sec = s_audio->getAudioCurrentTime();
      if (sec != s_lastSec) {
        s_lastSec = sec;
        g_radioSec = sec;
        uiFlashRadioLine();
      }
      g_radioBitrate = s_audio->getBitRate();
      if (g_radioState == RADIO_CONNECTING) {
        g_radioState = RADIO_PLAYING;
        Serial.printf("[Radio] 出音 bitrate=%u\n", (unsigned)g_radioBitrate);
        uiFlashRadioLine();
      }
    }
    g_radioHasAudio = flow || (now - s_lastAudioAt < 6000);
    if (g_radioState == RADIO_CONNECTING && now - s_connectAt > 15000) {
      s_audio->stopSong();
      failAndRetry("no audio in 15s");
    } else if (g_radioState == RADIO_PLAYING && now - s_lastAudioAt > 20000) {
      s_audio->stopSong();
      failAndRetry("audio stalled 20s");
    }
  }
  if (!s_userStopped && g_radioState == RADIO_ERROR) {
    if (s_retryAt == 0) s_retryAt = now + 8000;
    if (now > s_retryAt) {
      s_retryAt = 0;
      doConnect(g_curStation);
    }
  }
}
