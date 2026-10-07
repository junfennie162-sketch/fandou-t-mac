// planner_rule.cpp —— S7-2-A/S7-3-A：规则 Planner
//
// 规则表（每条都必须落到"屏幕上的哪个元素"上，坐标从该元素 box 内取）：
//   R1 goal≈unlock screen（1 步）
//      屏幕有锁屏痕迹（文本含「上滑解锁」或窗口类型 2110）→
//        优先 type=Swiper 元素；否则**根锚定**（用感知到的根元素 box 做 78%H→22%H 上滑）
//        expect=unlocked；无痕迹 → fail(not_lock_screen)
//   R2 goal≈toggle notification panel（2 步，状态驱动）
//      通知栏 = 窗口类型 2109（S7-2-0 实测：下拉后新增 win8/type 2109，收起后消失）
//        step0 期望"展开"：已在 → done（本步已完成）；不在 → 根锚定下滑 5%H→55%H，expect=panel_present
//        step1 期望"收起"：在   → 根锚定上滑 55%H→5%H，expect=panel_absent；不在 → done
//   其余 goal → fail(no_rule_for_goal)
#include "agent/planner_rule.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>

namespace tmac_sa {
namespace agent {
namespace {

// 通知栏窗口类型（S7-2-0 实测值，evidence/66 [20b]：下拉后 windows 多出 8/type 2109）
constexpr int32_t kNotifPanelWindowType = 2109;
// 锁屏窗口类型（S7-1a/1b 实测值）
constexpr int32_t kLockScreenWindowType = 2110;

std::string Lower(const std::string &s)
{
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

bool Has(const std::string &hay, const char *needle)
{
    return hay.find(needle) != std::string::npos;
}

bool LooksLikeLockScreen(const Digest &d, const ElementRef **hintOut)
{
    const ElementRef *hint = d.FindByText("上滑解锁");
    if (hintOut != nullptr) {
        *hintOut = hint;
    }
    return hint != nullptr || d.HasWindowType(kLockScreenWindowType);
}

// 根锚定的垂直滑动：从 (cx, y1 + h*fromPct/100) 到 (cx, y1 + h*toPct/100)
// —— 坐标完全来自"感知到的根元素 box"，不是写死的屏幕坐标。
bool FillRootSwipe(const Digest &d, int fromPct, int toPct, Action &a, std::string &why)
{
    if (!d.HasRoot()) {
        why = "拿不到根元素几何（无法确定屏幕范围）→ 不乱滑";
        return false;
    }
    const int cx = (d.rootX1 + d.rootX2) / 2;
    const int h = d.rootY2 - d.rootY1;
    a.kind = "swipe";
    a.x1 = cx;
    a.y1 = d.rootY1 + h * fromPct / 100;
    a.x2 = cx;
    a.y2 = d.rootY1 + h * toPct / 100;
    a.durMs = 300;
    a.targetA11yId = d.rootA11yId;
    char buf[192];
    snprintf(buf, sizeof(buf), "根锚定：根 box=[%d,%d,%d,%d] 的 %d%%H→%d%%H 处垂直滑动（x=中线 %d）",
             d.rootX1, d.rootY1, d.rootX2, d.rootY2, fromPct, toPct, cx);
    why = buf;
    return true;
}

Action Fail(const char *rule, const std::string &why)
{
    Action a;
    a.kind = "fail";
    a.rule = rule;
    a.why = why;
    return a;
}

Action Done(const std::string &rule, const std::string &why, const char *expect)
{
    Action a;
    a.kind = "done";
    a.rule = rule;
    a.why = why;
    a.expect = expect;
    return a;
}

// ── R1：解锁（1 步）
Action PlanUnlock(int stepIndex, const Digest &d)
{
    (void)stepIndex;
    const ElementRef *hint = nullptr;
    if (!LooksLikeLockScreen(d, &hint)) {
        return Fail("not_lock_screen", "屏幕上没有锁屏痕迹（无「上滑解锁」文本、无窗口类型 2110）→ 不乱滑");
    }
    Action a;
    const ElementRef *area = d.FindByType("Swiper");
    if (area != nullptr && area->HasBox()) {
        const int inset = std::max(1, area->Height() / 8);
        a.kind = "swipe";
        a.x1 = area->CenterX();
        a.y1 = area->y2 - inset;
        a.x2 = area->CenterX();
        a.y2 = area->y1 + inset;
        a.durMs = 300;
        a.targetA11yId = area->a11yId;
        a.rule = "lock_swipe_area_type_swiper";
        char why[256];
        snprintf(why, sizeof(why), "锁屏痕迹=%s；上滑区域取自元素 %s#%" PRId64 " box=[%d,%d,%d,%d]（两端点都在框内）",
                 hint != nullptr ? "文本「上滑解锁」" : "窗口类型 2110", area->type.c_str(), area->a11yId,
                 area->x1, area->y1, area->x2, area->y2);
        a.why = why;
    } else {
        std::string why;
        if (!FillRootSwipe(d, 78, 22, a, why)) {
            return Fail("no_swipeable_area", "有锁屏痕迹但既无 Swiper 也" + why);
        }
        a.rule = "lock_swipe_root_anchored";
        a.why = "锁屏痕迹=" + std::string(hint != nullptr ? "文本「上滑解锁」" : "窗口类型 2110") +
                "；无 Swiper → " + why;
    }
    a.expect = kExpectUnlocked;
    return a;
}

// ── R2：通知栏展开/收起（2 步，状态驱动）
Action PlanNotification(int stepIndex, const Digest &d)
{
    // 前置条件：通知栏目标要求"已解锁的屏幕"。锁屏上从顶部下滑会把锁屏划走（实测），
    // 而不是展开通知栏 → 这里如实拒绝，不猜着滑（前置条件不满足就不动）。
    const ElementRef *hint = nullptr;
    if (LooksLikeLockScreen(d, &hint)) {
        return Fail("needs_unlock",
                    "通知栏目标的前置条件不满足：当前在锁屏（解锁后才有通知栏）→ 不乱滑");
    }
    const bool panelOn = d.HasWindowType(kNotifPanelWindowType);
    if (stepIndex <= 0) {   // step0：期望"展开"
        if (panelOn) {
            return Done("notif_already_open", "通知栏已经在展开状态（窗口类型 2109 在）→ 本步无需动作",
                        kExpectPanelPresent);
        }
        Action a;
        std::string why;
        if (!FillRootSwipe(d, 5, 55, a, why)) {   // 从顶部往下拉
            return Fail("notif_no_root", "要展开通知栏但" + why);
        }
        a.rule = "notif_open_swipe_down";
        a.why = "通知栏未展开（无窗口类型 2109）→ " + why;
        a.expect = kExpectPanelPresent;
        return a;
    }
    // step1：期望"收起"
    if (!panelOn) {
        return Done("notif_already_closed", "通知栏已经是收起状态（窗口类型 2109 不在）→ 本步无需动作",
                    kExpectPanelAbsent);
    }
    Action a;
    std::string why;
    if (!FillRootSwipe(d, 55, 5, a, why)) {   // 从下往上推
        return Fail("notif_no_root", "要收起通知栏但" + why);
    }
    a.rule = "notif_close_swipe_up";
    a.why = "通知栏已展开（窗口类型 2109 在）→ " + why;
    a.expect = kExpectPanelAbsent;
    return a;
}

}  // namespace

std::string Action::Describe() const
{
    char buf[640];
    if (kind == "swipe") {
        snprintf(buf, sizeof(buf),
                 "{\"action\":\"swipe\",\"from\":[%d,%d],\"to\":[%d,%d],\"dur_ms\":%d,"
                 "\"target_a11yId\":%" PRId64 ",\"rule\":\"%s\",\"source\":\"%s\",\"expect\":\"%s\","
                 "\"why\":\"%s\"}",
                 x1, y1, x2, y2, durMs, targetA11yId, rule.c_str(), source.c_str(), expect.c_str(),
                 why.c_str());
    } else if (kind == "click") {
        snprintf(buf, sizeof(buf),
                 "{\"action\":\"click\",\"point\":[%d,%d],\"target_a11yId\":%" PRId64
                 ",\"rule\":\"%s\",\"source\":\"%s\",\"expect\":\"%s\",\"why\":\"%s\"}",
                 x, y, targetA11yId, rule.c_str(), source.c_str(), expect.c_str(), why.c_str());
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"action\":\"%s\",\"rule\":\"%s\",\"source\":\"%s\",\"expect\":\"%s\",\"why\":\"%s\"}",
                 kind.c_str(), rule.c_str(), source.c_str(), expect.c_str(), why.c_str());
    }
    return buf;
}

int PlanSteps(const std::string &goal)
{
    const std::string g = Lower(goal);
    if (Has(g, "notification") || Has(goal, "通知")) {
        return 2;   // 展开 + 收起
    }
    if (Has(g, "unlock") || Has(goal, "解锁")) {
        return 1;
    }
    return 0;       // 未知目标
}

Action Decide(const std::string &goal, int stepIndex, const Digest &d)
{
    if (!d.ok) {
        return Fail("digest_not_ok", "感知失败：" + d.error);
    }
    const std::string g = Lower(goal);
    if (Has(g, "notification") || Has(goal, "通知")) {
        return PlanNotification(stepIndex, d);
    }
    if (Has(g, "unlock") || Has(goal, "解锁")) {
        return PlanUnlock(stepIndex, d);
    }
    return Fail("no_rule_for_goal", "规则表里没有这个目标（v1 支持 unlock screen / toggle notification panel）：" + goal);
}

}  // namespace agent
}  // namespace tmac_sa
