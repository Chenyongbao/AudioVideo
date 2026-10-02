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
