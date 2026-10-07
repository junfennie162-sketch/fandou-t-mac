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

// 面积最大的「可点且有实体框」元素（纯几何，来自感知）
const ElementRef *LargestClickable(const Digest &d)
{
    const ElementRef *best = nullptr;
    long bestArea = 0;
    for (const auto &e : d.elements) {
        if (!e.clickable || !e.HasBox()) {
            continue;
        }
        const long area = static_cast<long>(e.Width()) * e.Height();
        if (area > bestArea) {
            bestArea = area;
            best = &e;
        }
    }
    return best;
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
        const ElementRef *area = d.FindByType("Swiper");
        std::string rule = "lock_swipe_area_type_swiper";
        if (area == nullptr || !area->HasBox()) {
            area = LargestClickable(d);
            rule = "lock_swipe_area_largest_clickable";
        }
        if (area == nullptr || !area->HasBox()) {
            a.kind = "fail";
            a.rule = "no_swipeable_area";
            a.why = "有锁屏痕迹但找不到可上滑区域（无 Swiper、无可点且有框的元素）";
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
