// agent_test_main.cpp —— S7-2-B/S7-3-A：agent 模块的测试入口（**同一套 agent 代码**，独立进程）
//
// 为什么需要它：SA 只在启动时跑一次闭环（goal 从文件读，不新增 IPC），因此
//   多步/失败路径这些用例没法在 SA 里按需触发。
// 本入口直接调用 `agent::RunGoalMulti()` —— 与 SA 用的是**同一份 agent 模块**，
// 只是换了触发方式与进程；因此这些证据同样是对真实 agent 逻辑的验证。
//
// 用法：
//   lut_agent_test --goal "unlock screen"                          单步目标（maxSteps 自动=1）
//   lut_agent_test --goal "toggle notification panel"              多步目标（maxSteps 自动=2）
//   lut_agent_test --goal "toggle notification panel" --max-steps 1 预算不足 → 期望如实报告未完成
//   lut_agent_test --goal "unlock screen" --max-nodes 1            感知地板
// 退出码：0 = PASS；1 = FAIL；2 = 参数错
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "agent/agent_loop.h"

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::string goal;
    int maxNodes = 0;
    int maxSteps = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--goal") == 0 && i + 1 < argc) {
            goal = argv[++i];
        } else if (strcmp(argv[i], "--max-nodes") == 0 && i + 1 < argc) {
            maxNodes = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            maxSteps = atoi(argv[++i]);
        }
    }
    if (goal.empty()) {
        printf("用法: lut_agent_test --goal \"<goal>\" [--max-steps N] [--max-nodes N]\n");
        return 2;
    }
    printf("[agent-test] goal=\"%s\" max_steps=%d（0=按 PlanSteps）max_nodes=%d（0=默认 200）\n",
           goal.c_str(), maxSteps, maxNodes);
    const ::tmac_sa::agent::LoopTrace t = ::tmac_sa::agent::RunGoalMulti(goal, maxSteps, maxNodes);
    printf("%s", ::tmac_sa::agent::TraceText(t).c_str());
    printf("[agent-test] trace_json=%s\n", ::tmac_sa::agent::TraceJson(t).c_str());
    printf("[agent-test] DONE goal=\"%s\" steps=%zu verdict=%s\n", goal.c_str(), t.steps.size(),
           t.pass ? "PASS" : "FAIL");
    return t.pass ? 0 : 1;
}
