// Offsets.hpp —— 版本相关的全部数字都在这一个文件里。
//
// 目标：Arena Breakout iOS 1.0.249（com.proximabeta.mf.uamo）
// 样本：UAGame_1.0.249_decrypted（arm64, cryptid=0, sha256 9D0295FE…7ABCAC7）
// 引擎：UE 4.25–4.27（FField x42 / PhysX_3.4 / UnLua x43）
//
// 状态标记：
//   VERIFIED-STATIC   —— 静态反编译 + 反射表证据（函数地址/指令片段可复现）
//   VERIFIED-RUNTIME  —— 设备上实跑日志确认
//   UNVERIFIED        —— 占位/推断，禁止直接上真机
//
// 证据详见工作区 offsets-report.md 与 gnames-report.md。

#pragma once
#include <stdint.h>

namespace off {

// ==================== 全局（链接期地址，相对 0x100000000） ====================
// 运行时地址 = 镜像基址 + (值 - 0x100000000)
constexpr uintptr_t GEngine_VA = 0x10DBF9A58;  // VERIFIED-RUNTIME  1656 处引用；运行时读到真实 UEngine*
constexpr uintptr_t GNames_VA  = 0x10D90D840;  // VERIFIED-STATIC   FNamePool **全局结构体本体**，不是指针
constexpr uintptr_t GNames_BLOCKS_OFF = 0xD8;  // VERIFIED-STATIC   FNameEntryAllocator::Blocks[]

// ==================== FNameEntry 布局（VERIFIED-STATIC） ====================
//   header = uint16 @ entry+0 ; bit0 = wide ; len = header >> 6 (10 bit)
//   text   = entry+2
//   id     = (block << 16) | (byteOffsetInBlock >> 1)
constexpr uint32_t FNAME_LEN_SHIFT = 6;
constexpr uint32_t FNAME_LEN_MASK  = 0x3FF;

// ==================== 世界入口（两条路，优先第一条） ====================
// ① UEngine::GetWorldFromContextObject（VERIFIED-STATIC）
//    证据：UGameplayStatics::GetAllActorsOfClass 实现 0x104976644 里就是 adrp GEngine 后调用它
//    用法：fn(engine, engine, 0) -> UWorld*
constexpr uintptr_t UENGINE_GET_WORLD_FN = 0x104CB57B8;
// ② 兜底：UEngine::GameViewport（VERIFIED-STATIC，运行时候选字段里存在 +0x788）
constexpr uintptr_t UENGINE_GAMEVIEWPORT = 0x788;
// 注意：FWorldContext 里也有 GameViewport 在 +0x210，别混用

// ==================== UWorld（VERIFIED-STATIC） ====================
constexpr uintptr_t UWORLD_PERSISTENT_LEVEL   = 0x30;   // ULevel*
constexpr uintptr_t UWORLD_GAME_STATE         = 0x120;  // AGameStateBase*
constexpr uintptr_t UWORLD_LEVELS             = 0x138;  // TArray<ULevel*>
constexpr uintptr_t UWORLD_LEVEL_COLLECTIONS  = 0x148;
constexpr uintptr_t UWORLD_OWNING_GAME_INSTANCE = 0x180;

// ==================== ULevel（VERIFIED-STATIC，medium） ====================
// Actors 未进反射表，靠"ULevel 属性数组 [0] 是 OwningWorld@0xb8 ⇒ 0x28..0xb8 为未反射区"推断
constexpr uintptr_t ULEVEL_ACTORS     = 0x28;   // TArray<AActor*>
constexpr uintptr_t ULEVEL_OWNING_WORLD = 0xB8;
constexpr uintptr_t ULEVEL_MODEL        = 0xC0;
constexpr uintptr_t ULEVEL_LEVEL_SCRIPT_ACTOR = 0xE8;

// ==================== UObject 公共头（VERIFIED-RUNTIME） ====================
constexpr uintptr_t UOBJECT_CLASS_PRIVATE = 0x10;  // UClass*
constexpr uintptr_t UOBJECT_NAME_PRIVATE  = 0x18;  // FName（uint32 索引）
constexpr uintptr_t UOBJECT_OUTER_PRIVATE = 0x20;

// ==================== ACharacter（VERIFIED-STATIC） ====================
constexpr uintptr_t CHAR_MESH               = 0x418;  // USkeletalMeshComponent*
constexpr uintptr_t CHAR_CHARACTER_MOVEMENT = 0x420;
constexpr uintptr_t CHAR_CAPSULE_COMPONENT  = 0x428;

// ==================== USkeletalMeshComponent / USkinnedMeshComponent（VERIFIED-STATIC） ====================
// FTransform = 0x30 字节：FQuat@0x00(16) + Translation@0x10(12) + Scale3D@0x1C(12)
constexpr uintptr_t SKEL_XFORM_SIZE      = 0x30;
constexpr uintptr_t SKEL_BONE_ARRAY      = 0x9B8;  // CachedComponentSpaceTransforms (TArray<FTransform>)
constexpr uintptr_t SKEL_BONE_NUM        = 0x9C0;
constexpr uintptr_t SKEL_BONE_MAX        = 0x9C4;
constexpr uintptr_t BONE_LOCAL_ARRAY     = 0x940;  // BoneSpaceTransforms（活数组）
constexpr uintptr_t BONE_LOCAL_NUM       = 0x948;
constexpr uintptr_t SKEL_CACHED_BONE_SPACE = 0x9A8;
constexpr uintptr_t SKEL_SPACEBASES_BASE = 0x700;  // TArray<TArray<FTransform>> 多缓冲
constexpr uintptr_t SKEL_SPACEBASES_IDX  = 0x740;
constexpr uintptr_t SKEL_SKELETALMESH    = 0x6D0;  // USkeletalMesh*
constexpr uintptr_t SKEL_MASTER_POSE     = 0x6D8;
constexpr uintptr_t SKEL_MESH_OBJECT     = 0x860;
constexpr uintptr_t REFSKEL_BONEINFO_NUM = 0x1E8;  // 资产骨数（GetNumBones 读这里）
constexpr uintptr_t REFSKEL_REFBONEPOSE  = 0x1F0;
constexpr uintptr_t REFSKEL_REFBONEPOSE_NUM = 0x1F8;

// 组件→世界变换：候选 0x270（布局推断：AttachParent@0x240 / AttachSocketName@0x248 /
// AttachChildren@0x250 / ClientAttachedChildren@0x260 之后第一个 16 字节对齐位）
// 运行时用"四元数模长≈1 + Scale∈(0.01,100) + 平移在合理范围"扫描确认，未确认前不改这里
constexpr uintptr_t SKEL_COMPONENT_TO_WORLD = 0x270;  // UNVERIFIED

// 世界坐标公式（UE4 行向量约定，先骨后组件）：
//   world = BoneCS.ToMatrixWithScale() * ComponentToWorld.ToMatrixWithScale()
//   只要位置： CW.TransformPosition(BoneCS.Translation)，Translation 在元素 +0x10

// ==================== AActor / 组件 ====================
constexpr uintptr_t ACTOR_ROOT_COMPONENT = 0x000;  // UNVERIFIED USceneComponent*
constexpr uintptr_t ACTOR_OWNER          = 0x000;  // UNVERIFIED

// ==================== 相机（未定位） ====================
constexpr uintptr_t PC_PLAYER_CAMERA_MANAGER = 0x000;  // UNVERIFIED
constexpr uintptr_t PCM_CAMERA_CACHE         = 0x000;  // UNVERIFIED
constexpr uintptr_t PCAM_CONTROL_ROTATION    = 0x000;  // UNVERIFIED

// ==================== FMinimalViewInfo（UE4 公开字段顺序，仍需核对） ====================
constexpr uintptr_t POV_LOCATION   = 0x00;   // FVector
constexpr uintptr_t POV_ROTATION   = 0x0C;   // FRotator (Pitch, Yaw, Roll)
constexpr uintptr_t POV_FOV        = 0x18;   // float
constexpr uintptr_t POV_ASPECT     = 0x1C;   // float
constexpr uintptr_t POV_PROJ_MODE  = 0x20;   // uint8

} // namespace off
