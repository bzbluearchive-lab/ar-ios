// EspCommon.hpp —— ESP 层的公共小东西：编译期字符串、设置项、每帧上下文、字符串工具
//
// 这一层故意不 include imgui.h：
//   画图在各自 .mm 里做，Common 只放"数据 + 纯函数"，这样 EspMath/EspCommon 都能在
//   桌面上用普通 g++ 编译做数值测试（见 README「自查」一节）。

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "EspMath.hpp"
#include "EspOffsets.hpp"

#include "../Core/Obfs.hpp"      // AR_XKEY（与 Makefile 的 -DAR_XKEY=0x5B 同一把钥匙）
#include "../Core/SafeMem.h"     // mem::read / mem::rd / mem::rdptr —— 本层唯一内存入口
#include "../Game/World.hpp"     // FVector / FRotator / FMinimalViewInfo / TArrayRaw / world::

namespace espx {

// ---------------------------------------------------------------- 编译期字符串
// 为什么不直接用 Obfs.hpp 的 XSTR：
//   XStr<N>::get() 把结果写进 `static char buf[N]`，而模板是按【长度】实例化的 ——
//   所有等长常量共用同一个 buf，第二次调用直接返回第一个串（"a" 和 "b" 会互相顶掉，
//   旧 Overlay.mm 里那排单字菜单标签就是这个现象）。
// 这里改成解码到【调用方给的栈缓冲】，长度绑在类型里，用完由调用方 memset 掉：
//
//     XDECL(lbl, "skeleton");      // 声明 char lbl[9] 并就地解码
//     ImGui::Checkbox(lbl, &S.skeleton);
//     memset(lbl, 0, sizeof(lbl)); // 需要的话
//
// 注意：解码后的明文只要还活着一帧，内存扫描就能看见 —— 所以别往 static 里放，
// 也别跨帧缓存。菜单/物资名这种每帧几十次的解码开销可以忽略。
template <size_t N>
struct Lit {
    char e[N];

    constexpr Lit(const char (&s)[N]) : e{} {
        for (size_t i = 0; i < N; ++i) e[i] = (char)(s[i] ^ AR_XKEY);
    }

    void dec(char (&out)[N]) const {
        for (size_t i = 0; i < N; ++i) out[i] = (char)(e[i] ^ AR_XKEY);
    }
};

// XDECL(名字, 字面量)：在当前作用域里声明一个已解码的 C 字符串
#define XDECL(name, lit) char name[sizeof(lit)]; (espx::Lit<sizeof(lit)>{lit}).dec(name)

// ---------------------------------------------------------------- 设置项
// 菜单只改这里；渲染层每帧读这里。没有其它状态。
struct Settings {
    // ---- 显示开关 ----
    bool skeleton  = true;      // 骨骼连线
    bool equipment = true;      // 头/胸/腰 装备框
    bool loot      = true;      // 附近物资
    bool distance  = true;      // 距离数字
    bool box       = true;      // 骨骼取不到时的兜底方框
    bool swallowTouch = true;   // 触摸落在 UI 上时不传给游戏

    // ---- 性能上限（每个都可从菜单调）----
    int  maxActors = 512;       // 每帧最多看多少个 actor
    int  maxBones  = 60;        // 每帧最多画多少根骨线
    int  maxLoot   = 96;        // 每帧最多画多少个物资

    // ---- 骨骼布局：0 = 紧凑 FTransform（0x30 步长）/ 1 = vectorized（0x50 步长）----
    // 选错的症状：骨线全部挤成一团或位置乱飘。见 EspBones.hpp 的 BoneLayout。
    int  boneLayout = 0;

    // ---- 参数 ----
    float maxDistM  = 300.0f;   // 显示半径（米）
    float lineThick = 1.6f;
    float boxThick  = 1.2f;
    float fontScale = 1.0f;     // 物资/距离文字相对字号

    // ---- 颜色（RGBA，0..1，菜单用 ColorEdit4 改）----
    float colSkel[4]  = { 0.00f, 1.00f, 0.47f, 0.86f };
    float colEquip[4] = { 0.47f, 0.78f, 1.00f, 0.78f };
    float colLoot[4]  = { 1.00f, 0.82f, 0.24f, 0.86f };
};

Settings& settings();

// ---------------------------------------------------------------- 每帧上下文
// 一帧只建一次，骨头 / 物资 / 菜单都读它，避免各自重复解世界与相机。
struct Frame {
    espm::CameraView cam{};
    espm::ScreenMap  scr{};
    float maxDistCm = 30000.0f;

    // 统计（菜单里显示，也是"到底有没有数据"的唯一判据）
    int visited   = 0;   // 本帧看了多少 actor
    int players   = 0;   // 认出的角色数
    int loot      = 0;   // 画出来的物资数
    int boneLines = 0;   // 画出的骨线数
};

// ---------------------------------------------------------------- 转换 / 工具
inline espm::Vec3 to_v3(const FVector& f) {
    espm::Vec3 v;
    v.X = f.X; v.Y = f.Y; v.Z = f.Z;
    return v;
}

inline FVector to_fv(const espm::Vec3& v) {
    FVector f;
    f.X = v.X; f.Y = v.Y; f.Z = v.Z;
    return f;
}

// FMinimalViewInfo -> CameraView。
// 屏幕比例优先用 pov.AspectRatio（游戏自己设的视口比例，分辨率变化/信箱模式时更准），
// 不合理就退回 drawable 的像素比例。
inline bool make_frame_camera(espm::CameraView& cam, const FMinimalViewInfo& pov,
                              const espm::ScreenMap& scr) {
    float aspect = scr.aspect();
    if (pov.AspectRatio > 0.1f && pov.AspectRatio < 20.0f) aspect = pov.AspectRatio;

    espm::Vec3 loc;
    loc.X = pov.Location.X; loc.Y = pov.Location.Y; loc.Z = pov.Location.Z;

    espm::Rot rot;
    rot.Pitch = pov.Rotation.Pitch; rot.Yaw = pov.Rotation.Yaw; rot.Roll = pov.Rotation.Roll;

    return espm::make_camera(cam, loc, rot, pov.FOV, aspect, scr.pxW, scr.pxH);
}

// 大小写不敏感的子串查找（hay / needle 都是已解码的明文）
inline bool contains_ci(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    const size_t hn = strlen(hay);
    const size_t nn = strlen(needle);
    if (nn > hn) return false;
    for (size_t i = 0; i + nn <= hn; ++i) {
        size_t j = 0;
        for (; j < nn; ++j) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == nn) return true;
    }
    return false;
}

// 类名瘦身：BP_PlayerCharacter_C -> PlayerCharacter（给屏幕文字用，别太宽）
inline void shorten_class(const char* in, char* out, size_t cap) {
    if (!out || cap < 2) return;
    out[0] = 0;
    if (!in) return;

    const char* p = in;
    while (strncmp(p, "BP_", 3) == 0) p += 3;
    if (p[0] == 'A' && p[1] >= 'A' && p[1] <= 'Z') p += 1;   // A 前缀（UCLASS 命名）

    size_t n = strlen(p);
    size_t cut = n;
    if (n > 2 && strcmp(p + n - 2, "_C") == 0) cut = n - 2;
    if (cut >= cap) cut = cap - 1;
    memcpy(out, p, cut);
    out[cut] = 0;
}

} // namespace espx
