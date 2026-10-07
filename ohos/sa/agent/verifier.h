// verifier.h —— S7-2-A/S7-3-A：判定「这一步是否达成」（只看感知到的界面，不看退出码）
#ifndef OHOS_LUTSA_AGENT_VERIFIER_H
#define OHOS_LUTSA_AGENT_VERIFIER_H

#include <string>

#include "agent/planner_rule.h"
#include "agent/screen_digest.h"

namespace tmac_sa {
namespace agent {

struct VerifyResult {
    bool pass = false;
    std::string why;     // 逐条件说明（哪个条件成立/不成立）
};

// 按「动作声明的期望（act.expect）」校验这一步：
//   unlocked      → before 在锁屏 && after 不在锁屏 && 界面有变化
//   panel_present → after 有通知栏窗口(2109) && 界面有变化（done 动作只要求"已在"）
//   panel_absent  → after 无通知栏窗口(2109) && 界面有变化（done 动作只要求"已不在"）
VerifyResult Verify(const std::string &goal, int stepIndex, const Digest &before, const Digest &after,
                    const Action &act);

}  // namespace agent
}  // namespace tmac_sa
#endif  // OHOS_LUTSA_AGENT_VERIFIER_H
