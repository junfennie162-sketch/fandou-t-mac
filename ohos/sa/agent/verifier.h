// verifier.h —— S7-2-A：判定「目标是否达成」（只看感知到的界面，不看退出码）
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

VerifyResult Verify(const std::string &goal, const Digest &before, const Digest &after,
                    const Action &act);

}  // namespace agent
}  // namespace tmac_sa
#endif  // OHOS_LUTSA_AGENT_VERIFIER_H
