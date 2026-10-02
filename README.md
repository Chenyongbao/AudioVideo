# body-worn-camera — 对标 GA/T 947.2 的执法记录仪音视频子系统

RV1126 平台执法记录仪音视频子系统的开发/验证项目。当前阶段在 WSL2 Ubuntu 下用
笔记本 USB 摄像头跑通全链路（采集 → H.264 编码 → fragmented MP4 分段 → SHA-256
manifest），核心中间件平台无关，之后逐模块移植到 RV1126（RKMPI/MPP）。

## 目录结构

```
platform/    平台抽象层：IVideoSource(V4L2) / IAudioSource(ALSA) / 统一毫秒时钟
             移植时新增 video_rkmpi.cpp(=RKMPI VI) 、audio_rkmpi.cpp(=AI/G711A)
encoder/     H.264 编码封装（PC: libx264；板端: MPP VENC，接口 IVideoEncoder 不变）
muxer/       fragmented MP4 分段写文件器（掉电安全：每 fragment 自包含）
middleware/  核心中间件（平台无关，直接移植）：
             EncodedFrameRing   GOP 对齐的编码帧环形预录缓冲（30s 仅 ~15MB）
             SegmentManager     分段录像管理（帧边界原子切换，间隙 <1 帧）
             Recorder           录制状态机 IDLE/RECORDING（预录回填入口）
             integrity          每段落盘后 SHA-256 → manifest.sha256
main.cpp     bwc_demo 演示程序
```

## 构建（WSL Ubuntu）

```bash
cmake -B build -S . && cmake --build build -j$(nproc)
```

依赖：`libavcodec/libavformat/libavutil/libswscale-dev`、`libasound2-dev`、
`libssl-dev`、`pkg-config`（本机均已具备）。

## USB 摄像头透传（Windows → WSL2）

WSL2 默认看不到 USB 设备，需在 **Windows PowerShell(管理员)** 执行：

```powershell
winget install usbipd
usbipd list                          # 找到摄像头的 BUSID
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

回到 WSL 验证：`ls /dev/video*` → 出现 `/dev/video0` 后运行：

```bash
./build/bwc_demo ./recordings
# 回车 = 触发录像（状态机 IDLE→RECORDING，含预录回填入口）
# q+回车 = 停止
```

输出：`recordings/seg_<ts>.mp4`（10s 一段，fragmented MP4，随时拔电已落盘部分可播）
+ `manifest.sha256`（每段 SHA-256 清单）。可用 `ffprobe` 检查分段连续性。

## 运行验证点（对应标准指标）

| 验证 | 方法 |
|---|---|
| 分段无缝 ≤0.04s | 连续录 1h，逐段 ffprobe 检查帧 PTS 连续性 |
| 掉电安全 | 录像中 kill -9 / 拔 USB，已落盘分段全部可播（fMP4 无 moov 依赖） |
| 预录回放 | 触发后首段包含触发前 30s 画面（EncodedFrameRing 回填，GOP 对齐） |
| 码率实测 | 1080p25 CBR 4~5.8Mbps，1h ≈2.5GB ≤2.6GB |

## RV1126 移植路径

1. `platform/video_v4l2.cpp` → `video_rkmpi.cpp`：RKMPI VI→VENC 双通道
   （主码流录像 + 子码流预览），VI 直接出 NV12，无需软件转换。
2. `encoder/h264_encoder.cpp` → MPP VENC（CBR + GOP/ROI 码控，对标 1080p25≈5.8Mbps）。
3. `platform/audio_alsa.cpp` → RKMPI AI（G.711A）；音频为主时钟，视频 PTS 向音频对齐。
4. OSD：RGA/VE 通道叠加时间戳/编号，编码前烧帧（不可分离）。
5. 板端交叉编译不进本项目主构建，小代码示例用 SDK 工具链单独编译后 adb 推送验证。
