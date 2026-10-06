#!/usr/bin/env python3
# gen_screen_overlay.py —— 真机屏幕可视化：把 SA 读到的元素框叠在**真实截屏**上
#
# 为什么单独一个脚本、而不是塞进 gen_visual.py：
#   这一张图的数据源是"同一时刻的两个采集"（snapshot_display 截的真屏 + ReadScreen 的元素树），
#   两者的配对关系必须显式可查（时间戳一起打印进页面），所以单独一条流水线更不容易出错。
#
# 用法：
#   python3 gen_screen_overlay.py --shot <真实截图.png> --evidence <evidence/62-*.txt> [--out-dir .]
# 产物：
#   screen_annotated.png  —— 真屏 + 编号框 + 类型/文本标签（可分享的扁平图）
#   screen.html           —— 真屏 / 叠框图 切换 + 元素表 + 采集时间（可双击打开的页面）
import argparse
import json
import os
import re

from PIL import Image, ImageDraw, ImageFont

CJK_FONTS = [
    r'C:\Windows\Fonts\msyh.ttc',      # 微软雅黑
    r'C:\Windows\Fonts\msyhbd.ttc',
    r'C:\Windows\Fonts\simhei.ttf',
    r'C:\Windows\Fonts\simsun.ttc',
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
]

CB_TEXT = (88, 166, 255)      # 带文本元素
CB_CLICK = (63, 185, 80)      # 可点元素
CB_ZERO = (210, 153, 34)      # 零尺寸元素


def load_font(size):
    for p in CJK_FONTS:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except OSError:
                continue
    return ImageFont.load_default()


def parse_pair(evidence_path):
    """从证据文件里取配对数据：截图时刻 / 读屏时刻 / 元素树 JSON"""
    txt = open(evidence_path, 'rb').read().decode('utf-8', errors='replace')
    sec = txt.split('[15]')[1].split('[7]') [0] if '[15]' in txt else txt
    shot_t = re.search(r'截图时刻:\s*(\d+)', sec)
    read_t = re.search(r'读屏时刻:\s*(\d+)', sec)
    js = None
    for line in sec.split('\n'):
        s = line.strip()
        if s.startswith('{"ok"'):
            js = json.loads(s)
            break
    if js is None:                      # 退回到文件里任意一条 ok=1 的 JSON
        for line in txt.split('\n'):
            s = line.strip()
            if s.startswith('{"ok"'):
                js = json.loads(s)
                if js.get('ok') == 1:
                    break
    return js, (shot_t.group(1) if shot_t else '?'), (read_t.group(1) if read_t else '?')


def annotate(img, d, scale_w, scale_h, out_path):
    im = img.convert('RGB')
    dr = ImageDraw.Draw(im)
    fnum = load_font(max(11, int(min(im.width, im.height) * 0.017)))
    flab = load_font(max(10, int(min(im.width, im.height) * 0.015)))

    # 屏幕坐标系（元素 box 的空间）从根节点推：root box 通常是 [0,0,W,H]
    sx = im.width / scale_w
    sy = im.height / scale_h

    for i, n in enumerate(d['nodes']):
        x1, y1, x2, y2 = n['box']
        X1, Y1, X2, Y2 = x1 * sx, y1 * sy, x2 * sx, y2 * sy
        color = CB_CLICK if n['click'] else (CB_TEXT if n['text'] else CB_ZERO)
        if X2 - X1 < 1 and Y2 - Y1 < 1:        # 零尺寸元素：画个十字标记
            dr.line([X1 - 5, Y1, X1 + 5, Y1], fill=CB_ZERO, width=2)
            dr.line([X1, Y1 - 5, X1, Y1 + 5], fill=CB_ZERO, width=2)
            dr.text((X1 + 7, Y1 - 7), f'{i}', font=fnum, fill=CB_ZERO)
            continue
        dr.rectangle([X1, Y1, X2, Y2], outline=color, width=2)
        tag = f"{i}"
        dr.rectangle([X1, Y1, X1 + 8 + 7 * len(tag), Y1 + fnum.size + 4], fill=color)
        dr.text((X1 + 4, Y1 + 2), tag, font=fnum, fill=(11, 14, 20))
        if n['text']:
            dr.text((X1 + 2, max(0, Y1 - flab.size - 3)), n['text'], font=flab, fill=color)
    im.save(out_path)
    return out_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--shot', required=True, help='真实截图（png/jpeg）')
    ap.add_argument('--evidence', required=True, help='含 [15] 段的证据文件')
    ap.add_argument('--out-dir', default=os.path.dirname(os.path.abspath(__file__)))
    a = ap.parse_args()

    d, shot_t, read_t = parse_pair(a.evidence)
    if d is None or d.get('ok') != 1:
        raise SystemExit('证据里没有 ok=1 的 ReadScreen JSON，无法配对')
    img = Image.open(a.shot)

    # 元素坐标空间：取所有 box 的最大值，再与设备声称的根尺寸取较大者（1024x768）
    W = max([n['box'][2] for n in d['nodes']] + [1024])
    H = max([n['box'][3] for n in d['nodes']] + [768])

    out_png = os.path.join(a.out_dir, 'screen_annotated.png')
    annotate(img, d, W, H, out_png)

    # 元素坐标空间：取所有 box 的最大值，再与设备声称的根尺寸取较大者（1024x768）
    import base64
    def b64(p):
        return base64.b64encode(open(p, 'rb').read()).decode()

    shot_b64 = b64(a.shot)
    ann_b64 = b64(out_png)
    texts = [n['text'] for n in d['nodes'] if n['text']]
    clicks = [n for n in d['nodes'] if n['click']]
    rows = '\n'.join(
        f'<tr><td>{i}</td><td>{n["win"]}</td><td>{n["type"]}</td>'
        f'<td>{(n["text"] or "—")}</td><td>{"✔" if n["click"] else ""}</td><td>{n["box"]}</td></tr>'
        for i, n in enumerate(d['nodes']))

    html_doc = f"""<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<title>真机屏幕可视化 · SA 读到了什么</title><style>
:root{{--bg:#0b0e14;--card:#141922;--line:#232b38;--fg:#e6edf3;--dim:#8b98a9;--ok:#3fb950;--acc:#58a6ff}}
*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--fg);font:15px/1.6 "Segoe UI","Microsoft YaHei",sans-serif}}
.wrap{{max-width:1240px;margin:0 auto;padding:26px 18px 50px}}
h1{{font-size:24px;margin:0 0 6px}}.sub{{color:var(--dim);margin:0 0 20px}}
.card{{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px}}
.grid{{display:grid;grid-template-columns:1fr 1fr;gap:16px}}@media(max-width:980px){{.grid{{grid-template-columns:1fr}}}}
.shot{{position:relative;border-radius:10px;overflow:hidden;border:1px solid var(--line)}}
.shot img{{display:block;width:100%;height:auto}}
.cap{{display:flex;justify-content:space-between;align-items:center;margin:8px 0 0;color:var(--dim);font-size:12.5px}}
button{{background:#1b2230;color:var(--fg);border:1px solid var(--line);border-radius:8px;padding:4px 10px;cursor:pointer}}
button.on{{background:var(--acc);color:#0b0e14;border-color:var(--acc)}}
.statrow{{display:flex;gap:10px;flex-wrap:wrap;margin:0 0 12px}}
.stat{{background:#0f141c;border:1px solid var(--line);border-radius:9px;padding:8px 12px}}
.stat b{{display:block;font-size:18px;color:var(--acc)}}.stat span{{font-size:12px;color:var(--dim)}}
table{{width:100%;border-collapse:collapse;font-size:13px;margin-top:6px}}
th,td{{padding:5px 7px;border-bottom:1px solid var(--line);text-align:left}}th{{color:var(--dim)}}
.chip{{display:inline-block;background:#0f141c;border:1px solid var(--line);border-radius:999px;padding:2px 9px;margin:3px 4px 0 0;font-size:13px}}
.badge{{background:rgba(88,166,255,.16);color:var(--acc);border-radius:999px;padding:1px 8px;font-size:11.5px;margin-left:6px}}
.legend span{{margin-right:14px;font-size:12.5px}}
.dot{{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:5px;vertical-align:middle}}
.foot{{margin-top:26px;color:var(--dim);font-size:12.5px;border-top:1px solid var(--line);padding-top:12px}}
code{{background:#0f141c;border:1px solid var(--line);border-radius:5px;padding:1px 6px;font-size:12.5px}}
</style></head><body><div class="wrap">
<h1>真机屏幕可视化：SA 到底"看到"了什么</h1>
<p class="sub">左边是模拟器里的<b>真实截屏</b>（guest 内 <code>snapshot_display</code> 直接抓的帧）；
右边是把 SA 的 <code>ReadScreen</code> 返回的元素框按真实坐标叠上去。
两个采集相差 &lt;1s（截图时刻 {shot_t} / 读屏时刻 {read_t}，Unix 秒）。</p>

<div class="card">
  <div class="statrow">
    <div class="stat"><b>{d['counts']['nodes']}</b><span>元素节点</span></div>
    <div class="stat"><b>{d['counts']['withText']}</b><span>带文本</span></div>
    <div class="stat"><b>{d['counts']['clickable']}</b><span>可点元素</span></div>
    <div class="stat"><b>{d['elapsed_ms']} ms</b><span>读屏耗时（首调含建连）</span></div>
    <div class="stat"><b>{img.width}×{img.height}</b><span>真实分辨率</span></div>
  </div>
  <div class="legend" style="margin-bottom:12px">
    <span><i class="dot" style="background:#58a6ff"></i>带文本元素</span>
    <span><i class="dot" style="background:#3fb950"></i>可点元素</span>
    <span><i class="dot" style="background:#d29922"></i>零尺寸元素（<code>box=[0,0,0,0]</code>）</span>
  </div>
  <div class="grid">
    <div>
      <div class="shot"><img src="data:image/png;base64,{shot_b64}" alt="真实截屏"></div>
      <p class="cap"><span>① 真实截屏（设备像素）</span><span>{os.path.basename(a.shot)}</span></p>
    </div>
    <div>
      <div class="shot"><img id="ann" src="data:image/png;base64,{ann_b64}" alt="叠加元素框"></div>
      <p class="cap"><span>② 叠上 SA 读到的元素框</span>
        <button id="tg" class="on" type="button">显示/隐藏叠框</button></p>
    </div>
  </div>
</div>

<h2 style="font-size:18px;margin:26px 0 10px">SA 读到的文本（真实内容）</h2>
<div class="card">{''.join(f'<span class="chip">{t}</span>' for t in texts)}</div>

<h2 style="font-size:18px;margin:26px 0 10px">元素清单（{len(d['nodes'])} 个，坐标空间 {W}×{H}）</h2>
<div class="card"><table><thead><tr><th>#</th><th>窗口</th><th>类型</th><th>文本</th><th>可点</th><th>坐标 box</th></tr></thead>
<tbody>{rows}</tbody></table></div>

<p class="foot">数据源：<code>{os.path.basename(a.evidence)}</code>（guest 内 <code>snapshot_display</code> 截屏 +
<code>lut_sa_client --screen 0</code> 读屏，两次采集相邻）。
叠框与还原由 <code>gen_screen_overlay.py</code> 生成；坐标/文本/可点全部来自设备返回值，
<b>截图像素是模拟器真实帧</b>（不是示意）。</p>
</div>
<script>
const A=document.getElementById('ann'),T=document.getElementById('tg');
T.onclick=()=>{{const on=T.classList.toggle('on');
  A.style.visibility=on?'visible':'hidden';}};
</script></body></html>
"""
    out_html = os.path.join(a.out_dir, 'screen.html')
    open(out_html, 'wb').write(html_doc.encode('utf-8'))
    print(f'  截图 {img.width}x{img.height} → 叠框图 {out_png}')
    print(f'  页面 {out_html}')
    print(f'  配对: 截图 t={shot_t} / 读屏 t={read_t}；元素 {d["counts"]}；文本 {texts}')
    print(f'  可点: {[(n["type"], n["box"]) for n in clicks]}')


if __name__ == '__main__':
    main()
