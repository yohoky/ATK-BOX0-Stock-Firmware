#include "gyro_imu.h"
#include "cfg_pins.h"
#include <Wire.h>

// QMI8658 寄存器 (仅用到的部分)
#define QMI_WHO_AM_I     0x00   // 应返回 0x05
#define QMI_CTRL7        0x08   // 使能加速度计
#define QMI_CTRL1        0x02   // 地址选择/复位
#define QMI_ACC_X_L      0x35   // 加速度 X 低字节
#define QMI_ACC_X_H      0x36

static uint8_t  s_addr = 0;
static bool     s_ok   = false;

struct ProbeEnt { uint8_t addr, who00, who0F, who75; };
static ProbeEnt s_probe[24];
static int      s_probeN = 0;

// 读任意从机任意寄存器(探测阶段用, 不依赖 s_addr)
static bool regReadAt(uint8_t addr, uint8_t reg, uint8_t* val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, 1) != 1) return false;
  if (!Wire.available()) return false;
  *val = Wire.read();
  return true;
}

String gyroProbeText() {
  String s;
  for (int i = 0; i < s_probeN; i++) {
    if (i) s += ';';
    char b[48];
    snprintf(b, sizeof(b), "0x%02X:00=%02X/0F=%02X/75=%02X",
             s_probe[i].addr, s_probe[i].who00, s_probe[i].who0F, s_probe[i].who75);
    s += b;
  }
  return s.length() ? s : String("no-ack");
}

// 读一个寄存器
static bool regRead(uint8_t reg, uint8_t* val) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)s_addr, 1) != 1) return false;
  if (!Wire.available()) return false;
  *val = Wire.read();
  return true;
}

static bool regWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool gyroInit() {
  s_ok = false;
  s_addr = 0;
  s_probeN = 0;

  // 官方 atk-dnesp32s3-box0/config.h 里只有 ES8311(0x18), 【完全没有 IMU 条目】,
  // 板上确实没有六轴传感器, 重力自动切换在本硬件上无法实现。
  // ★ 不再全总线扫 0x08..0x77: 弱上拉时会冒出大量幽灵 ACK, 每次启动还把
  //   整条 I2C 刷一遍, 可能把 ES8311 的寄存器序列冲掉。只核对常见 IMU 地址。
  static const uint8_t imuCand[] = {0x6A, 0x6B, 0x68, 0x69, 0x4C, 0x4D, 0x1E, 0x18, 0x19};
  for (uint8_t a : imuCand) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() != 0) continue;
    ProbeEnt e = { a, 0xFF, 0xFF, 0xFF };
    uint8_t v = 0;
    if (regReadAt(a, 0x00, &v)) e.who00 = v;   // QMI8658=0x05 / BMA4xx=0x13
    if (regReadAt(a, 0x0F, &v)) e.who0F = v;   // LIS2DH/LIS3DH=0x33 / SC7A20=0x11
    if (regReadAt(a, 0x75, &v)) e.who75 = v;   // MPU6050=0x68 / ICM42688=0x47
    if (s_probeN < 24) s_probe[s_probeN++] = e;
    if (e.who00 == 0x05) {                      // 确认为 QMI8658
      s_addr = a;
      s_ok = true;
    }
  }
  Serial.printf("[IMU] probe: %s\n", gyroProbeText().c_str());
  if (!s_ok) {
    Serial.println("[IMU] 未发现 QMI8658(WHO_AM_I=0x05), 重力自动切换降级为列表模式");
    return false;
  }

  // 软复位
  regWrite(0x01, 0x01);   // CTRL2 reset
  delay(20);

  // CTRL1: 地址配置(bit0=SA0 由硬件定), 这里只需唤醒
  regWrite(QMI_CTRL1, 0x60);   // 地址自动 + 常规模式
  // CTRL7: 使能加速度计, 量程 ±4g, 采样率约 250Hz
  regWrite(QMI_CTRL7, 0x03);   // accel enable + 使能

  // 读回验证
  uint8_t who = 0;
  if (!regRead(QMI_WHO_AM_I, &who) || who != 0x05) {
    s_ok = false;
    Serial.printf("[IMU] WHO_AM_I 校验失败 (0x%02X)\n", who);
    return false;
  }

  Serial.printf("[IMU] QMI8658 ok @0x%02X\n", s_addr);
  return true;
}

bool gyroPresent() { return s_ok; }
uint8_t gyroAddr() { return s_addr; }

// 扫描 I2C 总线(SDA=11/SCL=12), 返回发现的所有从机地址
int gyroI2CScan(uint8_t* out, int maxN) {
  // 只核对已知设备地址。全总线扫描会报出 32 个随机假地址(两次查询完全不同),
  // 且每查一次 /api/diag 都要刷一遍总线, 对 codec 有干扰。
  static const uint8_t known[] = {0x18, 0x19, 0x1E, 0x4C, 0x4D, 0x68, 0x69, 0x6A, 0x6B};
  int n = 0;
  for (uint8_t a : known) {
    if (n >= maxN) break;
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) out[n++] = a;
  }
  return n;
}

// ---------------- v7.1 决定性硬件验证 ----------------
// 官方宣传写「电子木鱼 3D 体感」, 但小智固件与板级 config.h 都没有 IMU 驱动。
// 这个接口严格扫一遍 0x08..0x77, 用"两次 ACK + 两次读一致"过滤幽灵 ACK,
// 一次性回答"板上到底有没有姿态传感器"。仅供登录后诊断调用, 不在开机路径执行。
static bool strictRegRead(uint8_t addr, uint8_t reg, uint8_t* val) {
  uint8_t a = 0, b = 0;
  if (!regReadAt(addr, reg, &a)) return false;
  if (!regReadAt(addr, reg, &b)) return false;
  if (a != b) return false;
  *val = a;
  return true;
}

int gyroFullScan(String& out, int pwrPin) {
  out = "";
  // 仅在显式传 pwr 时临时拉高一个 GPIO 供电(排查"传感器需单独使能"的情况);
  // 本项目已占用的引脚一律排除, 避免误动 LCD / codec / 电源锁存 / 按键。
  static const int used[] = {0, 1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14,
                             21, 38, 39, 40, 41, 42, 47, 48};
  bool powered = false;
  if (pwrPin >= 0 && pwrPin <= 48) {
    bool busy = false;
    for (int u : used) if (u == pwrPin) { busy = true; break; }
    if (!busy) {
      pinMode(pwrPin, OUTPUT);
      digitalWrite(pwrPin, HIGH);
      powered = true;
      delay(80);
    }
  }
  int n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() != 0) continue;
    delayMicroseconds(150);
    Wire.beginTransmission(a);
    if (Wire.endTransmission() != 0) continue;   // 二次确认, 过滤幽灵 ACK
    uint8_t v00 = 0xFF, v0F = 0xFF, v75 = 0xFF;
    strictRegRead(a, 0x00, &v00);
    strictRegRead(a, 0x0F, &v0F);
    strictRegRead(a, 0x75, &v75);
    char b[40];
    snprintf(b, sizeof(b), "%s0x%02X:00=%02X/0F=%02X/75=%02X",
             n ? ";" : "", a, v00, v0F, v75);
    out += b;
    if (++n >= 48) break;
  }
  if (powered) { digitalWrite(pwrPin, LOW); pinMode(pwrPin, INPUT); }
  return n;
}

GyroOrientation gyroRead() {
  if (!s_ok) return GYRO_UNKNOWN;

  uint8_t xl = 0, xh = 0, yl = 0, yh = 0;
  if (!regRead(QMI_ACC_X_L, &xl)) return GYRO_UNKNOWN;
  if (!regRead(QMI_ACC_X_H, &xh)) return GYRO_UNKNOWN;
  if (!regRead(QMI_ACC_X_L + 2, &yl)) return GYRO_UNKNOWN;   // Y 低 = 0x37
  if (!regRead(QMI_ACC_X_H + 2, &yh)) return GYRO_UNKNOWN;   // Y 高 = 0x38

  int16_t ax = (int16_t)((xh << 8) | xl);
  int16_t ay = (int16_t)((yh << 8) | yl);

  // 竖屏: 重力主要在 Y 轴 (|ay| > |ax|); 横屏: 重力主要在 X 轴
  if (abs(ax) > abs(ay)) return GYRO_LANDSCAPE;
  return GYRO_PORTRAIT;
}
