#!/usr/bin/env python3
# gen_dashboard.py —— LUT-SA SystemAbility Capability Dashboard（REPLAY 模式，静态生成）
#
# 证明："LUT-SA is an OpenHarmony SystemAbility with embedded LLM runtime and AI capability foundation"
# 面向开发者/评审/研究人员；不做用户侧 GUI、不做手机模拟器、不做 OCR。
#
# 硬约束（v1）：① 不修改 SA 任何代码；② 不新增 IPC；③ 不做 LIVE 采集；
#              ④ 只用：源码静态信息 + evidence 文件 + lut_sa_client 已有输出 + 构建产物。
#
# 铁律：**状态不可手填**。每个字段的 status 都由 parse/grep 推导，推导依据（basis）写进
#       data/model.json；聚合能力与"文档漂移"同样走规则/程序化检查。
#
# 用法：python3 ohos/sa/dashboard/gen_dashboard.py
import datetime
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
SA = HERE.parent
REPO = SA.parent.parent
EVID = SA / 'evidence'
OUT = HERE / 'site'
DATA = HERE / 'data'

EVIDENCE_ORDER = [
    '61-s7-1b-read-screen.txt', '60-s7-1a-a11y-probe.txt', '59-sa-action-grant.txt',
    '58-sa-engine-persist.txt', '48-sa-quota.txt', '47-sa-caller-admission.txt',
    '45-sa-tmac-kcfg-pair-fix.txt', '43-sa-tmac-kcfg-path-fix.txt',
    '42-sa-real-inference-sta3.txt', '41-sa-robustness-sta2.txt', '11-sa-boot-traces.txt',
]
RANK = {'PASS': 0, 'PARTIAL': 1, 'READY': 2, 'ABSENT': 3}
CODE_DIRS = [SA / 'component', SA / 'engine', SA, SA / 'ref_kernel']
CHIP = {'PASS': 'ok', 'PARTIAL': 'warn', 'READY': 'ready', 'ABSENT': 'no'}


def read_bytes(p: Path) -> str:
    return p.read_bytes().decode('utf-8', 'replace').replace('\x00', '')


class Evidence:
    def __init__(self):
        self.files = {p.name: read_bytes(p).split('\n') for p in sorted(EVID.glob('*.txt'))}

    def query(self, pattern, names=None):
        for name in (names or EVIDENCE_ORDER):
            for i, line in enumerate(self.files.get(name, []), 1):
                m = re.search(pattern, line)
                if m:
                    return m, f'evidence/{name}:{i}', i
        return None, None, None

    def query_all(self, pattern, names=None):
        out = []
        for name in (names or list(self.files)):
            for i, line in enumerate(self.files.get(name, []), 1):
                m = re.search(pattern, line)
                if m:
                    out.append((m, f'evidence/{name}:{i}'))
        return out

    def count(self, pattern, names=None):
        return len(self.query_all(pattern, names))


def code_hits(pattern):
    rx, hits = re.compile(pattern), []
    for d in CODE_DIRS:
        if not d.exists():
            continue
        for ext in ('*.cpp', '*.h', '*.cc'):
            for p in d.glob(ext):
                for i, line in enumerate(read_bytes(p).split('\n'), 1):
                    if rx.search(line):
                        hits.append(f'{p.relative_to(REPO)}:{i}')
    return hits


def facts(ev: Evidence):
    F = {}

    def put(key, label, value, source, status, basis):
        F[key] = dict(label=label, value=value, source=source, status=status, basis=basis)

    # ── SA 注册 / 生命周期 / IPC 往返
    m, src, _ = ev.query(r'Publish SA:(\d+) result:(\d)')
    put('sa.publish', 'SA 注册/发布（samgr）',
        f'SA:{m.group(1)} publish result={m.group(2)}' if m else '未找到', src,
        'PASS' if m and m.group(2) == '1' else 'ABSENT', 'evidence 中出现 "Publish SA:6901 result:1"')
    m, src, _ = ev.query(r'SAMGR: insert SA:(\d+)')
    put('sa.samgr_insert', 'samgr 服务表登记', f'insert SA:{m.group(1)}' if m else '未找到', src,
        'PASS' if m else 'ABSENT', 'evidence 中出现 "SAMGR: insert SA:6901"')
    m, src, _ = ev.query(r'\[GetSystemAbility\(6901\)\] ok')
    put('ipc.getsa', '跨进程取到 SA 6901', 'ok' if m else '未找到', src, 'PASS' if m else 'ABSENT',
        '客户端在另一进程 GetSystemAbility(6901) 成功')
    m, src, _ = ev.query(r'\[iface_cast\] ok')
    put('ipc.iface_cast', 'IPC 代理建立（iface_cast）', 'ok' if m else '未找到', src,
        'PASS' if m else 'ABSENT', 'IRemoteObject → ILutSa 转型成功')

    # ── 推理 / 只读方法（ErrCode=0 → PASS）
    m, src, _ = ev.query(r'\[NativeVersion\] ErrCode=(\d+)')
    put('m.NativeVersion', 'NativeVersion', f'ErrCode={m.group(1)}' if m else '未找到', src,
        'PASS' if m and m.group(1) == '0' else 'ABSENT', '客户端打印 ErrCode=0')
    m, src, _ = ev.query(r'\[LoadModel\((?P<p>[^)]*)\)\] ErrCode=(?P<e>\d+)(?:\s*\((?P<ms>[\d.]+) ms\))?')
    put('m.LoadModel', 'LoadModel',
        (f'ErrCode={m.group("e")}' + (f' · {m.group("ms")} ms' if m.group('ms') else '')) if m else '未找到',
        src, 'PASS' if m and m.group('e') == '0' else 'ABSENT',
        'ErrCode=0 = 真加载成功；ErrCode=22 的样本是预期拒绝（形状表/坏路径/超配额）')
    m, src, _ = ev.query(r'\[Generate#\d+\([^)]*\)\] ErrCode=(\d+)')
    put('m.Generate', 'Generate', f'ErrCode={m.group(1)}' if m else '未找到', src,
        'PASS' if m and m.group(1) == '0' else 'ABSENT', 'ErrCode=0 = 真出 token')
    m, src, _ = ev.query(r'\[determinism\] 两次同 prompt 输出一致: (\w+)')
    put('m.Generate.det', 'Generate 确定性', m.group(1) if m else '未找到', src,
        'PASS' if m and m.group(1) == 'yes' else 'ABSENT', '客户端打印"两次同 prompt 输出一致: yes"')
    m, src, _ = ev.query(r'\[Generate#3\(贪心/换 prompt\)\] ErrCode=0\s*->\s*(.*)')
    put('m.Generate.nonconst', 'Generate 非常量', (m.group(1).strip()[:40] if m else '未找到'), src,
        'PASS' if m else 'ABSENT', '贪心换 prompt 后输出不同（非常量证明）')
    # 注意：evidence 里 [14a] 的拒绝面（201）先于成功面出现，所以这里显式匹配 ErrCode=0
    m, src, _ = ev.query(r'\[--screen (?P<b>\d+)\] ErrCode=0')
    put('m.ReadScreen', 'ReadScreen',
        (f'ErrCode=0 (budget={m.group("b")})') if m else '未找到', src,
        'PASS' if m else 'ABSENT',
        '显式匹配成功面（ErrCode=0）；拒绝面单独列为 m.ReadScreen.denied（201）')
    m, src, _ = ev.query(r'\[--metrics\] ErrCode=(\d+)')
    put('m.GetMetrics', 'GetMetrics', f'ErrCode={m.group(1)}' if m else '未找到', src,
        'PASS' if m and m.group(1) == '0' else 'ABSENT', 'ErrCode=0（注意：该方法不设准入门）')
    m, src, _ = ev.query(r'PASS: LUT kernel warm-up \((?P<s>[^,]+), (?P<w>[^,]+), (?P<u>[\d.]+) us\)')
    put('m.SelfTest', 'SelfTest（调优内核暖机）',
        (f'{m.group("s")} · {m.group("w")} · {m.group("u")} us') if m else '未找到', src,
        'PASS' if m else 'ABSENT', '暖机 PASS 且标注"调优内核" → 调优路径命中')
    m, src, _ = ev.query(r'PASS: ref LUT kernel numeric check \((?P<d>[^)]*)\)')
    put('m.SelfTest.ref', 'SelfTest（参考内核数值自检）', m.group('d') if m else '未找到', src,
        'PASS' if m else 'ABSENT', '参考核 got/expect 一致')
    m, src, _ = ev.query(r'\[Release\] ErrCode=(\d+)')
    put('m.Release', 'Release', f'ErrCode={m.group(1)}' if m else '未找到', src,
        'PASS' if m and m.group(1) == '0' else 'ABSENT', 'ErrCode=0')

    # ── 动作层：规则推导（契约可测 ≠ 设备执行成功）
    m_ams, src_ams, _ = ev.query(r'"ams_try":(\d+)')
    # 客户端 --action 模式打印的标签是 [ExecuteAction]（不是 --action）
    m_act, src_act, _ = ev.query(r'\[ExecuteAction\] ErrCode=(\d+)')
    ams_try = int(m_ams.group(1)) if m_ams else None
    act_ec = int(m_act.group(1)) if m_act else None
    if act_ec in (0, 201) and ams_try == 0:
        st, basis = 'PASS', f'契约 ErrCode={act_ec} 且 ams_try=0（真实拉起成功）'
    elif act_ec in (0, 201):
        st, basis = 'READY', (f'契约实测 ErrCode={act_ec}（策略+JSON 契约可用），'
                              f'但设备执行 ams_try={ams_try} → 真实拉起未发生')
    else:
        st, basis = 'ABSENT', '未找到 --action 的 ErrCode 证据'
    put('m.ExecuteAction', 'ExecuteAction（决策契约）',
        f'契约 ErrCode={act_ec} · 设备执行 ams_try={ams_try}', src_ams or src_act or '—', st, basis)
    kw = ev.query(r'\[ExecuteIntent\] ErrCode=0.*?"source":"keyword"')
    put('m.ExecuteIntent', 'ExecuteIntent（keyword 路径）',
        'ErrCode=0 且 source=keyword' if kw[0] else '未找到', kw[1] or '—',
        'PASS' if kw[0] else 'ABSENT',
        '关键词路径实测命中（同一行含 ErrCode 与 source 标注）')
    n_model = ev.count(r'\[ExecuteIntent\][^\n]*"source":"model"')
    n_kw = ev.count(r'\[ExecuteIntent\][^\n]*"source":"keyword"')
    put('m.ExecuteIntent.model', 'ExecuteIntent（model 路径）',
        f'source:model {n_model} 次 / source:keyword {n_kw} 次（全库统计）', 'evidence/*（grep 统计）',
        'PASS' if n_model else 'ABSENT',
        f'全证据库 grep "source":"model" = {n_model} → 模型输出从未通过严格校验、从未被采纳')

    # ── 策略四门
    n201 = ev.count(r'ErrCode=201')
    ok0 = ev.query(r'\[--screen \d+\] ErrCode=0')
    put('p.allow_uids', '调用方准入（allow_uids.txt）',
        f'拒绝面 201 出现 {n201} 次 · 放行面 ErrCode=0 存在（同一进程不重启）',
        (ev.query(r'ErrCode=201')[1] or 'evidence/47,59,61'), 'PASS' if (n201 and ok0[0]) else 'ABSENT',
        'allow_uids 门的两面均由客户端可见信号判定：[14a] 白名单不含调用方 → 201；[14b] 恢复默认档位 → 0；'
        '（SA 侧 hilog 不进证据包，故不用 hilog 文本判定）')
    q = ev.count(r'model_mb=1')
    put('p.quota', '配额（quota.txt）', f'model_mb=1 场景 {q} 次（超限 ErrCode=22）', 'evidence/48,59',
        'PASS' if q else 'ABSENT', '超限立即 22 且原因可读；删表恢复 0')
    g = ev.count(r'动作白名单')
    put('p.actions_allow', '动作白名单（actions_allow.txt）', f'相关判据 {g} 处（未授权 bundle → 201）',
        'evidence/49,59,61', 'PASS' if g else 'ABSENT', '正向授权与拒绝双证（camera 放行 0 / 收回 201）')
    put('p.intents', '意图表（intents.txt）', f'关键词命中 {n_kw} 次', 'evidence/52,61',
        'PASS' if n_kw else 'ABSENT', '内置 7 条表 + 运行期可覆盖；命中后仍要过动作白名单')

    # ── LLM Runtime
    m, src, _ = ev.query(r'\[metrics\][^\n]*engine=(?P<e>\w+) n_ctx=(?P<c>\d+) threads=(?P<t>\d+) infer=(?P<i>\d+)')
    put('r.engine', '引擎状态（SA 进程内）',
        (f'engine={m.group("e")} · n_ctx={m.group("c")} · threads={m.group("t")} · infer={m.group("i")}')
        if m else '未找到', src, 'PASS' if m and m.group('e') == 'ready' else 'ABSENT',
        'GetMetrics 的 engine= 字段（引擎在 SA 进程内就绪）')
    peak = [x[0].group(1) for x in ev.query_all(r'peak_rss=([\d.]+) MB')]
    put('r.rss', 'SA 进程峰值内存', f'{max(float(x) for x in peak):.1f} MB（全库最大）' if peak else '未找到',
        ev.query(r'peak_rss=')[1] or '—', 'PASS' if peak else 'ABSENT', 'GetMetrics 的 peak_rss')
    m, src, _ = ev.query(r'kcfg sections=(\d+)')
    put('r.kcfg', 'kcfg 形状表命中', f'sections={m.group(1)}' if m else '未找到', src,
        'PASS' if m else 'ABSENT', '引擎启动时读到的 kcfg 段数 > 0 = 形状表可用')
    m, src, _ = ev.query(r'diag: \[sa\] LoadModel ok in (\d+) ms')
    put('r.load_diag', '引擎自报加载耗时', f'{m.group(1)} ms' if m else '未找到', src,
        'PASS' if m else 'ABSENT', '引擎壳 diag（与客户端 wall time 对照）')
    m, src, _ = ev.query(r'admission rejected: (.{0,110})')
    put('r.admission', '模型准入（加载前拦截）', m.group(1) if m else '未找到', src,
        'PASS' if m else 'ABSENT', '形状表不匹配 → 加载前拒绝（不打死 SA）；修复后同模型加载成功')
    m, src, _ = ev.query(r'\[shim\] diag: after_prompt_decode.*?nan_or_inf=(\d+)')
    put('r.diag_decode', 'prompt decode 数值健康度', f'nan_or_inf={m.group(1)}' if m else '未找到', src,
        'PASS' if m and m.group(1) == '0' else 'PARTIAL', '引擎壳 diag：NaN/Inf 计数为 0')

    # ── 感知链路各段 + 截图段
    m, src, _ = ev.query(r'ps\(accessib\):\s*(\S+)', names=['59-sa-action-grant.txt'])
    put('v.a11y_service', 'Accessibility Service 存活', m.group(1) if m else '未找到', src,
        'PASS' if m else 'ABSENT', 'evidence/59 [12] 侦察段 ps 输出含 accessibility 进程')
    m, src, _ = ev.query(r'verdict=(TREE-OK-WITH-TEXT)', names=['60-s7-1a-a11y-probe.txt'])
    put('m.ReadScreen.probe', '独立探针（不进 SA）拿树', m.group(1) if m else '未找到', src,
        'PASS' if m else 'ABSENT', 'evidence/60 探针 verdict 行')
    m, src, _ = ev.query(r'\[--screen \d+\] ErrCode=201')
    put('m.ReadScreen.denied', 'ReadScreen 拒绝面（准入）', 'ErrCode=201' if m else '未找到', src,
        'PASS' if m else 'ABSENT', '白名单不含调用方 → 201（主准入层实测）')
    shot = ev.query(r'\[15\]')[0] is not None
    png = ev.count(r'lut_screen\.png')
    put('v.screenshot', '真屏截图段 [15]',
        f'[15] 段={shot} · lut_screen.png 提及 {png} 次', 'evidence/*（grep）',
        'PASS' if (shot and png) else 'ABSENT',
        '需要 evidence 同时出现 [15] 段与落盘 lut_screen.png（参数已修，待重跑）')

    # 聚合与漂移在 aggregate_facts() 里算（必须在 a.* / repo.* 事实合并之后）

    # ── 文档/证据一致性（每项当场验证）
    drift = []
    if not (EVID / '62-real-screen-capture.txt').exists():
        drift.append('evidence/62 被文档引用但不存在（截图段 [15] 未落证据）')
    n_methods = len(re.findall(r'^\s*(?:String|void)\s+\w+\s*\(', read_bytes(SA / 'component' / 'ILutSa.idl'), re.M))
    m6 = re.search(r'(\d+)\s*个方法', read_bytes(SA / 'component' / 'README.md'))
    if m6 and int(m6.group(1)) != n_methods:
        drift.append(f'component/README.md 写"{m6.group(1)} 个方法"，IDL 实为 {n_methods} 个')
    l13b = [l for l in read_bytes(EVID / '61-s7-1b-read-screen.txt').split('\n') if 'no window matched' in l]
    if l13b:
        drift.append('取证脚本 [13b] 硬编码窗口号过期 → ' + l13b[0].strip()[:56])
    if n_model == 0:
        drift.append('模型分类路径 0 次被采纳（source:model = 0）')
    n45 = len(list(EVID.glob('45-*.txt')))
    if n45 > 1:
        drift.append(f'证据编号撞车：45-* 共 {n45} 份')
    put('d.drift', '文档/证据一致性漂移', f'{len(drift)} 项', '程序化检查（见 basis）',
        'ABSENT' if not drift else 'PARTIAL', '；'.join(drift) if drift else '无漂移')
    return F


def aggregate_facts(F):
    """聚合能力与文档漂移 —— 必须在所有事实（含 a.* / repo.* / v.*）合并之后再调用。"""
    def agg(key, label, subs, note):
        sts = [F[k]['status'] for k in subs if k in F]
        if sts and all(x == 'PASS' for x in sts):
            s = 'PASS'
        elif sts and all(x == 'ABSENT' for x in sts):
            s = 'ABSENT'
        elif 'PASS' in sts and 'ABSENT' in sts:
            s = 'PARTIAL'
        elif sts:
            s = sorted(sts, key=lambda x: RANK[x])[-1]
        else:
            s = 'ABSENT'
        F[key] = dict(label=label, value=f'子项 {len(sts)}：' + '/'.join(sts), source='（聚合事实，见 basis）',
                      status=s,
                      basis=f'{note}；聚合规则：全 PASS→PASS，全 ABSENT→ABSENT，混合→PARTIAL，否则取最弱者')

    agg('cap.ipc', 'IPC Service', ['m.NativeVersion', 'm.LoadModel', 'm.Generate', 'm.GetMetrics',
                                   'm.SelfTest', 'm.Release', 'm.ReadScreen'], '六个推理/只读方法 + 读屏均 ErrCode=0')
    agg('cap.llm', 'LLM Inference', ['r.engine', 'm.LoadModel', 'm.Generate', 'm.SelfTest'],
        '引擎就绪 + 真加载 + 真出 token + 内核暖机 PASS')
    agg('cap.exec', 'Action Interface', ['m.ExecuteAction'], '设备执行维度')
    agg('cap.policy', 'Security Policy', ['p.allow_uids', 'p.quota', 'p.actions_allow', 'p.intents'],
        '四道门均有 0 与 201/22 双面实测')
    agg('cap.perception', 'Screen Perception', ['m.ReadScreen'], '读屏实测')
    agg('cap.understanding', 'Understanding', ['m.ExecuteIntent', 'm.ExecuteIntent.model'],
        '关键词路径通、模型路径从未被采纳')
    agg('cap.ai_foundation', 'AI Foundation',
        ['m.ReadScreen', 'm.ExecuteIntent', 'm.ExecuteAction', 'a.planner'], '感知+意图+动作契约齐备、决策层缺失')
    agg('cap.agent_runtime', 'Agent Runtime', ['a.planner', 'a.closed_loop'], '决策与闭环')
    return F


def foundation_facts():
    F = {}
    for key, label, pat in [
        ('a.planner', 'Planner（多步规划/状态机）', r'\bPlanner\b|plan_next|next_action|planning_loop'),
        ('a.closed_loop', '闭环（动作后重新观察）', r'ReReadScreen|verify_screen|closed_loop|act_then_observe'),
        ('a.gesture', '元素级动作/手势注入', r'InjectGesture|->ExecuteAction\(\s*elem|\.ExecuteAction\(\s*elem'),
        ('a.coord_input', '坐标/按键注入消费点', r'INJECT_INPUT_EVENT'),
        ('a.multisession', '多会话（会话表/并发）', r'multi_session|SessionTable|session_table|sessions\['),
        ('a.selfdescribe', 'SA 自述接口（GetCapabilities/GetStats）', r'GetCapabilities|GetStats'),
    ]:
        hits = code_hits(pat)
        F[key] = dict(label=label, value=f'源码命中 {len(hits)} 处',
                      source=(hits[0] if hits else 'ohos/sa/**（正则扫描）'),
                      status='PASS' if hits else 'ABSENT',
                      basis=f'正则 {pat!r} 在 ohos/sa 源码命中 {len(hits)} 次 → {"有实现" if hits else "无实现"}')
    return F


def submodule_facts():
    dot = REPO / '3rdparty' / 'llama.cpp' / '.git'
    gitdir = None
    if dot.is_file():
        m = re.search(r'gitdir:\s*(.+)', read_bytes(dot))
        if m:
            gitdir = (dot.parent / m.group(1).strip()).resolve()
    elif dot.is_dir():
        gitdir = dot
    commit, refs = None, []
    if gitdir and (gitdir / 'HEAD').exists():
        head = read_bytes(gitdir / 'HEAD').strip()
        commit = head.split()[-1] if head.startswith('ref:') else head
        if head.startswith('ref:'):
            rp = gitdir / head.split(' ', 1)[1].strip()
            if rp.exists():
                commit = read_bytes(rp).strip()
        if (gitdir / 'refs').exists():
            for r in (gitdir / 'refs').rglob('*'):
                if r.is_file() and commit and commit[:40] in read_bytes(r):
                    refs.append(str(r.relative_to(gitdir)))
        pr = gitdir / 'packed-refs'
        if pr.exists() and commit and commit[:40] in read_bytes(pr):
            refs.append('packed-refs')
    return {'repo.submodule': dict(
        label='3rdparty/llama.cpp（引擎源码可复现性）',
        value=f'HEAD={commit[:12] if commit else "?"} · 可达 ref {len(refs)} 个',
        source='3rdparty/llama.cpp/.git → HEAD + refs（程序化比对）',
        status='PASS' if refs else 'PARTIAL',
        basis=('HEAD 可在本地 ref 中找到 → 可复现' if refs else
               'HEAD 不在任何本地 ref 中（detached）→ 重新克隆无法复现当前引擎源码'))}


def static_meta():
    idl = read_bytes(SA / 'component' / 'ILutSa.idl')
    cfg = read_bytes(SA / 'component' / 'etc' / 'init' / 'lut_sa.cfg')
    gn = read_bytes(SA / 'intree' / 'vendor' / 'ohemu' / 'lutsa' / 'BUILD.gn')
    libs = re.findall(r'libs\s*=\s*\[(.*?)\]', gn, re.S)
    ext = re.findall(r'external_deps\s*=\s*\[(.*?)\]', gn, re.S)
    m_perms = re.search(r'"permission"\s*:\s*\[(.*?)\]', cfg, re.S)
    m_acls = re.search(r'"permission_acls"\s*:\s*\[(.*?)\]', cfg, re.S)
    kdir = REPO / 'ohos' / 'staging-x64' / 't-mac' / 'lib'
    return dict(
        methods=re.findall(r'^\s*(?:String|void)\s+(\w+)\s*\(([^)]*)\)', idl, re.M),
        codes=re.findall(r'(COMMAND_\w+)', read_bytes(SA / 'component' / 'idl' / 'ilut_sa.h')),
        permissions=re.findall(r'"([^"]+)"', m_perms.group(1)) if m_perms else [],
        permission_acls=re.findall(r'"([^"]+)"', m_acls.group(1)) if m_acls else [],
        libs=re.findall(r'"([^"]+)"', libs[0]) if libs else [],
        external_deps=re.findall(r'"([^"]+)"', ext[0]) if ext else [],
        kcfg_lines=len(read_bytes(kdir / 'kcfg.ini').split('\n')) if (kdir / 'kcfg.ini').exists() else 0,
        kernels_cc_bytes=(kdir / 'kernels.cc').stat().st_size if (kdir / 'kernels.cc').exists() else 0,
    )


CSS = """
:root{--bg:#0b0e14;--panel:#121821;--panel2:#0f141b;--line:#222b38;--fg:#e6edf3;--dim:#8b98a9;
--ok:#3fb950;--warn:#d29922;--ready:#58a6ff;--no:#f85149;--mono:ui-monospace,SFMono-Regular,Consolas,"Liberation Mono",monospace}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:13.5px/1.55 "Segoe UI","Microsoft YaHei",system-ui,sans-serif}
a{color:var(--ready);text-decoration:none}a:hover{text-decoration:underline}
.wrap{display:grid;grid-template-columns:216px 1fr;min-height:100vh}
nav{background:var(--panel2);border-right:1px solid var(--line);padding:14px 10px;position:sticky;top:0;height:100vh;overflow:auto}
nav .brand{font-family:var(--mono);font-size:12px;color:var(--dim);margin-bottom:6px}
nav .brand b{color:var(--fg);display:block;font-size:15px;letter-spacing:.3px;margin-bottom:2px}
nav ol{list-style:none;margin:12px 0 0;padding:0;counter-reset:p}
nav li{counter-increment:p;margin:2px 0}
nav li a{display:block;padding:6px 8px;border-radius:6px;color:var(--fg);font-size:12.5px}
nav li a:before{content:"P" counter(p) "  ";font-family:var(--mono);color:var(--dim);font-size:11px}
nav li a.on{background:#1b2230;outline:1px solid var(--line)}
nav .mode{margin-top:14px;padding:8px;border:1px solid var(--line);border-radius:6px;font-family:var(--mono);font-size:11px;color:var(--dim);line-height:1.5}
main{padding:20px 24px 60px;max-width:1520px}
h1{font-size:19px;margin:0 0 3px;font-family:var(--mono)}
h2{font-size:13.5px;margin:26px 0 10px;border-left:3px solid var(--ready);padding-left:9px;font-family:var(--mono)}
h3{font-size:11.5px;margin:0 0 9px;color:var(--dim);font-family:var(--mono);text-transform:uppercase;letter-spacing:.7px}
.sub{color:var(--dim);font-size:12.5px;margin:0 0 16px}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px;margin:10px 0}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:12px}.grid3{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}
@media(max-width:1000px){.grid2,.grid3{grid-template-columns:1fr}}
table{width:100%;border-collapse:collapse;font-size:12.5px}
th,td{padding:5px 8px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
th{color:var(--dim);font-weight:600;font-family:var(--mono);font-size:11px;text-transform:uppercase;letter-spacing:.4px}
td.k{font-family:var(--mono);color:#cdd9e5}td.v{font-family:var(--mono);word-break:break-word}
.src{font-family:var(--mono);font-size:11px;color:var(--dim)}
.chip{display:inline-block;border-radius:4px;padding:1px 7px;font-family:var(--mono);font-size:11px;border:1px solid;white-space:nowrap}
.chip.ok{color:var(--ok);border-color:rgba(63,185,80,.5);background:rgba(63,185,80,.09)}
.chip.warn{color:var(--warn);border-color:rgba(210,153,34,.5);background:rgba(210,153,34,.09)}
.chip.ready{color:var(--ready);border-color:rgba(88,166,255,.5);background:rgba(88,166,255,.09)}
.chip.no{color:var(--no);border-color:rgba(248,81,73,.5);background:rgba(248,81,73,.09)}
.kv{display:grid;grid-template-columns:186px 1fr;gap:3px 12px;font-size:12.5px}
.kv .k{font-family:var(--mono);color:var(--dim)}.kv .v{font-family:var(--mono)}
pre{background:var(--panel2);border:1px solid var(--line);border-radius:8px;padding:10px;overflow:auto;font:12px/1.5 var(--mono);max-height:360px;margin:0}
details{background:var(--panel2);border:1px solid var(--line);border-radius:8px;padding:8px 10px;margin:6px 0}
summary{cursor:pointer;font-family:var(--mono);font-size:12.5px}
.tree{font-family:var(--mono);font-size:12px;line-height:1.55}
.tree .n{white-space:pre}.tree .t{color:#9fd1a0}.tree .c{color:var(--dim)}.tree .i{color:#7d8aa0}
.note{color:var(--dim);font-size:12px;margin:8px 0 0}
.chain{display:flex;flex-wrap:wrap;gap:8px}
.chain .node{flex:1 1 160px;background:var(--panel2);border:1px solid var(--line);border-radius:8px;padding:8px 10px;font-size:12px}
.chain .node b{display:block;font-family:var(--mono);font-size:12.5px;margin-bottom:3px}
.chain .arrow{align-self:center;color:var(--dim);font-family:var(--mono)}
footer{margin-top:34px;border-top:1px solid var(--line);padding-top:12px;color:var(--dim);font-size:11px;font-family:var(--mono)}
"""

PAGES = [('index.html', 'P0', 'Architecture Overview'), ('page1_sa.html', 'P1', 'SystemAbility Overview'),
         ('page2_ipc.html', 'P2', 'IPC Capability'), ('page3_runtime.html', 'P3', 'LLM Runtime'),
         ('page4_perception.html', 'P4', 'Perception Capability'), ('page5_roadmap.html', 'P5', 'Agent Roadmap')]


def head(title, cur):
    nav = '\n'.join(f'    <li><a class="{"on" if f == cur else ""}" href="{f}">{t}</a></li>' for f, _, t in PAGES)
    return f"""<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<title>LUT-SA Capability Dashboard · {title}</title><link rel="stylesheet" href="app.css"></head><body>
<div class="wrap"><nav>
  <div class="brand"><b>LUT-SA</b>Capability Dashboard · v1</div>
  <ol>
{nav}
  </ol>
  <div class="mode">MODE: REPLAY<br>DATA: source + evidence<br>SA CODE: UNMODIFIED<br>NO LIVE / NO IPC ADDED</div>
</nav><main>"""


def foot(gen_at, note=''):
    return (f'<footer>生成于 {gen_at} · 数据 = 源码静态解析 + ohos/sa/evidence/* + lut_sa_client 既有输出 + 构建产物<br>'
            f'所有状态由 parse/grep 推导并附 basis（见 data/model.json），非人工填写 · {note}</footer></main></div></body></html>')


def chip(st):
    return f'<span class="chip {CHIP.get(st, "no")}">{st}</span>'


def kv(rows):
    return '  <div class="kv">\n' + '\n'.join(f'    <div class="k">{k}</div><div class="v">{v}</div>'
                                              for k, v in rows) + '\n  </div>'


def fact_table(F, keys):
    rows = ''.join(f'<tr><td class="k">{F[k]["label"]}</td><td class="v">{F[k]["value"]}</td>'
                   f'<td>{chip(F[k]["status"])}</td><td class="src">{F[k]["source"]}</td></tr>'
                   for k in keys if k in F)
    return ('<table><thead><tr><th>项</th><th>实测值 / 事实</th><th>状态</th><th>source</th></tr></thead>'
            f'<tbody>{rows}</tbody></table>')


def main():
    ev = Evidence()
    F = {}
    F.update(facts(ev))
    F.update(foundation_facts())
    F.update(submodule_facts())
    aggregate_facts(F)
    M = static_meta()
    OUT.mkdir(parents=True, exist_ok=True)
    DATA.mkdir(parents=True, exist_ok=True)
    (OUT / 'app.css').write_bytes(CSS.encode('utf-8'))
    gen_at = datetime.datetime.now().strftime('%Y-%m-%d %H:%M')

    screen, screen_src = None, None
    for i, line in enumerate(ev.files.get('61-s7-1b-read-screen.txt', []), 1):
        s = line.strip()
        if s.startswith('{"ok"') and '"budget":40' in s:
            screen, screen_src = json.loads(s), f'evidence/61-s7-1b-read-screen.txt:{i}'
            break
    texts = [n['text'] for n in screen['nodes'] if n['text']] if screen else []
    clicks = [n for n in screen['nodes'] if n['click']] if screen else []
    C = lambda k: chip(F[k]['status'])   # noqa: E731  —— 页面只允许这样引用状态
    V = lambda k: F[k]['value']          # noqa: E731
    S = lambda k: F[k]['source']         # noqa: E731

    # ── P0 Architecture Overview
    p0 = f"""{head('Architecture Overview', 'index.html')}
<h1>LUT-SA · OpenHarmony SystemAbility Capability Dashboard</h1>
<p class="sub">SystemAbility with embedded LLM runtime and AI capability foundation ·
REPLAY 模式（源码静态解析 + 设备实测证据 + 构建产物；未改动 SA 代码）</p>

<div class="panel"><h3>This proves</h3>
{kv([
  ('SA Host', f'SA_ID=6901 注册并由 samgr 发布；进程 lut_sa（uid system）常驻 {C("sa.publish")}'),
  ('IPC Surface', f'ILutSa {len(M["methods"])} 个方法；跨进程实测通过 {C("cap.ipc")}（执行侧 {C("cap.exec")}）'),
  ('Embedded LLM', f'llama.cpp + t-mac LUT 内核静态链入 SA 进程；真加载、真出 token {C("cap.llm")}'),
  ('AI Foundation', f'感知（ReadScreen）+ 意图解析 + 动作契约 + 四道策略门 {C("cap.ai_foundation")}'),
  ('Agent Runtime', f'Planner / 闭环 / 真实设备执行 {C("cap.agent_runtime")}'),
])}</div>

<h2>System architecture（宿主 → 设备 → SA 内部）</h2>
<div class="panel"><pre>宿主 Windows / Git Bash
  └─ wsl.exe -d ohbuild ─► WSL(root)
       ├─ install_into_tree.sh   组件源码 + 引擎源码 + dmlc shim        →  /src/ohos
       ├─ build_engine.sh        OH clang + -fexceptions                →  prebuilt/libllama_engine.a
       ├─ GN / ninja             libtmac_sa.z.so · lut_sa_client · lut_a11y_dump
       ├─ loop 注入镜像          system.img（库/客户端/探针/cfg/脚本）· userdata.img（模型/数据）
       └─ qemu_boot_lutsa.sh     QEMU x86_64_virt（serial file + monitor sock + hdc 8710）

Guest: OpenHarmony 标准系统（qemu_x86_64_linux_full）
  init ─ lut_sa.cfg ─► sa_main ─► libtmac_sa.z.so       SA 6901 · uid=system · secon=compiler_service
       │   permissions: ACCESSIBILITY_EXTENSION_ABILITY · QUERY_ACCESSIBILITY_ELEMENT · INJECT_INPUT_EVENT(reserved)
       ├─ samgr        注册 + Publish                                    [{F['sa.publish']['status']}]
       ├─ IPC surface  ILutSa x9（手写 proxy/stub 契约）                  [{F['cap.ipc']['status']} / exec {F['cap.exec']['status']}]
       ├─ LLM runtime  engine_shim(C ABI) → llama.cpp + t-mac LUT        [{F['cap.llm']['status']}]
       ├─ Perception   AccessibilityUITestAbility → a11y(801) → 元素树    [{F['cap.perception']['status']}]
       ├─ Action       AMS(180) Raw IPC @1001 → ams_try=22 → 动作 JSON    [{F['cap.exec']['status']}]
       ├─ Policy       /data/lut_sa/{{allow_uids,quota,actions_allow,intents}}  [{F['cap.policy']['status']}]
       └─ Evidence     rt_stderr.txt · rt_stdout.txt · lut_evidence{{,2}}.txt
  init ─ lut_evidence{{,2}}.cfg ─► sh ─► lut_sa_client（另一进程跨 IPC 调 6901）</pre></div>

<h2>Capability layers（Agent 分层 · 状态全部来自事实）</h2>
<div class="panel"><table><thead><tr><th>层</th><th>内容</th><th>状态</th><th>依据</th></tr></thead><tbody>
<tr><td class="k">L0</td><td class="v">设备/构建环境（QEMU 冷启动 + 双通道取证）</td><td>{C('sa.publish')}</td><td class="src">{S('sa.publish')}</td></tr>
<tr><td class="k">L1</td><td class="v">感知（ReadScreen 无障碍元素树）</td><td>{C('cap.perception')}</td><td class="src">{S('m.ReadScreen')}</td></tr>
<tr><td class="k">L2</td><td class="v">理解（模型分类 / 关键词）</td><td>{C('cap.understanding')}</td><td class="src">{V('m.ExecuteIntent.model')}</td></tr>
<tr><td class="k">L3</td><td class="v">决策（Planner / 多步）</td><td>{C('a.planner')}</td><td class="src">{F['a.planner']['basis']}</td></tr>
<tr><td class="k">L4</td><td class="v">执行（元素动作 / 坐标注入）</td><td>{C('cap.exec')}</td><td class="src">{F['m.ExecuteAction']['basis']}</td></tr>
<tr><td class="k">L5</td><td class="v">闭环 Agent</td><td>{C('cap.agent_runtime')}</td><td class="src">{F['a.closed_loop']['basis']}</td></tr>
</tbody></table></div>

<h2>Today's real chains（诚实版）</h2>
<div class="panel"><div class="chain">
<div class="node"><b>① 话语 → 动作</b>Intent → 关键词表 → 动作 JSON → 白名单门 → <b>停</b><br>{C('m.ExecuteIntent')} {C('m.ExecuteAction')}</div>
<span class="arrow">|</span>
<div class="node"><b>② 屏幕 → 结构</b>ReadScreen → 元素树 JSON → 准入门 → <b>停</b><br>{C('m.ReadScreen')}</div>
<span class="arrow">|</span>
<div class="node"><b>缺口</b>把 ①② 首尾相接（Planner + 执行 + 再观察）= Agent<br>{C('cap.agent_runtime')}</div>
</div></div>

<h2>Repo map（主线 vs 遗留）</h2>
<div class="panel"><table><thead><tr><th>路径</th><th>作用</th><th>状态</th></tr></thead><tbody>
<tr><td class="k">ohos/sa/component/</td><td class="v">SA 业务层 + IDL + GN 组件 + init/profile/sepolicy</td><td>{C('cap.ipc')}</td></tr>
<tr><td class="k">ohos/sa/engine/</td><td class="v">engine_shim（C ABI）+ gguf 准入</td><td>{C('cap.llm')}</td></tr>
<tr><td class="k">ohos/sa/intree/</td><td class="v">装树 / 编引擎 / 整链验证 / QEMU 启动</td><td>{C('sa.publish')}</td></tr>
<tr><td class="k">ohos/staging-x64|arm64/</td><td class="v">冻结 LUT 内核包（kernels.cc + kcfg.ini + 头）</td><td>{C('r.kcfg')}</td></tr>
<tr><td class="k">3rdparty/llama.cpp</td><td class="v">推理引擎源码（含 OHOS 移植本地提交）</td><td>{C('repo.submodule')}</td></tr>
<tr><td class="k">python/ · deploy/ · tools/</td><td class="v">TVM 内核生成 + 模型转换流水线（离线）</td><td>{C('cap.llm')}</td></tr>
<tr><td class="k">ohos/hap/ · ohos/hello/ · ohos/build/</td><td class="v">应用形态 HAP / P0 脚手架 / 本地冒烟产物</td><td>{C('repo.submodule')}</td></tr>
</tbody></table>
<p class="note">文档/证据漂移（{V('d.drift')}）：<span class="src">{F['d.drift']['basis'][:120]}…</span></p></div>
{foot(gen_at, 'Page 0/6')}"""

    # ── P1 SystemAbility Overview
    p1 = f"""{head('SystemAbility Overview', 'page1_sa.html')}
<h1>Page 1 · SystemAbility Overview</h1>
<p class="sub">这是系统级服务，不是普通 App：SA_ID / 进程身份 / 权限 / 运行时依赖 / 生命周期，全部来自注册文件与实测。</p>

<div class="grid2">
<div class="panel"><h3>Service identity</h3>
{kv([('service', 'LUT-SA · 端侧低比特 LLM 推理系统服务'),
     ('sa_id', '6901 <span class="src">lut_sa_ability.cpp:40,43</span>'),
     ('libpath', 'libtmac_sa.z.so <span class="src">sa_profile/lut_sa.json:5</span>'),
     ('run-on-create / auto-restart', 'true / true <span class="src">sa_profile/lut_sa.json:7-9</span>'),
     ('process', 'lut_sa <span class="src">etc/init/lut_sa.cfg:3</span>'),
     ('uid / gid', 'system / system <span class="src">lut_sa.cfg:6-7</span>'),
     ('selinux', 'u:r:compiler_service:s0 <span class="src">lut_sa.cfg:8</span>'),
     ('start-mode / once', 'boot / 0（常驻）<span class="src">lut_sa.cfg:9-10</span>')])}</div>
<div class="panel"><h3>Declared permissions（init native token）</h3>
{kv([('permission', '<br>'.join(M['permissions']))] + ([('permission_acls', '<br>'.join(M['permission_acls']) + ' <span class="src">（预留，代码未消费）</span>')] if M['permission_acls'] else []))}
<p class="note">init 按 cfg 的 permission 构造 native token（GetAccessTokenId→SetSelfTokenID）；
缺该项时 SA 内读屏返回 <span class="src">register_failed_ret_1005</span>（FIX-87 实测）。</p></div>
</div>

<h2>Runtime dependencies（链接面）</h2>
<div class="panel">{kv([('static lib', ', '.join(M['libs']) + ' <span class="src">BUILD.gn:89-97</span>'), ('external_deps', ', '.join(M['external_deps']) + ' <span class="src">BUILD.gn:63-72</span>')])}</div>

<h2>Lifecycle & IPC（实测）</h2>
<div class="panel">{fact_table(F, ['sa.publish', 'sa.samgr_insert', 'ipc.getsa', 'ipc.iface_cast'])}</div>

<h2>Capability checklist</h2>
<div class="panel"><table><thead><tr><th>能力</th><th>关键实测</th><th>状态</th><th>source</th></tr></thead><tbody>
<tr><td class="k">IPC Service</td><td class="v">{V('m.NativeVersion')} · {V('m.GetMetrics')}</td><td>{C('cap.ipc')}</td><td class="src">{S('m.NativeVersion')}</td></tr>
<tr><td class="k">LLM Inference</td><td class="v">{V('m.LoadModel')} · {V('r.engine')}</td><td>{C('cap.llm')}</td><td class="src">{S('r.engine')}</td></tr>
<tr><td class="k">Screen Perception</td><td class="v">{V('m.ReadScreen')} · nodes={screen['counts']['nodes'] if screen else '?'} texts={screen['counts']['withText'] if screen else '?'}</td><td>{C('cap.perception')}</td><td class="src">{S('m.ReadScreen')}</td></tr>
<tr><td class="k">Action Interface</td><td class="v">{V('m.ExecuteAction')}</td><td>{C('cap.exec')}</td><td class="src">{S('m.ExecuteAction')}</td></tr>
<tr><td class="k">Security Policy</td><td class="v">{V('p.allow_uids')} · {V('p.quota')}</td><td>{C('cap.policy')}</td><td class="src">{S('p.allow_uids')}</td></tr>
</tbody></table>
<p class="note">状态定义：PASS=有跨进程实测证据 · PARTIAL=部分路径可用 · READY=接口完备但端到端动作未发生 · ABSENT=无实现/无证据。</p></div>
{foot(gen_at, 'Page 1/6')}"""

    # ── P2 IPC Capability
    rows = ''.join(
        f'<tr><td class="k">{name}</td><td class="v">{F.get(f"m.{name}", {}).get("value", "—")}</td>'
        f'<td>{chip(F.get(f"m.{name}", {}).get("status", "ABSENT"))}</td>'
        f'<td class="src">{F.get(f"m.{name}", {}).get("source", "—")}</td></tr>'
        for name, _ in M['methods'])
    details = ''.join(
        f'<details><summary>{name}({args}) <span class="src">{F.get(f"m.{name}", {}).get("source", "")}</span></summary>'
        f'<pre>请求：InterfaceToken + {args or "（无参数）"}\n应答：errCode{"（仅 errCode）" if name == "LoadModel" else " + result"}\n'
        f'状态依据：{F.get(f"m.{name}", {}).get("basis", "见总表")}</pre></details>'
        for name, args in M['methods'])
    p2 = f"""{head('IPC Capability', 'page2_ipc.html')}
<h1>Page 2 · IPC Capability（ILutSa）</h1>
<p class="sub">描述符 <span class="src">u"OHOS.LutSa.ILutSa"</span>（idl/ilut_sa.h:27）· 事务码枚举 <span class="src">idl/ilut_sa.h:11-23</span> ·
应答 = Int32 errCode（成功才追加 String16）。共 {len(M['methods'])} 个方法。</p>
<div class="panel"><table><thead><tr><th>Method</th><th>实测值 / 事实</th><th>状态</th><th>source</th></tr></thead>
<tbody>{rows}</tbody></table>
<p class="note">ExecuteAction 两个维度：<b>决策契约</b>={F['m.ExecuteAction']['status']}（策略与 JSON 契约实测）·
<b>设备执行</b>=ams_try {F['m.ExecuteAction']['value'].split('ams_try=')[-1]}（未发生）。</p></div>

<h2>Call chain（跨进程）</h2>
<div class="panel"><pre>lut_sa_client.cpp:99-123   samgr-&gt;GetSystemAbility(6901) + iface_cast
        ↓  (binder / IPC)
lut_sa_stub.cpp:17-140     ReadInterfaceToken 校验 → 按事务码分派
        ↓
lut_sa_ability.cpp:266+    InferAllowed 准入 → 业务实现
        ↓
lut_sa.cpp / lut_screen.cpp / engine_shim.cc</pre></div>

<h2>方法详情</h2>
{details}

<h2>准入策略（四道门）</h2>
<div class="panel">{fact_table(F, ['p.allow_uids', 'p.quota', 'p.actions_allow', 'p.intents'])}</div>
{foot(gen_at, 'Page 2/6')}"""

    # ── P3 LLM Runtime
    loads = ev.query_all(r'\[LoadModel\((?P<p>[^)]*)\)\] ErrCode=(?P<e>\d+)(?:\s*\((?P<ms>[\d.]+) ms\))?',
                         names=['61-s7-1b-read-screen.txt', '42-sa-real-inference-sta3.txt'])[:8]
    load_rows = ''.join(
        f'<tr><td class="src">{m.group("p").split("/")[-1]}</td><td class="v">{m.group("e")}</td>'
        f'<td class="v">{m.group("ms") or "—"}</td><td class="src">{src}</td></tr>' for m, src in loads)
    gens = ev.query_all(r'\[Generate#(\d+)\(([^)]*)\)\] ErrCode=(\d+)(?:\s*->\s*(.*))?',
                        names=['42-sa-real-inference-sta3.txt'])[:4]
    gen_rows = ''.join(
        f'<tr><td class="k">#{m.group(1)}</td><td class="v">{m.group(2)}</td><td class="v">{m.group(3)}</td>'
        f'<td class="v">{(m.group(4) or "").strip()[:40] or "—"}</td><td class="src">{src}</td></tr>'
        for m, src in gens)
    parts = read_bytes(EVID / '61-s7-1b-read-screen.txt').split('[shim]')
    engine_log = ('[shim]' + parts[-1])[:1600] if len(parts) > 1 else '（未找到引擎日志片段）'
    p3 = f"""{head('LLM Runtime', 'page3_runtime.html')}
<h1>Page 3 · LLM Runtime（SA 进程内）</h1>
<p class="sub">LLM 不是外部 API：llama.cpp 与 t-mac LUT 内核以<b>静态库</b>编入 SA 进程（libtmac_sa.z.so）；
加载 / 推理 / 释放全在 SA 内完成，证据是进程内 GetMetrics 与引擎日志。</p>

<div class="grid2">
<div class="panel"><h3>Backend</h3>{kv([
 ('backend', 'llama.cpp（源：3rdparty/llama.cpp）<span class="src">build_engine.sh:77-83</span>'),
 ('compiler', 'OH 树自带 clang · target x86_64-linux-ohos <span class="src">build_engine.sh:22-43</span>'),
 ('flags', '-fexceptions（C 与 C++）· -O2 -fPIC -DGGML_USE_TMAC <span class="src">build_engine.sh:60-99</span>'),
 ('link', f'{", ".join(M["libs"])} <span class="src">BUILD.gn:89-97</span>')])}</div>
<div class="panel"><h3>Optimization · t-mac LUT</h3>{kv([
 ('kernel pair', f'ohos/staging-x64/t-mac/lib/kernels.cc（{M["kernels_cc_bytes"]:,} B）+ kcfg.ini（{M["kcfg_lines"]} 行）'),
 ('kcfg sections', V('r.kcfg') + f' <span class="src">{S("r.kcfg")}</span>'),
 ('ref kernel', 'ref_kernel（便携标量，仅 SelfTest 数值锚点）<span class="src">ref_kernel/lut_kernel_ref.cpp:135-182</span>'),
 ('dispatch', '精确形状匹配 (M×bits,k,n,b) <span class="src">ref_kernel/t-mac/tmac_gemm_wrapper.h:25-44</span>')])}</div>
</div>

<div class="grid3">
<div class="panel"><h3>Model</h3>{kv([('format', 'GGUF · mmap'), ('path', '/data/local/tmp/model.gguf'), ('size', '1,012,845,664 B（仓外）')])}</div>
<div class="panel"><h3>Context</h3>{kv([('n_ctx / batch', '512 / 512 <span class="src">lut_sa.h:18-23</span>'), ('threads', '4 <span class="src">engine_shim.cc:260-279</span>'), ('KV / flash_attn', 'f16 / false')])}</div>
<div class="panel"><h3>Measured</h3>{kv([('engine', V('r.engine')), ('peak RSS', V('r.rss')), ('load diag', V('r.load_diag'))])}</div>
</div>

<h2>Load sequence（客户端 wall time）</h2>
<div class="panel"><table><thead><tr><th>模型</th><th>ErrCode</th><th>ms</th><th>source</th></tr></thead>
<tbody>{load_rows or '<tr><td colspan=4>未找到</td></tr>'}</tbody></table>
<p class="note">ErrCode=22 的样本是<b>预期拒绝</b>（形状表不匹配 / 坏路径 / 超配额）——见 Admission。</p></div>

<h2>Inference（真出 token）</h2>
<div class="panel"><table><thead><tr><th>#</th><th>场景</th><th>ErrCode</th><th>输出</th><th>source</th></tr></thead>
<tbody>{gen_rows or '<tr><td colspan=5>未找到</td></tr>'}</tbody></table>
{fact_table(F, ['m.Generate.det', 'm.Generate.nonconst'])}</div>

<h2>Admission（加载前拦截 · 不打死 SA）</h2>
<div class="panel">{fact_table(F, ['r.admission', 'r.diag_decode'])}</div>

<h2>Engine log（进程内可观测性）</h2>
<div class="panel"><pre>{engine_log}</pre>
<p class="note">来源：SA 进程 stderr（/data/lut_sa/rt_stderr.txt，随证据包回收）。</p></div>

<h2>未测字段（如实列出）</h2>
<div class="panel"><table><thead><tr><th>字段</th><th>现状</th><th>限制</th></tr></thead><tbody>
<tr><td class="k">TTFT / tok/s</td><td class="v">未测量（仅可由客户端 wall time 与 nPredict 派生）</td><td class="src">需引擎内置计时（LIVE 阶段）</td></tr>
<tr><td class="k">库体积 / llama_* 符号数</td><td class="v">构建自证存在，但落在宿主 harness 日志（仓外 wsl/sta3.txt）</td><td class="src">需把自证行落进 evidence</td></tr>
<tr><td class="k">中文分词正确性</td><td class="v">diag 把非 ASCII 统一打成 "?"，无法判定</td><td class="src">engine_shim.cc:182-186 打印行为</td></tr>
</tbody></table></div>
{foot(gen_at, 'Page 3/6')}"""

    # ── P4 Perception
    def tree_html(d):
        if not d:
            return '<p class="note">未找到元素树 JSON</p>'
        out, cur = [], None
        for n in d['nodes']:
            if n['win'] != cur:
                cur = n['win']
                out.append(f'<div class="n i">Window #{cur}</div>')
            pad = '  ' * (n['d'] // 2 + 1)
            label = n['text'] or f'<span class="c">{n["type"]}</span>'
            flags = (['clickable'] if n['click'] else []) + ([] if n['vis'] else ['invisible'])
            out.append(f'<div class="n">{pad}<span class="t">{label}</span> '
                       f'<span class="i">{n["type"]}</span> <span class="c">{n["box"]}'
                       f'{("  [" + ", ".join(flags) + "]") if flags else ""}</span></div>')
        return '\n'.join(out)

    p4 = f"""{head('Perception Capability', 'page4_perception.html')}
<h1>Page 4 · Perception Capability（ReadScreen）</h1>
<p class="sub">不是 OCR：读的是系统无障碍<b>元素树</b>（结构 + 文本 + 可点 + 边界），返回有界结构化 JSON。
隐私：屏幕原文不写日志，只回给通过准入的调用方。</p>

<div class="panel"><h3>Pipeline</h3><div class="chain">
<div class="node"><b>System Screen</b>1024×768 锁屏<br><span class="src">{V('v.screenshot')}</span> {C('v.screenshot')}</div>
<span class="arrow">→</span>
<div class="node"><b>Accessibility Service</b>SA 801 存活<br><span class="src">{V('v.a11y_service')}</span> {C('v.a11y_service')}</div>
<span class="arrow">→</span>
<div class="node"><b>Element Tree</b>AccessibilityUITestAbility<br><span class="src">lut_screen.cpp:142-177</span> {C('m.ReadScreen')}</div>
<span class="arrow">→</span>
<div class="node"><b>Structured JSON</b>有界 + 计数<br><span class="src">lut_screen.cpp:244-257</span> {C('m.ReadScreen')}</div>
</div></div>

<div class="grid2">
<div class="panel"><h3>Metrics（一次真实调用）</h3>
{kv([('verdict', f'ok={screen["ok"]} · connected={screen["connected"]} · user={screen["user"]}' if screen else '—'),
     ('counts', str(screen['counts']) if screen else '—'),
     ('latency', f'首调 {screen["elapsed_ms"]} ms（含建连）/ 后续 29–32 ms' if screen else '—'),
     ('windows', (f'{len(screen["windows"])} 个：' + ', '.join(f'{w["id"]}(t{w["type"]},L{w["layer"]})' for w in screen['windows'])) if screen else '—'),
     ('budget/truncated', f'{screen["budget"]} / {screen["truncated"]}' if screen else '—'),
     ('texts', ' · '.join(texts) or '—')])}
<p class="note">准入双证：白名单不含调用方 → <b>201</b>（{V('m.ReadScreen.denied')}）；默认档位 → <b>0</b>。
有界性：budget=8 → nodes=8 / truncated=1。</p></div>
<div class="panel"><h3>Limits（代码注释 + 实测）</h3>
<table><tbody>
<tr><td class="k">通道语义</td><td class="v">AccessibilityUITestAbility（测试框架口子；生产应换正式无障碍扩展）</td></tr>
<tr><td class="k">连接</td><td class="v">进程级单例；首次调用建连（~0.5–0.7s）</td></tr>
<tr><td class="k">只读</td><td class="v">不调用任何注入类 API（{V('a.coord_input')}）</td></tr>
<tr><td class="k">硬上限</td><td class="v">默认 40 / 硬顶 200 节点 · 深度 8 · 每层 40 父节点 · 文本 160B</td></tr>
<tr><td class="k">已知坑</td><td class="v">顶层窗口 4 层内全容器（必须下钻）；活动窗口不可重复遍历（FIX-88）</td></tr>
</tbody></table></div>
</div>

<h2>Element tree（真实返回结构，缩进 = 深度）</h2>
<div class="panel"><div class="tree">{tree_html(screen)}</div>
<p class="note">clickable = {len(clicks)} 个：{', '.join(f'{c["type"]}(#{c["a11yId"]})' for c in clicks) or '—'}</p></div>

<h2>JSON contract（原样）</h2>
<div class="panel"><pre>{json.dumps(screen, ensure_ascii=False, indent=1)[:2400] if screen else '—'}</pre>
<p class="note">source: {screen_src or '—'}</p></div>

<h2>Perception 相关实测（含拒绝面与探针）</h2>
<div class="panel">{fact_table(F, ['m.ReadScreen', 'm.ReadScreen.probe', 'm.ReadScreen.denied'])}</div>
{foot(gen_at, 'Page 4/6')}"""

    # ── P5 Roadmap
    p5 = f"""{head('Agent Roadmap', 'page5_roadmap.html')}
<h1>Page 5 · Agent Runtime Roadmap</h1>
<p class="sub">四档分列，不伪装进度：<b>Completed</b> 必须有跨进程实测证据；<b>In Progress</b> 部分可用；
<b>Next</b> 接口就位待落地；<b>Not Started</b> 以源码正则扫描命中数为证（0 = 无实现）。</p>

<div class="grid2">
<div class="panel"><h3>Completed</h3><table><tbody>
<tr><td class="k">SystemAbility Host</td><td class="v">{V('sa.publish')}</td><td>{C('sa.publish')}</td></tr>
<tr><td class="k">IPC Service</td><td class="v">ILutSa ×{len(M['methods'])} · {V('ipc.getsa')}</td><td>{C('cap.ipc')}</td></tr>
<tr><td class="k">LLM Runtime</td><td class="v">{V('r.engine')}</td><td>{C('cap.llm')}</td></tr>
<tr><td class="k">Perception</td><td class="v">{V('m.ReadScreen')}</td><td>{C('cap.perception')}</td></tr>
<tr><td class="k">Security Policy</td><td class="v">{V('p.allow_uids')}</td><td>{C('cap.policy')}</td></tr>
</tbody></table></div>
<div class="panel"><h3>In Progress</h3><table><tbody>
<tr><td class="k">理解层</td><td class="v">{V('m.ExecuteIntent.model')}</td><td>{C('cap.understanding')}</td></tr>
<tr><td class="k">动作层</td><td class="v">{V('m.ExecuteAction')}</td><td>{C('cap.exec')}</td></tr>
<tr><td class="k">文档/证据一致性</td><td class="v">{V('d.drift')}</td><td>{C('d.drift')}</td></tr>
</tbody></table></div>
</div>

<div class="grid2">
<div class="panel"><h3>Next（接口就位待落地）</h3><table><tbody>
<tr><td class="k">元素级动作 / 手势注入</td><td class="v">{V('a.gesture')}</td><td>{C('a.gesture')}</td></tr>
<tr><td class="k">真屏截图证据段 [15]</td><td class="v">{V('v.screenshot')}</td><td>{C('v.screenshot')}</td></tr>
<tr><td class="k">SA 自述接口</td><td class="v">{V('a.selfdescribe')}</td><td>{C('a.selfdescribe')}</td></tr>
</tbody></table></div>
<div class="panel"><h3>Not Started（正则命中 = 0）</h3><table><tbody>
<tr><td class="k">Planner</td><td class="v">{V('a.planner')}</td><td>{C('a.planner')}</td></tr>
<tr><td class="k">闭环（动作后重新观察）</td><td class="v">{V('a.closed_loop')}</td><td>{C('a.closed_loop')}</td></tr>
<tr><td class="k">多会话 / 并发</td><td class="v">{V('a.multisession')}</td><td>{C('a.multisession')}</td></tr>
</tbody></table></div>
</div>

<h2>Agent chain（每条边的状态都来自证据）</h2>
<div class="panel"><div class="chain">
<div class="node"><b>Intent</b>话语 / 意图<br>{C('cap.understanding')}<br><span class="src">{V('m.ExecuteIntent.model')}</span></div>
<span class="arrow">→</span>
<div class="node"><b>Planner</b>下一步决策<br>{C('a.planner')}<br><span class="src">{V('a.planner')}</span></div>
<span class="arrow">→</span>
<div class="node"><b>Action</b>动作契约<br>{C('cap.exec')}<br><span class="src">{V('m.ExecuteAction')}</span></div>
<span class="arrow">→</span>
<div class="node"><b>SA</b>SystemAbility<br>{C('cap.ipc')}<br><span class="src">6901 · {len(M['methods'])} 方法</span></div>
<span class="arrow">→</span>
<div class="node"><b>Device</b>真实界面变化<br>{C('cap.agent_runtime')}<br><span class="src">{V('a.closed_loop')}</span></div>
</div>
<p class="note">今天两条半链各自通、中间断开：① 话语→动作 JSON→（白名单门）→停；② 读屏→元素树 JSON→（准入门）→停。首尾相接才是 Agent。</p></div>

<h2>Runbook（复现这一页的证据）</h2>
<div class="panel"><pre>bash ohos/sa/intree/install_into_tree.sh /src/ohos &lt;repo&gt;/ohos/sa        # 装树（幂等）
bash ohos/sa/intree/build_engine.sh /src/ohos &lt;tree&gt;/vendor/ohemu/lutsa # 编引擎静态库
ninja -w dupbuild=warn -C &lt;out&gt; ohemu/lutsa/libtmac_sa.z.so lut_sa_client
bash ohos/sa/intree/sta3_verify.sh                                      # 注镜像 + 冷启动 + 打包证据
python3 ohos/sa/dashboard/gen_dashboard.py                             # 重新生成本页</pre>
<p class="note">每个事实的 basis 见 <span class="src">ohos/sa/dashboard/data/model.json</span>。</p></div>
{foot(gen_at, 'Page 5/6')}"""

    pages = {'index.html': p0, 'page1_sa.html': p1, 'page2_ipc.html': p2,
             'page3_runtime.html': p3, 'page4_perception.html': p4, 'page5_roadmap.html': p5}
    for name, doc in pages.items():
        (OUT / name).write_bytes(doc.encode('utf-8'))

    (DATA / 'model.json').write_bytes(json.dumps(dict(
        generated_at=gen_at, mode='REPLAY', sa_code_modified=False, ipc_added=False, live=False,
        facts=F,
        static=dict(methods=M['methods'], permissions=M['permissions'], permission_acls=M['permission_acls'],
                    libs=M['libs'], external_deps=M['external_deps'], kcfg_lines=M['kcfg_lines'],
                    kernels_cc_bytes=M['kernels_cc_bytes']),
        perception=dict(source=screen_src, screen=screen),
        evidence_index=[dict(file=p.name, bytes=p.stat().st_size, lines=len(ev.files[p.name]))
                        for p in sorted(EVID.glob('*.txt'))],
    ), ensure_ascii=False, indent=1).encode('utf-8'))

    st = {}
    for f in F.values():
        st[f['status']] = st.get(f['status'], 0) + 1
    print(f'已生成 {len(pages)} 页 → {OUT}')
    print('  状态分布（全部由规则推导）：' + ' · '.join(f'{k}={v}' for k, v in sorted(st.items())))
    print(f'  事实 {len(F)} 条 → {DATA / "model.json"}')
    print(f'  元素树 nodes={len(screen["nodes"]) if screen else 0} texts={len(texts)} clickable={len(clicks)}')


if __name__ == '__main__':
    main()
