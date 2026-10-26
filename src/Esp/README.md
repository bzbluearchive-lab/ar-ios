# src/Esp —— 只显示渲染层（Metal hook + ImGui 叠加 + UE4 W2S）

一套**独立可替换**的显示层：挂 `CAMetalLayer` 的 drawable，用 ImGui 的 Metal 后端在游戏
自己的 render pass 之后叠一次 `LoadAction = Load` 的 pass，画骨骼 / 头胸腰装备框 / 附近物资 / 菜单。

**零写入**：全层只有 `mem::read` / `mem::rd` / `mem::rdptr`（`vm_read_overwrite`），
没有 `mem::wr`、没有 `vm_write`、没有改页保护。跑一次 `tools/preflight_esp.py` +
本目录 README 第 8 节的检查即可复核。

> **编译验证范围（如实说明）**：本目录是在没有 macOS / Theos / iOS SDK 的机器上写的。
> `EspMath.hpp` / `EspCommon.hpp` / `EspBones.mm` / `EspLoot.mm` / `EspMenu.mm` 都是
> **纯 C++（没有一行 ObjC 语法）**，已经用 `g++ -std=c++17 -Wall -Wextra` 真正编译过，
> 其中 `EspBones` + `EspLoot` 还用"假内存 + 假偏移"把整条数据通路跑了一遍（结果见第 8 节）。
> **只有 `EspRender.mm` 没经过 clang** —— 那段是 Metal / ObjC runtime，本机编不了。
> 第一次真机构建要修的大概率只有 ImGui 版本差异与偏移未填两类，见第 6 节。

---

## 1. 文件清单与职责

| 文件 | 职责 | 依赖 |
| --- | --- | --- |
| `EspMath.hpp` | 纯数学：`Vec3/Vec2/Rot`、4x4 行主序矩阵、`CameraView`、UE4 `FSceneView::WorldToScreen`（角度式 + 矩阵式两条等价路径）、`pixels_per_cm`、`Rect`、`ScreenMap`（像素/点/安全区） | 只用 `<math.h>` `<string.h>`。**不 include ObjC / ImGui / game 头**，可在桌面上用 g++ 单独编 |
| `EspOffsets.hpp` | ESP 专用但 `Game/Offsets.hpp` 里没有的偏移（重新打开 `namespace off`，访问路径仍是 `off::XXX`）。全部 `0x000` = 待填 | `../Game/Offsets.hpp` |
| `EspCommon.hpp` | `espx::Settings`（开关/上限/颜色）、`espx::Frame`（每帧上下文）、编译期 XOR 字符串 `XDECL`、`FMinimalViewInfo -> CameraView`、`contains_ci` / `shorten_class` | `EspMath.hpp`、`EspOffsets.hpp`、`../Core/Obfs.hpp`、`../Core/SafeMem.h`、`../Game/World.hpp` |
| `EspBones.hpp/.mm` | 骨骼：`TArray<FTransform>` 读取（紧凑 0x30 / vectorized 0x50 两套布局）、组件空间 -> 世界、骨架连线、头/胸/腰装备框、兜底方框、可选读 `EquippedItems` 装备名、`dump()` 认领骨骼索引 | 上面三个 + `../Game/UObject.hpp`（取类名画距离）+ `imgui.h` |
| `EspLoot.hpp/.mm` | 物资：类名关键词（XOR）匹配 -> 根位置 -> W2S -> 圆点 + 名称 + 距离 | 同上 |
| `EspMenu.hpp/.mm` | 菜单：三项显示开关、颜色（`ColorEdit4`）、半径/线宽/上限调节、骨骼布局选择、帧率统计、44×44 常驻按钮、命中判定 | `EspCommon.hpp` + `imgui.h` |
| `EspRender.hpp/.mm` | `espx::settings()` 的唯一定义、ImGui 初始化、Metal 挂点（`nextDrawable` / `present` / `presentAtTime:`）、`UIApplication sendEvent:` 触摸注入、每帧驱动、安全区读取 | 全部上面 + `../Core/Hook.hpp` + `imgui_impl_metal.h` |

`espx::settings()` 定义在 `EspRender.mm`（全工程唯一一处定义），别的 `.mm` 只用声明。

**符号命名空间**：`espm::` = 数学，`espx::` = 本层。刻意避开 `esp::`（旧 `src/Render/ESP.mm`
用的），所以即便两个目录同时编也不会符号冲突 —— 但 **ImGui context 不能共存**，见第 6 节。

---

## 2. 依赖

* **ImGui 1.90.9**（CI 里 `tools/fetch-imgui.sh` / workflow 固定 tag 拉 `vendor/imgui`）：
  需要编译 `imgui.cpp` / `imgui_draw.cpp` / `imgui_tables.cpp` / `imgui_widgets.cpp` +
  `backends/imgui_impl_metal.mm`（`Makefile.legacy-tweak` 里的 `wildcard` 已经包含了）。
* **框架**：`Metal` `QuartzCore` `UIKit` `Foundation`（`ARCheat_FRAMEWORKS` 已含）。
* **用到的 ImGui API**（全部在 1.90.9 长稳范围，唯一需要留意的是 `SeparatorText`，
  见第 6 节版本差异）：`CreateContext` / `GetIO` / `NewFrame` / `Render` / `GetBackgroundDrawList` /
  `Begin`(带 `p_open`) / `End` / `Checkbox` / `SliderFloat` / `SliderInt` / `Combo` /
  `ColorEdit4` / `SeparatorText` / `Text` / `TextDisabled` / `SetNextWindowPos` /
  `SetNextWindowBgAlpha` / `GetWindowPos` / `GetWindowSize` / `ColorConvertFloat4ToU32` /
  `GetStyle().ScaleAllSizes` / `io.AddMousePosEvent` / `io.AddMouseButtonEvent` / `io.DeltaTime`
* **后端签名**（1.90.9）：`ImGui_ImplMetal_Init(id<MTLDevice>) -> bool`、
  `ImGui_ImplMetal_CreateDeviceObjects(id<MTLDevice>) -> bool`（**带设备参数**，不是无参版）、
  `ImGui_ImplMetal_NewFrame(MTLRenderPassDescriptor*)`、
  `ImGui_ImplMetal_RenderDrawData(ImDrawData*, id<MTLCommandBuffer>, id<MTLRenderCommandEncoder>)`。

---

## 3. 接入（两份 patch，**没有直接改工程文件**）

### 3.1 `Makefile.legacy-tweak`

```diff
--- a/Makefile.legacy-tweak
+++ b/Makefile.legacy-tweak
@@
 ARCheat_FILES = \
 	src/Core/Stealth.mm \
 	src/Game/UObject.mm \
 	src/Game/World.mm \
 	src/Game/Scene.mm \
-	src/Render/ESP.mm \
-	src/Render/Overlay.mm \
+	src/Esp/EspBones.mm \
+	src/Esp/EspLoot.mm \
+	src/Esp/EspMenu.mm \
+	src/Esp/EspRender.mm \
 	Tweak.xm
```

说明：

* **必须同时去掉 `src/Render/ESP.mm` 与 `src/Render/Overlay.mm`**。原因不是符号冲突，是
  ImGui：`Overlay.mm` 会 `ImGui::CreateContext()` 并自己驱动 `NewFrame/Render`。一个进程
  只能有一个 context，两套交替跑第一帧就崩。
* 不需要加 `-I`：`ARCheat_CFLAGS` 已有 `-Isrc`，且本层全部用相对 include
  （`"../Core/SafeMem.h"` 这类），与 `src/Render/*.mm` 的写法一致。
* 不需要加框架：Metal / QuartzCore / UIKit / Foundation 已在 `ARCheat_FRAMEWORKS` 里；
  `os/lock.h` 属 libSystem，不用额外链接。
* 想保留旧 Overlay 做对照实验，就**别把两份同时编进同一个 dylib**（见第 6 节"互斥"）。

### 3.2 `Tweak.xm`

```diff
--- a/Tweak.xm
+++ b/Tweak.xm
@@
-#include "src/Render/Overlay.hpp"
+#include "src/Esp/EspRender.hpp"
@@
 static void install_overlay_once(void) {
     if (g_overlayInstalled) return;
     g_overlayInstalled = true;
-    overlay_install();
-    ARLog(@"overlay installed (world-independent)");
+    espx::install();
+    ARLog(@"espx installed (world-independent)");
 }
```

其余部分（目标进程守卫、kill switch、0.8s 后无条件挂渲染点、世界解析独立重试）一行都不用动
—— `espx::install()` 与 `overlay_install()` 的语义完全一致：**只挂渲染点，不依赖世界就绪**。

> 上面两块 diff 的 `@@` 故意没写行号（`@@ -20,7 +20,8 @@` 这种一改上下文就失效）。
> 应用方式：`git apply -C1 <patch>`，或按上下文手工合并 —— 上下文行已核对过与当前
> `Makefile.legacy-tweak` / `Tweak.xm` 逐行一致（含 Makefile 的 tab）。

### 3.3 可选：真机认领骨骼索引（不改 patch，临时贴进去用完就删）

`try_bringup()` 里"世界就绪"那一支加上：

```objc
        ARLog(@"世界就绪 world=0x%lx actors=%zu", (unsigned long)c.world, list.size());
        g_worldReady = true;

        // ---- 认领骨骼索引用（临时）：把前 24 根骨骼的世界坐标打出来 ----
        if (const char* dump = getenv("AR_ESP_DUMP")) {
            (void)dump;
            for (size_t i = 0; i < list.size() && i < 64; ++i) {
                espx::bones::MeshRef mr;
                if (!espx::bones::read_mesh(list[i], mr)) continue;
                ARLog(@"actor[%zu] bones=%d", i, (int)mr.boneNum);
                espx::bones::dump(list[i],
                                  [](const char* s) { ARLog(@"%s", [NSString stringWithUTF8String:s]); },
                                  24);
                break;      // 只打第一个有骨骼的
            }
        }
```

看数值认领：站立的角色，头部骨骼 Z 应该比骨盆高 ~150cm、双手 X 分居两侧、脚 Z 接近地面。
认出来之后把 `EspBones.mm` 顶部的 `kPairs` / `kBody` 换成实测索引。

---

## 4. 待填偏移清单（**跑起来之前必须全部填掉**）

现状：`src/Game/Offsets.hpp` + 本目录 `EspOffsets.hpp` 里**全部是 `0x000`**。
填 0 的运行表现是"不崩，但一个像素都不画"——因为 `frame_draw` 在 `world::camera()` 就
返回了。**菜单和常亮小按钮会照常出现**，这是判断"渲染链路通没通"的唯一信号。

### 4.1 引擎链（决定"能不能拿到世界"）

| 符号 | 位置 | 作用 | 定位/验证 |
| --- | --- | --- | --- |
| `off::GEngine` | `Offsets.hpp` | `UEngine*` 全局 | 在 `__DATA` 扫到候选后，用 `GEngine->GameViewport->World` 链回读，`Actors.Num` 落在几十~几百才算对 |
| `off::GNames` | `Offsets.hpp` | `FNamePool*` | 可不填：`ue::fname::locate()` 会扫 `__DATA` 并用 `entry[0] == "None"` 自检（`fname::self_check()` 打日志） |
| `off::GWorld` | `Offsets.hpp` | 裸 `UWorld*` 兜底 | 只在链式解析失败时用，且要与链式结果交叉校验 |
| `off::UENGINE_GAMEVIEWPORT` | `Offsets.hpp` | `UEngine -> UGameViewportClient*` | 拿到后 `world != 0` |
| `off::GVC_WORLD` | `Offsets.hpp` | `UGameViewportClient -> UWorld*` | 同上 |
| `off::UENGINE_GAMEINSTANCE` / `off::GI_WORLD` / `off::GI_LOCALPLAYERS` | `Offsets.hpp` | 备用链 | 与上一条互为备份 |
| `off::UWORLD_PERSISTENT_LEVEL` | `Offsets.hpp` | `UWorld -> ULevel*` | `level != 0` |
| `off::ULEVEL_ACTORS` | `Offsets.hpp` | `ULevel -> TArray<AActor*>` | **关键**：`Num` 在 `[1, 4000]`；填错立刻表现为 0 或 4 亿 |
| `off::ACTOR_ROOT_COMPONENT` | `Offsets.hpp` | `AActor -> USceneComponent*` | 读出的 actor 位置有限且量级合理（不会出现 1e20） |
| `off::SCENE_COMPONENT_TO_WORLD` | `Offsets.hpp` | 组件 `ComponentToWorld` | 平移在 `+0x30`（`World.mm` 与 `EspMath::kMatTrans` 都按这个读） |

### 4.2 角色骨骼（决定"画不画得出人"）

| 符号 | 位置 | 作用 | 验证 |
| --- | --- | --- | --- |
| `off::CHAR_MESH` | `Offsets.hpp` | `AActor -> USkeletalMeshComponent*` | 非空，且下面骨骼数 40..400 |
| `off::SKEL_BONE_ARRAY` | `Offsets.hpp` | `TArray<FTransform>`（组件空间） | `bones::dump()` 打出的坐标能连成人形 |
| `off::SKEL_COMPONENT_TO_WORLD` | `Offsets.hpp` | 组件 `ComponentToWorld` | 同上；填错的症状是整具骨架平移一个身位 |
| `off::SKEL_BONE_NUM` | `EspOffsets.hpp` | 骨骼数字段（可选） | 不填就用 `TArray.Num` |
| `off::SKEL_BONE_WORLD_ARRAY` | `EspOffsets.hpp` | 世界空间骨骼数组（可选） | 填了就不走 C2W 变换，`MeshRef::worldSpace = true` |

### 4.3 相机（决定"往哪儿画"）

| 符号 | 位置 | 作用 | 验证 |
| --- | --- | --- | --- |
| `off::PC_PLAYER_CAMERA_MANAGER` | `Offsets.hpp` | `APlayerController -> APlayerCameraManager*` | 非空；否则 `world::camera()` 直接失败 |
| `off::PCM_CAMERA_CACHE` | `Offsets.hpp` | `FCameraCacheEntry`（`POV` 在 entry 起始） | `POV.Location` 与角色位置同量级、`FOV` 在 60~110 |
| `off::POV_LOCATION` / `POV_ROTATION` / `POV_FOV` / `POV_ASPECT` | `Offsets.hpp` | `FMinimalViewInfo` 字段（UE4 公开字段顺序：`0x00 / 0x0C / 0x18 / 0x1C`，仍要核对） | 转头时 `Yaw` 在动、抬头时 `Pitch` 在动 |

### 4.4 可选增强

| 符号 | 位置 | 不填的后果 |
| --- | --- | --- |
| `off::ACTOR_EQUIPPED_ITEMS` | `EspOffsets.hpp` | 头上不显示装备名，只用头/胸/腰骨骼框（**默认就是这样，不影响其它功能**） |
| `off::PSC_TEAM_ID` / `off::PSC_HEALTH` | `Offsets.hpp` | 本层不用（只显示、不分队、不显血） |

填完之后的最短验收：菜单里 `actors > 0`（世界通了）→ `players > 0`（骨骼通了）→
屏幕上有绿线（W2S 通了）→ `loot > 0`（关键词表对了）。

---

## 5. 骨骼索引与布局

### 5.1 两套内存布局（`EspBones.hpp` 的 `BoneLayout`）

| 布局 | 步长 | 平移 | 适用 |
| --- | --- | --- | --- |
| `kLayoutCompact`（默认，`boneLayout = 0`） | `0x30` | `+0x10`（`FQuat` 之后） | 你给的规格：`FQuat(16) + FVector(12) + FVector(12)`，补齐到 `0x30` |
| `kLayoutVectorized`（`boneLayout = 1`） | `0x50` | `+0x30`（3×4 旋转块之后） | `FTransformVectorized` 构建（x64/arm64 上 `ComponentToWorld` 常见这种） |

选错的症状：所有骨线挤成一团、或者人形整体飞出去。菜单里能直接切，不用重新编译。

### 5.2 骨架连线 / 头胸腰索引

`EspBones.mm` 顶部的 `kPairs[32]`（连线）与 `kBody`（head/neck/chest/pelvis）**全是占位值**，
用来先把链路打通；真机按 3.3 的 `dump()` 认领后替换。填错不会崩，只会画出奇怪的连法。

---

## 6. 已知风险（按踩坑概率排序）

1. **与旧 `Overlay.mm` 互斥（必读）**
   两者都 `ImGui::CreateContext()` 并各自 `NewFrame/Render`。同时编进同一个 dylib =
   两个 context + 两套帧循环，第一帧就崩（典型表现：app 起一秒闪退）。要么用本层（推荐，
   patch 已按这个写），要么继续用旧的。
2. **hook 时机**
   `install()` 在 `+0.8s` 调，此时可能还没建出第一个 `CAMetalLayer` —— 不影响：
   换的是**类**方法，类一定存在，第一个 drawable 出现时自然生效。
   反之，如果游戏在 `nextDrawable` 之前就 `present` 过一次（极少见），那第一帧不画，之后正常。
3. **drawable 为空 / 纹理拿不到**
   `nextDrawable` 可能返回 `nil`（三重缓冲用尽、后台、屏幕锁定），`[drawable texture]` 也可能
   为空。两条都判了，直接本帧返回，不建 pass、不 commit。
4. **`present` 没被调用**
   本层假设游戏走 `[drawable present]` / `[drawable presentAtTime:]`（`presentDrawable:` 在
   CB 完成时最终会调到这两个）。**如果游戏只 `presentDrawable:` 而不回调 drawable 的
   present**，表现是"菜单/描边一次都不出现"。回退方案（换挂点，不是换功能）：
   挂 `-[CAMetalLayer nextDrawable]` 记录 layer，然后 swizzle `-[_MTLCommandBuffer presentDrawable:]`
   （用 `objc_getClassList` 找实现了 `presentDrawable:` 且类名前缀 `_MTL` 的那个类），在原文
   调用**之前**用**同一个 CB**（`self`）起一个 `LoadAction = Load` 的 pass 画 ImGui ——
   同一条 CB 内 submission 顺序天然正确，比我们另开 CB 更稳。
5. **pass 顺序 / 双重呈现**
   我们另开一条 CB，在 `present` 原文之前 commit。`present` 是"GPU 完成后由驱动触发"的，
   所以我们的 pass 排在游戏之后、呈现之前 —— 这是旧 `Overlay.mm` 验证过的顺序。
   若真机看到"偶发一帧没有叠加"，改用第 4 条的回退方案（同 CB）即可彻底消掉。
6. **ImGui 版本差异**
   固定 1.90.9。唯一需要留意的是 `ImGui::SeparatorText()`（1.89.7 才加）。若你本地的
   `vendor/imgui` 比这更老，编译会报这一处：把三处 `ImGui::SeparatorText(x)` 改成
   `ImGui::Separator(); ImGui::TextDisabled(x);` 即可，其它调用全是长稳 API。
   `ImGui_ImplMetal_CreateDeviceObjects(dev)` 必须带设备参数（1.90.9 签名），无参版本会报参数数量错。
7. **触摸吞事件**
   `Settings::swallowTouch = true` 时，"起点落在菜单/按钮上"的那根手指移动/抬起事件不会
   递给游戏（否则拖菜单会顺带转视角/开枪）。同一 `UIEvent` 里如果有别的手指，会一起被吞
   —— 极端情况下可能吞掉一次按压。菜单里可以关掉这一项。
8. **坐标空间**
   触摸用 `[t locationInView:nil]`（窗口坐标，点）× `scale` 换成像素；绘制用 drawable 像素。
   如果游戏把 `CAMetalLayer` 放在非全屏的子视图里，两者会错位 —— 表现是"按钮点不到"。
   解法：把 `hk_sendEvent` 里的 `locationInView:nil` 换成 `locationInView:<那个 view>`，
   并把 `ScreenMap::scale` 用 `layer.bounds` 的换算（本层已经是这么算的）。
9. **安全区**
   `ScreenMap::insTop/Bottom/Left/Right` 只用于摆 UI（按钮下移到刘海之下），**不参与 W2S**
   —— 游戏是整屏渲染，把安全区算进 W2S 会让所有框整体偏移。
10. **骨骼布局猜错**（见 5.1）：菜单里切 `bone layout` 即可，不用重新编译。
11. **性能上限**
   每帧最多 `maxActors`（默认 512）个 actor、`maxBones`（默认 60）根骨线、`maxLoot`
    （默认 96）个物资；每个 actor 只做"类名 + 根位置 + 骨骼数组头"三次有界读，逐根骨才读
    40 字节。超上限的部分直接不画，不做排队。
12. **明文指纹**
    UI 文本全部走 `XDECL`（`espx::Lit`：编译期 XOR，解码到栈缓冲，用完就地 `memset`）。
    **源码里那个字面量是 XOR 的输入，躲不掉**；要确认的是**二进制里没有**：
    `strings -a ARCheat.dylib | grep -i -E 'skeleton|loot items|equipment col'` 应无命中。
    若有命中（某些 `-O0` 构建会把常量折叠成字面量），把 `-DAR_XKEY` 换一个值重新编译。
    注：`src/Core/Obfs.hpp` 的 `XSTR` 有个坑 —— `XStr<N>::get()` 的解码结果放在
    `static char buf[N]`，模板按**长度**实例化，所以等长常量会共用同一个缓冲（第二次调用
    直接返回第一个串）。本层因此自己做了 `XDECL`（解码到调用方给的栈缓冲）。旧
    `Overlay.mm` 里那排单字标签（`XSTR("a")`…`XSTR("f")`）全是一个字，就是这个现象。
13. **ARC / 内存**
    `.mm` 全程 `-fobjc-arc`：ObjC 对象一律由强引用成员持有（`MetalState`），函数指针里的
    `id` 参数/返回值是 `IMP` 同款签名（与旧 `Overlay.mm` 一致）。**没有裸 `new`/`delete`**、
    没有 `malloc`、没有 `std::vector` —— 全是定长栈缓冲与文件级静态量，避免在渲染线程上分配。

---

## 7. 回滚

* 代码级：`git checkout -- Makefile.legacy-tweak Tweak.xm`，再 `rm -rf src/Esp`。
* 运行时止血：照旧用 kill switch —— 建空文件 `/var/mobile/Library/Preferences/.sbx.state`，
  注入即静默（`Tweak.xm` 的 `kill_switch_on()` 在 `install()` 之前就返回了）。
* 菜单级止血：点左上角常驻按钮关掉菜单；三项显示开关全关后本层只剩每帧一次世界解析。

---

## 8. 自查结果

### 8.1 真跑过的（可复现）

| 检查 | 方法 | 结果 |
| --- | --- | --- |
| **编译** | `g++ -std=c++17 -Wall -Wextra`（ImGui/mach 用桩头）编 `EspBones.mm` / `EspLoot.mm` / `EspMenu.mm` | **0 error 0 warning** |
| **数据通路** | 把 `Game/Offsets.hpp` 的 24 个 `0x000` 换成互不冲突的测试值，桩掉 `vm_read_overwrite` 让它从 2MB 假内存取字节（`mem::read` 照原样跑），搭出真的 UE4 布局 | **全部通过**，明细见下表 |
| W2S 正确性 | 4000 组随机相机/目标点，**角度式与矩阵式对拍** | 可见性判定 100% 一致；坐标最大绝对偏差 **0.011 px** |
| 边界用例 | `pitch = 0` 正前方、背后点、45° 边缘、抬头 45°、`aspect = 1`、`yaw = 90` | 全部符合预期 |
| `quat_rotate` / `mat3x4_rotate` | 绕 Z 90° 定点用例 | 通过 |
| `ScreenMap` | 2532×1170 设备 + `bounds` 844×390 + 顶部 59pt 安全区 | `scale = 3`、`uiScale = 3`、点/像素换算通过 |
| include 解析 | 逐个 `#include "..."` 对 `-I` 目录求存在性 | 项目内全部解析到（`imgui*.h` 本地未拉，CI 会拉） |
| 零写入 | 全文搜 `mem::wr(` / `vm_write` / `mach_vm_write` / `vm_protect` / `mprotect(` | 0 命中 |
| 括号平衡 / 符号一致性 | 去注释后统计 `{} () []`；头文件声明 ↔ `.mm` 定义对照 | 通过 |
| 明文 UI 文本 | 除 `XDECL` 的 XOR 输入外，无 `skeleton/loot/equipment` 等字面量 | 通过 |

数据通路那一栏的具体断言（全部 PASS）：
```
[1] 紧凑布局 0x30 / 平移 0x10   骨骼数=100、bone[0]=C2W 平移、bone[5]=+50cm、越界/负索引返回 false
[2] C2W 绕 Z 转 90°            局部 (10,0,0) -> 世界 (0,10,0)+C2W    ✔ 四元数组合正确
[3] vectorized 布局 0x50/0x30   同一组输入算出的世界坐标与紧凑布局完全一致 ✔ 两套布局等价
[4] 骨骼数过滤                 20 / 500 根都被判为"不是角色"        ✔ 40..400 区间生效
[5] 绘制与预算                 15 条骨线 + 3 个装备框 + 1 行距离；maxBones=4 -> 正好 4 条；
                               超半径不画；背对不画                    ✔ 上限与剔除都生效
[6] 根位置回退                 actor_location 失败 -> 用骨盆骨骼定位    ✔ 三级回退
[7] 物资                       Pickup 类名命中、角色类名不命中、maxLoot 到顶即停 ✔
[8] 相机姿态                   yaw=0 正前方落屏幕中心；yaw=90 时 +Y 落屏幕中心 ✔
```

验证脚手架（假内存 + mach/ImGui 桩头 + 对拍脚本）放在 `default-workspace/.tmp-espx/`
（`test_math.cpp` / `sim/` / `check_esp.py`），**没有入库**；想复跑就照上面三行命令。
它跑的是**从本目录原样复制**过去的 `.mm` —— 唯一不同的是 `Game/Offsets.hpp` 里那 24 个
`0x000` 被替换成了测试值。

### 8.2 真没做过的
`EspRender.mm`（Metal 挂点 + ImGui 后端 + 触摸注入）是**真 ObjC**，本机没有 clang / iOS SDK，
**未编译验证**。这一段参照的是工程里已知能编过的 `src/Render/Overlay.mm`（同一套
`@protocol` 自声明 + `method_setImplementation` + `ImGui_ImplMetal_*` 调用顺序），
并且只用 iOS 15 SDK 里确定存在的类型：`MTLDevice` / `MTLCommandBuffer` /
`MTLRenderPassDescriptor` / `MTLRenderCommandEncoder` / `MTLTexture` / `CAMetalLayer` /
`UIEvent` / `UITouch` / `UIEdgeInsets` / `os_unfair_lock`。

第一次构建最可能报的三处，以及改法：

1. `ImGui::SeparatorText` 未声明 → 本机 vendor 的 ImGui 比 1.89.7 老（改法见 6.6）。
2. `ImGui_ImplMetal_CreateDeviceObjects` 参数数量不对 → 本地是 1.91+（无参版）或 1.89-（无此名）。
   按 `vendor/imgui/backends/imgui_impl_metal.h` 里的声明改调用即可，只有一个调用点。
3. `-Wunused-const-variable` 之类的告警被 `-Werror` 放大 → 本工程 `Makefile.legacy-tweak`
   没有 `-Werror`；`if (off::XXX)` 这种"填 0 时恒假"的分支如果触发 `-Wunreachable-code`，
   加 `-Wno-unreachable-code`。

---

## 9. 与旧实现的差异（为什么不是照抄 `src/Render/ESP.mm`）

1. **W2S 修正**。旧的 `ESP.mm` / `Aim.mm` 里这段：

   ```
   x2 = cp*x1 + sp*z1;    // 这是【前向/深度】分量
   z2 = -sp*x1 + cp*z1;   // 这是【上】分量
   sx = (x2 / (z2*tanHalf*aspect) + 1) * 0.5 * W;   // 拿"上"当深度除
   sy = (1 - y1 / (z2*tanHalf)) * 0.5 * H;
   ```

   两个除法都用 `z2`（上分量）当深度。后果：`pitch == 0` 时 `z2` 恒为 0，函数直接 `return false`
   （整屏不画）；`pitch != 0` 时水平坐标随抬头低头乱飘。
   本层按 `FSceneView::WorldToScreen` 重写：深度 = `dot(d, fwd)`，水平 = `dot(d, right)`，
   垂直 = `dot(d, up)`，并对两条路径做了对拍（第 8 节）。
2. **世界读取不再每次分配**：不再调 `world::actors()`（内部 `std::vector` + `reserve`），
   改成直接读 `TArray` 头 + 带 `maxActors` 上限的下标循环。
3. **装备从"类名 + Owner"改成"骨骼框 + 可选 EquippedItems"**：只显示形态下不需要每次
   遍历两遍 actor，也不需要 `Owner` 链。
4. **UI 文本走 `XDECL`** 而不是 `XSTR`（后者有等长常量共用缓冲的问题，见 6.12）。
