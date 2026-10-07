// planner_rule.h —— S7-2-A/S7-3-A：规则 Planner（不接模型，source=rule）
//
// 铁律：动作里的每个坐标都必须能追到 Digest 里的某个元素 box（`targetA11yId` 指向它）。
//       找不到依据就返回 fail，**绝不凭空造坐标**。
//
// S7-3-A：支持多步——`PlanSteps(goal)` 给出该目标需要几步；`Decide(goal, stepIndex, digest)`
//         按**当前屏幕状态 + 步序**给出这一步的动作（状态驱动，不是盲目照顺序发）。
#ifndef OHOS_LUTSA_AGENT_PLANNER_RULE_H
#define OHOS_LUTSA_AGENT_PLANNER_RULE_H

#include <cstdint>
#include <string>

#include "agent/screen_digest.h"

namespace tmac_sa {
namespace agent {

// 本步的期望结果（planner 声明、verifier 校验）——用字符串便于进 trace
constexpr const char *kExpectUnlocked = "unlocked";          // 已离开锁屏
constexpr const char *kExpectPanelPresent = "panel_present"; // 通知栏已展开
constexpr const char *kExpectPanelAbsent = "panel_absent";   // 通知栏已收起

struct Action {
    std::string kind;          // swipe | click | back | done | fail
    // swipe 用（坐标来自 target 元素 box 内的两个点）
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0, durMs = 300;
    // click 用
    int x = 0, y = 0;
    int64_t targetA11yId = -1; // 依据元素（可追溯；fail/done 为 -1）
    std::string rule;          // 命中的规则名（如 notif_open_swipe_down）
    std::string why;           // 人读依据
    std::string source = "rule";
    std::string expect;        // 本步期望（verifier 用；见上面的 kExpect*）

    std::string Describe() const;   // 一行 JSON（进 trace）
};

// 该目标需要几步（用于判断 maxSteps 够不够、以及"目标是否真的完成"）
int PlanSteps(const std::string &goal);

// 规则决策：goal + 步序 + 当前屏幕摘要 → 这一步的动作
Action Decide(const std::string &goal, int stepIndex, const Digest &d);

}  // namespace agent
}  // namespace tmac_sa
#endif  // OHOS_LUTSA_AGENT_PLANNER_RULE_H
