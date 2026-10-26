// EspBones.hpp —— 角色骨骼：读骨骼数组、组件空间 -> 世界、连线、头/胸/腰装备框
//
// 数据来源（全部只读，走 mem::read）：
//   actor + off::CHAR_MESH                  -> USkeletalMeshComponent*
//   mesh  + off::SKEL_BONE_ARRAY            -> TArray<FTransform>（组件空间，每元素 0x30 或 0x50）
//   mesh  + off::SKEL_COMPONENT_TO_WORLD    -> ComponentToWorld（取旋转 + 平移）
//   （可选）mesh + off::SKEL_BONE_WORLD_ARRAY -> 已经是世界空间的骨骼（有就直接用）
//
// 世界坐标两步走：
//   local  = 读 bone[i].Translation
//   world  = rotate(c2wRotation, local) + c2wTranslation
// 注意【不要】用 RootComponent 的位置代替：角色的 Mesh 常挂在胶囊体下面，两者的
// ComponentToWorld 不是一回事，用错了骨骼整体会偏一个身位。

#pragma once
#include <stdint.h>
#include <stddef.h>

#include "EspCommon.hpp"
#include "../Game/World.hpp"

namespace espx {
namespace bones {

// ---------------------------------------------------------------- 骨骼数组布局
// UE4 有两种内存布局，猜错不会崩，但骨头会挤成一团：
//
//   紧凑 FTransform（非 vectorized 构建 / 部分手游包）
//     +0x00 FQuat   Rotation   (16B: x, y, z, w)
//     +0x10 FVector Translation(12B)
//     +0x1C FVector Scale3D    (12B)   -> 整体补齐到 0x30
//
//   FTransformVectorized（x64/arm64 上 USceneComponent::ComponentToWorld 常见）
//     +0x00..0x2F 旋转按 3x4 矩阵存（12 float）
//     +0x30 FVector Translation
//     +0x40 FVector Scale3D        -> 整体 0x50
//
// 默认取紧凑（与你给的规格一致：0x30 步长 / 平移在 0x10）。真机上用
// bones::dump() 打几条出来看数值量级就能判定该选哪个。
struct BoneLayout {
    uint32_t stride   = 0x30;   // 单个骨骼元素的大小
    uint32_t rot      = 0x00;   // 旋转块起点
    uint32_t rotKind  = 0;      // 0 = FQuat（4 float）/ 1 = 3x4 行主序矩阵（12 float）
    uint32_t trans    = 0x10;   // 本元素平移
    uint32_t c2wRot   = 0x00;   // ComponentToWorld 的旋转起点（相对组件）
    uint32_t c2wTrans = 0x10;   // ComponentToWorld 的平移（相对组件）
};

extern const BoneLayout kLayoutCompact;      // 0x30 步长 / quat / 平移 0x10
extern const BoneLayout kLayoutVectorized;   // 0x50 步长 / 3x4 / 平移 0x30

// idx: 0 = 紧凑，1 = vectorized，其它值按 0 处理
const BoneLayout& layout_for(int idx);

// ---------------------------------------------------------------- 骨架连线
// 索引是【占位】。用 bones::dump(actor, log) 把 0..30 号骨骼的世界坐标打出来，
// 对照模型认领 head / neck / spine / hand / knee / foot 到底是几号，再改这张表。
// 填错的后果：画出奇怪的连法（不会崩、不会误判）。
struct BonePair { int a, b; };

extern const BonePair kPairs[32];   // 前 kPairCount 项有效
extern const int      kPairCount;

// 头 / 颈 / 胸 / 腰：装备框与"头顶文字高度"都用这四个（同样占位，认领后改）
struct BodyIdx { int head, neck, chest, pelvis; };
extern const BodyIdx kBody;

// ---------------------------------------------------------------- 一个角色的骨骼句柄
// 每帧每个角色只解一次数组头，之后逐根骨读 40 字节。
struct MeshRef {
    uintptr_t mesh      = 0;      // USkeletalMeshComponent*
    uintptr_t boneData  = 0;      // TArray<FTransform>.Data
    int32_t   boneNum   = 0;      // 有效骨骼数
    const BoneLayout* lay = nullptr;

    bool      worldSpace = false; // true = boneData 已经是世界空间（走 SKEL_BONE_WORLD_ARRAY）

    uintptr_t c2wAddr   = 0;      // ComponentToWorld 地址
    float     c2wRot[12] = { 0 }; // 旋转块（rotKind==0 时前 4 项是 quat）
    espm::Vec3 c2wPos{};
    bool      ok        = false;
};

// 读组件 + 骨骼数组头 + ComponentToWorld。失败返回 false（actor 没骨骼网格 / 偏移没填）
bool read_mesh(uintptr_t actor, MeshRef& out);

// 第 idx 根骨骼的世界坐标
bool bone_world(const MeshRef& m, int idx, espm::Vec3& out);

// 便捷：kBody.head 的世界坐标（拿不到返回 false）
bool head_world(const MeshRef& m, espm::Vec3& out);

// 画一个角色（骨骼连线 + 装备框 + 兜底方框）。返回 true = 这个 actor 被当成角色画了。
// 内部自己判断"是不是角色"：有 SkeletalMeshComponent 且骨骼数在 [40, 400]。
bool try_actor(uintptr_t actor, Frame& f);

// 诊断：把 0..count-1 号骨骼的世界坐标丢给 log（认领索引时用；真机上从 Tweak 里调）
void dump(uintptr_t actor, void (*log)(const char*), int count = 32);

} // namespace bones
} // namespace espx
