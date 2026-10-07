// agent_loop.h —— S7-2-A：最小 Agent Loop（observe → digest → decide → policy → act → verify）
//
// v1 范围（按用户指示）：**单步闭环**、规则决策（不接模型）、进程内 MMI 执行、不接 IPC。
// 触发方式：SA 启动时读 /data/lut_sa/agent_goal.txt（存在才跑），把 trace 写回
//           /data/lut_sa/agent_trace.json + stderr —— 这样"跑在 SA 进程内"且**不需要新增 IPC**。
#ifndef OHOS_LUTSA_AGENT_AGENT_LOOP_H
#define OHOS_LUTSA_AGENT_AGENT_LOOP_H

#include <string>

namespace tmac_sa {
namespace agent {

struct LoopResult {
    bool ran = false;             // 是否真的执行了（goal 为空则不跑）
    std::string goal;
    std::string traceJson;
    std::string traceText;
    bool pass = false;
};

// 跑一次单步闭环。maxNodes <= 0 → 用读屏默认上限。
LoopResult RunGoal(const std::string &goal, int maxNodes);

// 便捷入口：读 /data/lut_sa/agent_goal.txt（不存在 → ran=false），跑完写
// /data/lut_sa/agent_trace.json，并把人读 trace 打到 stdout/stderr。
// 返回是否执行过。
bool RunGoalFromFile(const char *goalPath, const char *tracePath);

}  // namespace agent
}  // namespace tmac_sa
#endif  // OHOS_LUTSA_AGENT_AGENT_LOOP_H
