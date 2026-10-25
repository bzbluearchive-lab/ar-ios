# 交付物版本表

每次改动重新出包就 +1，文件名带版本号，避免同名混淆。
表格按版本递减排列，**最上面一行才是当前要装的包**。

| 版本 | 文件 | 大小 | sha256（前16） | 状态 | 说明 |
|---|---|---|---|---|---|
| **v3** | `ARProbe-xprobe-v3.ipa` | 17,003 B | `F5FFCFB537A95421` | ⏳ 待真机验证 | 跨进程探针：`task_for_pid` + 读 GEngine + FName 类名校验。拆成 `1 Light` / `2 Deep` 两个按钮，每步日志立即落盘 |
| v2 | `ARProbe-imGUI-v2.ipa` | 933,185 B | `5C074422E3CE479C` | ❌ 已废弃 | 注入版 ARProbe（内含 ARCheat.dylib + ImGui 叠加层）。启动约 1 秒后 Metal 断言崩溃（`__assert_rtn` on `com.Metal.CompletionQueueDispatch`） |
| v1 | `ARProbe-blank-v1.ipa` | 11,956 B | `8E832B47E10C2950` | ✅ 可用 | 裸 MTKView 空壳，只用来验证 app 能装能跑 |
| v1 | `ARCheat-v1.dylib` | 2,870,240 B | `0321541BBCB4B185` | ⚠️ 偏移为空 | 首个能加载的 dylib（fat arm64+arm64e）。`Offsets.hpp` 全是占位符，装上不显示任何东西 |
| v1 | `ARCheat-v1.zip` | 922,477 B | `73F7DBD8E3FE15A1` | ⚠️ 同上 | 同上 + plist + 注入说明 |

## 版本规则

- 文件名格式：`<组件>-<形态>-v<版本>.<扩展名>`
  - `ARProbe-xprobe-vN`：跨进程探针（不注入游戏）
  - `ARProbe-imGUI-vN`：注入版探针（含 ImGui 叠加层）
  - `ARCheat-vN`：游戏用 dylib
- 版本号来源：[`probe/VERSION`](../probe/VERSION)，CI 构建时读它写进 `CFBundleShortVersionString`，并用它命名产物
- 每次改代码 → 先把 `probe/VERSION` +1 → 再 dispatch，就不会出现两个同名不同内容的包

## 装包前的自检

1. 文件名里的版本号 = 你要的版本
2. sha256 前 16 位和上表一致
3. 同一个 bundle id 只能装一个：`ARProbe-*` 都是 `com.local.arprobe`，**换版本要先卸载**
