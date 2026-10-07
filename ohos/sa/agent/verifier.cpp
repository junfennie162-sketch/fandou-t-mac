// verifier.cpp —— S7-2-A/S7-3-A：按「本步期望」判定（全部基于感知对比，条件逐条写进 why）
//
// 通用纪律：即使"动作没生效"，也不能被误判成成功——每条期望都要求"目标状态成立"+"界面确实变化"。
//   · done 动作（本步无需动作，例如通知栏已经展开）：只要求"目标状态成立"，不要求变化。
#include "agent/verifier.h"

#include <cstdio>

namespace tmac_sa {
namespace agent {
namespace {

constexpr int32_t kNotifPanelWindowType = 2109;
constexpr int32_t kLockScreenWindowType = 2110;

bool LockScreen(const Digest &d, std::string *how)
{
    const bool byText = d.FindByText("上滑解锁") != nullptr;
    const bool byWin = d.HasWindowType(kLockScreenWindowType);
    if (how != nullptr) {
        *how = std::string("文本「上滑解锁」=") + (byText ? "有" : "无") + " 窗口类型2110=" +
               (byWin ? "有" : "无");
    }
    return byText || byWin;
}

bool Changed(const Digest &b, const Digest &a)
{
    return (b.windowIds != a.windowIds) || (b.withText != a.withText) || (b.visited != a.visited);
}

std::string Windows(const Digest &d)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "windows=%s visited=%d withText=%d", d.windowIds.c_str(), d.visited,
             d.withText);
    return buf;
}

}  // namespace

VerifyResult Verify(const std::string &goal, int stepIndex, const Digest &before, const Digest &after,
                    const Action &act)
{
    VerifyResult r;
    const bool isDone = (act.kind == "done");
    const bool changed = Changed(before, after);

    if (act.expect == kExpectUnlocked) {
        std::string howB, howA;
        const bool lockB = LockScreen(before, &howB);
        const bool lockA = LockScreen(after, &howA);
        r.pass = lockB && !lockA && changed;
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "期望=已解锁 → ① before 在锁屏=%d（%s）② after 已离开锁屏=%d（%s）③ 界面有变化=%d"
                 "（%s → %s）→ %s",
                 lockB ? 1 : 0, howB.c_str(), lockA ? 0 : 1, howA.c_str(), changed ? 1 : 0,
                 Windows(before).c_str(), Windows(after).c_str(), r.pass ? "PASS" : "FAIL");
        r.why = buf;
        return r;
    }
    if (act.expect == kExpectPanelPresent) {
        const bool panelA = after.HasWindowType(kNotifPanelWindowType);
        r.pass = isDone ? panelA : (panelA && changed);
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "期望=通知栏展开 → ① after 有窗口类型2109=%d ② 界面有变化=%d%s（%s → %s）→ %s",
                 panelA ? 1 : 0, changed ? 1 : 0, isDone ? "（done：不要求变化）" : "",
                 Windows(before).c_str(), Windows(after).c_str(), r.pass ? "PASS" : "FAIL");
        r.why = buf;
        return r;
    }
    if (act.expect == kExpectPanelAbsent) {
        const bool panelA = after.HasWindowType(kNotifPanelWindowType);
        r.pass = isDone ? !panelA : (!panelA && changed);
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "期望=通知栏收起 → ① after 无窗口类型2109=%d ② 界面有变化=%d%s（%s → %s）→ %s",
                 panelA ? 0 : 1, changed ? 1 : 0, isDone ? "（done：不要求变化）" : "",
                 Windows(before).c_str(), Windows(after).c_str(), r.pass ? "PASS" : "FAIL");
        r.why = buf;
        return r;
    }
    r.pass = false;
    char buf[400];
    if (act.kind == "fail") {
        // planner 如实拒绝：本步没有动作，也就没有可判定的期望 —— 直接把拒绝原因带出来
        snprintf(buf, sizeof(buf), "本步无动作（planner 如实拒绝 rule=\"%s\"）：%s",
                 act.rule.c_str(), act.why.c_str());
    } else {
        snprintf(buf, sizeof(buf), "本步没有可判定的期望（expect=\"%s\"，goal=\"%s\" step=%d）",
                 act.expect.c_str(), goal.c_str(), stepIndex);
    }
    r.why = buf;
    return r;
}

}  // namespace agent
}  // namespace tmac_sa
