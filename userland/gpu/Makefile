# Makefile for Daisy GPU userland components
# Target: iOS 16 arm64 (iPhone X / A11 ABI)

ifeq ($(origin CC),default)
CC := xcrun --sdk iphoneos clang
endif
ARCH ?= arm64
MIN_IOS ?= 16.0
SDK_PATH ?= $(shell xcrun --sdk iphoneos --show-sdk-path 2>/dev/null)

PREFIX ?= /daisy
LIBDIR ?= $(PREFIX)/lib

CFLAGS ?= -arch $(ARCH) -miphoneos-version-min=$(MIN_IOS) -O2 -Wall
ifneq ($(SDK_PATH),)
CFLAGS += -isysroot $(SDK_PATH)
endif

CODESIGN ?= codesign -f -s -
ENTITLEMENTS ?= ../ent.plist

OUT ?= out

ALL_TARGETS = $(OUT)/bbd_wrap $(OUT)/libdaisy-inject.dylib

all: $(ALL_TARGETS)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/bbd_wrap: bbd_wrap.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<
	if [ -f "$(ENTITLEMENTS)" ]; then $(CODESIGN) --entitlements "$(ENTITLEMENTS)" $@; else $(CODESIGN) $@; fi

$(OUT)/libdaisy-inject.dylib: daisy_inject.c | $(OUT)
	$(CC) $(CFLAGS) -dynamiclib -install_name $(LIBDIR)/libdaisy-inject.dylib \
		-framework CoreFoundation -framework IOKit -o $@ $<
	if [ -f "$(ENTITLEMENTS)" ]; then $(CODESIGN) --entitlements "$(ENTITLEMENTS)" $@; else $(CODESIGN) $@; fi

$(OUT)/libtopaz-inject.dylib: topaz_inject.c | $(OUT)
	$(CC) $(CFLAGS) -dynamiclib -install_name /topaz/lib/libtopaz-inject.dylib \
		-framework CoreFoundation -framework IOKit -o $@ $<
	if [ -f "$(ENTITLEMENTS)" ]; then $(CODESIGN) --entitlements "$(ENTITLEMENTS)" $@; else $(CODESIGN) $@; fi

# eagl_gen.h generation requires gl.xml and exported-gl.txt from a built Mesa:
#   make eagl_gen.h MESA_OUT=../mesa/build-mesa/out
MESA_OUT ?= ../mesa/build-mesa/out
eagl_gen.h: eagl_slots.txt $(MESA_OUT)/gl.xml $(MESA_OUT)/exported-gl.txt eagl.c
	python3 mkeagl.py eagl_slots.txt $(MESA_OUT)/gl.xml $(MESA_OUT)/exported-gl.txt eagl.c > $@

# libdaisy-gl.dylib requires Mesa's libOSMesa.8.dylib and generated eagl_gen.h
$(OUT)/libdaisy-gl.dylib: eagl.c gl_osmesa.c eagl_gen.h | $(OUT)
	$(CC) $(CFLAGS) -dynamiclib -install_name $(LIBDIR)/libdaisy-gl.dylib \
		-I. -framework CoreFoundation -framework IOKit -framework IOSurface -lobjc \
		$(MESA_OUT)/libOSMesa.8.dylib -o $@ eagl.c gl_osmesa.c
	if [ -f "$(ENTITLEMENTS)" ]; then $(CODESIGN) --entitlements "$(ENTITLEMENTS)" $@; else $(CODESIGN) $@; fi

clean:
	rm -rf $(OUT) eagl_gen.h

.PHONY: all clean

