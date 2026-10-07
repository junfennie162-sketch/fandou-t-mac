// trace.cpp —— S7-2-A/S7-3-A：把多步闭环写成证据（JSON + 人读两种形态）
#include "agent/trace.h"

#include <cstdio>

namespace tmac_sa {
namespace agent {
namespace {

std::string Esc(const std::string &in, size_t maxLen = 200)
{
    std::string out;
    for (char ch : in) {
        unsigned char u = static_cast<unsigned char>(ch);
        if (u == '"' || u == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(u));
        } else if (u < 32 || u == 127) {
            out.push_back(' ');
        } else {
            out.push_back(static_cast<char>(u));
        }
    }
    if (out.size() > maxLen) {
        out.resize(maxLen);
        out += "...";
    }
    return out;
}

void StepJson(const StepTrace &s, std::string &out)
{
    char head[128];
    snprintf(head, sizeof(head), "  {\"step_id\": %d,\n", s.stepId);
    out += head;
    out += "   \"before\": {\"ok\": " + std::string(s.before.ok ? "1" : "0") + ", \"summary\": \"" +
           Esc(s.before.Summary(), 300) + "\"},\n";
    out += "   \"action\": " + s.action.Describe() + ",\n";
    out += "   \"policy\": {\"allow\": " + std::string(s.policyAllow ? "1" : "0") + ", \"note\": \"" +
           Esc(s.policyNote) + "\"},\n";
    out += "   \"exec\": {\"event\": \"" + Esc(s.execEvent) + "\", \"events\": " +
           std::to_string(s.execEvents) + ", \"ret\": " + std::to_string(s.execRet) + "},\n";
    out += "   \"after\": {\"ok\": " + std::string(s.after.ok ? "1" : "0") + ", \"summary\": \"" +
           Esc(s.after.Summary(), 300) + "\"},\n";
    out += "   \"verdict\": {\"pass\": " + std::string(s.verify.pass ? "1" : "0") + ", \"why\": \"" +
           Esc(s.verify.why, 400) + "\"},\n";
    out += "   \"observe_retries\": " + std::to_string(s.observeRetries) + ", \"step_ms\": " +
           std::to_string(s.stepMs) + "}";
}

}  // namespace

std::string TraceJson(const LoopTrace &t)
{
    std::string out = "{\n";
    out += " \"goal\": \"" + Esc(t.goal, 80) + "\",\n";
    out += " \"plan_steps\": " + std::to_string(t.planSteps) + ", \"max_steps\": " +
           std::to_string(t.maxSteps) + ",\n";
    out += " \"verdict\": \"" + std::string(t.pass ? "PASS" : "FAIL") + "\", \"stopped_at\": " +
           std::to_string(t.stoppedAt) + ",\n";
    out += " \"why\": \"" + Esc(t.why, 300) + "\", \"total_ms\": " + std::to_string(t.totalMs) + ",\n";
    out += " \"steps\": [\n";
    for (size_t i = 0; i < t.steps.size(); ++i) {
        StepJson(t.steps[i], out);
        out += (i + 1 < t.steps.size()) ? ",\n" : "\n";
    }
    out += " ]\n}";
    return out;
}

std::string TraceText(const LoopTrace &t)
{
    std::string out;
    out += "===== S7-3-A Agent Loop（多步闭环）=====\n";
    out += "GOAL        : " + t.goal + "\n";
    out += "PLAN        : " + std::to_string(t.planSteps) + " 步（本次预算 maxSteps=" +
           std::to_string(t.maxSteps) + "）\n";
    for (const auto &s : t.steps) {
        char head[128];
        snprintf(head, sizeof(head), "--- step %d ---\n", s.stepId);
        out += head;
        out += "  observe(before): " + std::string(s.before.ok ? "ok" : "fail") + "  " + s.before.Summary() + "\n";
        out += "  decision       : " + s.action.Describe() + "\n";
        out += "  policy         : " + std::string(s.policyAllow ? "ALLOW" : "DENY") + " —— " + s.policyNote + "\n";
        out += "  action(MMI)    : " + s.execEvent + " → ret=" + std::to_string(s.execRet) +
               "（events=" + std::to_string(s.execEvents) + "）\n";
        out += "  observe(after) : " + std::string(s.after.ok ? "ok" : "fail") + "  " + s.after.Summary() + "\n";
        out += "  verdict        : " + std::string(s.verify.pass ? "PASS" : "FAIL") + " —— " + s.verify.why + "\n";
        out += "  observe_retries: " + std::to_string(s.observeRetries) + " · step_ms=" +
               std::to_string(s.stepMs) + "\n";
    }
    out += "SUMMARY     : " + std::string(t.pass ? "PASS" : "FAIL");
    if (!t.pass && t.stoppedAt >= 0) {
        out += "（停在第 " + std::to_string(t.stoppedAt) + " 步，不自动重试动作）";
    }
    out += "\nWHY         : " + t.why + "\n";
    out += "TOTAL       : " + std::to_string(t.totalMs) + " ms\n";
    return out;
}

}  // namespace agent
}  // namespace tmac_sa
