#!/usr/bin/env python3
# gen_visual.py —— 把设备实测证据渲染成一个可双击打开的页面（ohos/sa/visual/index.html）
#
# 为什么用生成器而不是手写 HTML：页面里的每个坐标/文本/计数都必须来自证据文件本身，
# 证据更新后重跑一次就能同步 —— 手写就必然漂移，而"漂移的展示"比没有展示更坏。
#
# 数据来源（只读，不改证据）：
#   evidence/61-s7-1b-read-screen.txt   → SA 的 ReadScreen 现场（元素树 JSON、指标、准入双证）
#   evidence/60-s7-1a-a11y-probe.txt    → 探针现场（三道门、全窗口统计）
#   component/ILutSa.idl                → 对外接口表
#   QEMU-DEPLOY.md                      → 踩坑账本条目数与最近几条
import html
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SA = os.path.dirname(HERE)                     # ohos/sa
EV = os.path.join(SA, 'evidence')
OUT = os.path.join(HERE, 'index.html')


def read(p):
    with open(p, 'rb') as f:
        return f.read().decode('utf-8', errors='replace')


def parse_readscreen():
    """从 evidence/61 里取 SA 返回的元素树 JSON（[14b] 那条 budget=40 的）"""
    txt = read(os.path.join(EV, '61-s7-1b-read-screen.txt'))
    for line in txt.split('\n'):
        s = line.strip()
        if s.startswith('{"ok"'):
            d = json.loads(s)
            if d.get('budget') == 40 and d.get('ok') == 1:
                return d
    raise SystemExit('没找到 [14b] 的 JSON')


def parse_probe():
    """从 evidence/60 里取探针的汇总（全窗口模式）"""
    txt = read(os.path.join(EV, '60-s7-1a-a11y-probe.txt'))
    m = re.search(r'\[a11y\] summary: withText=(\d+) clickable=(\d+)', txt)
    t = re.search(r'\[a11y\] texts=\[(.*?)\]', txt)
    done = re.search(r'verdict=(\S+)', txt)
    return {
        'withText': int(m.group(1)) if m else 0,
        'clickable': int(m.group(2)) if m else 0,
        'texts': [x.strip('"') for x in (t.group(1).split(',') if t else []) if x.strip('"')],
        'verdict': done.group(1) if done else '?',
    }


def parse_idl():
    txt = read(os.path.join(SA, 'component', 'ILutSa.idl'))
    return re.findall(r'^\s*(?:String|void)\s+(\w+)\s*\(', txt, re.M)


def parse_ledger():
    txt = read(os.path.join(SA, 'QEMU-DEPLOY.md'))
    nums = [int(n) for n in re.findall(r'FIX-(\d+)b?', txt)]
    rows = re.findall(r'\|\s*\*?\*?FIX-(\d+)\*?\*?\s*\|\s*([^|]{0,90})', txt)
    return (max(nums) if nums else 0), rows[-4:]


PHASES = [
    ('S0–S6', 'SA 基底 / 注册 / 跨 IPC / 引擎真推理 / 准入 / 配额 / 动作层 / 接入文档', 'done', 'evidence/42–59'),
    ('S7-0', '侦察：无障碍服务在跑 + 客户端 API + 权限档位', 'done', 'evidence/59 [12]'),
    ('S7-1a', '独立探针实测：三道门（4004 → 1005 → 权限）逐个打掉，拿到带文本元素树', 'done', 'evidence/60'),
    ('S7-1b', 'SA 封 <b>ReadScreen</b>（独立 IDL 方法）：准入双证 + 有界 + 隐私不落盘', 'done', 'evidence/61'),
    ('S7-2', '理解 + 决定：元素树 + 话语 → 下一步动作 JSON（严格校验，source 如实）', 'next', '待做 · evidence/62'),
    ('S7-3/4', '执行闭环：打开设置 → 进子页 → 返回（×3 + 断网）', 'todo', '待做 · evidence/63'),
]

GATES = [
    ('v1 普通客户端', 'GetWindows / GetRoot', '4004', 'RET_ERR_NO_CONNECTION',
     'channel 只发给"登记过的无障碍 ability"', 'no'),
    ('v2 UITest 口子', 'RegisterAbilityListener', '1005', 'RET_ERR_NO_PERMISSION',
     '进程 native token 缺 ACCESSIBILITY_EXTENSION_ABILITY', 'no'),
    ('v2 + init 权限', 'RegisterAbilityListener → Connect', '0', 'RET_OK',
     '权限来自 init 服务 cfg 的 permission 字段（SET self token）', 'yes'),
]


def esc(s):
    return html.escape(str(s), quote=True)


def build_screen(d):
    """按真实 box 把元素铺回 1024x768：位置/文本/大小来自设备，配色为示意"""
    W = H = None
    nodes = d['nodes']
    maxx = max((n['box'][2] for n in nodes), default=1024) or 1024
    maxy = max((n['box'][3] for n in nodes), default=768) or 768
    W, H = max(maxx, 1024), max(maxy, 768)
    out = []
    for i, n in enumerate(nodes):
        x1, y1, x2, y2 = n['box']
        w, h = max(1, x2 - x1), max(1, y2 - y1)
        if x2 <= x1 and y2 <= y1:      # 零尺寸元素（实测里真有：box=[0,0,0,0]）单独标注
            out.append(f'<div class="el zero" data-i="{i}" '
                       f'style="left:{x1/W*100:.3f}%;top:{y1/H*100:.3f}%;width:1px;height:1px"></div>')
            continue
        text = n['text']
        # 字号按框高推，单位用 cqh（容器高度百分比）—— 必须随手机框等比缩放，
        # 用 px 会在小尺寸重建里溢出糊成一团（第一版就是这个问题，截图里看得见）
        fs = h / H * 100 * 0.72 if text else 0
        cls = 'el click' if n['click'] else ('el text' if text else 'el plain')
        inner = esc(text) if text else f"<span class='ty'>{esc(n['type'])}</span>"
        style = (f"left:{x1/W*100:.3f}%;top:{y1/H*100:.3f}%;width:{w/W*100:.3f}%;height:{h/H*100:.3f}%;"
                 + (f"font-size:{fs:.2f}cqh;" if fs else ""))
        out.append(f'<div class="{cls}" data-i="{i}" style="{style}" title="#{i} {esc(n["type"])} {esc(text)}">{inner}</div>')
    return W, H, '\n'.join(out)


def main():
    d = parse_readscreen()
    probe = parse_probe()
    methods = parse_idl()
    fix_max, fix_last = parse_ledger()
    W, H, screen_html = build_screen(d)
    texts = [n['text'] for n in d['nodes'] if n['text']]
    clickable = [n for n in d['nodes'] if n['click']]

    rows = '\n'.join(
        f'<tr data-i="{i}" class="{"rclick" if n["click"] else ""}">'
        f'<td>{i}</td><td>{n["win"]}</td><td>{esc(n["type"])}</td>'
        f'<td class="t">{esc(n["text"]) or "—"}</td>'
        f'<td>{"✔" if n["click"] else ""}</td><td>{esc(n["box"])}</td></tr>'
        for i, n in enumerate(d['nodes']))

    gates = '\n'.join(
        f'<div class="gate {cls}"><div class="gname">{esc(a)}</div><div class="gcall">{esc(b)}</div>'
        f'<div class="gret">{esc(c)} <span>{esc(e)}</span></div><div class="gwhy">{esc(f)}</div></div>'
        for a, b, c, e, f, cls in GATES)

    phases = '\n'.join(
        f'<li class="{st}"><span class="pname">{name}</span><span class="pdesc">{desc}</span>'
        f'<span class="pev">{ev}</span></li>'
        for name, desc, st, ev in PHASES)

    texts_html = ''.join(f'<span class="chip">{esc(t)}</span>' for t in texts)
    probe_texts = ''.join(f'<span class="chip">{esc(t)}</span>' for t in probe['texts'])
    api_html = '\n'.join(
        f'<li class="{"new" if m == "ReadScreen" else ""}">{esc(m)}'
        + ('<span class="badge">本轮新增 · S7-1b</span>' if m == 'ReadScreen' else '')
        + '</li>' for m in methods)
    def clean(t):
        "账本标题里的 markdown 标记（**、反引号）不进页面"
        return re.sub(r'\*\*|`', '', t).strip().rstrip('|').strip()
    fixes = '\n'.join(f'<li><b>FIX-{n}</b> {esc(clean(t))}</li>' for n, t in reversed(fix_last))

    doc = f"""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>LUT-SA 读屏实测 · 可视化</title>
<style>
  :root{{--bg:#0b0e14;--card:#141922;--line:#232b38;--fg:#e6edf3;--dim:#8b98a9;--ok:#3fb950;--warn:#d29922;--bad:#f85149;--acc:#58a6ff;}}
  *{{box-sizing:border-box}}
  body{{margin:0;background:var(--bg);color:var(--fg);font:15px/1.65 "Segoe UI","Microsoft YaHei",system-ui,sans-serif}}
  .wrap{{max-width:1180px;margin:0 auto;padding:28px 20px 60px}}
  h1{{font-size:26px;margin:0 0 6px}} h2{{font-size:19px;margin:34px 0 12px;padding-left:10px;border-left:3px solid var(--acc)}}
  .sub{{color:var(--dim);margin:0 0 22px}}
  .card{{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:18px}}
  .grid{{display:grid;grid-template-columns:minmax(330px,420px) 1fr;gap:18px;align-items:start}}
  @media(max-width:900px){{.grid{{grid-template-columns:1fr}}}}
  /* 屏幕重建：位置/大小全部来自设备返回的 box */
  .phone{{position:relative;aspect-ratio:{W}/{H};background:linear-gradient(160deg,#101521,#1b2130);
         border:1px solid var(--line);border-radius:14px;overflow:hidden;container-type:size}}
  .el{{position:absolute;display:flex;align-items:center;justify-content:center;white-space:pre-wrap;
       text-align:center;overflow:hidden;border-radius:3px}}
  .el.text{{color:#f2f6fb;font-weight:300;letter-spacing:.5px}}
  .el .ty{{font-size:1.7cqh;color:#5b6980;letter-spacing:0}}
  .el.click{{outline:1px dashed rgba(88,166,255,.55);background:rgba(88,166,255,.06)}}
  .el.zero{{outline:1px solid var(--warn);background:transparent}}
  .el.hi{{outline:2px solid var(--acc);background:rgba(88,166,255,.22);z-index:5}}
  .hint{{color:var(--dim);font-size:12.5px;margin-top:10px}}
  table{{width:100%;border-collapse:collapse;font-size:13.5px}}
  th,td{{padding:6px 8px;border-bottom:1px solid var(--line);text-align:left}}
  th{{color:var(--dim);font-weight:600}}
  td.t{{color:#dbe6f3}} tr.rclick td{{background:rgba(88,166,255,.06)}}
  tr.hi td{{background:rgba(88,166,255,.18)}}
  .statrow{{display:flex;gap:10px;flex-wrap:wrap;margin:12px 0 0}}
  .stat{{background:#0f141c;border:1px solid var(--line);border-radius:9px;padding:9px 12px;min-width:120px}}
  .stat b{{display:block;font-size:19px;color:var(--acc)}} .stat span{{font-size:12px;color:var(--dim)}}
  .chip{{display:inline-block;background:#0f141c;border:1px solid var(--line);border-radius:999px;
        padding:3px 10px;margin:3px 4px 0 0;font-size:13px}}
  .gate{{border:1px solid var(--line);border-radius:10px;padding:10px 12px;margin-bottom:8px;background:#0f141c}}
  .gate.yes{{border-color:rgba(63,185,80,.5)}}
  .gname{{color:var(--dim);font-size:12.5px}} .gcall{{font-family:Consolas,monospace;font-size:13px;color:#c9d6e4}}
  .gret{{font:600 14px Consolas,monospace}} .gret span{{color:var(--dim);font-weight:400;font-size:12.5px}}
  .gate.no .gret{{color:var(--bad)}} .gate.yes .gret{{color:var(--ok)}}
  .gwhy{{color:var(--dim);font-size:12.5px}}
  ol.phases{{list-style:none;padding:0;margin:0}}
  ol.phases li{{display:grid;grid-template-columns:88px 1fr auto;gap:10px;padding:9px 12px;border:1px solid var(--line);
               border-radius:10px;margin-bottom:7px;background:#0f141c;align-items:baseline}}
  ol.phases li.done{{border-left:3px solid var(--ok)}} ol.phases li.next{{border-left:3px solid var(--acc)}}
  ol.phases li.todo{{border-left:3px solid var(--line);color:var(--dim)}}
  .pname{{font-weight:700}} .pdesc{{font-size:13.5px}} .pev{{font-size:12px;color:var(--dim);font-family:Consolas,monospace}}
  ul.api{{list-style:none;padding:0;margin:0;font-family:Consolas,monospace;font-size:13.5px;columns:2}}
  ul.api li{{padding:4px 0;color:#c9d6e4}}
  ul.api li.new{{color:var(--fg)}} .badge{{background:rgba(88,166,255,.16);color:var(--acc);border-radius:999px;
        padding:1px 8px;font-size:11.5px;margin-left:6px;font-family:inherit}}
  ul.fix{{list-style:none;padding:0;margin:0;font-size:13px;color:var(--dim)}} ul.fix li{{padding:3px 0}}
  .foot{{margin-top:34px;color:var(--dim);font-size:12.5px;border-top:1px solid var(--line);padding-top:14px}}
  code{{background:#0f141c;border:1px solid var(--line);border-radius:5px;padding:1px 6px;font-size:12.5px}}
</style></head><body><div class="wrap">
<h1>LUT-SA：读屏能力实测（设备回传数据可视化）</h1>
<p class="sub">SystemAbility 6901 的第 9 个方法 <code>ReadScreen</code> —— 下面每一个坐标、文本、计数都来自设备证据
 <code>evidence/61</code> / <code>evidence/60</code>，由 <code>visual/gen_visual.py</code> 现场读取生成，不是手抄。</p>

<h2>① SA 当时“看到”的屏幕（按真实 box 还原）</h2>
<div class="grid">
  <div>
    <div class="phone">{screen_html}</div>
    <p class="hint">虚线框 = 设备上报 <code>click:1</code> 的可点元素；悬浮任意元素可与右侧表格互相高亮。
    零尺寸元素（<code>box=[0,0,0,0]</code>）用黄色小点标出。配色为示意，<b>位置/文本/字号-占比来自实测</b>。</p>
  </div>
  <div class="card">
    <div class="statrow">
      <div class="stat"><b>{d['counts']['nodes']}</b><span>元素节点</span></div>
      <div class="stat"><b>{d['counts']['withText']}</b><span>带文本</span></div>
      <div class="stat"><b>{d['counts']['clickable']}</b><span>可点元素</span></div>
      <div class="stat"><b>{d['elapsed_ms']} ms</b><span>首调（含建连）</span></div>
      <div class="stat"><b>{len(d['windows'])}</b><span>窗口</span></div>
    </div>
    <p class="hint" style="margin-top:12px">SA 读到的文本（真实内容）：</p>
    <div>{texts_html}</div>
    <p class="hint">窗口栈（id / type / layer）：{esc(json.dumps(d['windows'], ensure_ascii=False))}</p>
    <table><thead><tr><th>#</th><th>窗口</th><th>类型</th><th>文本</th><th>可点</th><th>坐标 box</th></tr></thead>
    <tbody>{rows}</tbody></table>
  </div>
</div>

<h2>② 三道门是怎么打掉的（S7-1a 探针，全部真错误码）</h2>
<div class="card">{gates}
  <p class="hint">探针全窗口模式：{probe['withText']} 条文本 / {probe['clickable']} 个可点，<code>verdict={esc(probe['verdict'])}</code>；
  样例文本：{probe_texts}</p></div>

<h2>③ 进度与接口</h2>
<div class="card">
  <ol class="phases">{phases}</ol>
  <p class="hint" style="margin-top:14px">SA 对外接口（{len(methods)} 个，来自 <code>ILutSa.idl</code>）：</p>
  <ul class="api">{api_html}</ul>
</div>

<h2>④ 账本（踩坑 {fix_max} 条，最近 4 条）</h2>
<div class="card"><ul class="fix">{fixes}</ul>
  <p class="hint">每条格式固定为「现象 → 真因 → 修法」，全文见 <code>QEMU-DEPLOY.md</code>。</p></div>

<p class="foot">数据出处：<code>ohos/sa/evidence/61-s7-1b-read-screen.txt</code>（SA 的 ReadScreen 现场）+
<code>ohos/sa/evidence/60-s7-1a-a11y-probe.txt</code>（探针三道门）+
<code>ohos/sa/component/ILutSa.idl</code> + <code>ohos/sa/QEMU-DEPLOY.md</code>。
重跑生成：<code>python3 ohos/sa/visual/gen_visual.py</code>。</p>
</div>
<script>
const els=[...document.querySelectorAll('.el')],rows=[...document.querySelectorAll('tbody tr')];
function mark(i,on){{els.forEach(e=>e.dataset.i===i&&e.classList.toggle('hi',on));
  rows.forEach(r=>r.dataset.i===i&&r.classList.toggle('hi',on));}}
els.forEach(e=>{{e.onmouseenter=()=>mark(e.dataset.i,true);e.onmouseleave=()=>mark(e.dataset.i,false);}});
rows.forEach(r=>{{r.onmouseenter=()=>mark(r.dataset.i,true);r.onmouseleave=()=>mark(r.dataset.i,false);}});
</script></body></html>
"""
    with open(OUT, 'wb') as f:
        f.write(doc.encode('utf-8'))
    print(f'已生成 {OUT}')
    print(f'  元素 {d["counts"]["nodes"]} / 文本 {d["counts"]["withText"]} / 可点 {d["counts"]["clickable"]}'
          f' / 首调 {d["elapsed_ms"]}ms / 窗口 {len(d["windows"])}')
    print(f'  接口 {len(methods)} 个（新增 {[m for m in methods if m == "ReadScreen"]}）'
          f' / 账本 FIX-1..{fix_max}')


if __name__ == '__main__':
    main()
