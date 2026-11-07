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
#define DBL_MS          280    // 中键双击窗口
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

static bool     s_mPending = false;    // 中键短按待确认(双击窗口)
static uint32_t s_mUp      = 0;
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

  // 中键双击窗口到期 -> 确认为短按
  if (s_mPending && (now - s_mUp) >= DBL_MS) {
    s_mPending = false;
    push(BTN_M_SHORT);
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
      if (i == 1 && s_mPending) {      // 中键第二下 -> 双击
        s_mPending = false;
        k.suppress = true;             // 这一下不再产生短按
        push(BTN_M_DOUBLE);
      }
    } else {                           // ---- 松开 ----
      if (!k.longFired && !k.suppress && !s_combo) {
        if (i == 1) { s_mPending = true; s_mUp = now; }
        else        { push(i == 0 ? BTN_L_SHORT : BTN_R_SHORT); }
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
    // 两根键在本次按住期间不再单独发事件(也不会连调音量)
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
        if (i == 1) { s_mPending = false; push(BTN_M_LONG); }
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
    // 必须全部释放再配 digital 域, 否则 GPIO4(M键) 读不到真实电平。
    rtc_gpio_deinit(p);
    gpio_hold_dis(p);
    gpio_reset_pin(p);
    if (i == 1) {
      // ★ 真正的根因: 官方 atk_dnesp32s3_box0.cc 中
      //     middle_button_(M_BUTTON_GPIO, true) / left,right(..., false)
      //   即【中键高电平有效, 左右键低电平有效】。之前三个键一律 INPUT_PULLUP,
      //   GPIO4 被内部上拉钳死在高电平, 按下与松开都读到 1,
      //   所以 btnEdgeM 恒为 0、开机后 M 键完全无反应(关机开机走的是硬件电源锁存)。
      pinMode(p, INPUT_PULLDOWN);
      gpio_pulldown_en(p);
      gpio_pullup_dis(p);
    } else {
      pinMode(p, INPUT_PULLUP);
      gpio_pullup_en(p);
    }
    delayMicroseconds(100);          // 等上/下拉把引脚稳定到未按电平
    // ★ 关键修复: 绝对不能用 digitalRead 的结果初始化 down。
    //   上电瞬间若上拉未稳/引脚浮空读到 LOW, down 被置 true,
    //   状态机认为"键已被按住"; 用户再按时 raw 一直是 true, 无任何跳变,
    //   于是"关机按 M 能开机(硬件唤醒正常), 开机后 M 键完全失效"。
    //   统一从"未按下"起步, 由 5ms 采样任务自行同步真实电平。
    s_k[i].raw = s_k[i].down = false;
    s_k[i].tChange = millis();
  }
  delay(2);
  Serial.printf("[BTN] L=%d M=%d R=%d (L/R: 1=未按 0=按下 | M: 0=未按 1=按下)\n",
                digitalRead(PIN_BTN_L), digitalRead(PIN_BTN_M), digitalRead(PIN_BTN_R));
  if (!s_q) s_q = xQueueCreate(16, sizeof(BtnEvent));
}

void btnTaskStart() {
  // 引脚 4(中键) 在某些批次的 BOX0 上接触不良, 所以所有功能都做了
  // "左右同时按" 的兜底路径, 不依赖中键也能全功能操作。
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
  s_mPending = false;
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
