// trace.cpp —— S7-2-A：把一步闭环写成证据（JSON + 人读两种形态）
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

}  // namespace

std::string TraceJson(const StepTrace &t)
{
    std::string out = "{\n";
    out += " \"goal\": \"" + Esc(t.goal, 80) + "\",\n";
    out += " \"steps\": " + std::to_string(t.steps) + ",\n";
    out += " \"total_ms\": " + std::to_string(t.totalMs) + ",\n";
    out += " \"observe_retries\": " + std::to_string(t.observeRetries) + ",\n";
    out += " \"observe_before\": {\"ok\": " + std::string(t.before.ok ? "1" : "0") +
           ", \"error\": \"" + Esc(t.before.error, 80) + "\", \"summary\": \"" +
           Esc(t.before.Summary(), 300) + "\"},\n";
    out += " \"decision\": " + t.action.Describe() + ",\n";
    out += " \"policy\": {\"allow\": " + std::string(t.policyAllow ? "1" : "0") + ", \"note\": \"" +
           Esc(t.policyNote) + "\"},\n";
    out += " \"exec\": {\"event\": \"" + Esc(t.execEvent) + "\", \"events\": " +
           std::to_string(t.execEvents) + ", \"ret\": " + std::to_string(t.execRet) + "},\n";
    out += " \"observe_after\": {\"ok\": " + std::string(t.after.ok ? "1" : "0") +
           ", \"summary\": \"" + Esc(t.after.Summary(), 300) + "\"},\n";
    out += " \"verify\": {\"pass\": " + std::string(t.verify.pass ? "1" : "0") + ", \"why\": \"" +
           Esc(t.verify.why, 400) + "\"}\n";
    out += "}";
    return out;
}

std::string TraceText(const StepTrace &t)
{
    std::string out;
    out += "===== S7-2-A Agent Loop（单步闭环）=====\n";
    out += "GOAL            : " + t.goal + "\n";
    out += "OBSERVE(before) : " + std::string(t.before.ok ? "ok" : ("fail:" + t.before.error)) + "\n";
    out += "                  " + t.before.Summary() + "\n";
    out += "DECISION        : " + t.action.Describe() + "\n";
    out += "POLICY          : " + std::string(t.policyAllow ? "ALLOW" : "DENY") + " —— " + t.policyNote + "\n";
    out += "EXEC            : " + t.execEvent + " → ret=" + std::to_string(t.execRet) + "（events=" +
           std::to_string(t.execEvents) + "）\n";
    out += "OBSERVE(after)  : " + std::string(t.after.ok ? "ok" : "fail") + "\n";
    out += "                  " + t.after.Summary() + "\n";
    out += "VERIFY          : " + std::string(t.verify.pass ? "PASS" : "FAIL") + " —— " + t.verify.why + "\n";
    out += "TOTAL           : " + std::to_string(t.totalMs) + " ms\n";
    out += "（观察重试 " + std::to_string(t.observeRetries) + " 次）\n";
    return out;
}

}  // namespace agent
}  // namespace tmac_sa
