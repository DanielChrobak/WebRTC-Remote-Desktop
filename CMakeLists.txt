# WebRTC Remote Desktop - CMake Build Configuration
# Copyright 2025-2026 Daniel Chrobak

cmake_minimum_required(VERSION 3.20)
project(WebRTCRemoteDesktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

# MSVC Optimization Flags
if(MSVC)
    string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /GL /GS- /fp:fast")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG /OPT:REF /OPT:ICF")
endif()

# Required Dependencies
find_package(LibDataChannel REQUIRED)
find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Opus REQUIRED)
find_package(OpenSSL REQUIRED)

find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_path(AVUTIL_INCLUDE_DIR libavutil/avutil.h)
find_library(AVUTIL_LIBRARY avutil)

# Source Files
set(HEADERS
    hpp/common.hpp
    hpp/capture.hpp
    hpp/encoder.hpp
    hpp/webrtc.hpp
    hpp/audio.hpp
    hpp/input.hpp
)

set(SOURCES main.cpp)

# Web Client Files
set(WEB_FILES
    index.html
    styles.css
)

set(JS_FILES
    js/input.js
    js/media.js
    js/network.js
    js/renderer.js
    js/state.js
    js/ui.js
)

# Executable Target
add_executable(WebRTCRemoteDesktop ${SOURCES} ${HEADERS})

# Enable OpenSSL support for cpp-httplib
target_compile_definitions(WebRTCRemoteDesktop PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(WebRTCRemoteDesktop PRIVATE
    ${AVCODEC_INCLUDE_DIR}
    ${AVUTIL_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/hpp
)

target_link_libraries(WebRTCRemoteDesktop PRIVATE
    LibDataChannel::LibDataChannel
    httplib::httplib
    nlohmann_json::nlohmann_json
    Opus::opus
    OpenSSL::SSL
    OpenSSL::Crypto
    ${AVCODEC_LIBRARY}
    ${AVUTIL_LIBRARY}
)

# Windows-specific Configuration
if(WIN32)
    target_link_libraries(WebRTCRemoteDesktop PRIVATE
        ws2_32
        d3d11
        dxgi
        dxguid
        ole32
        windowsapp
    )

    target_compile_options(WebRTCRemoteDesktop PRIVATE
        /await:strict
        /EHsc
        /W4
        /wd4100
        /wd4189
    )

    target_compile_definitions(WebRTCRemoteDesktop PRIVATE
        WINRT_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
        _CRT_SECURE_NO_WARNINGS
    )

    target_include_directories(WebRTCRemoteDesktop PRIVATE
        "$ENV{WindowsSdkDir}Include/$ENV{WindowsSDKVersion}cppwinrt"
    )
endif()

# Output Directory
set_target_properties(WebRTCRemoteDesktop PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/Debug
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/Release
)

# Copy Web Files to Output Directory
foreach(WEB_FILE ${WEB_FILES})
    add_custom_command(TARGET WebRTCRemoteDesktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/${WEB_FILE}
            $<TARGET_FILE_DIR:WebRTCRemoteDesktop>/${WEB_FILE}
    )
endforeach()

add_custom_command(TARGET WebRTCRemoteDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:WebRTCRemoteDesktop>/js
)

foreach(JS_FILE ${JS_FILES})
    add_custom_command(TARGET WebRTCRemoteDesktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/${JS_FILE}
            $<TARGET_FILE_DIR:WebRTCRemoteDesktop>/${JS_FILE}
    )
endforeach()

foreach(CONFIG_FILE ${CONFIG_FILES})
    add_custom_command(TARGET WebRTCRemoteDesktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/${CONFIG_FILE}
            $<TARGET_FILE_DIR:WebRTCRemoteDesktop>/${CONFIG_FILE}
    )
endforeach()

# Installation Configuration
install(TARGETS WebRTCRemoteDesktop RUNTIME DESTINATION .)
install(FILES ${CMAKE_SOURCE_DIR}/index.html DESTINATION .)
install(FILES ${CMAKE_SOURCE_DIR}/styles.css DESTINATION .)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/js DESTINATION .)

# Install Runtime DLLs
install(DIRECTORY ${CMAKE_BINARY_DIR}/bin/Release/
    DESTINATION .
    FILES_MATCHING PATTERN "*.dll"
)

# CPack Configuration
set(CPACK_PACKAGE_NAME "WebRTCRemoteDesktop")
set(CPACK_PACKAGE_VENDOR "WebRTC Remote Desktop")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Low-latency WebRTC remote desktop with AV1 encoding")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_VERSION_MAJOR "1")
set(CPACK_PACKAGE_VERSION_MINOR "0")
set(CPACK_PACKAGE_VERSION_PATCH "0")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "WebRTCRemoteDesktop")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE.txt")

# NSIS Installer Settings
set(CPACK_NSIS_DISPLAY_NAME "WebRTC Remote Desktop")
set(CPACK_NSIS_PACKAGE_NAME "WebRTC Remote Desktop")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

if(EXISTS "${CMAKE_SOURCE_DIR}/icon.ico")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/icon.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/icon.ico")
endif()

set(CPACK_NSIS_CREATE_ICONS_EXTRA
    "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\WebRTC Remote Desktop.lnk' '$INSTDIR\\\\WebRTCRemoteDesktop.exe'"
)

set(CPACK_NSIS_DELETE_ICONS_EXTRA
    "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\WebRTC Remote Desktop.lnk'"
)

set(CPACK_GENERATOR "ZIP")

include(CPack)
