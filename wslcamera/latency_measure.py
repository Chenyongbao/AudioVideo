#!/usr/bin/env python3
# ============================================================================
# 端到端延迟测量(手机拍屏法)
# 配合 Qt 客户端 F 键开启的测试元素:
#   - 视频画面左上角色块: 显示的是"链路远端"的时钟(被延迟 D)
#   - 窗口边框:           显示的是"本机当下"的时钟
#   两者同源(同一 QElapsedTimer),3s 周期: 0-2s 白 / 2-3s 黑
#   同屏录像里,色块跳变时刻 Tp 比边框跳变时刻 Tb 晚 D:
#       D = (Tp - Tb) mod 3000 ms
# 用法:
#   ffmpeg -i screen.mp4 -vsync 0 /tmp/lat/f_%04d.png   # 先抽帧(或脚本自动做)
#   python3 latency_measure.py /tmp/lat/f_%04d.png
# 依赖: pip install pillow numpy
# ============================================================================
import sys, glob, re
import numpy as np
from PIL import Image

PERIOD_MS = 3000.0

def load_frames(pattern):
    files = sorted(glob.glob(pattern.replace('%04d', '*')))
    if not files:
        print('no frames matched:', pattern); sys.exit(1)
    return files

def detect_regions(img):
    """返回 (色块box, 边框采样带)。色块: 左上区域找 48px 方块;边框: 画面最外圈。"""
    a = np.asarray(img.convert('L'), dtype=np.float32)
    h, w = a.shape
    # 边框带: 外圈 10px 环
    border = np.concatenate([a[:10, :].ravel(), a[-10:, :].ravel(),
                             a[:, :10].ravel(), a[:, -10:].ravel()])
    # 色块: 在左上 1/4 区域内,用与边框的时间相关性最强的小窗口搜索
    # 简化: 直接取 (0.02w, 0.05h) 起 6%x6% 区域(QText 下方 48px 块),分辨率无关
    x0, y0 = int(w*0.02), int(h*0.06)
    x1, y1 = x0 + max(24, int(w*0.06)), y0 + max(24, int(h*0.06))
    return (x0, y0, x1, y1), border

def series(files):
    patch, border = [], []
    boxes = None
    for f in files:
        img = Image.open(f)
        if boxes is None:
            boxes = detect_regions(img)
        a = np.asarray(img.convert('L'), dtype=np.float32)
        (x0, y0, x1, y1), _ = boxes
        patch.append(a[y0:y1, x0:x1].mean())
        border.append(np.concatenate([a[:10,:].ravel(), a[-10:,:].ravel(),
                                      a[:,:10].ravel(), a[:,-10:].ravel()]).mean())
    return np.array(patch), np.array(border), boxes

def transitions(x):
    """二值化后返回每次跳变的帧索引。"""
    mid = (x.max() + x.min()) / 2
    b = x > mid
    return np.where(np.diff(b.astype(int)) != 0)[0] + 1, b

def main():
    pattern = sys.argv[1] if len(sys.argv) > 1 else '/tmp/lat/f_%04d.png'
    fps = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    files = load_frames(pattern)
    print(f'frames: {len(files)}')
    patch, border, boxes = series(files)
    print('patch region box:', boxes[0])

    pt, pb = transitions(patch)
    bt, bb = transitions(border)
    if len(pt) < 2 or len(bt) < 2:
        print('transitions too few: patch', len(pt), 'border', len(bt)); sys.exit(1)

    # 用帧率把跳变帧号换算成毫秒相位(取同方向跳变配对:白->黑 与 黑->白 分别算)
    def phases(t_idx, binar):
        return [ (i / fps * 1000.0) % PERIOD_MS for i in t_idx ]

    pp = phases(pt, pb)
    bp = phases(bt, bb)
    # 对每种方向配对求 (Tp - Tb) mod 3000 的圆均值
    diffs = []
    for d in pp:
        best = min(((d - e) % PERIOD_MS for e in bp), key=lambda v: min(v, PERIOD_MS-v))
        diffs.append(best)
    diffs = np.array(diffs)
    # 圆均值(处理 0/3000 边界)
    ang = np.exp(1j * 2*np.pi*diffs/PERIOD_MS)
    D = (np.angle(ang.mean()) * PERIOD_MS / (2*np.pi)) % PERIOD_MS
    spread = np.std(diffs)

    print(f'patch transitions @frames {pt.tolist()} phases {[round(x) for x in pp]}')
    print(f'border transitions @frames {bt.tolist()} phases {[round(x) for x in bp]}')
    print(f'\n=== 端到端延迟 ≈ {D:.0f} ms (±{spread:.0f} ms, {len(diffs)} 次跳变) ===')
    print('注: 结果含 手机采样/屏幕刷新 抖动;多次拍摄取中位数更稳。')

if __name__ == '__main__':
    main()
