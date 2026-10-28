# tweak/Makefile —— 越狱注入插件（Theos tweak）
#
# 目标游戏：暗区突围国际服（bundle id com.proximabeta.mf.uamo，进程名 UAGame）
# 越狱环境：Dopamine (rootless) -> THEOS_PACKAGE_SCHEME=rootless，装在 /var/jb
#
# 用法（在 CI 或任意 macOS 上）：
#   make clean package THEOS_PACKAGE_SCHEME=rootless
# 产物：packages/*.deb，用 Sileo/Filza 安装，或 ssh 进去 dpkg -i

export THEOS_PACKAGE_SCHEME = rootless
ARCHS = arm64 arm64e
TARGET = iphone:clang:latest:15.0
INSTALL_TARGET_PROCESSES = SpringBoard

include $(THEOS)/makefiles/common.mk

TWEAK_NAME = AREsp

AREsp_FILES = Tweak.xm
AREsp_CFLAGS = -fobjc-arc -std=c++17 -Wno-error -Wno-unused-function
AREsp_FRAMEWORKS = UIKit Foundation QuartzCore

include $(THEOS_MAKE_PATH)/tweak.mk
