cmake_minimum_required(VERSION 3.16)
project(FogPlayMoonlight LANGUAGES C CXX)
# devkitA64 binutils defaults to archive timestamps/UIDs; keep output stable.
foreach(language C CXX)
  set(CMAKE_${language}_ARCHIVE_CREATE "<CMAKE_AR> qcD <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_${language}_ARCHIVE_APPEND "<CMAKE_AR> qD <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_${language}_ARCHIVE_FINISH "<CMAKE_RANLIB> -D <TARGET>")
endforeach()
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(UPSTREAM "${ROOT}/third_party/moonlight/moonlight-switch")
set(PREPARED "${ROOT}/third_party/moonlight/prepared")
set(PRIVATE_PREFIX "${ROOT}/third_party/moonlight/switch")
set(RTC_PREFIX "${ROOT}/third_party/rtc/switch")
if(NOT EXISTS "${RTC_PREFIX}/lib/libmbedcrypto.a")
  message(FATAL_ERROR "Build the app's RTC dependencies first; Moonlight must share its exact mbedTLS ABI")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CURL_SYMBOL_MAP "${PRIVATE_PREFIX}/curl-symbols.map")
if(NOT EXISTS "${CURL_SYMBOL_MAP}")
  message(FATAL_ERROR "Build private curl with tools/moonlight/build-curl.sh first")
endif()
set(CMAKE_SKIP_INSTALL_RULES ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(ENET_NO_INSTALL ON CACHE BOOL "" FORCE)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  message(FATAL_ERROR "This dependency build targets Nintendo Switch")
endif()
add_compile_options(-fPIE -ffunction-sections -fdata-sections)
add_compile_definitions(MBEDTLS_USER_CONFIG_FILE="${ROOT}/tools/rtc/mbedtls-user-config.h")
include_directories(BEFORE "${RTC_PREFIX}/include")
set(MOONLIGHT_TLS_LIBRARIES
  "${RTC_PREFIX}/lib/libmbedtls.a" "${RTC_PREFIX}/lib/libmbedx509.a"
  "${RTC_PREFIX}/lib/libmbedcrypto.a" "${RTC_PREFIX}/lib/libeverest.a" "${RTC_PREFIX}/lib/libp256m.a")
add_library(moonlight-curl STATIC IMPORTED GLOBAL)
set_target_properties(moonlight-curl PROPERTIES
  IMPORTED_LOCATION "${PRIVATE_PREFIX}/lib/libmoonlight-curl.a"
  INTERFACE_INCLUDE_DIRECTORIES "${PRIVATE_PREFIX}/include"
  INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB
  INTERFACE_LINK_LIBRARIES "${MOONLIGHT_TLS_LIBRARIES}")
add_library(MbedTLS::mbedcrypto INTERFACE IMPORTED GLOBAL)
set_target_properties(MbedTLS::mbedcrypto PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${RTC_PREFIX}/include"
  INTERFACE_LINK_LIBRARIES "${MOONLIGHT_TLS_LIBRARIES}")
add_subdirectory("${UPSTREAM}/extern/moonlight-common-c" moonlight-common-c)
# Replace only explicitly prepared translation units; leave the pinned checkout untouched.
get_target_property(MOONLIGHT_SOURCES moonlight-common-c SOURCES)
set(PREPARED_CORE_SOURCES Connection VideoStream RtpVideoQueue VideoDepacketizer ControlStream LinkedBlockingQueue PlatformSockets)
foreach(name IN LISTS PREPARED_CORE_SOURCES)
  list(LENGTH MOONLIGHT_SOURCES MOONLIGHT_SOURCE_COUNT)
  list(FILTER MOONLIGHT_SOURCES EXCLUDE REGEX "(^|/)${name}\\.c$")
  list(LENGTH MOONLIGHT_SOURCES MOONLIGHT_FILTERED_COUNT)
  math(EXPR EXPECTED_COUNT "${MOONLIGHT_SOURCE_COUNT} - 1")
  if(NOT MOONLIGHT_FILTERED_COUNT EQUAL EXPECTED_COUNT)
    message(FATAL_ERROR "Expected exactly one upstream ${name}.c source")
  endif()
endforeach()
set_property(TARGET moonlight-common-c PROPERTY SOURCES "${MOONLIGHT_SOURCES}")
foreach(name IN LISTS PREPARED_CORE_SOURCES)
  target_sources(moonlight-common-c PRIVATE "${PREPARED}/core/${name}.c")
endforeach()
target_sources(moonlight-common-c PRIVATE "${CMAKE_CURRENT_LIST_DIR}/FogMoonlightDiagnostics.c"
  "${CMAKE_CURRENT_LIST_DIR}/FogMoonlightRecovery.c")
target_include_directories(moonlight-common-c PRIVATE
  "${UPSTREAM}/extern/moonlight-common-c/src" "${CMAKE_CURRENT_LIST_DIR}")
target_sources(moonlight-common-c PRIVATE "${CMAKE_CURRENT_LIST_DIR}/signal-switch.c")
add_library(gamestream STATIC
  "${PREPARED}/libgamestream/client.cpp"
  "${PREPARED}/libgamestream/http.cpp"
  "${PREPARED}/libgamestream/xml.cpp"
  "${PREPARED}/crypto/Data.cpp"
  "${PREPARED}/crypto/MbedTLSCryptoManager.cpp")
# Keep the private curl headers ahead of the old portlibs curl headers.
# Imported includes otherwise become -isystem and lose to portlibs' -I.
set_target_properties(gamestream PROPERTIES NO_SYSTEM_FROM_IMPORTED TRUE)
target_compile_features(gamestream PUBLIC cxx_std_17)
target_compile_definitions(gamestream PUBLIC USE_MBEDTLS_CRYPTO=1)
target_include_directories(gamestream PUBLIC
  "${PREPARED}/libgamestream" "${PREPARED}/crypto"
  "${CMAKE_CURRENT_LIST_DIR}/compat"
  "${PRIVATE_PREFIX}/include"
  "$ENV{DEVKITPRO}/portlibs/switch/include")
target_link_libraries(gamestream PUBLIC moonlight-common-c moonlight-curl expat ${MOONLIGHT_TLS_LIBRARIES} z)
# Rename references in the archive, leaving public headers and app curl untouched.
# Track the map/tool as compile dependencies so cached consumer archives are rebuilt.
get_target_property(GAMESTREAM_SOURCES gamestream SOURCES)
set_property(SOURCE ${GAMESTREAM_SOURCES} APPEND PROPERTY OBJECT_DEPENDS
  "${CURL_SYMBOL_MAP};${CMAKE_CURRENT_LIST_DIR}/namespace-curl.py")
add_custom_command(TARGET gamestream POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/namespace-curl.py"
    "$<TARGET_FILE:gamestream>" --map "${CURL_SYMBOL_MAP}"
    --nm "${CMAKE_NM}" --objcopy "${CMAKE_OBJCOPY}" --ranlib "${CMAKE_RANLIB}"
  VERBATIM)
add_executable(moonlight-link-smoke link-smoke.cpp)
target_include_directories(moonlight-link-smoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(moonlight-link-smoke PRIVATE gamestream)
# Pull in both implementations and the app's cookie API in a complete Switch link.
add_executable(moonlight-curl-coexist-smoke link-smoke.cpp curl-coexist-smoke.c)
target_include_directories(moonlight-curl-coexist-smoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_directories(moonlight-curl-coexist-smoke BEFORE PRIVATE
  "${ROOT}/third_party/ffmpeg-diag/switch/lib" "${RTC_PREFIX}/lib"
  "$ENV{DEVKITPRO}/portlibs/switch/lib" "$ENV{DEVKITPRO}/libnx/lib" "${PRIVATE_PREFIX}/lib")
target_compile_definitions(moonlight-curl-coexist-smoke PRIVATE MOONLIGHT_CURL_COEXIST_SMOKE)
target_include_directories(moonlight-curl-coexist-smoke BEFORE PRIVATE
  "$ENV{DEVKITPRO}/portlibs/switch/include")
target_link_libraries(moonlight-curl-coexist-smoke PRIVATE gamestream curl)
