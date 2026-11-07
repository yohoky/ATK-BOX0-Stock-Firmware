#pragma once
#include <Arduino.h>

void webBegin();          // 启动 HTTP 服务 (STA 或 AP 模式均可)
void webLoopHandler();    // 处理延后重启等
