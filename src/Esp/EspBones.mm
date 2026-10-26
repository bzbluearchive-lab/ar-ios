// EspBones.mm —— 骨骼连线 + 头/胸/腰 装备框（纯只读 + ImGui 画线）
//
// 零写入：这个文件里没有 mem::wr，也没有任何 vm_write / memcpy 到目标内存。
// 所有读取走 SafeMem（vm_read_overwrite），坏指针 = 读失败 = 本根骨骼跳过，不崩。

#include "EspBones.hpp"

#include <stdio.h>
#include <string.h>

#include "../Game/UObject.hpp"      // ue::uobj::class_of（画距离文字时取类名）
#include "imgui.h"

namespace espx {
namespace bones {

// ---------------------------------------------------------------- 布局表
// 字段顺序与 BoneLayout 声明一致：stride / rot / rotKind / trans / c2wRot / c2wTrans
const BoneLayout kLayoutCompact    = { 0x30, 0x00, 0, 0x10, 0x00, 0x10 };
const BoneLayout kLayoutVectorized = { 0x50, 0x00, 1, 0x30, 0x00, 0x30 };

const BoneLayout& layout_for(int idx) {
    return (idx == 1) ? kLayoutVectorized : kLayoutCompact;
}

// ---------------------------------------------------------------- 骨架连线（占位索引）
// 用 bones::dump() 认领后替换。现在的值只是"人形 rig 的常见连法"，不是实测结果。
const BonePair kPairs[32] = {
    {  0,  1 }, {  1,  2 }, {  2,  3 },          // 脊柱链（pelvis -> spine -> spine2）
    {  3,  4 }, {  4,  5 },                      // 颈 -> 头
    {  6,  7 }, {  7,  8 }, {  8,  9 },          // 左肩 -> 左肘 -> 左手
    { 10, 11 }, { 11, 12 }, { 12, 13 },          // 右肩 -> 右肘 -> 右手
    { 14, 15 }, { 15, 16 },                      // 左髋 -> 左膝 -> 左脚（占位，脚在 16）
    { 17, 18 }, { 18, 19 },                      // 右髋 -> 右膝 -> 右脚
};
const int kPairCount = 15;

const BodyIdx kBody = {
    5,   // head   待填
    4,   // neck   待填
    2,   // chest  待填
    0    // pelvis 待填
};

// ---------------------------------------------------------------- 小工具
// 与 ImGui 解耦不了（要 ImU32），所以放在这里而不是 EspCommon.hpp
static inline ImU32 c32(const float c[4]) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3]));
}

// FString：TCHAR* Data + int32 Num + int32 Max = 16 字节，Data 指向 UTF-16
constexpr uintptr_t kFStringSize = 0x10;

static bool read_fstring(uintptr_t addr, char* out, size_t cap) {
    if (!out || cap < 2) return false;
    out[0] = 0;
    TArrayRaw s{};
    if (!mem::rd(addr, s)) return false;
    if (!s.Data || s.Num <= 0 || s.Num > 256) return false;

    size_t n = (size_t)s.Num;
    if (n > cap - 1) n = cap - 1;
    for (size_t i = 0; i < n; ++i) {
        uint16_t ch = 0;
        if (!mem::read(s.Data + i * 2u, &ch, 2)) { out[i] = 0; return i > 0; }
        out[i] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
    }
    out[n] = 0;
    return out[0] != 0;
}

// ---------------------------------------------------------------- 读角色
bool read_mesh(uintptr_t actor, MeshRef& out) {
    out = MeshRef{};
    if (!actor) return false;

    const BoneLayout& L = layout_for(settings().boneLayout);
    out.lay = &L;

    const uintptr_t mesh = mem::rdptr(actor + off::CHAR_MESH);
    if (!mesh) return false;

    TArrayRaw arr{};
    if (!mem::rd(mesh + off::SKEL_BONE_ARRAY, arr)) return false;
    if (!arr.Data) return false;

    // 骨骼数区间就是"这是不是一个角色"的主要判据（道具/载具都不会有 40~400 根骨头）
    int32_t num = arr.Num;
    if (off::SKEL_BONE_NUM) {
        int32_t n2 = 0;
        if (mem::rd(mesh + off::SKEL_BONE_NUM, n2) && n2 >= 40 && n2 <= 400) num = n2;
    }
    if (num < 40 || num > 400) return false;

    out.mesh     = mesh;
    out.boneData = arr.Data;
    out.boneNum  = num;
    out.lay      = &L;

    // 可选路径：有的构建直接暴露"世界空间骨骼数组"，那就省掉 C2W 变换（也省一次乘法）
    if (off::SKEL_BONE_WORLD_ARRAY) {
        TArrayRaw w{};
        if (mem::rd(mesh + off::SKEL_BONE_WORLD_ARRAY, w) &&
            w.Data && w.Num >= 40 && w.Num <= 400) {
            out.boneData   = w.Data;
            out.boneNum    = w.Num;
            out.worldSpace = true;
        }
    }

    if (!out.worldSpace) {
        out.c2wAddr = mesh + off::SKEL_COMPONENT_TO_WORLD;

        // ComponentToWorld：旋转块 + 平移。旋转块按布局取 4 float(quat) 或 12 float(3x4)
        const size_t rotBytes = (L.rotKind == 0) ? (sizeof(float) * 4u) : (sizeof(float) * 12u);
        float rb[12] = { 0 };
        if (!mem::read(out.c2wAddr + L.c2wRot, rb, rotBytes)) return false;
        for (int i = 0; i < 12; ++i) out.c2wRot[i] = rb[i];

        if (!mem::rd(out.c2wAddr + L.c2wTrans, out.c2wPos)) return false;
        if (!espm::fin3(out.c2wPos)) return false;
    }

    out.ok = true;
    return true;
}

bool bone_world(const MeshRef& m, int idx, espm::Vec3& out) {
    if (!m.ok || !m.lay || !m.boneData) return false;
    if (idx < 0 || idx >= m.boneNum) return false;

    const BoneLayout& L = *m.lay;
    const uintptr_t base = m.boneData + (uintptr_t)idx * (uintptr_t)L.stride;

    // 一次读覆盖"旋转块 + 平移"：紧凑 0x1C 字节、vectorized 0x3C 字节，
    // 两者都小于各自步长，所以最后一根骨骼也不会读出数组末尾。
    float raw[16] = { 0 };
    const size_t need = (size_t)L.trans + 12u;
    if (need > sizeof(raw)) return false;
    if (!mem::read(base, raw, need)) return false;

    const size_t off4 = (size_t)(L.trans / 4u);
    const espm::Vec3 local = espm::V3(raw[off4 + 0], raw[off4 + 1], raw[off4 + 2]);
    if (!espm::fin3(local)) return false;

    // 世界空间数组：平移就是世界坐标，不用再过组件变换
    if (m.worldSpace) {
        out = local;
        return true;
    }

    espm::Vec3 rot;
    if (L.rotKind == 0) rot = espm::quat_rotate(m.c2wRot, local);
    else                rot = espm::mat3x4_rotate(m.c2wRot, local);

    out = espm::V3(rot.X + m.c2wPos.X, rot.Y + m.c2wPos.Y, rot.Z + m.c2wPos.Z);
    return espm::fin3(out);
}

bool head_world(const MeshRef& m, espm::Vec3& out) {
    return bone_world(m, kBody.head, out);
}

// ---------------------------------------------------------------- 装备名（可选路径）
// off::ACTOR_EQUIPPED_ITEMS 没填（0x000）就直接返回，退化成上面的骨骼框。
static void draw_equipped(uintptr_t actor, Frame& f, float x, float y) {
    if (!off::ACTOR_EQUIPPED_ITEMS) return;

    TArrayRaw arr{};
    if (!mem::rd(actor + off::ACTOR_EQUIPPED_ITEMS, arr)) return;
    if (!arr.Data || arr.Num <= 0 || arr.Num > 16) return;

    const Settings& S = settings();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImU32 col = c32(S.colEquip);
    const float lh = 14.0f * f.scr.uiScale;

    int shown = 0;
    for (int32_t i = 0; i < arr.Num && shown < 4; ++i) {
        char raw[72];
        if (!read_fstring(arr.Data + (uintptr_t)i * kFStringSize, raw, sizeof(raw))) continue;

        char name[72];
        shorten_class(raw, name, sizeof(name));     // 有的游戏在这里存的是类路径
        if (!name[0]) continue;

        dl->AddText(ImVec2(x + 6.0f, y - 16.0f + (float)shown * lh), col, name);
        shown++;
    }
}

// ---------------------------------------------------------------- 画一个角色
bool try_actor(uintptr_t actor, Frame& f) {
    Settings& S = settings();
    if (!S.skeleton && !S.equipment && !S.box) return false;

    MeshRef m;
    if (!read_mesh(actor, m)) return false;         // 不是骨骼角色（或偏移没填）

    // 根位置：兜底框、距离、文字基准都用它。
    // 三级回退：RootComponent 的 ComponentToWorld -> 骨盆骨骼 -> 组件自身平移。
    // （走 worldSpace 骨骼数组时 c2wPos 是 0，所以必须先试骨盆，否则角色会被判到原点）
    espm::Vec3 root{};
    FVector rootF{};
    if (world::actor_location(actor, rootF)) {
        root = to_v3(rootF);
    } else if (!bone_world(m, kBody.pelvis, root)) {
        root = m.c2wPos;
    }
    if (!espm::fin3(root)) return false;

    // 深度只用来判"在不在相机后面"；显示与半径用真实距离（侧前方的目标深度会偏小）
    if (!(espm::depth_of(f.cam, root) > espm::kMinDepth)) return false;   // 背后 / 贴脸
    const float distCm = espm::dist(root, f.cam.loc);
    if (f.maxDistCm > 0.0f && distCm > f.maxDistCm) return false;        // 超出显示半径

    espm::Vec2 rootS{};
    const bool rootOn = espm::world_to_screen(f.cam, root, rootS);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImU32 colSkel  = c32(S.colSkel);
    const ImU32 colEquip = c32(S.colEquip);

    bool drew = false;
    float headY = rootOn ? rootS.Y : 0.0f;

    // 头骨骼：装备框和文字高度都要它
    espm::Vec3 headW{};
    espm::Vec2 headS{};
    const bool headOk = head_world(m, headW) && espm::world_to_screen(f.cam, headW, headS);
    if (headOk) headY = headS.Y;

    // ---- 骨骼连线（每帧总预算 S.maxBones，不是每个角色一份）----
    if (S.skeleton) {
        int budget = S.maxBones - f.boneLines;
        const int pairs = (kPairCount < 32) ? kPairCount : 32;
        for (int i = 0; i < pairs && budget > 0; ++i) {
            espm::Vec3 wa{}, wb{};
            if (!bone_world(m, kPairs[i].a, wa)) continue;
            if (!bone_world(m, kPairs[i].b, wb)) continue;

            espm::Vec2 sa{}, sb{};
            if (!espm::world_to_screen(f.cam, wa, sa)) continue;
            if (!espm::world_to_screen(f.cam, wb, sb)) continue;

            dl->AddLine(ImVec2(sa.X, sa.Y), ImVec2(sb.X, sb.Y), colSkel, S.lineThick);
            --budget;
            ++f.boneLines;
        }
        if (f.boneLines > 0) drew = true;
    }

    // ---- 装备框：头 / 胸 / 腰（按真实尺寸 + 各自深度换算像素）----
    if (S.equipment) {
        struct BoxSpec { int bone; float wCm; float hCm; };
        const BoxSpec specs[3] = {
            { kBody.head,   22.0f, 26.0f },   // 头
            { kBody.chest,  46.0f, 34.0f },   // 胸（含护甲位）
            { kBody.pelvis, 36.0f, 26.0f },   // 腰
        };
        for (int i = 0; i < 3; ++i) {
            espm::Vec3 bw{};
            espm::Vec2 bs{};
            if (!bone_world(m, specs[i].bone, bw)) continue;
            if (!espm::world_to_screen(f.cam, bw, bs)) continue;

            const float k = espm::pixels_per_cm(f.cam, espm::depth_of(f.cam, bw));
            if (!(k > 0.0f)) continue;

            const float hw = specs[i].wCm * 0.5f * k;
            const float hh = specs[i].hCm * 0.5f * k;
            if (hw < 1.5f || hw > 600.0f) continue;          // 太远/太近都不画

            dl->AddRect(ImVec2(bs.X - hw, bs.Y - hh), ImVec2(bs.X + hw, bs.Y + hh),
                        colEquip, 1.5f, 0, S.boxThick);
            drew = true;
        }
    }

    // ---- 兜底方框：骨骼一条都没画出来时才用 ----
    if (!drew && S.box && rootOn) {
        const float k = espm::pixels_per_cm(f.cam, distCm);
        if (k > 0.0f) {
            const float h = 180.0f * k;                      // 1.8m
            const float w = h * 0.42f;
            if (h > 3.0f && h < f.scr.pxH * 2.0f) {
                dl->AddRect(ImVec2(rootS.X - w * 0.5f, rootS.Y - h),
                            ImVec2(rootS.X + w * 0.5f, rootS.Y),
                            colSkel, 1.5f, 0, S.boxThick);
                drew = true;
            }
        }
    }

    // ---- 身上的装备名（偏移没填时 draw_equipped 自己跳过）----
    if (S.equipment && (rootOn || headOk)) {
        draw_equipped(actor, f, rootOn ? rootS.X : headS.X, headY);
    }

    // ---- 距离（+ 类名，类名取不到就只显示距离）----
    if (S.distance && rootOn) {
        char cls[96];
        char name[96];
        cls[0] = 0;
        ue::uobj::class_of(actor, cls, sizeof(cls));
        shorten_class(cls, name, sizeof(name));

        XDECL(lbl_unknown, "char");
        XDECL(fmt, "%s %.0fm");

        char line[192];
        snprintf(line, sizeof(line), fmt, name[0] ? name : lbl_unknown, distCm / 100.0f);
        dl->AddText(ImVec2(rootS.X - 24.0f, headY - 16.0f), colSkel, line);
    }

    return drew;
}

// ---------------------------------------------------------------- 诊断
void dump(uintptr_t actor, void (*log)(const char*), int count) {
    if (!log) return;

    MeshRef m;
    if (!read_mesh(actor, m)) {
        log("  bones: read_mesh 失败（没有骨骼网格 / CHAR_MESH 或 SKEL_BONE_ARRAY 偏移没填）");
        return;
    }

    char line[160];
    snprintf(line, sizeof(line), "  mesh=0x%lx bones=%d stride=0x%x c2w=(%.1f %.1f %.1f)",
             (unsigned long)m.mesh, (int)m.boneNum, (unsigned)(m.lay ? m.lay->stride : 0u),
             m.c2wPos.X, m.c2wPos.Y, m.c2wPos.Z);
    log(line);

    if (count > m.boneNum) count = m.boneNum;
    for (int i = 0; i < count; ++i) {
        espm::Vec3 w{};
        if (!bone_world(m, i, w)) continue;
        snprintf(line, sizeof(line), "  bone[%02d] %.1f %.1f %.1f", i, w.X, w.Y, w.Z);
        log(line);
    }
}

} // namespace bones
} // namespace espx
