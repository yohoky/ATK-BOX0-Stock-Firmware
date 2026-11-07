#pragma once
// ============================================================
// 正点原子 ESP32 AI BOX1 (atk-dnesp32s3-box0) 引脚定义
// 来源: xiaozhi-esp32 main/boards/alientek/atk-dnesp32s3-box0/config.h
// ============================================================

// 电源控制
#define PIN_SYS_POW     2    // 系统电源保持(高有效)
#define PIN_CODEC_PWR   14   // 音频codec供电使能
#define PIN_SPK_EN      21   // 功放(NS4150)使能
#define PIN_CHG_STATUS  48   // 充电状态指示(CHRG_PIN, 低=充电中)
#define PIN_CHG_CTRL    47   // 充电控制(CHG_CTRL_PIN)
#define PIN_BAT_VSEN    1    // 电池电压检测 ADC1_CH0 (BAT_VSEN_PIN)

// ---- 电池电量标定: 直接采用官方 power_manager.h 的 levels[] 表 ----
//   官方用 ADC_ATTEN_DB_12 / 12bit, 连续采 10 次取平均:
//     2951=0%(约3.80V) 3019=20% 3037=40% 3091=60%(约3.88V) 3124=80% 3231=100%
//   ⚠ 不要再用 "1:1 分压 × 3.3V" 换算, 那会算出 5189mV 这种不可能的电压。
static inline int batPctFromRaw(int raw) {
  static const int adc[] = {2951, 3019, 3037, 3091, 3124, 3231};
  static const int pct[] = {0, 20, 40, 60, 80, 100};
  if (raw <= adc[0]) return 0;
  if (raw >= adc[5]) return 100;
  for (int i = 0; i < 5; i++)
    if (raw >= adc[i] && raw < adc[i + 1])
      return pct[i] + (raw - adc[i]) * (pct[i + 1] - pct[i]) / (adc[i + 1] - adc[i]);
  return 100;
}
// 近似电压(仅诊断显示用): 官方注释 2951≈3.80V, 3091≈3.88V 线性外推
static inline int batMvFromRaw(int raw) {
  long mv = 3800L + (long)(raw - 2951) * 80L / 140L;
  if (mv < 3600) mv = 3600;
  if (mv > 4200) mv = 4200;
  return (int)mv;
}

// I2S -> ES8311
#define PIN_I2S_MCLK    13
#define PIN_I2S_WS      10
#define PIN_I2S_BCLK    5
#define PIN_I2S_DOUT    9    // ESP32 -> ES8311 (播放)
#define PIN_I2S_DIN     6    // ES8311 -> ESP32 (麦克风, 本项目不用)

// ES8311 I2C
#define PIN_I2C_SDA     11
#define PIN_I2C_SCL     12
#define ES8311_I2C_ADDR 0x18

// LCD (SPI, ST7789 240x240)
#define PIN_LCD_SCLK    39
#define PIN_LCD_MOSI    40
#define PIN_LCD_DC      38
#define PIN_LCD_CS      41
#define PIN_LCD_BL      42

// 三按键 (低电平按下)
#define PIN_BTN_L       3
#define PIN_BTN_M       4
#define PIN_BTN_R       0
