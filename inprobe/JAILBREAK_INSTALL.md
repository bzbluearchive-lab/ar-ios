# 越狱环境（Dopamine / ElleKit）下的快速迭代方案

> 适用前提：设备已越狱（Dopamine 3.0.10 rootless），`/var/jb` 存在。
> 目的：**不再为每次改动重传 2.7 GB 的 IPA**。改一次逻辑 = 换一个 110 KB 的 dylib + 重启游戏。

## 为什么这条路比 TrollStore 注入好

| | TrollStore + 整包替换 | 越狱 tweak |
|---|---|---|
| 每次迭代 | 打包 2.7 GB + 传输 + 重装 | 覆盖一个 110 KB 文件 |
| 迭代耗时 | 20–40 分钟 | 30 秒 |
| 签名 | 要 TrollStore 重签 | ElleKit 加载，不需要签名 |
| 卸载恢复 | 重装原版 IPA | 删掉一个文件 |

## 安装步骤

### 1. 放 dylib

```
/var/jb/Library/MobileSubstrate/DynamicLibraries/ARInProbe.dylib
```

（rootless 越狱里**必须**在 `/var/jb` 前缀下；直接放 `/Library/...` 无效）

### 2. 放过滤器 plist

同名 `.plist`，只对目标 app 生效：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Filter</key>
	<dict>
		<key>Bundles</key>
		<array>
			<string>com.proximabeta.mf.uamo.esp</string>
			<string>com.proximabeta.mf.uamo</string>
		</array>
	</dict>
</dict>
</plist>
```

文件名与 dylib 同名：`/var/jb/Library/MobileSubstrate/DynamicLibraries/ARInProbe.plist`

### 3. 重启游戏

不用 respring，直接杀游戏进程再启动即可。
tweak 加载失败时先 `respring` 一次再试。

## 输出位置

dylib 会依次尝试这些路径写日志，第一个可写的生效：

```
<游戏容器>/Documents/boot.log                      ← 首选
/var/mobile/Library/Preferences/aresp_boot.log     ← 越狱后可写
/tmp/aresp_boot.log
/var/tmp/aresp_boot.log
```

`boot.log` 里会有：

```
[boot] crash handlers installed
[boot] constructor entered pid=...
[boot] bootlog path resolved: ...
[boot] base=0x... images=N
[boot] calling hide_self_image
[boot] hide returned N
[boot] probe started
[boot] engine=0x...
[boot] world=0x...
[CRASH] sig=11 (SIGSEGV) addr=0x...      ← 崩了就有这行，带信号和地址
```

## 关键：这条路的副作用

ElleKit 注入任何进程都会留下痕迹（`image[0]` 可能变成 `systemhook.dylib`）。
商业外挂 pdxrs 正是靠这个在 0.888 秒自杀。

所以我们用这条路时，**要观察两件事**：

1. 游戏会不会因为 ElleKit 注入而崩（若崩 → ACE 检测越狱注入，这条路也不通）
2. 我们的 `hide_self_image` 是否还能正常跑（`hide returned N`）

## 如果 ElleKit 注入被检测

还有一个更底层的选项：直接用 `launchctl` 环境变量注入

```bash
# 需要 root + 可写 LaunchDaemon
launchctl setenv DYLD_INSERT_LIBRARIES /var/jb/usr/lib/ARInProbe.dylib
```

但 `DYLD_INSERT_LIBRARIES` 是最容易被检测的（pdxrs 的第一个检查就是 `getenv("DYLD_INSERT_LIBRARIES")`），**不建议**。

## 回滚

```bash
rm /var/jb/Library/MobileSubstrate/DynamicLibraries/ARInProbe.dylib
rm /var/jb/Library/MobileSubstrate/DynamicLibraries/ARInProbe.plist
```
