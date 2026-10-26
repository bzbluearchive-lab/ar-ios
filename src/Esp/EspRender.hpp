// EspRender.hpp —— 呈现层对外接口（ImGui 初始化 + Metal 挂点 + 帧回调 + 触摸注入）
//
// 两种接法，二选一：
//   A. 独立形态（推荐）：Tweak 里只调 espx::install()，本层自己挂 Metal 与 sendEvent。
//   B. 借宿主形态：已有别的帧回调（比如旧的 Render/Overlay.mm），在它的回调里调
//      espx::on_frame(drawable, w, h)；触摸自己算好像素坐标调 espx::on_touch()。
//
// ★ A 与 B 的 ImGui 上下文不能共存：一个进程只能有一个 ImGui context，两套
//   NewFrame/Render 交替跑会直接崩。要么只用本层，要么只用旧的 Overlay ——
//   具体改法见 README 里的 patch。

#pragma once
#include <stdint.h>

#include "EspCommon.hpp"

#ifdef __OBJC__
#import <Foundation/Foundation.h>
#endif

namespace espx {

// 挂点：-[CAMetalLayer nextDrawable] + drawable 的 present/presentAtTime: + -[UIApplication sendEvent:]
void install();

// ImGui + Metal 后端是否就绪（设备对象建成功才算 ready）
bool ready();

// 一帧（若外部已有 Metal 钩子，从这里驱动）。vpW/vpH 是 drawable 像素尺寸。
void on_frame(
#ifdef __OBJC__
    id drawable,
#else
    void* drawable,
#endif
    float vpW, float vpH);

// 触摸注入。px/py 是【像素】坐标；phase：0 = down / 1 = move / 2 = up。
// 返回 false = ImGui 还没起来，调用方不必吞事件。
bool on_touch(float px, float py, int phase);

// 已经画了多少帧（诊断：0 说明挂点没进，不是画不出来）
int64_t frame_count();

} // namespace espx
