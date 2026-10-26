// EspLoot.mm —— 物资绘制（只读 + ImGui 文字/圆点）
//
// 每帧每个 actor 的代价：一次类名解析（FNamePool 两次读）+ 一次根位置读 + 投影。
// 类名解析是这里面最贵的一步，所以先做"距离剔除"再做类名：
// 半径外的 actor 连名字都不用解。顺序反了在几百个 actor 的场景里会白烧 CPU。

#include "EspLoot.hpp"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../Game/UObject.hpp"     // ue::uobj::class_of
#include "imgui.h"

namespace espx {
namespace loot {

// ---------------------------------------------------------------- 关键词
// 每个词解码 -> 比较 -> 立刻擦掉，栈上不驻留明文。
// 想加词就在这里加一行 MATCH_ONE("xxx")，别去改调用逻辑。
#define MATCH_ONE(lit)                       \
    do {                                     \
        XDECL(w, lit);                       \
        const bool hit = contains_ci(cls, w); \
        memset(w, 0, sizeof(w));             \
        if (hit) return true;                \
    } while (0)

static bool match_loot_class(const char* cls) {
    if (!cls || !*cls) return false;
    MATCH_ONE("Pickup");
    MATCH_ONE("Loot");
    MATCH_ONE("Item");
    MATCH_ONE("Supply");
    MATCH_ONE("Crate");
    MATCH_ONE("Case");
    MATCH_ONE("Ammo");
    MATCH_ONE("Weapon");
    MATCH_ONE("Medic");
    MATCH_ONE("Valuab");
    return false;
}

#undef MATCH_ONE

bool looks_like_loot(uintptr_t actor) {
    if (!actor) return false;
    char cls[96];
    cls[0] = 0;
    if (!ue::uobj::class_of(actor, cls, sizeof(cls))) return false;
    return match_loot_class(cls);
}

static inline ImU32 c32(const float c[4]) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3]));
}

// ---------------------------------------------------------------- 画
bool try_actor(uintptr_t actor, Frame& f) {
    Settings& S = settings();
    if (!S.loot) return false;
    if (!actor) return false;
    if (f.loot >= S.maxLoot) return false;          // 每帧条数上限

    // 1) 位置先取：拿不到就没必要解类名
    FVector posF{};
    if (!world::actor_location(actor, posF)) return false;
    const espm::Vec3 pos = to_v3(posF);

    const float d = espm::depth_of(f.cam, pos);
    if (!(d > espm::kMinDepth)) return false;                 // 相机后方
    const float distCm = espm::dist(pos, f.cam.loc);          // 距离显示用真实距离
    if (f.maxDistCm > 0.0f && distCm > f.maxDistCm) return false;

    espm::Vec2 s{};
    if (!espm::world_to_screen(f.cam, pos, s)) return false;
    if (s.X < -64.0f || s.Y < -64.0f || s.X > f.scr.pxW + 64.0f || s.Y > f.scr.pxH + 64.0f)
        return false;                               // 屏幕外不画（文字也不用排）

    // 2) 类名判定
    char cls[96];
    cls[0] = 0;
    if (!ue::uobj::class_of(actor, cls, sizeof(cls))) return false;
    if (!match_loot_class(cls)) return false;

    // 3) 画：一个小圆点 + 名字（+ 距离）
    char name[96];
    shorten_class(cls, name, sizeof(name));

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImU32 col = c32(S.colLoot);
    const float px = f.scr.scale > 0.0f ? f.scr.scale : 1.0f;

    dl->AddCircleFilled(ImVec2(s.X, s.Y), 3.0f * px, col, 10);

    char line[192];
    if (S.distance) {
        XDECL(fmt, "%s %.0fm");
        snprintf(line, sizeof(line), fmt, name[0] ? name : cls, distCm / 100.0f);
    } else {
        snprintf(line, sizeof(line), "%s", name[0] ? name : cls);
    }
    dl->AddText(ImVec2(s.X + 5.0f * px, s.Y - 7.0f * px), col, line);

    ++f.loot;
    return true;
}

} // namespace loot
} // namespace espx
