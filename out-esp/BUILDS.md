# 交付物版本表

每次改动重新出包就 +1。**文件名、app 显示名、bundle id、版本号四者一致**，
所以装到设备上不可能混淆，而且不同版本可以**并存安装**（bundle id 不同）。

**最上面一行才是当前要装的包。**

| 版本 | 文件 | 大小 | sha256（前16） | bundle id | 状态 | 说明 |
|---|---|---|---|---|---|---|
| **v5** | `ARProbe-xprobe-v5.ipa` | 17,548 B | `102E123B41A19A6A` | `com.local.arprobe.v5` | ⏳ **要装的** | 跨进程探针。bundle id 带版本，可与旧版并存；`1 Light`/`2 Deep` 两键；每步日志立即落盘 |
| v4 | `ARProbe-xprobe-v4.ipa` | 17,544 B | `534998C21407C1EA` | `com.local.arprobe` | ⬜ 已被 v5 取代 | 首次把版本写进包内（显示名 `ARProbe-v4`），但 bundle id 仍共用 |
| v3 | `ARProbe-xprobe-v3.ipa` | 17,003 B | `F5FFCFB537A95421` | `com.local.arprobe` | ⬜ 已被取代 | light/deep 拆分 + 逐步落盘日志 |
| v2 | `ARProbe-imGUI-v2.ipa` | 933,185 B | `5C074422E3CE479C` | `com.local.arprobe` | ❌ 废弃 | 注入版（含 ARCheat.dylib + ImGui）。启动约 1 秒后 Metal 断言崩溃（`__assert_rtn` on `com.Metal.CompletionQueueDispatch`）|
| v1 | `ARProbe-blank-v1.ipa` | 11,956 B | `8E832B47E10C2950` | `com.local.arprobe` | ✅ 可用 | 裸 MTKView 空壳，只验证能装能跑 |
| v1 | `ARCheat-v1.dylib` | 2,870,240 B | `0321541BBCB4B185` | — | ⚠️ 偏移为空 | 游戏用 dylib（fat arm64+arm64e），`Offsets.hpp` 全占位符 |
| v1 | `ARCheat-v1.zip` | 922,477 B | `73F7DBD8E3FE15A1` | — | ⚠️ 同上 | 同上 + plist + 注入说明 |

## 命名规则

```
ARProbe-xprobe-vN.ipa
  ├─ 文件名            ARProbe-xprobe-vN.ipa
  ├─ 主屏显示名        ARProbe-vN
  ├─ CFBundleIdentifier com.local.arprobe.vN     ← 每版唯一，可并存
  ├─ CFBundleVersion    N
  └─ CFBundleShortVersionString  0.1.N
```

版本号来源：[`probe/VERSION`](../probe/VERSION)。CI 读它写进 plist 并命名产物。

出包流程：**改代码 → `probe/VERSION` +1 → dispatch**，四者自动保持一致。

## 装包自检

1. 文件名版本号 = 你要的版本
2. sha256 前 16 位与上表一致
3. 从 v5 起 bundle id 每版不同 → **可以直接装，不用卸载旧的**；旧的想清掉就在主屏删掉对应 `ARProbe-vN`
4. 装完主屏显示 `ARProbe-vN`，app 内顶部显示 `build 0.1.N`，两处对得上才算装对了
