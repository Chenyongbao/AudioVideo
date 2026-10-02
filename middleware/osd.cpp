#include "middleware/osd.hpp"
#include <ctime>
#include <cstdio>
#include <unistd.h>  // access

// 字体路径集中定义（板端无此文件则 init 显式失败，不静默黑屏）
static const char* kFontPath = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

OsdOverlay::~OsdOverlay() {
    if (graph_) avfilter_graph_free(&graph_);
    if (sws_to_yuv_) sws_freeContext(sws_to_yuv_);
    if (sws_from_) sws_freeContext(sws_from_);
    if (filter_frame_) av_frame_free(&filter_frame_);
}

bool OsdOverlay::init(int width, int height, int fps, const std::string& device_id) {
    w_ = width; h_ = height; fps_ = fps; device_id_ = device_id;
    // 字体存在性检查：drawtext 对缺失 fontfile 会静默失败（帧上无字且无报错），
    // init 时显式暴露（板端换 RGA 叠加，无此依赖）
    if (access(kFontPath, F_OK) != 0) {
        fprintf(stderr, "[osd] font missing: %s（OSD 将不可用）\n", kFontPath);
        return false;
    }
    sws_to_yuv_ = sws_getContext(w_, h_, AV_PIX_FMT_NV12,
                                 w_, h_, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_to_yuv_) return false;
    filter_frame_ = av_frame_alloc();
    return sws_to_yuv_ && filter_frame_;
}

bool OsdOverlay::rebuildGraph(const std::string& text) {
    if (graph_) avfilter_graph_free(&graph_);
    graph_ = avfilter_graph_alloc();
    if (!graph_) return false;

    char args[512];
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/%d:pixel_aspect=1/1",
             w_, h_, AV_PIX_FMT_YUV420P, fps_);
    const AVFilter* bufsrc = avfilter_get_by_name("buffer");
    const AVFilter* bufsink = avfilter_get_by_name("buffersink");
    if (avfilter_graph_create_filter(&src_ctx_, bufsrc, "in", args, nullptr, graph_) < 0)
        return false;
    if (avfilter_graph_create_filter(&sink_ctx_, bufsink, "out", nullptr, nullptr, graph_) < 0)
        return false;

    // 两个独立 drawtext 实例串联：第一行时间戳 + 第二行设备编号
    // （avfilter_graph_create_filter 只接受单个 filter 参数，不能传整条链）
    const AVFilter* dt = avfilter_get_by_name("drawtext");
    char desc1[512], desc2[512];
    snprintf(desc1, sizeof(desc1),
             "fontfile=%s:"             // PC 端 drawtext 软画（板端换 RGA 通道叠加，
             "text='%s':fontcolor=white:fontsize=24:x=12:y=12:shadowx=2:shadowy=2",  // 无逐秒重建开销）
             kFontPath, text.c_str());
    snprintf(desc2, sizeof(desc2),
             "fontfile=%s:"
             "text='ID:%s':fontcolor=white:fontsize=20:x=12:y=44:shadowx=2:shadowy=2",
             kFontPath, device_id_.c_str());
    AVFilterContext* dt1 = nullptr;
    AVFilterContext* dt2 = nullptr;
    if (avfilter_graph_create_filter(&dt1, dt, "dt1", desc1, nullptr, graph_) < 0)
        return false;
    if (avfilter_graph_create_filter(&dt2, dt, "dt2", desc2, nullptr, graph_) < 0)
        return false;
    if (avfilter_link(src_ctx_, 0, dt1, 0) < 0) return false;
    if (avfilter_link(dt1, 0, dt2, 0) < 0) return false;
    if (avfilter_link(dt2, 0, sink_ctx_, 0) < 0) return false;
    if (avfilter_graph_config(graph_, nullptr) < 0) return false;
    return true;
}

bool OsdOverlay::apply(VideoFrame& frame, int64_t wall_secs, bool time_trusted) {
    // 时间戳每秒刷新（首帧 last_text_sec_=-1 必建图）：重建 filter 图
    // （每秒一次，开销可忽略；板端换 RGA 无此开销）
    if (wall_secs != last_text_sec_) {
        char buf[64];
        time_t t = (time_t)wall_secs;
        struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
        // 不可信时间加 * 前缀（6.4.17：时间未校准时警示证据效力）
        std::string text = time_trusted ? buf : std::string("*") + buf;
        if (!rebuildGraph(text)) {
            static int err_printed = 0;
            if (err_printed++ < 3)
                fprintf(stderr, "[osd] rebuildGraph failed (drawtext desc parse?)\n");
            return false;
        }
        last_text_sec_ = wall_secs;
        text_cache_ = text;
    }

    // NV12 -> YUV420P（sws 就地展开为三平面）
    AVFrame* in = av_frame_alloc();
    in->format = AV_PIX_FMT_NV12;
    in->width = w_; in->height = h_;
    if (av_image_fill_arrays(in->data, in->linesize, frame.data.data(),
                             AV_PIX_FMT_NV12, w_, h_, 1) < 0) { av_frame_free(&in); return false; }
    uint8_t* yuv_data = nullptr;
    int yuv_size = av_image_alloc(&yuv_data, in->linesize, w_, h_, AV_PIX_FMT_YUV420P, 1);
    if (yuv_size < 0) { av_frame_free(&in); return false; }
    // sws_scale 需要 NV12 输入平面指针数组（Y 平面 + 交错 UV 平面）
    const uint8_t* nv12_planes[2] = { frame.data.data(), frame.data.data() + (size_t)w_ * h_ };
    const int nv12_ls[2] = { w_, w_ };
    // 目标同样是平面指针数组（Y/U/V 各自偏移）
    uint8_t* dst_planes[3] = { yuv_data, yuv_data + (size_t)w_ * h_, yuv_data + (size_t)w_ * h_ * 5 / 4 };
    sws_scale(sws_to_yuv_, nv12_planes, nv12_ls, 0, h_, dst_planes, in->linesize);

    // 送入 filter 烧帧（数据已是 YUV420P 三平面，format 必须同步改，否则按 NV12
    // 双平面布局拷贝会触发 imgutils 断言）
    in->format = AV_PIX_FMT_YUV420P;
    in->data[0] = dst_planes[0];
    in->data[1] = dst_planes[1];
    in->data[2] = dst_planes[2];
    if (av_buffersrc_add_frame(src_ctx_, in) < 0) {
        static int e1 = 0; if (e1++ < 3) fprintf(stderr, "[osd] buffersrc_add_frame failed\n");
        av_freep(&yuv_data); av_frame_free(&in); return false;
    }
    int ret = av_buffersink_get_frame(sink_ctx_, filter_frame_);
    av_frame_free(&in);
    if (ret < 0) {
        static int e2 = 0; if (e2++ < 3) fprintf(stderr, "[osd] buffersink_get_frame failed (%d)\n", ret);
        av_freep(&yuv_data); return false;
    }

    // filter 输出可能有 padding：拷贝成紧凑 YUV420P 三平面
    frame.data.resize((size_t)w_ * h_ * 3 / 2);
    uint8_t* dst = frame.data.data();
    uint8_t* dY = dst;
    uint8_t* dU = dst + (size_t)w_ * h_;
    uint8_t* dV = dU + (size_t)w_ * h_ / 4;
    for (int r = 0; r < h_; ++r)
        memcpy(dY + (size_t)r * w_, filter_frame_->data[0] + (size_t)r * filter_frame_->linesize[0], w_);
    for (int r = 0; r < h_ / 2; ++r) {
        memcpy(dU + (size_t)r * w_ / 2, filter_frame_->data[1] + (size_t)r * filter_frame_->linesize[1], w_ / 2);
        memcpy(dV + (size_t)r * w_ / 2, filter_frame_->data[2] + (size_t)r * filter_frame_->linesize[2], w_ / 2);
    }
    av_frame_unref(filter_frame_);
    av_freep(&yuv_data);
    return true;
}
