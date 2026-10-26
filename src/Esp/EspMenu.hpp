// EspMenu.hpp —— ImGui 菜单 + 常驻小按钮 + 命中判定
//
// 为什么要有常驻按钮：菜单被关掉（或 ImGui 状态被游戏重置）之后，没有入口就再也
// 打不开了。按钮画在 background draw list 上，与菜单开关无关，永远可见。
//
// 命中判定用的是【上一帧记下来的矩形】（像素坐标），因为触摸事件在两帧之间到达，
// 那时 ImGui 还没跑这一帧的 NewFrame —— 不能靠 io.WantCaptureMouse。

#pragma once
#include <stdint.h>

#include "EspCommon.hpp"

namespace espx {
namespace menu {

// 画（每帧一次，在 ImGui::NewFrame() 之后、Render() 之前）。f 只用于显示统计数字。
void draw(const espm::ScreenMap& scr, Frame& f);

bool open();
void toggle();

// 命中判定参数都是【像素】：触摸点 = 点坐标 * scr.scale
bool hit_button(float px, float py);   // 只命中左上角常驻按钮
bool hit_window(float px, float py);   // 只命中菜单窗口
bool hit_any(float px, float py);      // 两者之一（决定要不要吃掉这次触摸）

} // namespace menu
} // namespace espx
