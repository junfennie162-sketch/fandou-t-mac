// planner_rule.h —— S7-2-A：第一版规则 Planner（不接模型，source=rule）
//
// 铁律：动作里的每个坐标都必须能追到 Digest 里的某个元素 box（`targetA11yId` 指向它）。
//       找不到依据就返回 fail，**绝不凭空造坐标**。
#ifndef OHOS_LUTSA_AGENT_PLANNER_RULE_H
#define OHOS_LUTSA_AGENT_PLANNER_RULE_H

#include <cstdint>
#include <string>

#include "agent/screen_digest.h"

namespace tmac_sa {
namespace agent {

// 动作空间（v1 封闭六种；每种都必须有依据）
struct Action {
    std::string kind;          // swipe | click | back | done | fail
    // swipe 用（坐标来自 target 元素 box 内的两个点）
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0, durMs = 300;
    // click 用
    int x = 0, y = 0;
    int64_t targetA11yId = -1; // 依据元素（可追溯；fail/done 为 -1）
    std::string rule;          // 命中的规则名（如 lock_hint_swipe）
    std::string why;           // 人读依据
    std::string source = "rule";

    std::string Describe() const;   // 一行 JSON（进 trace）
};

// 规则决策：goal + 当前屏幕摘要 → 一个动作
Action Decide(const std::string &goal, const Digest &d);

}  // namespace agent
}  // namespace tmac_sa
#endif  // OHOS_LUTSA_AGENT_PLANNER_RULE_H
