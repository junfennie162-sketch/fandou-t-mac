// verifier.cpp —— S7-2-A：目标达成判定（全部基于感知对比，条件逐条写进 why）
//
// goal="unlock screen" 的判据（三条全中才算 pass）：
//   ① before 确实在锁屏（有「上滑解锁」文本 或 有窗口类型 2110）
//   ② after 不再在锁屏（两者都没有了）
//   ③ 界面确实变化过（窗口列表 / 文本数 / 访问节点数任一不同）
// 这样即使"动作没生效"，也不可能被误判成成功（①③ 会拦住）。
#include "agent/verifier.h"

#include <cstdio>

namespace tmac_sa {
namespace agent {
namespace {

bool LockScreen(const Digest &d, std::string *how)
{
    const bool byText = d.FindByText("上滑解锁") != nullptr;
    const bool byWin = d.HasWindowType(2110);
    if (how != nullptr) {
        *how = std::string("文本「上滑解锁」=") + (byText ? "有" : "无") + " 窗口类型2110=" +
               (byWin ? "有" : "无");
    }
    return byText || byWin;
}

}  // namespace

VerifyResult Verify(const std::string &goal, const Digest &before, const Digest &after,
                    const Action &act)
{
    VerifyResult r;
    const std::string g = goal;
    const bool isUnlock = g.find("unlock") != std::string::npos || g.find("解锁") != std::string::npos;

    if (!isUnlock) {
        r.pass = false;
        r.why = "v1 没有该目标的判据（只有 unlock screen）：" + goal;
        return r;
    }
    std::string howBefore, howAfter;
    const bool lockBefore = LockScreen(before, &howBefore);
    const bool lockAfter = LockScreen(after, &howAfter);
    const bool changed = (before.windowIds != after.windowIds) || (before.withText != after.withText) ||
                         (before.visited != after.visited);

    r.pass = lockBefore && !lockAfter && changed;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "① before 在锁屏=%d（%s）② after 已离开锁屏=%d（%s）③ 界面有变化=%d"
             "（windows %s→%s，withText %d→%d，visited %d→%d）→ %s",
             lockBefore ? 1 : 0, howBefore.c_str(), lockAfter ? 0 : 1, howAfter.c_str(),
             changed ? 1 : 0, before.windowIds.c_str(), after.windowIds.c_str(), before.withText,
             after.withText, before.visited, after.visited, r.pass ? "PASS" : "FAIL");
    r.why = buf;
    (void)act;
    return r;
}

}  // namespace agent
}  // namespace tmac_sa
