// EspOffsets.hpp —— ESP 层要用、但 Game/Offsets.hpp 里还没有的偏移
//
// 为什么不直接往 src/Game/Offsets.hpp 里加：
//   那个文件是全工程共用的（World.mm / Scene.mm / UObject.mm 都在读），多人同时改
//   容易冲突。这里用 namespace off 重新打开，名字与 Offsets.hpp 里同风格，访问路径
//   仍然是 off::XXX —— 合并时把这一节整体搬进 Offsets.hpp，代码一行都不用改。
//
// 规则同 Offsets.hpp：
//   - 0x000 = 待填。填之前走代码里的"降级路径"（骨骼框代替装备名、TArray.Num 代替
//     骨骼数字段），不会崩、只是少一块信息。
//   - 只读。这里没有一个偏移是给 mem::wr 用的（本层零写入，见 README「铁律」）。

#pragma once
#include <stdint.h>
#include "../Game/Offsets.hpp"      // off::CHAR_MESH / SKEL_BONE_ARRAY / ... 都在这里

namespace off {

// ==================== 角色身上的装备（可选路径） ====================
// 待填：APawn/AActor 上那个"已装备物品"的 TArray<FString>（不同游戏字段名不同，
//       常见叫法 EquippedItems / Inventory / Slots）。
// 读法：TArray<FString>，元素 FString 16 字节（TCHAR* Data + int32 Num + int32 Max），
//       Data 是 UTF-16。EspBones.mm 里 read_fstring() 处理它。
// 没填（0x000）时：EspBones 自动退化成"头/胸/腰骨骼框"，不报错、不影响其余绘制。
constexpr uintptr_t ACTOR_EQUIPPED_ITEMS = 0x000;    // 待填 TArray<FString>

// ==================== 骨骼（三条可选捷径） ====================
// 待填：世界空间骨骼变换数组（TArray<FTransform>，已经是世界坐标，不用再过 C2W）。
//       有的构建把它挂在 USkeletalMeshComponent 上（GetBoneMatrix / SpaceBases 之后
//       的那份），拿不到就留 0：EspBones 走"组件空间 + ComponentToWorld"主路径。
constexpr uintptr_t SKEL_BONE_WORLD_ARRAY = 0x000;   // 待填 TArray<FTransform>（世界空间）

// 待填：骨骼数量字段（int32）。拿不到就用 TArray.Num —— 绝大多数构建两者相同。
//       用途：某些版本 TArray.Num 是"容量"而不是"已用数量"时用它纠偏。
constexpr uintptr_t SKEL_BONE_NUM = 0x000;           // 待填 int32

// ====== 依赖 Offsets.hpp 已有的项（列出来只为对照，这里不重复定义） ======
// off::CHAR_MESH                 AActor -> USkeletalMeshComponent*
// off::SKEL_BONE_ARRAY           USkeletalMeshComponent -> TArray<FTransform>（组件空间）
// off::SKEL_COMPONENT_TO_WORLD   USkeletalMeshComponent -> FTransform / FMatrix(3x4)
// off::ACTOR_ROOT_COMPONENT      AActor -> USceneComponent*
// off::SCENE_COMPONENT_TO_WORLD  USceneComponent -> ComponentToWorld
// off::ULEVEL_ACTORS             ULevel -> TArray<AActor*>
// off::PCM_CAMERA_CACHE          APlayerCameraManager -> FCameraCacheEntry（POV 在起始）
// off::POV_LOCATION / _ROTATION / _FOV / _ASPECT

} // namespace off
