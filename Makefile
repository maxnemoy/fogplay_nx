# Based on the official switchbrew/switch-examples application template.
# Toolchain and libraries: official devkitPro distribution.
.SUFFIXES:
.DEFAULT_GOAL := all
NEEDS_TOOLCHAIN := $(if $(MAKECMDGOALS),$(filter-out clean distclean,$(MAKECMDGOALS)),all)
ifneq ($(strip $(NEEDS_TOOLCHAIN)),)
ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO or run ./build.sh)
endif
include $(DEVKITPRO)/libnx/switch_rules
endif

APP_VERSION := 0.4.24
APP_TITLE := FogPlay NX $(APP_VERSION)
APP_AUTHOR := maxnemoy
TARGET := fogplay_nx
BUILD := build
OUTPUT := $(CURDIR)/dist/$(TARGET)
PORTLIBS := $(DEVKITPRO)/portlibs/switch
PC_ENV := PKG_CONFIG_LIBDIR=$(PORTLIBS)/lib/pkgconfig
PACKAGES := libcurl SDL2_ttf SDL2_image sdl2 libavcodec libavutil libswscale opus
RTC_PREFIX := $(CURDIR)/third_party/rtc/switch
FFMPEG_DIAG := $(CURDIR)/third_party/ffmpeg-diag/switch
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
INCLUDE := -I$(CURDIR)/src -I$(CURDIR)/third_party -isystem $(RTC_PREFIX)/include -isystem $(DEVKITPRO)/libnx/include -DRTC_STATIC -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=1
CFLAGS := -g -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ $(INCLUDE) $(shell $(PC_ENV) pkg-config --cflags $(PACKAGES) 2>/dev/null)
CXXFLAGS := $(CFLAGS) -std=c++17 -fexceptions -frtti -MD -MP
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections,-Map,$(CURDIR)/build/$(TARGET).map
LIBPATHS := -L$(FFMPEG_DIAG)/lib -L$(RTC_PREFIX)/lib -L$(PORTLIBS)/lib -L$(DEVKITPRO)/libnx/lib
LIBS := -Wl,--start-group -ldatachannel -lusrsctp -lsrtp2 -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -ljuice $(shell $(PC_ENV) pkg-config --static --libs $(PACKAGES) 2>/dev/null) -lnx -lm -Wl,--end-group
LD := $(CXX)
CPPFILES := $(shell find src -name '*.cpp' | sort)
OFILES := $(patsubst src/%.cpp,build/%.o,$(CPPFILES))
APP_ICON := $(CURDIR)/assets/icon.jpg
NROFLAGS := --icon=$(APP_ICON) --nacp=$(OUTPUT).nacp --romfsdir=$(CURDIR)/romfs
ROMFS_FILES := $(shell find romfs -type f 2>/dev/null)

.PHONY: all clean distclean FORCE
all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp $(ROMFS_FILES) $(APP_ICON)
$(OUTPUT).elf: $(OFILES) build/.flags $(FFMPEG_DIAG)/lib/libavcodec.a $(wildcard $(RTC_PREFIX)/lib/*.a) | dist
$(OUTPUT).nacp: Makefile | dist

build/%.o: src/%.cpp build/.flags | build
	@mkdir -p $(dir $@)
	@echo compiling $(notdir $<)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build dist:
	mkdir -p $@

# Preserve the stamp mtime when effective flags are unchanged.
quote = '$(subst ','"'"',$(1))'
build/.flags: FORCE | build
	@printf '%s\n' $(call quote,$(CXX)) $(call quote,$(CXXFLAGS)) $(call quote,$(LDFLAGS)) $(call quote,$(LIBPATHS)) $(call quote,$(LIBS)) > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

clean:
	rm -rf -- "$(CURDIR)/build"
	rm -f -- "$(CURDIR)/dist/fogplay_nx.elf" "$(CURDIR)/dist/fogplay_nx.nacp" "$(CURDIR)/dist/fogplay_nx.nro" "$(CURDIR)/dist/fogplay_nx.nro.sha256"

distclean: clean
	rm -f -- "$(CURDIR)/dist/fogplay_nx-sd.zip" "$(CURDIR)/dist/verification.json"

-include $(OFILES:.o=.d)

