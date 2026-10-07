// trace.h —— S7-2-A：每步证据（goal / before / decision / action / after / verify）
// 与 S7-2-0 的判据纪律一致：记录真实返回码与前后两帧摘要；屏幕原文只进证据文件、不进日志。
#ifndef OHOS_LUTSA_AGENT_TRACE_H
#define OHOS_LUTSA_AGENT_TRACE_H

#include <string>

#include "agent/executor_mmi.h"
#include "agent/planner_rule.h"
#include "agent/screen_digest.h"
#include "agent/verifier.h"

namespace tmac_sa {
namespace agent {

struct StepTrace {
    std::string goal;
    Digest before;
    Action action;
    std::string policyNote;      // 动作空间校验结论（allow/deny + 原因）
    bool policyAllow = false;
    std::string execEvent;       // 实际发出的注入事件描述
    int execRet = -1;
    int execEvents = 0;
    Digest after;
    VerifyResult verify;
    long totalMs = 0;
    int steps = 1;
    int observeRetries = 0;   // 感知未就绪时的观察重试次数（只重试 observe）
};

std::string TraceJson(const StepTrace &t);   // 机器可读（有界）
std::string TraceText(const StepTrace &t);   // 人读（进 stderr / evidence）

}  // namespace agent
}  // namespace tmac_sa
#endif  // OHOS_LUTSA_AGENT_TRACE_H
