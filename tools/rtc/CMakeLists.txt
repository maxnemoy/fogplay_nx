cmake_minimum_required(VERSION 3.16)
project(FogPlayRTC LANGUAGES C CXX)
set(CMAKE_SKIP_INSTALL_RULES ON)
get_filename_component(RTC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  # libnx sockaddr has sa_len at byte 0 and its family at byte 1. Keep
  # usrsctp's private constructors and libdatachannel's AF_CONN producer aligned.
  add_compile_definitions(_DEFAULT_SOURCE HAVE_SCONN_LEN)
  # Older configurations cached this header-member check as absent. Recheck
  # the patched public header instead of retaining that incompatible result.
  unset(have_sconn_len CACHE)
  add_compile_options(-fPIE)
  add_compile_options(-include "${CMAKE_CURRENT_LIST_DIR}/compat/switch-posix.h")
  include_directories("${CMAKE_CURRENT_LIST_DIR}/compat")
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/mbedtls-user-config.h" CACHE FILEPATH "" FORCE)
add_subdirectory("${RTC_ROOT}/third_party/rtc/mbedtls" mbedtls)
if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  target_sources(mbedcrypto PRIVATE "${CMAKE_CURRENT_LIST_DIR}/entropy-switch.c")
endif()
add_library(MbedTLS::MbedTLS ALIAS mbedtls)
set(MBEDTLS_INCLUDE_DIRS "${RTC_ROOT}/third_party/rtc/mbedtls/include" CACHE PATH "" FORCE)
set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
set(MBEDTLS OFF CACHE BOOL "" FORCE)
set(GCM OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(USE_NICE OFF CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(sctp_build_programs OFF CACHE BOOL "" FORCE)
set(sctp_werror OFF CACHE BOOL "" FORCE)
add_subdirectory("${RTC_ROOT}/third_party/rtc/libdatachannel" libdatachannel)

if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  target_sources(juice-static PRIVATE "${CMAKE_CURRENT_LIST_DIR}/ifaddrs-switch.c" "${CMAKE_CURRENT_LIST_DIR}/pipe-switch.c" "${CMAKE_CURRENT_LIST_DIR}/numeric-switch.c")
endif()
add_executable(rtc-link-smoke link-smoke.cpp)
target_link_libraries(rtc-link-smoke PRIVATE datachannel-static)
if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  target_include_directories(rtc-link-smoke PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/include")
  target_link_libraries(rtc-link-smoke PRIVATE curl z)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  add_executable(rtc-host-loopback host-loopback.cpp)
  target_link_libraries(rtc-host-loopback PRIVATE datachannel-static)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  add_executable(rtc-switch-probe switch-probe.cpp)
  target_include_directories(rtc-switch-probe PRIVATE "${RTC_ROOT}/third_party/rtc/libdatachannel/deps/usrsctp/usrsctplib")
  target_link_libraries(rtc-switch-probe PRIVATE datachannel-static)
  nx_generate_nacp("${CMAKE_CURRENT_BINARY_DIR}/rtc-switch-probe.nacp"
    NAME "RTC Local Probe" AUTHOR "maxnemoy" VERSION "0.3.10")
  nx_create_nro(rtc-switch-probe NACP "${CMAKE_CURRENT_BINARY_DIR}/rtc-switch-probe.nacp")
else()
  add_executable(rtc-wakeup-check-test wakeup-check-test.cpp pipe-switch.c)
  target_compile_features(rtc-wakeup-check-test PRIVATE cxx_std_17)
  add_executable(rtc-numeric-layout-contract numeric-layout-contract.c)
  add_executable(rtc-numeric-contract numeric-contract.c)
  add_executable(rtc-pipe-contract pipe-contract.c)
  target_compile_options(rtc-pipe-contract PRIVATE -UNDEBUG)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  option(RTC_JOINABLE_CLEANUP_TEST "Exercise the Switch cleanup thread ownership on host" OFF)
  if(RTC_JOINABLE_CLEANUP_TEST)
    target_compile_definitions(datachannel-static PUBLIC RTC_TEST_JOINABLE_CLEANUP=1)
  endif()
  add_executable(rtc-cleanup-thread-test cleanup-thread-test.cpp)
  target_link_libraries(rtc-cleanup-thread-test PRIVATE datachannel-static dl pthread)
  target_link_options(rtc-cleanup-thread-test PRIVATE -Wl,--export-dynamic)
  add_executable(rtc-loopback-failure-test loopback-failure-test.cpp)
  target_link_libraries(rtc-loopback-failure-test PRIVATE datachannel-static)
  add_executable(rtc-init-rollback-test init-rollback-test.cpp)
  target_link_libraries(rtc-init-rollback-test PRIVATE datachannel-static dl pthread)
  target_include_directories(rtc-init-rollback-test PRIVATE
    "${RTC_ROOT}/third_party/rtc/libdatachannel/src"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/include/rtc"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/deps/plog/include")
  target_compile_definitions(rtc-init-rollback-test PRIVATE USE_MBEDTLS=1)
  target_link_options(rtc-init-rollback-test PRIVATE -Wl,--wrap=pipe -Wl,--export-dynamic)
endif()

# Direct production conn_poll.c regression, deterministic packet supply + real
# local socket errors. No RTC globals, external signaling or paid sessions.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(JUICE_SOURCE "${RTC_ROOT}/third_party/rtc/libdatachannel/deps/libjuice")
  add_executable(rtc-udp-budget-test udp-budget-test.c
    "${JUICE_SOURCE}/src/addr.c" "${JUICE_SOURCE}/src/log.c" "${JUICE_SOURCE}/src/timestamp.c")
  target_include_directories(rtc-udp-budget-test PRIVATE "${JUICE_SOURCE}/include/juice")
  target_compile_definitions(rtc-udp-budget-test PRIVATE _DEFAULT_SOURCE JUICE_STATIC)
  target_compile_options(rtc-udp-budget-test PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
  target_link_options(rtc-udp-budget-test PRIVATE -Wl,--gc-sections)
  target_link_libraries(rtc-udp-budget-test PRIVATE pthread)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(rtc-ice-diagnostics-test ice-diagnostics-test.cpp)
  target_compile_options(rtc-ice-diagnostics-test PRIVATE -UNDEBUG)
  target_link_libraries(rtc-ice-diagnostics-test PRIVATE datachannel-static)
  target_include_directories(rtc-ice-diagnostics-test PRIVATE
    "${RTC_ROOT}/third_party/rtc/libdatachannel/deps/libjuice/include")
  target_link_options(rtc-ice-diagnostics-test PRIVATE -Wl,--wrap=juice_get_diagnostics)
  add_executable(rtc-dtls-queue-test dtls-queue-test.cpp)
  target_include_directories(rtc-dtls-queue-test PRIVATE
    "${RTC_ROOT}/third_party/rtc/libdatachannel/src"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/include/rtc")
  target_compile_options(rtc-dtls-queue-test PRIVATE -UNDEBUG)
  target_link_libraries(rtc-dtls-queue-test PRIVATE datachannel-static)
  add_executable(rtc-dtls-diagnostics-test dtls-diagnostics-test.cpp)
  target_compile_options(rtc-dtls-diagnostics-test PRIVATE -UNDEBUG)
  target_link_libraries(rtc-dtls-diagnostics-test PRIVATE datachannel-static)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(rtc-dtls-receive-test dtls-receive-test.cpp)
  target_include_directories(rtc-dtls-receive-test PRIVATE
    "${RTC_ROOT}/third_party/rtc/libdatachannel/src"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/include/rtc"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/deps/plog/include"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/deps/libjuice/include"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/deps/usrsctp/usrsctplib"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/deps/libsrtp/include")
  target_compile_definitions(rtc-dtls-receive-test PRIVATE USE_MBEDTLS=1)
  target_compile_options(rtc-dtls-receive-test PRIVATE -UNDEBUG)
  target_link_libraries(rtc-dtls-receive-test PRIVATE datachannel-static mbedtls)
  add_executable(rtc-dtls-ingress-test dtls-ingress-test.cpp)
  target_include_directories(rtc-dtls-ingress-test PRIVATE
    "${RTC_ROOT}/third_party/rtc/libdatachannel/src"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/include/rtc"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/deps/plog/include"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/deps/libjuice/include"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/deps/usrsctp/usrsctplib"
    "${RTC_ROOT}/third_party/rtc/libdatachannel/deps/libsrtp/include")
  target_compile_definitions(rtc-dtls-ingress-test PRIVATE USE_MBEDTLS=1)
  target_compile_options(rtc-dtls-ingress-test PRIVATE -UNDEBUG)
  target_link_libraries(rtc-dtls-ingress-test PRIVATE datachannel-static mbedtls)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(rtc-media-send-diagnostics-test media-send-diagnostics-test.cpp)
  target_compile_options(rtc-media-send-diagnostics-test PRIVATE -UNDEBUG)
  target_link_libraries(rtc-media-send-diagnostics-test PRIVATE datachannel-static)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(rtc-rtcp-feedback-test rtcp-feedback-test.cpp)
  target_compile_options(rtc-rtcp-feedback-test PRIVATE -UNDEBUG)
  target_include_directories(rtc-rtcp-feedback-test PRIVATE "${RTC_ROOT}/src")
  target_link_libraries(rtc-rtcp-feedback-test PRIVATE datachannel-static)
endif()
