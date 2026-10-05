cmake_minimum_required(VERSION 3.16)

project(AudioVideo VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

# ==================== FFmpeg SDK (Windows shared dev 包) ====================
# SDK 根目录（含 include/ 与 lib/），换机器只改这一处：
#   cmake -B build -S . -DFFMPEG_DIR="D:/其它路径/ffmpeg-x.y-full_build-shared"
set(FFMPEG_DIR "D:/FFmpeg/ffmpeg-9.0.2-full_build-shared" CACHE PATH "FFmpeg SDK root")

find_path(FFMPEG_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    PATHS ${FFMPEG_DIR}/include
    NO_DEFAULT_PATH)

find_library(AVCODEC_LIBRARY  NAMES avcodec  PATHS ${FFMPEG_DIR}/lib NO_DEFAULT_PATH)
find_library(AVFORMAT_LIBRARY NAMES avformat PATHS ${FFMPEG_DIR}/lib NO_DEFAULT_PATH)
find_library(AVUTIL_LIBRARY   NAMES avutil   PATHS ${FFMPEG_DIR}/lib NO_DEFAULT_PATH)
find_library(SWSCALE_LIBRARY  NAMES swscale  PATHS ${FFMPEG_DIR}/lib NO_DEFAULT_PATH)

if(NOT FFMPEG_INCLUDE_DIR OR NOT AVCODEC_LIBRARY OR NOT AVFORMAT_LIBRARY
   OR NOT AVUTIL_LIBRARY OR NOT SWSCALE_LIBRARY)
    message(FATAL_ERROR
        "FFmpeg SDK not found in ${FFMPEG_DIR}\n"
        "需要 full_build-shared 版（含 include/ 头文件与 lib/ 导入库），\n"
        "可用 -DFFMPEG_DIR=<路径> 指定其它位置。")
endif()

message(STATUS "FFmpeg SDK : ${FFMPEG_DIR}")
message(STATUS "avcodec lib: ${AVCODEC_LIBRARY}")

# 接口库：把 include 路径与 4 个导入库打包给目标使用
add_library(ffmpeg-libs INTERFACE)
target_include_directories(ffmpeg-libs INTERFACE ${FFMPEG_INCLUDE_DIR})
target_link_libraries(ffmpeg-libs INTERFACE
    ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}  ${SWSCALE_LIBRARY})
# ===========================================================================

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
        video/videowidget.cpp
        video/videowidget.h
        video/streamworker.cpp
        video/streamworker.h
        video/timeline.cpp
        video/timeline.h
        video/playbackworker.cpp
        video/playbackworker.h
        video/yolotestworker.cpp
        video/yolotestworker.h
        core/deviceclient.cpp
        core/deviceclient.h
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(AudioVideo
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
else()
    if(ANDROID)
        add_library(AudioVideo SHARED
            ${PROJECT_SOURCES}
        )
    else()
        add_executable(AudioVideo
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(AudioVideo PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    ffmpeg-libs)

# 运行时 DLL 自动拷贝（bin/*.dll -> 可执行文件输出目录），免手动
# （必须放在 add_executable(AudioVideo ...) 之后，TARGET 才存在）
file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
if(FFMPEG_DLLS)
    add_custom_command(TARGET AudioVideo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_DLLS} $<TARGET_FILE_DIR:AudioVideo>
        COMMENT "Copying FFmpeg DLLs to output directory")
endif()

# 冒烟测试：链接检查（FFmpeg 头文件可见性 + 导入库符号）
target_compile_definitions(AudioVideo PRIVATE
    FFMPEG_SDK_DIR="${FFMPEG_DIR}")

set_target_properties(AudioVideo PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS AudioVideo
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(AudioVideo)
endif()
