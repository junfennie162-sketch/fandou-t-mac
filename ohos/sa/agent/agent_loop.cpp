// agent_loop.cpp —— S7-2-A：最小 Agent Loop（单步闭环）
//
// 编排：observe(ReadScreenSnapshot) → digest → decide(rule) → policy(校验) → act(MMI) → verify(再读屏)
// 纪律（全部来自前几轮的实测教训）：
//   · 判据是"界面确实变化"，不看退出码（FIX-90：受理≠生效）
//   · 坐标必须来自本次感知的元素 box；policy 会再校验一次（禁止凭空坐标）
//   · 失败也留证：trace 里写清停在哪一步、看到什么、返回什么
//   · 屏幕原文只进 trace（证据文件），SA 日志只记计数
#include "agent/agent_loop.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

#include "agent/executor_mmi.h"
#include "agent/planner_rule.h"
#include "agent/screen_digest.h"
#include "agent/trace.h"
#include "agent/verifier.h"

namespace tmac_sa {
namespace agent {
namespace {

// agent 的快照预算：默认 40 太浅（锁屏的 Swiper 在更深层，实测 truncated=1/clickable=0）
constexpr int kAgentMaxNodes = 200;
constexpr int kMaxObserveRetry = 6;        // 感知未就绪时的观察重试上限（有界）
constexpr int kObserveRetryDelayMs = 1000; // 每次重试间隔（ms）

struct PolicyResult {
    bool allow = false;
    std::string why;
};

const ElementRef *FindById(const Digest &d, int64_t a11yId)
{
    for (const auto &e : d.elements) {
        if (e.a11yId == a11yId) {
            return &e;
        }
    }
    return nullptr;
}

bool Inside(const ElementRef &t, int x, int y)
{
    return x >= t.x1 && x <= t.x2 && y >= t.y1 && y <= t.y2;
}

bool KindAllowed(const std::string &k)
{
    return k == "swipe" || k == "click" || k == "back" || k == "done" || k == "fail";
}

// 动作空间校验：v1 只认规则决策，且**坐标必须落在依据元素 box 内**
PolicyResult CheckPolicy(const Action &a, const Digest &d)
{
    PolicyResult p;
    if (!KindAllowed(a.kind)) {
        p.why = "动作不在白名单内：" + a.kind;
        return p;
    }
    if (a.source != "rule") {
        p.why = "v1 只接受 source=rule（不接模型）";
        return p;
    }
    if (a.kind == "back" || a.kind == "done" || a.kind == "fail") {
        p.allow = true;
        p.why = "无坐标动作（" + a.kind + "），直接放行";
        return p;
    }
    const ElementRef *t = FindById(d, a.targetA11yId);
    if (t == nullptr) {
        p.why = "依据元素不在本次感知结果里（禁止凭空坐标）a11yId=" + std::to_string(a.targetA11yId);
        return p;
    }
    if (!t->HasBox()) {
        p.why = "依据元素没有实体框（box 为零）a11yId=" + std::to_string(t->a11yId);
        return p;
    }
    if (a.kind == "swipe") {
        if (!Inside(*t, a.x1, a.y1) || !Inside(*t, a.x2, a.y2)) {
            p.why = "滑动端点超出依据元素 box";
            return p;
        }
        p.allow = true;
        p.why = "滑动两端点都在元素 " + t->type + "#" + std::to_string(t->a11yId) + " box=[…] 内";
        return p;
    }
    if (!Inside(*t, a.x, a.y)) {
        p.why = "点击点超出依据元素 box";
        return p;
    }
    p.allow = true;
    p.why = "点击点来自元素 " + t->type + "#" + std::to_string(t->a11yId) + " box 内";
    return p;
}

std::string ReadTrim(const char *path, size_t maxLen)
{
    FILE *f = fopen(path, "r");
    if (f == nullptr) {
        return "";
    }
    std::string s;
    char buf[256];
    while (fgets(buf, sizeof(buf), f) != nullptr && s.size() < maxLen) {
        s += buf;
    }
    fclose(f);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

void WriteFile(const char *path, const std::string &content)
{
    FILE *f = fopen(path, "w");
    if (f == nullptr) {
        return;
    }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
}

}  // namespace

LoopResult RunGoal(const std::string &goal, int maxNodes)
{
    LoopResult res;
    res.goal = goal;
    if (goal.empty()) {
        return res;   // 没有目标就不跑
    }
    res.ran = true;
    const auto t0 = std::chrono::steady_clock::now();

    StepTrace tr;
    tr.goal = goal;

    // ① observe（before）+ ② decide —— 若因「感知未就绪」而 fail，则有界重试**观察**
    // 实测教训（S7-2-B boot2）：锁屏出现时状态栏文本先于 Swiper 进树 → 一次 observe 可能
    // 看不到可滑区域。这是"世界还没准备好"，agent 应当等一等再观察；
    // **只重试 observe，绝不重试动作**（动作是否重试由 verify 决定，v1 一律不重试）。
    int retries = 0;
    for (;;) {
        tr.before = BuildDigest(ReadScreenSnapshot(maxNodes > 0 ? maxNodes : kAgentMaxNodes));
        tr.action = Decide(goal, tr.before);
        const bool retryable = tr.action.kind == "fail" &&
                               (tr.action.rule == "not_lock_screen" || tr.action.rule == "no_swipeable_area");
        if (!retryable || retries >= kMaxObserveRetry) {
            break;
        }
        ++retries;
        usleep(kObserveRetryDelayMs * 1000);
    }
    tr.observeRetries = retries;

    // ③ policy
    const PolicyResult pol = CheckPolicy(tr.action, tr.before);
    tr.policyAllow = pol.allow;
    tr.policyNote = pol.why;

    // ④ act（只在 policy 放行时）
    if (pol.allow && tr.action.kind != "done" && tr.action.kind != "fail") {
        ExecResult er;
        if (tr.action.kind == "swipe") {
            er = InjectSwipe(tr.action.x1, tr.action.y1, tr.action.x2, tr.action.y2, tr.action.durMs);
        } else if (tr.action.kind == "click") {
            er = InjectClick(tr.action.x, tr.action.y);
        } else {
            er = InjectBack();
        }
        tr.execEvent = er.event;
        tr.execRet = er.ret;
        tr.execEvents = er.events;
        usleep(400 * 1000);   // 等界面稳定再读（S7-2-0 的 settle 经验）
    } else if (tr.action.kind == "done" || tr.action.kind == "fail") {
        tr.execEvent = "(无需注入：" + tr.action.kind + ")";
        tr.execRet = 0;
    } else {
        tr.execEvent = "(policy denied，未注入)";
        tr.execRet = -1;
    }

    // ⑤ observe（after）+ ⑥ verify
    tr.after = BuildDigest(ReadScreenSnapshot(maxNodes > 0 ? maxNodes : kAgentMaxNodes));
    tr.verify = Verify(goal, tr.before, tr.after, tr.action);

    tr.totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0).count();
    tr.steps = 1;

    res.traceJson = TraceJson(tr);
    res.traceText = TraceText(tr);
    res.pass = tr.verify.pass;
    return res;
}

bool RunGoalFromFile(const char *goalPath, const char *tracePath)
{
    const std::string goal = ReadTrim(goalPath, 200);
    if (goal.empty()) {
        return false;   // 没有目标文件/空文件 → 不跑（默认行为不变）
    }
    const LoopResult r = RunGoal(goal, 0);
    // 人读 trace 打到 stdout（SA 的 stdout 被重定向到 /data/lut_sa/rt_stdout.txt，随证据回收）
    printf("%s", r.traceText.c_str());
    fflush(stdout);
    if (tracePath != nullptr) {
        WriteFile(tracePath, r.traceJson + "\n");
    }
    return true;
}

}  // namespace agent
}  // namespace tmac_sa
