#!/bin/sh
# 板端一键编译:产物统一输出到 /home/mrchen/body-worn-camera/build/
# 用法: ./build.sh [detect_test]  (默认编 h264_pipeline;带参数编 NPU 冒烟)
# 目录: core/主程序  detect/检测  record/录像证据  net/协议  hw/硬件封装  tools/调试探针
set -e
SDK=/home/mrchen/rv1126_rv1109_linux
SYS=$SDK/buildroot/output/rockchip_rv1126_rv1109/host/arm-buildroot-linux-gnueabihf/sysroot
CROSS=$SDK/buildroot/output/rockchip_rv1126_rv1109/host/usr/bin/arm-linux-gnueabihf-g++
RKAPI=$SDK/external/rknpu/rknn/rknn_api/librknn_api
OUT=/home/mrchen/body-worn-camera/build
mkdir -p $OUT
cd $(dirname $0)

if [ "$1" = "detect_test" ]; then
  $CROSS -O2 --sysroot=$SYS -std=c++14 -I$RKAPI/include -o $OUT/detect_test \
      detect/detect_test.cpp \
      $SDK/external/rknpu/rknn/rknn_api/examples/rknn_yolov5_demo/src/postprocess.cc \
      -L$RKAPI/lib -lrknn_api -ldl -lm
  echo "build: $OUT/detect_test"
else
  $CROSS -O2 --sysroot=$SYS -std=c++14 -pthread \
      -I$SDK/external/mpp/inc -I$SYS/usr/include/rga -I$SYS/usr/include \
      -I$SYS/usr/include/liveMedia -I$SYS/usr/include/BasicUsageEnvironment \
      -I$SYS/usr/include/UsageEnvironment -I$SYS/usr/include/groupsock \
      -I$RKAPI/include \
      -Icore -Idetect -Irecord -Inet -Ihw \
      -o $OUT/h264_pipeline \
      core/h264_pipeline.cpp \
      record/recorder.cpp record/hashchain.cpp \
      net/rtsp_server.cpp net/onvif.cpp \
      detect/yolo_detector.cpp detect/yolo_postprocess.cc \
      hw/mpp_h264_encoder.cpp \
      -lrockchip_mpp -lrga -lliveMedia -lBasicUsageEnvironment -lUsageEnvironment \
      -lgroupsock -lavformat -lavcodec -lavutil -lrknn_api -lcrypto -lpthread -lm
  echo "build: $OUT/h264_pipeline"
fi
