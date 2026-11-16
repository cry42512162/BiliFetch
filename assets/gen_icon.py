# -*- coding: utf-8 -*-
"""BiliFetch 应用图标生成器：4096 超采样绘制 → 1024 预览 PNG + 多尺寸 ICO"""
from PIL import Image, ImageDraw

S = 4                      # 超采样倍数（画布 4096 → 输出 1024）
N = 1024                   # 输出边长
W = N * S

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

# ---------- 1. 渐变底板（圆角方形徽章） ----------
TOP, BOTTOM = (79, 179, 245), (24, 94, 216)      # bilibili 蓝渐变
badge = Image.new("RGB", (W, W), BOTTOM)
grad = Image.new("RGB", (W, W))
gd = ImageDraw.Draw(grad)
for y in range(W):
    gd.line([(0, y), (W, y)], fill=lerp(TOP, BOTTOM, y / (W - 1)))
mask = Image.new("L", (W, W), 0)
md = ImageDraw.Draw(mask)
inset, radius = 64 * S, 210 * S
md.rounded_rectangle([inset, inset, W - inset, W - inset], radius=radius, fill=255)
icon = Image.new("RGBA", (W, W), (0, 0, 0, 0))
icon.paste(grad, (0, 0), mask)

d = ImageDraw.Draw(icon)
WHITE = (255, 255, 255, 255)
ARROW = (30, 102, 224, 255)                        # 电视内箭头（同底色系深蓝）

# ---------- 2. 天线（圆头斜线） ----------
lw = 46 * S
d.line([(400 * S, 330 * S), (296 * S, 178 * S)], fill=WHITE, width=lw)
d.line([(624 * S, 330 * S), (728 * S, 178 * S)], fill=WHITE, width=lw)
for x, y in ((400, 330), (296, 178), (624, 330), (728, 178)):
    r = lw // 2
    d.ellipse([x * S - r, y * S - r, x * S + r, y * S + r], fill=WHITE)

# ---------- 3. 电视主体（白色圆角矩形） ----------
tv = [202 * S, 325 * S, 822 * S, 795 * S]
d.rounded_rectangle(tv, radius=72 * S, fill=WHITE)

# ---------- 4. 电视内：下载箭头 ----------
# 竖杆
d.rounded_rectangle([472 * S, 388 * S, 552 * S, 566 * S], radius=24 * S, fill=ARROW)
# 箭头三角（带圆角观感：先画三角再补角）
d.polygon([(394 * S, 528 * S), (630 * S, 528 * S), (512 * S, 700 * S)], fill=ARROW)
# 底部小横条（托盘，暗示“落地/归档”）
d.rounded_rectangle([396 * S, 726 * S, 628 * S, 774 * S], radius=24 * S, fill=ARROW)

# ---------- 5. 输出 ----------
out = icon.resize((N, N), Image.LANCZOS)
out.save(r"D:\C++\bilibili fetch\assets\app_1024.png", "PNG")

ico_path = r"D:\C++\bilibili fetch\assets\app.ico"
out.save(ico_path, "ICO",
         sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
                (64, 64), (128, 128), (256, 256)])
print("saved:", ico_path)
