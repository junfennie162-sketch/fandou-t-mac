// trace.h —— S7-2-A/S7-3-A：每步证据 + 多步汇总
// 纪律与 S7-2-0 一致：记录真实返回码与前后两帧摘要；屏幕原文只进证据文件、不进日志。
#ifndef OHOS_LUTSA_AGENT_TRACE_H
#define OHOS_LUTSA_AGENT_TRACE_H

#include <string>
#include <vector>

#include "agent/executor_mmi.h"
#include "agent/planner_rule.h"
#include "agent/screen_digest.h"
#include "agent/verifier.h"

namespace tmac_sa {
namespace agent {

// 一步的完整证据（用户要求：step_id / before / action / after / verdict 全在）
struct StepTrace {
    int stepId = 0;
    Digest before;
    Action action;
    std::string policyNote;
    bool policyAllow = false;
    std::string execEvent;
    int execRet = -1;
    int execEvents = 0;
    Digest after;
    VerifyResult verify;
    int observeRetries = 0;      // 感知未就绪时的观察重试次数（只重试 observe）
    long stepMs = 0;
};

// 多步闭环的汇总
struct LoopTrace {
    std::string goal;
    int planSteps = 0;           // 该目标需要几步（planner 的 PlanSteps）
    int maxSteps = 1;            // 本次预算
    std::vector<StepTrace> steps;
    bool pass = false;
    int stoppedAt = -1;          // 失败停在第几步（-1 = 全部完成）
    std::string why;             // 汇总原因（成功/失败都写清）
    long totalMs = 0;
};

std::string TraceJson(const LoopTrace &t);   // 机器可读（有界）
std::string TraceText(const LoopTrace &t);   // 人读（进 stderr / evidence）

}  // namespace agent
}  // namespace tmac_sa
#endif  // OHOS_LUTSA_AGENT_TRACE_H
