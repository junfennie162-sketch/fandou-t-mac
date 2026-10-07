// executor_mmi.h —— S7-2-A：SA 进程内的执行器（MMI 输入注入）
//
// 依据：S7-2-A0 已实测（evidence/70）——本进程调 MMI 的 SimulateInputEvent 能真的改变屏幕，
//       权限来自 init cfg 的 permission_acls: ohos.permission.INJECT_INPUT_EVENT。
// 职责：**只负责把给定坐标/按键发出去并如实返回结果**；不决策、不校验（校验在 policy）。
#ifndef OHOS_LUTSA_AGENT_EXECUTOR_MMI_H
#define OHOS_LUTSA_AGENT_EXECUTOR_MMI_H

#include <string>

namespace tmac_sa {
namespace agent {

struct ExecResult {
    int ret = -1;          // 最后一个事件的返回码（0 = 受理）
    std::string event;     // 人读描述（进 trace）
    int events = 0;        // 实际发出的事件数
};

ExecResult InjectSwipe(int x1, int y1, int x2, int y2, int durMs);
ExecResult InjectClick(int x, int y);
ExecResult InjectBack();

}  // namespace agent
}  // namespace tmac_sa
#endif  // OHOS_LUTSA_AGENT_EXECUTOR_MMI_H
