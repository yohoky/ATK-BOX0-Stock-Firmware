#pragma once
#include <Arduino.h>

// ============================================================
// QMI8658 六轴 IMU 探测(横竖屏自动切换)
//   ⚠ 官方 atk-dnesp32s3-box0/config.h 经核对【没有 IMU 定义】,
//     I2C 总线上只有 ES8311(0x18/0x19)。gyroPresent() 会稳定为 false,
//     重力自动切换在本板上硬件不支持, 只能用菜单/按键手动切显示模式。
//   I2C 总线: 与 ES8311 共用 (SDA=11, SCL=12, 见 cfg_pins.h)
// ============================================================

// 横竖屏判断结果
enum GyroOrientation {
  GYRO_UNKNOWN = 0,   // 未检测到传感器
  GYRO_PORTRAIT,      // 竖屏 (重力沿 Y 轴)
  GYRO_LANDSCAPE      // 横屏 (重力沿 X 轴)
};

// 初始化: 探测 I2C 地址并配置传感器。返回是否成功。
bool gyroInit();

// 读当前姿态。无传感器返回 GYRO_UNKNOWN。
GyroOrientation gyroRead();

// 传感器是否在线(诊断页用)
bool gyroPresent();
// 诊断: 返回 I2C 总线上每个应答地址 + 常见 WHO_AM_I 寄存器读值,
//   格式 "0x18:00=00/0F=00/75=00;..." (0xFF=读失败), 用于确认板上到底有没有 IMU
String gyroProbeText();

// I2C 地址(诊断页显示, 0x6A 或 0x6B)
uint8_t gyroAddr();

// 诊断: 扫描当前 I2C 总线(SDA=11/SCL=12)上所有从机地址。
//   返回发现的设备数, 地址写入 out(最多 maxN 个)。
int gyroI2CScan(uint8_t* out, int maxN);
