cmake_minimum_required(VERSION 3.16)
project(body-worn-camera CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale)
# ALSA 仅 Linux 平台需要
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
endif()

include_directories(${CMAKE_SOURCE_DIR})

# 平台层（PC 版：V4L2 + ALSA；移植时替换为 rkmpi 实现）
add_library(platform STATIC
  platform/video_v4l2.cpp
  platform/audio_alsa.cpp
  platform/clock.cpp
)
target_include_directories(platform PUBLIC ${CMAKE_SOURCE_DIR})
# video_v4l2 的 MJPG 模式用 libavcodec 解码回 NV12
target_link_libraries(platform PUBLIC PkgConfig::AV)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(platform PUBLIC PkgConfig::ALSA)
endif()

# 编码/封装层（libavcodec 编码 + fragmented MP4 分段封装）
add_library(avcodec_wrap STATIC
  encoder/h264_encoder.cpp
  encoder/aac_encoder.cpp
  muxer/fmp4_segment_writer.cpp
)
target_link_libraries(avcodec_wrap PUBLIC PkgConfig::AV platform)

# 核心中间件：环形预录缓冲 / 分段管理 / 录制状态机 / SHA256 manifest
add_library(middleware STATIC
  middleware/ring_buffer.cpp
  middleware/recorder.cpp
  middleware/integrity.cpp
)
target_link_libraries(middleware PUBLIC avcodec_wrap platform)

# SHA-256 依赖 OpenSSL
find_package(OpenSSL REQUIRED)
target_link_libraries(middleware PUBLIC OpenSSL::Crypto)

# 演示主程序：USB 摄像头 -> H.264 -> 分段 fMP4 + 预录缓冲
add_executable(bwc_demo main.cpp)
target_link_libraries(bwc_demo PRIVATE middleware pthread)

# AAC 双流封装单测（不依赖摄像头/声卡）
add_executable(aac_test apps/aac_test.cpp)
target_link_libraries(aac_test PRIVATE middleware OpenSSL::Crypto pthread)
