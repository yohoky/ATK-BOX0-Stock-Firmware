#pragma once
#include <Arduino.h>

void stockTaskStart();     // 启动行情抓取任务 (core 0)
void stockRequestRefresh();// 网页保存自选股后立即刷新
void stockRebuildTicker(); // 通知 UI 重建滚动条
void stockPrevAmtInfo(char* day, size_t dayN, int* ok, double* baseAmt); // 同时段放量基准诊断
