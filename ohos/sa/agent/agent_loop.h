// agent_loop.h —— S7-2-A/S7-3-A：Agent Loop（observe → digest → decide → policy → act → verify）
//
// v1（S7-2-A）：单步闭环、规则决策、进程内 MMI 执行、不接 IPC。
// S7-3-A：**支持 maxSteps>1**——每一步独立跑完六段并各自 verify；
//          任一步 verify 失败就**立即停止并如实报告**（**不自动重试动作**）。
// 触发方式：SA 启动时读 /data/lut_sa/agent_goal.txt（存在才跑），trace 写
//           /data/lut_sa/agent_trace.json + stdout —— 跑在 SA 进程内且**不需要新增 IPC**。
#ifndef OHOS_LUTSA_AGENT_AGENT_LOOP_H
#define OHOS_LUTSA_AGENT_AGENT_LOOP_H

#include <string>

#include "agent/trace.h"

namespace tmac_sa {
namespace agent {

// 跑一次多步闭环。maxSteps<=0 → 用 PlanSteps(goal)（即"刚好够"）；
// maxNodes<=0 → 用 agent 默认快照预算（200）。
LoopTrace RunGoalMulti(const std::string &goal, int maxSteps, int maxNodes);

// 便捷入口：读 goalPath（不存在 → ran=false），跑完写 tracePath，并把人读 trace 打到 stdout。
bool RunGoalFromFile(const char *goalPath, const char *tracePath);

}  // namespace agent
}  // namespace tmac_sa
#endif  // OHOS_LUTSA_AGENT_AGENT_LOOP_H
