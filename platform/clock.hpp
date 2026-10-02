#pragma once
#include <cstdint>

// 单调毫秒时钟（音视频统一时间源；板端对齐 audio master clock 逻辑）
int64_t now_ms();
