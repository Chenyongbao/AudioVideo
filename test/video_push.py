#!/usr/bin/env python3
# ============================================================================
# 视频文件推流器:video → NV12 → TCP 8888(板端同款协议),YOLO 性能测试用
# 用法:
#   python3 video_push.py <video> [mode]
#     mode=realtime  按 15fps 节奏推(默认,模拟摄像头)
#     mode=full      不限速全速推(测检测器极限吞吐)
# 先用 ffmpeg 把视频转成一整个 NV12 文件(640x480),再循环按帧发送。
# 协议与 mjpeg_nv12_sender 完全一致: [4B 小端长度][NV12 460800B]
# ============================================================================
import socket, struct, subprocess, sys, time

W, H = 640, 480
FRAME = W * H * 3 // 2          # 460800
BOARD = ('192.168.137.250', 8888)

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    video = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else 'realtime'
    raw = '/tmp/testsrc_nv12.raw'

    # 转码:统一 640x480 NV12(与链路同规格),覆盖任意源分辨率
    subprocess.run(['ffmpeg', '-v', 'error', '-i', video,
                    '-vf', f'scale={W}:{H}', '-pix_fmt', 'nv12',
                    '-f', 'rawvideo', '-y', raw], check=True)

    data = open(raw, 'rb').read()
    n = len(data) // FRAME
    if n == 0:
        print('video too short'); sys.exit(1)
    print(f'{video}: {n} frames @ {W}x{H}, mode={mode}')

    s = socket.create_connection(BOARD)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    frame = data[:FRAME]
    hdr = struct.pack('<I', FRAME)
    interval = 1.0 / 15 if mode == 'realtime' else 0
    sent, t0 = 0, time.time()
    try:
        while True:                                  # 无限循环(测试中可随时 Ctrl-C)
            s.sendall(hdr + frame)
            sent += 1
            if interval:
                t = time.time()
                nxt = t0 + sent * interval           # 按绝对节拍防漂移
                d = nxt - time.time()
                if d > 0: time.sleep(d)
            if sent % (15 * 5) == 0:
                fps = sent / (time.time() - t0)
                print(f'sent {sent} ({fps:.1f} fps)')
    except BrokenPipeError:
        print('board closed')
    except KeyboardInterrupt:
        pass
    print(f'done, sent {sent}, avg {sent/(time.time()-t0):.1f} fps')

if __name__ == '__main__':
    main()
