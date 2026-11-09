#include "buttons.h"
#include "cfg_pins.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

// ── 时间参数 ───────────────────────────────────────────────
#define DEBOUNCE_MS     15     // 去抖
#define LONG_MS         480    // 长按判定
#define REPEAT_MS       170    // 长按后连发(音量)
#define MULTI_MS        320    // v7.2: M 连击窗口(单击/三连/四连最终判定)
#define COMBO_MAX_MS    1200   // 左右同时按: 小于此值松开 = 组合短按
#define COMBO_POWER_MS  1500   // 左右同时按住超过此值 = 关机
#define MR_COMBO_MS     3000   // 中+右同时按住超过此值 = 强制进下载模式

struct Key {
  uint8_t  pin;
  bool     raw       = false;
  bool     down      = false;
  bool     longFired = false;
  bool     suppress  = false;   // 本次按住期间不再产生普通事件
  uint32_t tChange   = 0;
  uint32_t tDown     = 0;
  uint32_t tRepeat   = 0;
};

static Key s_k[3];                     // 0=左 1=中 2=右
static QueueHandle_t s_q = nullptr;
static volatile uint32_t s_edge[3] = {0, 0, 0};   // 电平变化次数(诊断 M 键用)

// v7.2 M 键连击状态机:
//   第 2 下"按下"时立即发 BTN_M_DOUBLE(保持旧版双击立即刷新行情的手感),
//   同时继续累计; 窗口(320ms)内没有更多按下时按总次数发最终事件:
//     1 -> SHORT(切显示)  2 -> 已发 DOUBLE  3 -> TRIPLE(新闻)  >=4 -> QUAD(收音机)
//   代价: 三连/四连会顺带多刷一次行情, 无感; 单击本来就要等双击窗口, 无回归。
static bool     s_mSeq     = false;
static uint8_t  s_mClicks  = 0;
static uint32_t s_mLastDown = 0;
static bool     s_combo    = false;    // 左右同时按生效中
static uint32_t s_comboAt  = 0;
static bool     s_comboLong = false;
static bool     s_comboWasLong = false; // 组合形成前已有一根键发过长按(音量) -> 不再当"双键短按"
static bool     s_mrCombo  = false;    // 中+右同时按生效中
static uint32_t s_mrAt     = 0;
static bool     s_mrLong   = false;

static void push(BtnEvent e) {
  if (!s_q) return;
  xQueueSend(s_q, &e, 0);
}

// 采样一次状态机(必须 >= 4ms 调一次, 否则短按可能漏)
static void sample() {
  uint32_t now = millis();

  // ---- v7.2: M 连击窗口到期, 发最终事件 ----
  if (s_mSeq && (now - s_mLastDown) >= MULTI_MS) {
    uint8_t c = s_mClicks;
    s_mSeq = false;
    s_mClicks = 0;
    if (c == 1)      push(BTN_M_SHORT);
    else if (c == 3) push(BTN_M_TRIPLE);
    else if (c >= 4) push(BTN_M_QUAD);
    // c == 2: 第 2 下按下时已立即发过 DOUBLE, 不重复
  }

  for (int i = 0; i < 3; i++) {
    Key& k = s_k[i];
    // 中键(GPIO4)=高电平有效, 左右键=低电平有效(见 btnInit 里的官方依据)
    bool raw = (i == 1) ? (digitalRead(k.pin) == HIGH)
                        : (digitalRead(k.pin) == LOW);

    if (raw != k.raw) {                // 电平变化 -> 重新计时去抖
      s_edge[i]++;                     // 诊断: 只要引脚有跳变就计数
      k.raw     = raw;
      k.tChange = now;
      continue;
    }
    if ((now - k.tChange) < DEBOUNCE_MS) continue;   // 未稳定
    if (raw == k.down) continue;                     // 无新状态

    k.down = raw;
    if (k.down) {                      // ---- 按下 ----
      k.tDown     = now;
      k.tRepeat   = now;
      k.longFired = false;
      k.suppress  = false;
      push(BTN_ANY_DOWN);              // 熄屏时"按下即亮", 不用等松开
      if (i == 1 && !s_k[0].down && !s_k[2].down) {
        // M 单独按下才计连击; M+右是强制下载组合, 不计
        if (!s_mSeq) { s_mSeq = true; s_mClicks = 0; }
        if (s_mClicks < 6) s_mClicks++;
        s_mLastDown = now;
        if (s_mClicks == 2) push(BTN_M_DOUBLE);   // 双击零延迟, 立即刷新行情
      }
    } else {                           // ---- 松开 ----
      // M 的短按统一由上面的连击窗口判定, 这里不再产生 s_mPending。
      // 不设 k.suppress, 否则长按循环会把 M 长按一起跳过。
      if (!k.longFired && !k.suppress && !s_combo && i != 1) {
        push(i == 0 ? BTN_L_SHORT : BTN_R_SHORT);
      }
    }
  }

  // ---- 左右同时按: 抑制单键, 只产生组合事件 ----
  bool both = s_k[0].down && s_k[2].down;
  if (both && !s_combo) {
    s_combo     = true;
    s_comboLong = false;
    s_comboAt   = now;
    s_comboWasLong = s_k[0].longFired || s_k[2].longFired;
    s_k[0].longFired = true;
    s_k[2].longFired = true;
    s_k[0].suppress  = true;
    s_k[2].suppress  = true;
  }
  if (s_combo) {
    if (both) {
      if (!s_comboLong && (now - s_comboAt) >= COMBO_POWER_MS) {
        s_comboLong = true;
        push(BTN_LR_LONG);
      }
    } else {
      uint32_t dur = now - s_comboAt;
      s_combo = false;
      if (!s_comboLong && !s_comboWasLong && dur >= 30 && dur < COMBO_MAX_MS)
        push(BTN_LR_SHORT);
    }
  }

  // ---- 中键 + 右键同时按: 强制进下载模式(刷机用) ----
  bool mrBoth = s_k[1].down && s_k[2].down && !s_k[0].down;   // M+R, 不含左键
  if (mrBoth && !s_mrCombo) {
    s_mrCombo = true;
    s_mrLong  = false;
    s_mrAt    = now;
    s_mSeq = false; s_mClicks = 0;   // M+R 组合不算连击, 避免误进新闻/收音机
    // 抑制 M 和 R 的单独事件
    s_k[1].suppress = true;
    s_k[2].suppress = true;
    s_k[1].longFired = true;
    s_k[2].longFired = true;
  }
  if (s_mrCombo) {
    if (mrBoth) {
      if (!s_mrLong && (now - s_mrAt) >= MR_COMBO_MS) {
        s_mrLong = true;
        push(BTN_MR_LONG);
      }
    } else {
      s_mrCombo = false;
    }
  }

  // ---- 单键长按 + 连发 ----
  for (int i = 0; i < 3; i++) {
    Key& k = s_k[i];
    if (!k.down || k.suppress || s_combo) continue;
    if (!k.longFired) {
      if ((now - k.tDown) >= LONG_MS) {
        k.longFired = true;
        if (i == 1) { s_mSeq = false; s_mClicks = 0; push(BTN_M_LONG); }
        else        { push(i == 0 ? BTN_L_LONG : BTN_R_LONG); }
      }
    } else if (i != 1 && (now - k.tRepeat) >= REPEAT_MS) {
      k.tRepeat = now;
      push(i == 0 ? BTN_L_LONG : BTN_R_LONG);   // 音量连调
    }
  }
}

static void btnTask(void* arg) {
  (void)arg;
  for (;;) {
    sample();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void btnInit() {
  s_k[0].pin = PIN_BTN_L;
  s_k[1].pin = PIN_BTN_M;
  s_k[2].pin = PIN_BTN_R;
  for (int i = 0; i < 3; i++) {
    gpio_num_t p = (gpio_num_t)s_k[i].pin;
    // 关机时 powerOff() 用 ext1 唤醒 + rtc_gpio_pullup_en 把按键交给 RTC 域,
    // 深睡唤醒(实为复位重启)后这些配置与 GPIO_HOLD 可能仍残留。
    rtc_gpio_deinit(p);
    gpio_hold_dis(p);
    gpio_reset_pin(p);
    if (i == 1) {
      // 官方 atk_dnesp32s3_box0.cc: middle_button_(M_BUTTON_GPIO, true)
      // 即【中键高电平有效】, 左右键低电平有效。
      pinMode(p, INPUT_PULLDOWN);
      gpio_pulldown_en(p);
      gpio_pullup_dis(p);
    } else {
      pinMode(p, INPUT_PULLUP);
      gpio_pullup_en(p);
    }
    delayMicroseconds(100);
    // 不能用 digitalRead 初始化 down, 否则上电瞬间浮空会把状态机卡死。
    s_k[i].raw = s_k[i].down = false;
    s_k[i].tChange = millis();
  }
  delay(2);
  Serial.printf("[BTN] L=%d M=%d R=%d (L/R: 1=未按 0=按下 | M: 0=未按 1=按下)\n",
                digitalRead(PIN_BTN_L), digitalRead(PIN_BTN_M), digitalRead(PIN_BTN_R));
  if (!s_q) s_q = xQueueCreate(16, sizeof(BtnEvent));
}

void btnTaskStart() {
  xTaskCreatePinnedToCore(btnTask, "btn", 3072, nullptr, 4, nullptr, 1);
}

BtnEvent btnPoll() {
  BtnEvent e = BTN_NONE;
  if (!s_q) return BTN_NONE;
  if (xQueueReceive(s_q, &e, 0) != pdTRUE) return BTN_NONE;
  return e;
}

void btnFlush() {
  if (s_q) xQueueReset(s_q);
  for (int i = 0; i < 3; i++) {
    s_k[i].suppress  = s_k[i].down;
    s_k[i].longFired = s_k[i].down;
  }
  s_mSeq = false;
  s_mClicks = 0;
}

uint8_t btnRawState() {
  uint8_t r = 0;
  if (digitalRead(PIN_BTN_L) == LOW) r |= 0x01;
  if (digitalRead(PIN_BTN_M) == HIGH) r |= 0x02;   // 中键高有效
  if (digitalRead(PIN_BTN_R) == LOW) r |= 0x04;
  return r;
}

uint32_t btnEdgeCount(int i) {
  return (i >= 0 && i < 3) ? s_edge[i] : 0;
}

void btnInject(BtnEvent e) {
  if (e != BTN_NONE) push(e);
}
