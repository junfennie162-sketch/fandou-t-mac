// planner_rule.cpp —— S7-2-A：规则 Planner v1（只支持 goal="unlock screen"，其余如实 fail）
//
// 规则表（每条都必须落到"屏幕上的哪个元素"上，坐标从该元素 box 内取）：
//   R1 解锁：屏幕上有锁屏痕迹（文本含「上滑解锁」或存在窗口类型 2110）
//            → 找可上滑区域（优先 type=Swiper；否则取面积最大的可点且有框元素）
//            → 在该元素 box 内做垂直上滑（起点靠下、终点靠上，都在框内）
//   其余 goal → fail（v1 不猜；把"不支持"如实写进 trace）
#include "agent/planner_rule.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>

namespace tmac_sa {
namespace agent {
namespace {

std::string Lower(const std::string &s)
{
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

// 锁屏痕迹：文本含「上滑解锁」或存在窗口类型 2110（S7-1a/1b 实测的锁屏窗口类型）
bool LooksLikeLockScreen(const Digest &d, const ElementRef **hintOut)
{
    const ElementRef *hint = d.FindByText("上滑解锁");
    if (hintOut != nullptr) {
        *hintOut = hint;
    }
    return hint != nullptr || d.HasWindowType(2110);
}

}  // namespace

std::string Action::Describe() const
{
    char buf[512];
    if (kind == "swipe") {
        snprintf(buf, sizeof(buf),
                 "{\"action\":\"swipe\",\"from\":[%d,%d],\"to\":[%d,%d],\"dur_ms\":%d,"
                 "\"target_a11yId\":%" PRId64 ",\"rule\":\"%s\",\"source\":\"%s\",\"why\":\"%s\"}",
                 x1, y1, x2, y2, durMs, targetA11yId, rule.c_str(), source.c_str(), why.c_str());
    } else if (kind == "click") {
        snprintf(buf, sizeof(buf),
                 "{\"action\":\"click\",\"point\":[%d,%d],\"target_a11yId\":%" PRId64
                 ",\"rule\":\"%s\",\"source\":\"%s\",\"why\":\"%s\"}",
                 x, y, targetA11yId, rule.c_str(), source.c_str(), why.c_str());
    } else {
        snprintf(buf, sizeof(buf), "{\"action\":\"%s\",\"rule\":\"%s\",\"source\":\"%s\",\"why\":\"%s\"}",
                 kind.c_str(), rule.c_str(), source.c_str(), why.c_str());
    }
    return buf;
}

Action Decide(const std::string &goal, const Digest &d)
{
    Action a;
    const std::string g = Lower(goal);

    if (!d.ok) {
        a.kind = "fail";
        a.rule = "digest_not_ok";
        a.why = "感知失败：" + d.error;
        return a;
    }

    // ── R1：解锁
    if (g.find("unlock") != std::string::npos || goal.find("解锁") != std::string::npos) {
        const ElementRef *hint = nullptr;
        if (!LooksLikeLockScreen(d, &hint)) {
            a.kind = "fail";
            a.rule = "not_lock_screen";
            a.why = "屏幕上没有锁屏痕迹（无「上滑解锁」文本、无窗口类型 2110）→ 不乱滑";
            return a;
        }
        // 优先用明确的"可滑区域"元素（Swiper）；找不到时**退到根锚定**——
        // 用感知到的根元素 box 做屏幕比例滑动（0.78H → 0.22H，x 取中线）。
        // 实测教训（S7-2-B boot3）：早先的"面积最大的可点元素"兜底是**猜测**，
        // 会滑在错误区域导致 verify FAIL；根锚定只依赖"屏幕多大"这个感知事实。
        const ElementRef *area = d.FindByType("Swiper");
        std::string rule = "lock_swipe_area_type_swiper";
        if (area == nullptr || !area->HasBox()) {
            if (!d.HasRoot()) {
                a.kind = "fail";
                a.rule = "no_swipeable_area";
                a.why = "有锁屏痕迹但既无 Swiper 也拿不到根元素几何（无法确定屏幕范围）→ 不乱滑";
                return a;
            }
            const int cx = (d.rootX1 + d.rootX2) / 2;
            const int h = d.rootY2 - d.rootY1;
            a.kind = "swipe";
            a.x1 = cx;
            a.y1 = d.rootY1 + h * 78 / 100;   // 起点靠下
            a.x2 = cx;
            a.y2 = d.rootY1 + h * 22 / 100;   // 终点靠上
            a.durMs = 300;
            a.targetA11yId = d.rootA11yId;
            a.rule = "lock_swipe_root_anchored";
            char why2[256];
            snprintf(why2, sizeof(why2),
                     "锁屏痕迹=%s；无 Swiper → 退到根锚定：根 box=[%d,%d,%d,%d] 的 78%%→22%% 高度处上滑（x=中线 %d）",
                     hint != nullptr ? "文本「上滑解锁」" : "窗口类型 2110", d.rootX1, d.rootY1, d.rootX2,
                     d.rootY2, cx);
            a.why = why2;
            return a;
        }
        // 关键：两个端点都取在该元素 box 内（policy 会再校验一次）
        const int inset = std::max(1, area->Height() / 8);
        a.kind = "swipe";
        a.x1 = area->CenterX();
        a.y1 = area->y2 - inset;      // 起点靠下
        a.x2 = area->CenterX();
        a.y2 = area->y1 + inset;      // 终点靠上
        a.durMs = 300;
        a.targetA11yId = area->a11yId;
        a.rule = rule;
        char why[256];
        snprintf(why, sizeof(why),
                 "锁屏痕迹=%s；上滑区域取自元素 %s#%" PRId64 " box=[%d,%d,%d,%d]（两端点都在框内）",
                 hint != nullptr ? "文本「上滑解锁」" : "窗口类型 2110", area->type.c_str(),
                 area->a11yId, area->x1, area->y1, area->x2, area->y2);
        a.why = why;
        return a;
    }

    // ── 其余 goal：v1 不猜
    a.kind = "fail";
    a.rule = "no_rule_for_goal";
    a.why = "规则表里没有这个目标（v1 只支持 unlock screen）：" + goal;
    return a;
}

}  // namespace agent
}  // namespace tmac_sa
