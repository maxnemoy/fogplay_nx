.SUFFIXES:
DEVKITPRO ?= /opt/devkitpro
include $(DEVKITPRO)/libnx/switch_rules
ROOT := $(abspath ../..)
APP_TITLE := FogPlay Moonlight Local Test
APP_AUTHOR := maxnemoy
APP_VERSION := $(shell cat VERSION)
TARGET := moonlight_probe
OUTPUT := $(CURDIR)/dist/$(TARGET)
PREFIX := $(ROOT)/third_party/moonlight/switch
RTC_PREFIX := $(ROOT)/third_party/rtc/switch
PORTLIBS := $(DEVKITPRO)/portlibs/switch
PACKAGES := SDL2_ttf sdl2 libavcodec libavutil libswscale opus expat
PC_ENV := PKG_CONFIG_LIBDIR=$(PORTLIBS)/lib/pkgconfig
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CXXFLAGS := -g -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ -std=c++17 -fexceptions -frtti -MMD -MP -I$(ROOT)/src -I$(ROOT)/third_party -I$(PREFIX)/include -I$(PREFIX)/include/gamestream -I$(PREFIX)/include/crypto -I$(DEVKITPRO)/libnx/include $(shell $(PC_ENV) pkg-config --cflags $(PACKAGES))
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections,-Map,$(CURDIR)/build/$(TARGET).map
LIBPATHS := -L$(ROOT)/third_party/ffmpeg-diag/switch/lib -L$(PREFIX)/lib -L$(PORTLIBS)/lib -L$(DEVKITPRO)/libnx/lib
LIBS := -Wl,--start-group -lgamestream -lmoonlight-curl -lmoonlight-common-c -lenet $(shell $(PC_ENV) pkg-config --static --libs $(PACKAGES)) -lmbedtls -lmbedx509 -lmbedcrypto -lz -lnx -lm -Wl,--end-group
CXXFLAGS += -DFOG_MOONLIGHT_PROBE_VERSION=\"$(APP_VERSION)\"
LIBPATHS := -L$(RTC_PREFIX)/lib $(LIBPATHS)
LIBS := $(subst -lmbedcrypto,-lmbedcrypto -leverest -lp256m,$(LIBS))
LD := $(CXX)
OFILES := build/main.o build/host_client.o build/media.o
NROFLAGS := --icon=$(ROOT)/assets/icon.jpg --nacp=$(OUTPUT).nacp
.PHONY: all
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES) $(wildcard $(PREFIX)/lib/*.a) | dist
$(OUTPUT).nacp: Makefile VERSION | dist
build/main.o: main.cpp host_client.hpp probe_buffers.hpp VERSION Makefile | build
	$(CXX) $(CXXFLAGS) -c $< -o $@
build/host_client.o: host_client.cpp host_client.hpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@
build/media.o: $(ROOT)/src/features/streaming/infrastructure/media.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@
build dist:
	mkdir -p $@
-include $(OFILES:.o=.d)
