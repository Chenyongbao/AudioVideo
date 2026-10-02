// ============================================================================
// bwc_demo —— 执法记录仪音视频子系统 PC 演示主程序。
// ----------------------------------------------------------------------------
// 【交互方式】终端命令驱动状态机（板端对应实体按键/屏幕事件）：
//   回车   = triggerRecord()：IDLE → RECORDING（回填预录）；
//            延录期再按 = 重置延录窗口回 RECORDING
//   s      = triggerStop()：RECORDING → STOPPING_PENDING（延录 5s，标准场景 25min）
//   m      = triggerMark()：当前分段写入重点标记（manifest 加 MARK 行）
//   q      = stop()：立即停止并收尾当前段
// 【配置要点】分辨率/码率/预录/分段/延录/配额全部在 Config 中，
//   板端对标值：1080p25、5.8Mbps、预录 30s、延录 25min（见 README 验证表）。
// 【运行输出】output_dir 下：seg_<单调ms>.mp4 分段 + manifest.sha256 证据账本
//   + index.json 检索索引（用 build_index 或手动生成）。
// ============================================================================
// bwc_demo：执法记录仪音视频子系统 PC 演示
// 用法：./bwc_demo [输出目录] ；运行中按回车模拟"开始录像"触发，q+回车退出
#include "middleware/recorder.hpp"
#include <cstdio>
#include <csignal>
#include <iostream>

int main(int argc, char** argv) {
    signal(SIGINT, [](int) { std::cerr << "\ninterrupted\n"; _exit(0); });
    auto stateName = [](Recorder::State s) {
        switch (s) {
            case Recorder::State::Idle: return "IDLE";
            case Recorder::State::Recording: return "RECORDING";
            case Recorder::State::StoppingPending: return "STOPPING_PENDING(延录中)";
        }
        return "?";
    };

    Recorder::Config cfg;
    if (argc > 1) cfg.output_dir = argv[1];
    // WSL USB 摄像头实测能力：YUYV 640x480@30 可用（720p YUYV 仅 10fps，故用 480p）
    cfg.width = 640; cfg.height = 480; cfg.fps = 30;
    cfg.bitrate_kbps = 4000;
    cfg.prerecord_ms = 30 * 1000;
    cfg.segment_ms = 10 * 1000;   // demo 用 10s 一段便于快速验证
    cfg.post_record_ms = 5 * 1000; // demo 延录 5s 便于验证（标准场景 25min）

    Recorder rec;
    if (!rec.start(cfg)) {
        fprintf(stderr, "启动失败：检查 /dev/video0 是否存在（WSL 需 usbipd 透传 USB 摄像头）\n");
        return 1;
    }
    fprintf(stderr, "录制管线已启动（未触发录像）-> %s\n回车=触发录像  s=停止触发(延录%dss)  q=退出\n",
            cfg.output_dir.c_str(), (int)(cfg.post_record_ms / 1000));

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "q" || line == "quit") break;
        if (line == "s") { rec.triggerStop(); fprintf(stderr, "[state] -> %s\n", stateName(rec.state())); continue; }
        if (line == "m" || line == "mark") { rec.triggerMark(); fprintf(stderr, "[mark] 重点标记已写入 manifest\n"); continue; }
        rec.triggerRecord();
        fprintf(stderr, "[state] -> %s\n", stateName(rec.state()));
    }
    rec.stop();
    fprintf(stderr, "已停止。检查 %s/ 下分段 mp4 与 manifest.sha256\n", cfg.output_dir.c_str());
    return 0;
}
