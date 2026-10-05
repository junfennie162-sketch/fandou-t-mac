// lut_a11y_dump.cpp —— S7-1a：OpenHarmony 无障碍「读屏」能力探针（**独立工具，不进 SA**）
//
// 为什么先做这个：S7 的 GUI Agent 第一步是「看到」界面。写 SA 的 ReadScreen 之前，
// 必须先把「这个镜像里的无障碍到底能不能被一个**独立原生进程**读出元素树」这个未知数打掉。
//
// 两条路的实测结论（v1 → v2 的由来，都有证据）：
//   v1 走 `AccessibleAbilityClient`（普通客户端）：`GetWindows`/`GetRoot` 一律
//      **ret=4004 = RET_ERR_NO_CONNECTION** —— 该客户端只有在被系统
//      `Init(channel, channelId)` 过之后才算连上，而 channel 只发给「登记过的无障碍 ability」。
//   v2 走 `AccessibilityUITestAbility`：这是**系统给原生进程留的读屏口子**
//      （uitest 工具自己就这么用）：RegisterAbilityListener → Connect(userId) →
//      管理器下发 channel → 可以 GetWindows/GetRoot/GetChildren/ExecuteAction…
//      与本进程的 uid/权限有关，所以下面把每一步的真实返回值都打出来。
//
// 纪律：只读、有界（最多 60 节点 / 4 层 / 每层最多 24 个父节点），本工具不做任何注入动作。
//       失败必须留下**真实错误码**，不允许把「没拿到」写成「大概不行」。
//
// 用法：lut_a11y_dump [windowId]
// 退出码：0 = 拿到树且有文本；1 = 拿到树但全空文本；2 = 句柄为空；3 = 没连上；4 = 连上但没树
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "accessibility_ui_test_ability.h"
#include "accessible_ability_listener.h"

using namespace OHOS;
using namespace OHOS::Accessibility;

namespace {
constexpr int kMaxNodes = 200;   // 单次 dump 的节点上限（把无障碍服务拖住就不好玩了）
constexpr int kMaxDepth = 8;     // 层数上限（实测：桌面窗口文本在 4 层以下，太浅看不到）
constexpr int kMaxKids = 40;     // 每层最多保留多少个父节点继续下钻
constexpr int kConnectWaitMs = 5000;   // 等 channel 回调的预算
constexpr int kMaxTexts = 30;    // 汇总行里最多列几条文本（S7-2 的输入预览）

int g_withText = 0;              // 非空文本的元素数 —— 这才是 S7-2 能用得上的「看着的东西」
int g_clickable = 0;             // 可点元素数 —— S7-3 执行侧的候选
std::vector<std::string> g_texts;   // 去重后的文本（按发现顺序）

// 最小监听器：只需要 connect/disconnect 两个信号；事件与按键一律忽略（本工具不消费输入）
class ProbeListener : public AccessibleAbilityListener {
public:
    void OnAbilityConnected() override
    {
        connected_.store(true);
        printf("[a11y] callback: OnAbilityConnected\n");
    }
    void OnAbilityDisconnected() override
    {
        connected_.store(false);
        printf("[a11y] callback: OnAbilityDisconnected\n");
    }
    void OnAccessibilityEvent(const AccessibilityEventInfo &) override {}
    bool OnKeyPressEvent(const std::shared_ptr<MMI::KeyEvent> &) override { return false; }

    bool Connected() const { return connected_.load(); }

private:
    std::atomic<bool> connected_{false};
};

// 把界面文本塞进 JSON 字符串：转义 " 与 \，控制字符降级成空格，截断 160 字节
std::string Esc(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
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
    if (out.size() > 160) {
        out.resize(160);
        out += "...";
    }
    return out;
}

// 默认只打印「带文本或可点」的节点：容器（Flex/Column/Stack…）对 S7-2/S7-3 没用，
// 全量打印会把证据冲成几百行。要看全量：LUT_A11Y_ALL=1
bool g_printAll = false;

void PrintNode(const AccessibilityElementInfo &e, int depth, int idx)
{
    std::string text = e.GetContent();
    if (!text.empty()) {
        ++g_withText;
        if (g_texts.size() < kMaxTexts && std::find(g_texts.begin(), g_texts.end(), text) == g_texts.end()) {
            g_texts.push_back(text);   // 汇总行只列去重后的前 kMaxTexts 条
        }
    }
    if (e.IsClickable()) {
        ++g_clickable;
    }
    if (!g_printAll && text.empty() && !e.IsClickable()) {
        printf("  {\"n\":%d,\"d\":%d,\"a11yId\":%" PRId64 ",\"winId\":%d,\"type\":\"%s\",\"container\":1,\"kids\":%d}\n",
               idx, depth, e.GetAccessibilityId(), e.GetWindowId(),
               Esc(e.GetComponentType()).c_str(), e.GetChildCount());
        return;
    }
    const Rect &box = e.GetRectInScreen();   // GetLeftTop... 是 Rect 的方法，不是元素的方法
    printf("  {\"n\":%d,\"d\":%d,\"a11yId\":%" PRId64 ",\"winId\":%d,\"type\":\"%s\",\"text\":\"%s\","
           "\"click\":%d,\"focus\":%d,\"vis\":%d,\"kids\":%d,\"box\":[%d,%d,%d,%d]}\n",
           idx, depth, e.GetAccessibilityId(), e.GetWindowId(),
           Esc(e.GetComponentType()).c_str(), Esc(text).c_str(),
           e.IsClickable() ? 1 : 0, e.IsFocusable() ? 1 : 0, e.IsVisible() ? 1 : 0,
           e.GetChildCount(),
           box.GetLeftTopXScreenPostion(), box.GetLeftTopYScreenPostion(),
           box.GetRightBottomXScreenPostion(), box.GetRightBottomYScreenPostion());
}

// 广度优先 + 硬上限；budget = 这次还能打印多少节点（多窗口共用总预算）；返回打印出来的节点数
int DumpTree(AccessibilityUITestAbility &ability, const AccessibilityElementInfo &root, const char *tag,
             int budget)
{
    printf("[a11y] tree-begin(%s)\n", tag);
    PrintNode(root, 0, 0);
    int printed = 1;
    std::vector<AccessibilityElementInfo> level{root};
    for (int depth = 1; depth <= kMaxDepth && printed < budget && !level.empty(); ++depth) {
        std::vector<AccessibilityElementInfo> next;
        for (const auto &parent : level) {
            std::vector<AccessibilityElementInfo> kids;
            RetError r = ability.GetChildren(parent, kids);
            if (r != RET_OK) {
                printf("[a11y] children-err d=%d ret=%d parentA11yId=%" PRId64 "\n",
                       depth, static_cast<int>(r), parent.GetAccessibilityId());
                continue;
            }
            for (const auto &k : kids) {
                if (printed >= budget) {
                    break;
                }
                PrintNode(k, depth, printed++);
                if (next.size() < kMaxKids) {
                    next.push_back(k);
                }
            }
        }
        level.swap(next);
    }
    printf("[a11y] tree-end(%s) printed=%d withText=%d\n", tag, printed, g_withText);
    return printed;
}

// 等 channel 回调（监听器的 OnAbilityConnected 由管理器线程触发）
bool WaitConnected(const ProbeListener &listener)
{
    for (int waited = 0; waited < kConnectWaitMs; waited += 100) {
        if (listener.Connected()) {
            printf("[a11y] connected after ~%d ms\n", waited);
            return true;
        }
        usleep(100 * 1000);
    }
    return listener.Connected();
}
}  // namespace

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc > 1 && strcmp(argv[1], "all") == 0) {
        g_printAll = true;                      // 「all」= 连容器也打（排障用）
    }
    if (getenv("LUT_A11Y_ALL") != nullptr) {
        g_printAll = true;
    }
    int wantWindowId = (argc > 1 && argv[1][0] != 'a') ? atoi(argv[1]) : -1;
    printf("[a11y] lut_a11y_dump begin (S7-1a v2: via AccessibilityUITestAbility, read-only,"
           " max %d nodes / depth %d)\n", kMaxNodes, kMaxDepth);

    auto ability = AccessibilityUITestAbility::GetInstance();
    if (ability == nullptr) {
        printf("[a11y] FATAL: AccessibilityUITestAbility::GetInstance() == nullptr\n");
        return 2;
    }

    // ① 登记监听器：这一步内部会查「调用方有没有无障碍扩展权限」，
    //    所以它的返回值本身就是**权限证据**
    auto listener = std::make_shared<ProbeListener>();
    RetError rr = ability->RegisterAbilityListener(listener);
    printf("[a11y] RegisterAbilityListener ret=%d\n", static_cast<int>(rr));
    if (rr != RET_OK) {
        printf("[a11y] DONE verdict=REGISTER-FAILED ret=%d\n", static_cast<int>(rr));
        return 3;
    }

    // ② 要 channel：userId=0 是接口默认值（当前用户）；失败再试 100（OH 的默认用户档）
    RetError rc = ability->Connect(0);
    printf("[a11y] Connect(0) ret=%d\n", static_cast<int>(rc));
    if (rc == RET_ERR_CONNECTION_EXIST) {
        printf("[a11y] note: 4003 = RET_ERR_CONNECTION_EXIST，按已连接处理\n");
    } else if (rc != RET_OK) {
        RetError rc2 = ability->Connect(100);
        printf("[a11y] Connect(100) ret=%d\n", static_cast<int>(rc2));
        if (rc2 != RET_OK && rc2 != RET_ERR_CONNECTION_EXIST) {
            printf("[a11y] DONE verdict=CONNECT-FAILED ret0=%d ret100=%d\n",
                   static_cast<int>(rc), static_cast<int>(rc2));
            return 3;
        }
    }
    bool ok = WaitConnected(*listener);
    printf("[a11y] connected=%d currentUserId=%d\n", ok ? 1 : 0, ability->GetCurrentUserId());

    // ③ 读窗口列表
    std::vector<AccessibilityWindowInfo> windows;
    RetError rw = ability->GetWindows(windows);
    printf("[a11y] GetWindows ret=%d count=%zu\n", static_cast<int>(rw), windows.size());
    for (size_t i = 0; i < windows.size() && i < 12; ++i) {
        printf("[a11y] win[%zu] id=%d type=%d layer=%d\n", i, windows[i].GetWindowId(),
               static_cast<int>(windows[i].GetWindowType()), windows[i].GetWindowLayer());
    }

    // ④ 取根：先全局根（活动窗口），再**把所有窗口都走一遍**
    //    —— 实测教训：顶层窗口（桌面）深度 4 以内全是容器、没有文本，文本常在应用窗口里
    AccessibilityElementInfo root;
    RetError r1 = ability->GetRoot(root);
    printf("[a11y] GetRoot ret=%d type=\"%s\" text=\"%s\" kids=%d a11yId=%" PRId64 "\n",
           static_cast<int>(r1), Esc(root.GetComponentType()).c_str(), Esc(root.GetContent()).c_str(),
           root.GetChildCount(), root.GetAccessibilityId());
    int printed = 0;
    if (wantWindowId < 0) {
        if (r1 == RET_OK) {
            printed += DumpTree(*ability, root, "GetRoot", kMaxNodes - printed);
        }
        for (auto &w : windows) {
            if (printed >= kMaxNodes) {
                break;
            }
            char tag[48] = {0};
            snprintf(tag, sizeof(tag), "win%d/type%d", w.GetWindowId(), static_cast<int>(w.GetWindowType()));
            AccessibilityElementInfo wr;
            RetError r2 = ability->GetRootByWindow(w, wr);
            if (r2 != RET_OK) {
                printf("[a11y] GetRootByWindow(winId=%d) ret=%d\n", w.GetWindowId(), static_cast<int>(r2));
                continue;
            }
            printed += DumpTree(*ability, wr, tag, kMaxNodes - printed);
        }
    } else {
        AccessibilityWindowInfo *pick = nullptr;
        for (auto &w : windows) {
            if (w.GetWindowId() == wantWindowId) {
                pick = &w;
                break;
            }
        }
        if (pick != nullptr) {
            AccessibilityElementInfo wr;
            RetError r2 = ability->GetRootByWindow(*pick, wr);
            printf("[a11y] GetRootByWindow(winId=%d) ret=%d type=\"%s\" text=\"%s\" kids=%d\n",
                   pick->GetWindowId(), static_cast<int>(r2), Esc(wr.GetComponentType()).c_str(),
                   Esc(wr.GetContent()).c_str(), wr.GetChildCount());
            if (r2 == RET_OK) {
                printed = DumpTree(*ability, wr, "GetRootByWindow", kMaxNodes);
            }
        } else {
            printf("[a11y] no window matched windowId=%d（可用窗口: ", wantWindowId);
            for (auto &w : windows) {
                printf("%d ", w.GetWindowId());
            }
            printf("）\n");
        }
    }

    ability->Disconnect(0);
    printf("[a11y] summary: withText=%d clickable=%d\n", g_withText, g_clickable);
    printf("[a11y] texts=[");
    for (size_t i = 0; i < g_texts.size(); ++i) {
        printf("%s\"%s\"", i ? "," : "", Esc(g_texts[i]).c_str());
    }
    printf("]\n");
    const char *verdict = (printed <= 0) ? "NO-TREE"
        : (g_withText > 0 ? "TREE-OK-WITH-TEXT" : "TREE-OK-NO-TEXT");
    printf("[a11y] DONE connected=%d printed=%d withText=%d verdict=%s\n",
           ok ? 1 : 0, printed, g_withText, verdict);
    if (printed <= 0) {
        return 4;
    }
    return g_withText > 0 ? 0 : 1;
}
