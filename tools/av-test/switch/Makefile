.SUFFIXES:
DEVKITPRO ?= /opt/devkitpro
include $(DEVKITPRO)/libnx/switch_rules

ROOT := $(abspath ../../..)
APP_TITLE := FogPlay AV Test
APP_AUTHOR := maxnemoy
APP_VERSION := 0.1.0
TARGET := fogplay_av_test
OUTPUT := $(CURDIR)/dist/$(TARGET)
PORTLIBS := $(DEVKITPRO)/portlibs/switch
PC_ENV := PKG_CONFIG_LIBDIR=$(PORTLIBS)/lib/pkgconfig
PACKAGES := SDL2_ttf sdl2 libavcodec libavutil libswscale opus
RTC_PREFIX := $(ROOT)/third_party/rtc/switch
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
INCLUDE := -I$(ROOT)/src -I$(ROOT)/third_party -isystem $(RTC_PREFIX)/include -isystem $(DEVKITPRO)/libnx/include -DRTC_STATIC -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=1
CFLAGS := -g -O2 -Wall -Wextra -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ $(INCLUDE) $(shell $(PC_ENV) pkg-config --cflags $(PACKAGES))
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fexceptions -frtti -MMD -MP
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections,-Map,$(CURDIR)/build/$(TARGET).map
LIBPATHS := -L$(RTC_PREFIX)/lib -L$(PORTLIBS)/lib -L$(DEVKITPRO)/libnx/lib
LIBS := -Wl,--start-group -ldatachannel -lusrsctp -lsrtp2 -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -ljuice $(shell $(PC_ENV) pkg-config --static --libs $(PACKAGES)) -lnx -lm -Wl,--end-group
LD := $(CXX)
OFILES := build/main.o build/media.o
NROFLAGS := --icon=$(DEVKITPRO)/libnx/default_icon.jpg --nacp=$(OUTPUT).nacp

.PHONY: all
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES) $(wildcard $(RTC_PREFIX)/lib/*.a) | dist
$(OUTPUT).nacp: Makefile | dist

build/main.o: main.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@
build/media.o: $(ROOT)/src/features/streaming/infrastructure/media.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@
build dist:
	mkdir -p $@
-include $(OFILES:.o=.d)
