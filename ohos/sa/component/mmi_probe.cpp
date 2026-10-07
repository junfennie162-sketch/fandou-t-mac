// mmi_probe.cpp —— S7-2-A0：验证「进程内 MMI 输入注入」这条路（独立探针，不进 SA 主链）
//
// 为什么需要它：S7-2-0 已实测出四条执行通道的判决（evidence/69）——
//   · 输入注入 `uitest uiInput` ✅ 唯一可用（但那是个外部 CLI 工具）
//   · a11y 元素动作 ExecuteAction ❌ 受理但不生效；a11y 手势 InjectGesture ❌ 4006 无能力
//   · 拉起 Ability（aa/AMS）❌ 本环境不可用
// 要让 Agent Loop **在 SA 进程内**闭环，就必须把"注入"这条唯一可用的路搬进我们自己的进程。
// 本探针就是回答：**我们自己的进程调 MMI 的 SimulateInputEvent 能不能真的改变屏幕**。
//
// 与 uitest 的关系：API 与事件构造方式照抄 uitest 的参考实现
//   （test/testfwk/arkxtest/uitest/server/system_ui_controller.cpp:772/899），
//   所以它可用性有先例；差别只是**由我们的进程发**，从而验证权限与链接是否成立。
//
// 纪律：动作数固定（每种一次）· 有界 · 每个事件都打印真实返回码 · 判据是"界面确实变化"（不看退出码）
//
// 用法：
//   lut_mmi_probe swipe [x1 y1 x2 y2 durMs]   默认 512 600 512 200 300（解锁上滑）
//   lut_mmi_probe click [x y]                 默认 512 400
//   lut_mmi_probe back                        发 KEYCODE_BACK
// 退出码：0 = 注入返回 0；2 = 参数/句柄错；3 = 没连上 a11y；4 = 注入返回非 0
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "input_manager.h"
#include "key_event.h"
#include "pointer_event.h"

#include "accessibility_ui_test_ability.h"
#include "accessible_ability_listener.h"

using namespace OHOS;
using namespace OHOS::Accessibility;

namespace {
constexpr int kMaxNodes = 200;
constexpr int kMaxDepth = 8;
constexpr int kMaxKids = 40;
constexpr int kSettleMs = 400;    // 注入后等界面稳定再读屏
constexpr int32_t kDisplayId = 0; // 单屏环境用 0（uitest 在非法时也回落到默认 display）

// ── 最小监听器（读屏用；与 lut_a11y_dump 同一套）
class ProbeListener : public AccessibleAbilityListener {
public:
    void OnAbilityConnected() override { connected_.store(true); }
    void OnAbilityDisconnected() override { connected_.store(false); }
    void OnAccessibilityEvent(const AccessibilityEventInfo &) override {}
    bool OnKeyPressEvent(const std::shared_ptr<MMI::KeyEvent> &) override { return false; }
    bool Connected() const { return connected_.load(); }

private:
    std::atomic<bool> connected_{false};
};

struct Snap {
    size_t windows = 0;
    std::string winIds;
    int nodes = 0, withText = 0, clickable = 0;
    std::vector<std::string> texts;
};

std::string Join(const std::vector<std::string> &v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += " | ";
        out += v[i];
    }
    return out;
}

void Walk(AccessibilityUITestAbility &ability, const AccessibilityElementInfo &e, int depth, Snap &s)
{
    if (s.nodes >= kMaxNodes || depth > kMaxDepth) {
        return;
    }
    ++s.nodes;
    const std::string t = e.GetContent();
    if (!t.empty()) {
        ++s.withText;
        if (s.texts.size() < 12) {
            s.texts.push_back(t);
        }
    }
    if (e.IsClickable()) {
        ++s.clickable;
    }
    std::vector<AccessibilityElementInfo> kids;
    if (ability.GetChildren(e, kids) != RET_OK) {
        return;
    }
    int kept = 0;
    for (const auto &k : kids) {
        if (kept++ >= kMaxKids) break;
        Walk(ability, k, depth + 1, s);
    }
}

Snap TakeSnap(AccessibilityUITestAbility &ability)
{
    Snap s;
    std::vector<AccessibilityWindowInfo> windows;
    if (ability.GetWindows(windows) == RET_OK) {
        s.windows = windows.size();
        for (size_t i = 0; i < windows.size() && i < 8; ++i) {
            s.winIds += std::to_string(windows[i].GetWindowId()) + " ";
        }
    }
    AccessibilityElementInfo root;
    if (ability.GetRoot(root) == RET_OK) {
        Walk(ability, root, 0, s);
    }
    const int32_t rootWin = root.GetWindowId();
    for (auto &w : windows) {
        if (w.GetWindowId() == rootWin) continue;
        AccessibilityElementInfo wr;
        if (ability.GetRootByWindow(w, wr) == RET_OK) {
            Walk(ability, wr, 0, s);
        }
    }
    return s;
}

void PrintSnap(const char *tag, const Snap &s)
{
    printf("[mmi-probe] %s: windows=%zu ids=[%s] nodes=%d withText=%d clickable=%d texts=[%s]\n",
           tag, s.windows, s.winIds.c_str(), s.nodes, s.withText, s.clickable, Join(s.texts).c_str());
}

// ── 注入：全部照抄 uitest 的事件构造方式
int InjectSwipe(int x1, int y1, int x2, int y2, int durMs)
{
    auto mgr = OHOS::MMI::InputManager::GetInstance();
    if (mgr == nullptr) {
        printf("[mmi-probe] FATAL InputManager::GetInstance() == nullptr\n");
        return -1;
    }
    const int steps = 8;
    int last = -1;
    for (int i = 0; i <= steps; ++i) {
        auto ev = OHOS::MMI::PointerEvent::Create();
        if (ev == nullptr) return -1;
        const double x = x1 + (x2 - x1) * static_cast<double>(i) / steps;
        const double y = y1 + (y2 - y1) * static_cast<double>(i) / steps;
        OHOS::MMI::PointerEvent::PointerItem item;
        item.SetPointerId(0);
        item.SetPressed(i < steps);                     // 末点抬起
        item.SetGlobalX(x);
        item.SetGlobalY(y);
        item.SetDisplayX(static_cast<int32_t>(x));
        item.SetDisplayY(static_cast<int32_t>(y));
        ev->AddPointerItem(item);
        ev->SetPointerId(0);
        ev->SetPointerAction(i == 0 ? OHOS::MMI::PointerEvent::POINTER_ACTION_DOWN
                                    : (i == steps ? OHOS::MMI::PointerEvent::POINTER_ACTION_UP
                                                  : OHOS::MMI::PointerEvent::POINTER_ACTION_MOVE));
        ev->SetSourceType(OHOS::MMI::PointerEvent::SOURCE_TYPE_TOUCHSCREEN);
        ev->SetTargetDisplayId(kDisplayId);
        const int r = mgr->SimulateInputEvent(ev, false);
        printf("[mmi-probe] event %d/%d (%d,%d) → ret=%d\n", i, steps, static_cast<int>(x),
               static_cast<int>(y), r);
        last = r;
        usleep(durMs * 1000 / (steps + 1));
    }
    return last;
}

int InjectClick(int x, int y)
{
    auto mgr = OHOS::MMI::InputManager::GetInstance();
    if (mgr == nullptr) {
        printf("[mmi-probe] FATAL InputManager::GetInstance() == nullptr\n");
        return -1;
    }
    int last = -1;
    for (int i = 0; i < 2; ++i) {                        // 0=DOWN 1=UP
        auto ev = OHOS::MMI::PointerEvent::Create();
        if (ev == nullptr) return -1;
        OHOS::MMI::PointerEvent::PointerItem item;
        item.SetPointerId(0);
        item.SetPressed(i == 0);
        item.SetGlobalX(x);
        item.SetGlobalY(y);
        item.SetDisplayX(x);
        item.SetDisplayY(y);
        ev->AddPointerItem(item);
        ev->SetPointerId(0);
        ev->SetPointerAction(i == 0 ? OHOS::MMI::PointerEvent::POINTER_ACTION_DOWN
                                    : OHOS::MMI::PointerEvent::POINTER_ACTION_UP);
        ev->SetSourceType(OHOS::MMI::PointerEvent::SOURCE_TYPE_TOUCHSCREEN);
        ev->SetTargetDisplayId(kDisplayId);
        const int r = mgr->SimulateInputEvent(ev, false);
        printf("[mmi-probe] event %s (%d,%d) → ret=%d\n", i == 0 ? "DOWN" : "UP", x, y, r);
        last = r;
        usleep(60 * 1000);
    }
    return last;
}

int InjectBack()
{
    auto mgr = OHOS::MMI::InputManager::GetInstance();
    if (mgr == nullptr) {
        printf("[mmi-probe] FATAL InputManager::GetInstance() == nullptr\n");
        return -1;
    }
    int last = -1;
    for (int i = 0; i < 2; ++i) {                        // 0=DOWN 1=UP
        auto ke = OHOS::MMI::KeyEvent::Create();
        if (ke == nullptr) return -1;
        const int32_t code = OHOS::MMI::KeyEvent::KEYCODE_BACK;
        ke->SetKeyCode(code);
        ke->SetKeyAction(i == 0 ? OHOS::MMI::KeyEvent::KEY_ACTION_DOWN
                                : OHOS::MMI::KeyEvent::KEY_ACTION_UP);
        OHOS::MMI::KeyEvent::KeyItem item;
        item.SetKeyCode(code);
        item.SetPressed(i == 0);
        ke->AddKeyItem(item);
        ke->SetTargetDisplayId(kDisplayId);
        const int r = mgr->SimulateInputEvent(ke);
        printf("[mmi-probe] keyEvent KEYCODE_BACK %s → ret=%d\n", i == 0 ? "DOWN" : "UP", r);
        last = r;
        usleep(60 * 1000);
    }
    return last;
}
}  // namespace

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) {
        printf("用法: lut_mmi_probe <swipe|click|back> [args]\n");
        return 2;
    }
    const char *kind = argv[1];

    // ① 连接读屏通道（before/after 快照都要用）
    auto ability = AccessibilityUITestAbility::GetInstance();
    if (ability == nullptr) {
        printf("[mmi-probe] FATAL GetInstance() == nullptr\n");
        return 2;
    }
    auto listener = std::make_shared<ProbeListener>();
    const RetError rr = ability->RegisterAbilityListener(listener);
    printf("[mmi-probe] RegisterAbilityListener ret=%d\n", static_cast<int>(rr));
    if (rr != RET_OK) return 3;
    RetError rc = ability->Connect(0);
    if (rc != RET_OK && rc != RET_ERR_CONNECTION_EXIST) {
        rc = ability->Connect(100);
        if (rc != RET_OK && rc != RET_ERR_CONNECTION_EXIST) {
            printf("[mmi-probe] Connect 失败 ret=%d\n", static_cast<int>(rc));
            return 3;
        }
    }
    for (int i = 0; i < 50 && !listener->Connected(); ++i) usleep(100 * 1000);
    printf("[mmi-probe] connected=%d\n", listener->Connected() ? 1 : 0);

    // ② before
    const Snap before = TakeSnap(*ability);
    PrintSnap("before", before);

    // ③ 注入
    int ret = -1;
    if (strcmp(kind, "swipe") == 0) {
        const int x1 = argc > 2 ? atoi(argv[2]) : 512;
        const int y1 = argc > 3 ? atoi(argv[3]) : 600;
        const int x2 = argc > 4 ? atoi(argv[4]) : 512;
        const int y2 = argc > 5 ? atoi(argv[5]) : 200;
        const int dur = argc > 6 ? atoi(argv[6]) : 300;
        printf("[mmi-probe] inject: SimulateInputEvent(PointerEvent x%d) swipe (%d,%d)->(%d,%d) %dms\n",
               9, x1, y1, x2, y2, dur);
        ret = InjectSwipe(x1, y1, x2, y2, dur);
    } else if (strcmp(kind, "click") == 0) {
        const int x = argc > 2 ? atoi(argv[2]) : 512;
        const int y = argc > 3 ? atoi(argv[3]) : 400;
        printf("[mmi-probe] inject: SimulateInputEvent(PointerEvent x2) click (%d,%d)\n", x, y);
        ret = InjectClick(x, y);
    } else if (strcmp(kind, "back") == 0) {
        printf("[mmi-probe] inject: SimulateInputEvent(KeyEvent x2) KEYCODE_BACK\n");
        ret = InjectBack();
    } else {
        printf("[mmi-probe] 未知动作 %s\n", kind);
        return 2;
    }
    printf("[mmi-probe] inject ret=%d\n", ret);
    usleep(kSettleMs * 1000);

    // ④ after + diff（判据：界面确实变化，不看退出码）
    const Snap after = TakeSnap(*ability);
    PrintSnap("after ", after);
    std::vector<std::string> added, gone;
    for (const auto &t : after.texts) {
        if (std::find(before.texts.begin(), before.texts.end(), t) == before.texts.end()) added.push_back(t);
    }
    for (const auto &t : before.texts) {
        if (std::find(after.texts.begin(), after.texts.end(), t) == after.texts.end()) gone.push_back(t);
    }
    const bool changed = (before.winIds != after.winIds) || (before.nodes != after.nodes) ||
                         (before.withText != after.withText) || !added.empty() || !gone.empty();
    printf("[mmi-probe] diff  : windows=%s nodes=%d→%d withText=%d→%d 新文本[%s] 消失[%s]\n",
           before.winIds == after.winIds ? "同" : "变", before.nodes, after.nodes, before.withText,
           after.withText, Join(added).c_str(), Join(gone).c_str());
    printf("[mmi-probe] verdict=%s kind=%s ret=%d\n", changed ? "CHANGED" : "UNCHANGED", kind, ret);
    ability->Disconnect(0);
    printf("[mmi-probe] DONE kind=%s rc=%d\n", kind, ret == 0 ? 0 : 4);
    return ret == 0 ? 0 : 4;
}
