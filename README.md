# body-worn-camera — RV1126 智能执法记录仪

RV1126 平台执法记录仪:笔记本摄像头 → WSL(软解 NV12)→ 板端(RGA+MPP 硬编+NPU 检测)
→ RTSP/ONVIF 对外发布,配套完整录像可靠性链(预录/断电安全/打点/哈希链)与 Qt 管理客户端。

## 架构与数据流

```
摄像头(WSL) ──MJPEG──> ffmpeg 软解 NV12 ──TCP 8888──> 板端 RV1126
                                                        ├─ RGA 硬件搬运 → VENC 硬编 H.264 (15fps)
                                                        ├─ NPU YOLOv5s 5fps 人/车检测(独立线程)
                                                        │    └─ 超阈值自动打点(10s 冷却)
                                                        ├─ 录像: 30s 预录 + fMP4 断电安全 + 空间保护
                                                        ├─ 证据链: SHA-256 链式哈希(防删/插/换段)
                                                        ├─ RTSP 8554/live (live555) ──┐
                                                        ├─ ONVIF 8899+3702 (发现+SOAP) ├→ 客户端
                                                        └─ 控制 UDP 7777 (命令通道) ──┘
```

## 目录结构

```
board/       板端源码(h264_pipeline 主程序 + 检测/录像/RTSP/ONVIF/哈希链模块)
             build.sh 一键交叉编译,产物统一输出 build/
wslcamera/   WSL 推流端(mjpeg_nv12_sender: MJPEG 采集→软解 NV12→8888)
test/        video_push.py 视频文件推流器(YOLO 性能回归测试,realtime/full 双模式)
build/       编译产物(不入库;build.sh 生成)
docs/        设计文档
recordings/  本地录像测试产物(不入库)
```

## 板端模块一览(board/)

| 模块 | 职责 |
|---|---|
| h264_pipeline.cpp | 主程序:收流→RGA→VENC→广播,全部 UDP 命令,优雅退出(信号捕获) |
| yolo_detector / yolo_postprocess | NPU 检测线程(latest-wins 帧槽,5fps)+ 后处理(anchors/NMS) |
| recorder.{hpp,cpp} | fMP4 录像 + 30s 预录环形缓冲 + 事件打点(.meta) |
| hashchain.{hpp,cpp} | SHA-256 链式哈希(HASH_VERIFY 逐段校验) |
| rtsp_server.{hpp,cpp} | live555 RTSP 发布(新客户端清积压+等 IDR 起播) |
| onvif.{hpp,cpp} | ONVIF Profile S 设备端(WS-Discovery 3702 + SOAP 8899,零依赖手写) |
| mpp_h264_encoder / mpp_jpeg_decoder | MPP 硬编/硬解封装 |

## 构建 / 部署 / 运行

```bash
# 板端(WSL 交叉编译)
board/build.sh                                    # → build/h264_pipeline
adb push build/h264_pipeline /tmp/
adb shell "cp /tmp/h264_pipeline /oem/usr/bin/ && /etc/init.d/S50h264_pipeline restart"

# 运行顺序:① 板端(start) ② 推流端 ③ Qt 客户端连接
adb shell "/etc/init.d/S50h264_pipeline start|stop|status"   # 或前台直跑日志直出
wslcamera/mjpeg_nv12_sender                                  # Ctrl-C 优雅退出

# Qt 客户端(Windows,前后端分离)
# C:\Users\Mr.chen\Desktop\QtProgrmas\AudioVideo
# core/(DeviceClient:UDP 命令+检测统计) / video/(流媒体线程) / mainwindow(纯视图)
```

## 主要验证结论

- 端到端色度无损(testsrc2 彩条 112/119≈源);延迟粗测 ~177ms
- NPU 检测吞吐 9.9 det/s @ 推理 62ms,进程 CPU 2.5%,不挤垮 15fps 编码链
- fMP4 kill -9 后可播;填盘实测 <20MB 自动停录;篡改 1 字节 → 哈希链 TAMPERED
- RTSP 新客户端零积压起播(清队列+等 IDR);IDR ~103KB 需 OutPacketBuffer 300KB
- 单播 ProbeMatch + SOAP 四动作全过(组播受 ICS 网段限制,环境问题)

## 历史说明

第一代 PC 验证原型(libx264 软编 + middleware 平台无关层)已完成使命并清理,
其核心设计(环形预录/fMP4 分段/SHA-256)由 board/ 模块重写升级;git 历史可回溯。
