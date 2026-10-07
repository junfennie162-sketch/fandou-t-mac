// agent_test_main.cpp —— S7-2-B：agent 模块的测试入口（**同一套 agent 代码**，独立进程）
//
// 为什么需要它：SA 只在启动时跑一次闭环（goal 从文件读，不新增 IPC），因此
//   "负例/感知不足"这两条失败路径没法在 SA 里按需触发。
// 本入口直接调用 `agent::RunGoal()` —— 与 SA 用的是**同一份 agent 模块**，
// 只是换了触发方式与进程；因此失败路径的证据同样是对真实 agent 逻辑的验证。
//
// 用法：
//   lut_agent_test --goal "unlock screen"                 全预算（默认 200 节点）
//   lut_agent_test --goal "unlock screen" --max-nodes 40   感知受限（复现 no_swipeable_area）
//   lut_agent_test --goal "open settings"                 不支持的 goal（no_rule_for_goal）
// 退出码：0 = verify PASS；1 = verify FAIL；2 = 参数错
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
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--goal") == 0 && i + 1 < argc) {
            goal = argv[++i];
        } else if (strcmp(argv[i], "--max-nodes") == 0 && i + 1 < argc) {
            maxNodes = atoi(argv[++i]);
        }
    }
    if (goal.empty()) {
        printf("用法: lut_agent_test --goal \"unlock screen\" [--max-nodes N]\n");
        return 2;
    }
    printf("[agent-test] goal=\"%s\" max_nodes=%d（0=默认 200）\n", goal.c_str(), maxNodes);
    const ::tmac_sa::agent::LoopResult r = ::tmac_sa::agent::RunGoal(goal, maxNodes);
    printf("%s", r.traceText.c_str());
    printf("[agent-test] trace_json=%s\n", r.traceJson.c_str());
    printf("[agent-test] DONE goal=\"%s\" ran=%d verify=%s\n", goal.c_str(), r.ran ? 1 : 0,
           r.pass ? "PASS" : "FAIL");
    return r.pass ? 0 : 1;
}
