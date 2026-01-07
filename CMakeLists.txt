cmake_minimum_required(VERSION 3.20)
project(ScreenShare LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

if(MSVC)
    string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /GL /GS- /fp:fast")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG /OPT:REF /OPT:ICF")
endif()

find_package(LibDataChannel REQUIRED)
find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Opus REQUIRED)
find_package(OpenSSL REQUIRED)
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_path(AVUTIL_INCLUDE_DIR libavutil/avutil.h)
find_library(AVUTIL_LIBRARY avutil)

set(HEADERS
    hpp/common.hpp
    hpp/capture.hpp
    hpp/encoder.hpp
    hpp/webrtc.hpp
    hpp/audio.hpp
    hpp/input.hpp
)

set(SOURCES main.cpp)

set(WEB_FILES index.html styles.css)

set(JS_FILES
    js/input.js
    js/media.js
    js/network.js
    js/renderer.js
    js/state.js
    js/ui.js
)

add_executable(ScreenShare ${SOURCES} ${HEADERS})

# Enable OpenSSL support for cpp-httplib
target_compile_definitions(ScreenShare PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(ScreenShare PRIVATE
    ${AVCODEC_INCLUDE_DIR}
    ${AVUTIL_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/hpp
)

target_link_libraries(ScreenShare PRIVATE
    LibDataChannel::LibDataChannel
    httplib::httplib
    nlohmann_json::nlohmann_json
    Opus::opus
    OpenSSL::SSL
    OpenSSL::Crypto
    ${AVCODEC_LIBRARY}
    ${AVUTIL_LIBRARY}
)

if(WIN32)
    target_link_libraries(ScreenShare PRIVATE ws2_32 d3d11 dxgi dxguid ole32 windowsapp)
    target_compile_options(ScreenShare PRIVATE /await:strict /EHsc /W4 /wd4100 /wd4189)
    target_compile_definitions(ScreenShare PRIVATE WINRT_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
    target_include_directories(ScreenShare PRIVATE "$ENV{WindowsSdkDir}Include/$ENV{WindowsSDKVersion}cppwinrt")
endif()

set_target_properties(ScreenShare PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/Debug
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/Release
)

foreach(WEB_FILE ${WEB_FILES})
    add_custom_command(TARGET ScreenShare POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/${WEB_FILE}
            $<TARGET_FILE_DIR:ScreenShare>/${WEB_FILE}
    )
endforeach()

add_custom_command(TARGET ScreenShare POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:ScreenShare>/js
)

foreach(JS_FILE ${JS_FILES})
    add_custom_command(TARGET ScreenShare POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/${JS_FILE}
            $<TARGET_FILE_DIR:ScreenShare>/${JS_FILE}
    )
endforeach()
