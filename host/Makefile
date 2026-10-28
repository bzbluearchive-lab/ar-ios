# 启动器 app
#
# 产出：.theos/obj/arm64/ARHost.app/
# 作用：配置 UI + 孵化 daemon。它自己不含任何作弊逻辑，划掉它不影响浮窗。

ARCHS = arm64
TARGET = iphone:clang:latest:15.0

include $(THEOS)/makefiles/common.mk

APPLICATION_NAME = ARHost

ARHost_FILES = main.mm ../src/Kernel/kfd_glue.c
# 注意：_CFLAGS 会传给所有源文件，包括新加的 C 文件 kfd_glue.c。
# -fobjc-arc 与 -std=c++17 对 C 源文件是非法参数（clang 直接报错），
# 所以它们必须放到 _CCFLAGS（C++/ObjC++ 专用）里。
ARHost_CFLAGS  = -I../src -I../src/Host -I. -Wno-error
ARHost_CCFLAGS = -fobjc-arc -std=c++17
ARHost_CXXFLAGS = -fobjc-arc -std=c++17
ARHost_FRAMEWORKS = UIKit Foundation
ARHost_CODESIGN_FLAGS = -Sentitlements.plist
ARHost_INSTALL_PATH = /Applications

include $(THEOS_MAKE_PATH)/application.mk

# 把 daemon 与 payload 一起塞进 app 包，方便整包分发
after-ARHost-stage::
	@echo "[i] app bundle: $$(ls -d .theos/obj/*/ARHost.app 2>/dev/null)"
