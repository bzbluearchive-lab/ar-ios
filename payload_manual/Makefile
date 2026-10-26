# payload_manual：手动映射用的 dylib
#
# 与 payload/ 的区别：不定义任何新的 ObjC 类（手动映射时 runtime 不认识我们的类），
# 只通过 objc_msgSend 使用系统已有的类。
#
# 产出：.theos/obj/arm64/ARPayloadManual.dylib

ARCHS = arm64
TARGET = iphone:clang:latest:15.0

include $(THEOS)/makefiles/common.mk

LIBRARY_NAME = ARPayloadManual

ARPayloadManual_FILES = main.mm
ARPayloadManual_CFLAGS = -fobjc-arc -std=c++17 -I../src -Wno-error -Wno-unused-function
ARPayloadManual_FRAMEWORKS = Foundation UIKit
ARPayloadManual_INSTALL_PATH = /usr/lib

include $(THEOS_MAKE_PATH)/library.mk
