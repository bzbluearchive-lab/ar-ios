// EspLoot.hpp —— 附近物资：类名匹配 + 根位置 + 距离文字
//
// 判定只用"类名里含什么"，不碰任何游戏私有结构体 —— 物资/箱子/弹药的字段布局
// 每个版本都在动，但它们的类名十年没变过（见 src/Game/Scene.hpp 的同一套思路）。
// 关键词全部走 XOR（espx::Lit），源码/二进制里没有明文类名可以扫。

#pragma once
#include <stdint.h>

#include "EspCommon.hpp"

namespace espx {
namespace loot {

// 画一个 actor（如果它像物资）。返回 true = 画了。
// 内部自带：距离上限、屏幕外剔除、每帧条数上限（settings().maxLoot）。
bool try_actor(uintptr_t actor, Frame& f);

// 类名是否像物资（给外面做诊断/统计用；同样只读类名）
bool looks_like_loot(uintptr_t actor);

} // namespace loot
} // namespace espx
