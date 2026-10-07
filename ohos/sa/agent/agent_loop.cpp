// agent_loop.cpp —— S7-2-A/S7-3-A：Agent Loop（多步）
//
// 编排（每一步都独立走完六段）：
//   observe(ReadScreenSnapshot) → digest → decide(rule, 带步序) → policy(校验) →
//   act(MMI) → settle → observe → verify(按本步期望)
// 纪律（全部来自前几轮的实测教训）：
//   · 判据是"界面确实变化"（FIX-90：受理≠生效），且要满足本步期望
//   · 坐标必须来自本次感知的元素 box；policy 再校验一次（禁止凭空坐标）
//   · 感知未就绪时可**重试观察**（有界）；但**动作绝不重试**——verify 失败即停止并如实报告
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

constexpr int kAgentMaxNodes = 200;        // agent 的快照预算（默认 40 太浅）
constexpr int kMaxObserveRetry = 6;        // 感知未就绪时的观察重试上限（有界）
constexpr int kObserveRetryDelayMs = 1000; // 每次重试间隔（ms）
constexpr int kSettleMs = 400;             // 动作后等界面稳定

struct PolicyResult {
    bool allow = false;
    std::string why;
};

// 找锚点元素：先查 elements；**根元素也算合法锚点**（它的几何确实来自感知——
// 见 ScreenSnapshot.hasRoot/rootX1..；S7-3-A 实测：根元素不在 elements 里（无文本、不可点），
// 早先 policy 因此把根锚定的动作判成"凭空坐标"而 DENY）
bool FindAnchor(const Digest &d, int64_t a11yId, ElementRef &out)
{
    for (const auto &e : d.elements) {
        if (e.a11yId == a11yId) {
            out = e;
            return true;
        }
    }
    if (d.HasRoot() && a11yId == d.rootA11yId) {
        out = ElementRef{};
        out.a11yId = d.rootA11yId;
        out.type = "root";
        out.clickable = false;
        out.visible = true;
        out.x1 = d.rootX1;
        out.y1 = d.rootY1;
        out.x2 = d.rootX2;
        out.y2 = d.rootY2;
        return true;
    }
    return false;
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
    ElementRef anchor;
    if (!FindAnchor(d, a.targetA11yId, anchor)) {
        p.why = "依据元素不在本次感知结果里（禁止凭空坐标）a11yId=" + std::to_string(a.targetA11yId);
        return p;
    }
    if (!anchor.HasBox()) {
        p.why = "依据元素没有实体框（box 为零）a11yId=" + std::to_string(anchor.a11yId);
        return p;
    }
    if (a.kind == "swipe") {
        if (!Inside(anchor, a.x1, a.y1) || !Inside(anchor, a.x2, a.y2)) {
            p.why = "滑动端点超出依据元素 box";
            return p;
        }
        p.allow = true;
        p.why = "滑动两端点都在元素 " + anchor.type + "#" + std::to_string(anchor.a11yId) + " box 内";
        return p;
    }
    if (!Inside(anchor, a.x, a.y)) {
        p.why = "点击点超出依据元素 box";
        return p;
    }
    p.allow = true;
    p.why = "点击点来自元素 " + anchor.type + "#" + std::to_string(anchor.a11yId) + " box 内";
    return p;
}

// 感知未就绪类失败（可重试**观察**；不会重试动作，因为这类决策不产生动作）
bool RetryablePerceptionFail(const Action &a)
{
    return a.kind == "fail" && (a.rule == "not_lock_screen" || a.rule == "no_swipeable_area" ||
                                a.rule == "digest_not_ok");
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

long MsSince(const std::chrono::steady_clock::time_point &t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

LoopTrace RunGoalMulti(const std::string &goal, int maxSteps, int maxNodes)
{
    LoopTrace t;
    t.goal = goal;
    t.planSteps = PlanSteps(goal);
    const int budget = (maxNodes > 0) ? maxNodes : kAgentMaxNodes;
    // 预算：显式给了就用显式的；否则用"刚好够"（PlanSteps），至少 1
    t.maxSteps = (maxSteps > 0) ? maxSteps : (t.planSteps > 0 ? t.planSteps : 1);

    const auto t0 = std::chrono::steady_clock::now();
    for (int step = 0; step < t.maxSteps; ++step) {
        StepTrace st;
        st.stepId = step;
        const auto st0 = std::chrono::steady_clock::now();

        // ① observe + ② decide（感知未就绪 → 有界重试观察）
        int retries = 0;
        for (;;) {
            st.before = BuildDigest(ReadScreenSnapshot(budget));
            st.action = Decide(goal, step, st.before);
            if (!RetryablePerceptionFail(st.action) || retries >= kMaxObserveRetry) {
                break;
            }
            ++retries;
            usleep(kObserveRetryDelayMs * 1000);
        }
        st.observeRetries = retries;

        // ③ policy
        const PolicyResult pol = CheckPolicy(st.action, st.before);
        st.policyAllow = pol.allow;
        st.policyNote = pol.why;

        // ④ act（只在 policy 放行且确有动作时；**不重试动作**）
        if (pol.allow && st.action.kind != "done" && st.action.kind != "fail") {
            ExecResult er;
            if (st.action.kind == "swipe") {
                er = InjectSwipe(st.action.x1, st.action.y1, st.action.x2, st.action.y2, st.action.durMs);
            } else if (st.action.kind == "click") {
                er = InjectClick(st.action.x, st.action.y);
            } else {
                er = InjectBack();
            }
            st.execEvent = er.event;
            st.execRet = er.ret;
            st.execEvents = er.events;
            usleep(kSettleMs * 1000);   // 等界面稳定再读
        } else if (st.action.kind == "done" || st.action.kind == "fail") {
            st.execEvent = "(无需注入：" + st.action.kind + ")";
            st.execRet = 0;
        } else {
            st.execEvent = "(policy denied，未注入)";
            st.execRet = -1;
        }

        // ⑤ observe（after）+ ⑥ verify
        st.after = BuildDigest(ReadScreenSnapshot(budget));
        st.verify = Verify(goal, step, st.before, st.after, st.action);
        st.stepMs = MsSince(st0);
        t.steps.push_back(st);

        if (!st.verify.pass) {
            t.pass = false;
            t.stoppedAt = step;
            t.why = "step " + std::to_string(step) + " verify FAIL（不自动重试动作，已停止）：" + st.verify.why;
            t.totalMs = MsSince(t0);
            return t;
        }
    }

    // 全部步都通过 → 还要检查"步数是否真的够完成目标"
    if (t.planSteps > 0 && static_cast<int>(t.steps.size()) < t.planSteps) {
        t.pass = false;
        t.stoppedAt = static_cast<int>(t.steps.size()) - 1;
        t.why = "maxSteps(" + std::to_string(t.maxSteps) + ") 不足以完成该目标（需要 " +
                std::to_string(t.planSteps) + " 步）→ 如实报告未完成";
    } else {
        t.pass = true;
        t.why = "全部 " + std::to_string(t.steps.size()) + " 步 verify PASS，目标达成";
    }
    t.totalMs = MsSince(t0);
    return t;
}

bool RunGoalFromFile(const char *goalPath, const char *tracePath)
{
    const std::string goal = ReadTrim(goalPath, 200);
    if (goal.empty()) {
        return false;   // 没有目标文件/空文件 → 不跑（默认行为不变）
    }
    const LoopTrace t = RunGoalMulti(goal, 0, 0);   // maxSteps 用 PlanSteps（刚好够）
    printf("%s", TraceText(t).c_str());
    fflush(stdout);
    if (tracePath != nullptr) {
        WriteFile(tracePath, TraceJson(t) + "\n");
    }
    return true;
}

}  // namespace agent
}  // namespace tmac_sa
