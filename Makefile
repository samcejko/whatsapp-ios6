# WhatsApp6 - unofficial WhatsApp linked-device client for jailbroken iOS. One source tree, two packages (Theos):
#   make package                               armv7 for iOS 6.x (iOS 9.3 SDK, deployment target 6.0)
#   make package THEOS_PACKAGE_SCHEME=rootless arm64 for iOS 14-17 (iOS 16.5 SDK, deployment target 14.0)
# (`make clean` between the two; CI builds both, see .github/workflows/build.yml.) mbedTLS/opus/qrcodegen are
# fetched by CI into vendor/; monocypher is vendored in-tree.

ifeq ($(THEOS_PACKAGE_SCHEME),rootless)
TARGET := iphone:clang:16.5:14.0
ARCHS := arm64
else
TARGET := iphone:clang:9.3:6.0
ARCHS := armv7
endif
DEBUG ?= 0

THEOS_DEVICE_IP ?= 192.168.1.176
THEOS_DEVICE_USER ?= root

include $(THEOS)/makefiles/common.mk

APPLICATION_NAME := WhatsApp6

OPUS_SRC := $(filter-out vendor/opus/src/opus_demo.c vendor/opus/src/repacketizer_demo.c vendor/opus/src/opus_compare.c vendor/opus/src/mlp_train.c, $(wildcard vendor/opus/src/*.c)) \
            $(filter-out vendor/opus/celt/opus_custom_demo.c, $(wildcard vendor/opus/celt/*.c)) \
            $(wildcard vendor/opus/silk/*.c) \
            $(wildcard vendor/opus/silk/fixed/*.c)

WhatsApp6_FILES := $(wildcard src/*.m) $(wildcard src/*/*.m) \
                   $(wildcard core/*.c) $(wildcard core/rtc/*.c) \
                   vendor/monocypher/monocypher.c \
                   vendor/mbedtls_glue.c \
                   $(wildcard vendor/mbedtls/library/*.c) \
                   $(OPUS_SRC) \
                   $(wildcard vendor/qrcodegen/*.c)

WhatsApp6_FRAMEWORKS := UIKit Foundation CoreGraphics QuartzCore CoreText Security ImageIO CoreMedia CoreVideo OpenGLES AudioToolbox AVFoundation

WhatsApp6_CFLAGS := -Isrc -Icore -Icore/rtc \
                    -Ivendor -Ivendor/monocypher -Ivendor/mbedtls/include -Ivendor/mbedtls/library \
                    -Ivendor/opus/include -Ivendor/opus/celt -Ivendor/opus/silk -Ivendor/opus/silk/fixed -Ivendor/opus -Ivendor/qrcodegen \
                    -DOPUS_BUILD -DFIXED_POINT=1 -DDISABLE_FLOAT_API -DUSE_ALLOCA -DHAVE_LRINT -DHAVE_LRINTF \
                    -Os -fvisibility=hidden \
                    -Wall -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable \
                    -Wno-deprecated-declarations -Wno-unknown-warning-option -Wno-unused-parameter -Wno-sign-compare \
                    -Wno-nullability-completeness -Wno-nullability-completeness-on-arrays -Wno-error

WhatsApp6_OBJCFLAGS := -fobjc-arc -Wunguarded-availability

WhatsApp6_LDFLAGS := -lz

include $(THEOS)/makefiles/application.mk

WhatsApp6_STAGED_APP := $(THEOS_STAGING_DIR)/Applications/WhatsApp6.app
WhatsApp6_STAGED_BIN := $(WhatsApp6_STAGED_APP)/WhatsApp6

ifeq ($(THEOS_PACKAGE_SCHEME),rootless)
after-stage::
	python3 "$(THEOS_PROJECT_DIR)/tools/modern_bundle.py" "$(WhatsApp6_STAGED_APP)"
	ldid -S"$(THEOS_PROJECT_DIR)/entitlements-arm64.xml" "$(WhatsApp6_STAGED_BIN)"
before-package::
	sed -i -e 's/^Depends: .*/Depends: firmware (>= 15.0)/' "$(THEOS_STAGING_DIR)/DEBIAN/control"
	printf '%s\n' '#!/bin/sh' '[ "$$1" = configure ] || exit 0' \
		'command -v uicache >/dev/null 2>&1 && { uicache -p /var/jb/Applications/WhatsApp6.app >/dev/null 2>&1 || uicache -a >/dev/null 2>&1; }' \
		'exit 0' > "$(THEOS_STAGING_DIR)/DEBIAN/postinst"
	printf '%s\n' '#!/bin/sh' '[ "$$1" = remove ] || exit 0' \
		'command -v uicache >/dev/null 2>&1 && uicache -a >/dev/null 2>&1' \
		'exit 0' > "$(THEOS_STAGING_DIR)/DEBIAN/postrm"
	chmod 0755 "$(THEOS_STAGING_DIR)/DEBIAN/postinst" "$(THEOS_STAGING_DIR)/DEBIAN/postrm"
else
after-stage::
	install_name_tool -change /System/Library/Frameworks/CFNetwork.framework/CFNetwork /System/Library/Frameworks/Foundation.framework/Foundation "$(WhatsApp6_STAGED_BIN)" || true
	ldid -S"$(THEOS_PROJECT_DIR)/entitlements.xml" "$(WhatsApp6_STAGED_BIN)"
endif
