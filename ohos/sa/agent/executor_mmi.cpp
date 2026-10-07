// executor_mmi.cpp —— S7-2-A：MMI 注入执行器（事件构造照抄 uitest 参考实现，见 evidence/70 结论）
#include "agent/executor_mmi.h"

#include <cstdio>
#include <unistd.h>

#include "input_manager.h"
#include "key_event.h"
#include "pointer_event.h"

namespace tmac_sa {
namespace agent {
namespace {
constexpr int32_t kDisplayId = 0;      // 单屏环境
constexpr int kSwipeSteps = 8;
constexpr int kHoldUs = 60 * 1000;     // 按下与抬起之间的间隔

OHOS::MMI::InputManager *Mgr()
{
    return OHOS::MMI::InputManager::GetInstance();
}

int SendPointer(int x, int y, bool pressed, int32_t action)
{
    auto mgr = Mgr();
    if (mgr == nullptr) {
        return -1;
    }
    auto ev = OHOS::MMI::PointerEvent::Create();
    if (ev == nullptr) {
        return -1;
    }
    OHOS::MMI::PointerEvent::PointerItem item;
    item.SetPointerId(0);
    item.SetPressed(pressed);
    item.SetGlobalX(x);
    item.SetGlobalY(y);
    item.SetDisplayX(x);
    item.SetDisplayY(y);
    ev->AddPointerItem(item);
    ev->SetPointerId(0);
    ev->SetPointerAction(action);
    ev->SetSourceType(OHOS::MMI::PointerEvent::SOURCE_TYPE_TOUCHSCREEN);
    ev->SetTargetDisplayId(kDisplayId);
    return mgr->SimulateInputEvent(ev, false);
}
}  // namespace

ExecResult InjectSwipe(int x1, int y1, int x2, int y2, int durMs)
{
    ExecResult r;
    char buf[256];
    snprintf(buf, sizeof(buf), "SimulateInputEvent(PointerEvent x%d) swipe (%d,%d)->(%d,%d) %dms",
             kSwipeSteps + 1, x1, y1, x2, y2, durMs);
    r.event = buf;
    const int stepUs = (durMs > 0 ? durMs * 1000 : 300000) / (kSwipeSteps + 1);
    for (int i = 0; i <= kSwipeSteps; ++i) {
        const int x = x1 + (x2 - x1) * i / kSwipeSteps;
        const int y = y1 + (y2 - y1) * i / kSwipeSteps;
        const int32_t act = (i == 0) ? OHOS::MMI::PointerEvent::POINTER_ACTION_DOWN
                                     : (i == kSwipeSteps ? OHOS::MMI::PointerEvent::POINTER_ACTION_UP
                                                         : OHOS::MMI::PointerEvent::POINTER_ACTION_MOVE);
        r.ret = SendPointer(x, y, i < kSwipeSteps, act);
        ++r.events;
        usleep(stepUs);
    }
    return r;
}

ExecResult InjectClick(int x, int y)
{
    ExecResult r;
    char buf[128];
    snprintf(buf, sizeof(buf), "SimulateInputEvent(PointerEvent x2) click (%d,%d)", x, y);
    r.event = buf;
    r.ret = SendPointer(x, y, true, OHOS::MMI::PointerEvent::POINTER_ACTION_DOWN);
    ++r.events;
    usleep(kHoldUs);
    r.ret = SendPointer(x, y, false, OHOS::MMI::PointerEvent::POINTER_ACTION_UP);
    ++r.events;
    return r;
}

ExecResult InjectBack()
{
    ExecResult r;
    r.event = "SimulateInputEvent(KeyEvent x2) KEYCODE_BACK";
    auto mgr = Mgr();
    if (mgr == nullptr) {
        return r;
    }
    for (int i = 0; i < 2; ++i) {
        auto ke = OHOS::MMI::KeyEvent::Create();
        if (ke == nullptr) {
            return r;
        }
        const int32_t code = OHOS::MMI::KeyEvent::KEYCODE_BACK;
        ke->SetKeyCode(code);
        ke->SetKeyAction(i == 0 ? OHOS::MMI::KeyEvent::KEY_ACTION_DOWN
                                : OHOS::MMI::KeyEvent::KEY_ACTION_UP);
        OHOS::MMI::KeyEvent::KeyItem item;
        item.SetKeyCode(code);
        item.SetPressed(i == 0);
        ke->AddKeyItem(item);
        ke->SetTargetDisplayId(kDisplayId);
        r.ret = mgr->SimulateInputEvent(ke);
        ++r.events;
        usleep(kHoldUs);
    }
    return r;
}

}  // namespace agent
}  // namespace tmac_sa
