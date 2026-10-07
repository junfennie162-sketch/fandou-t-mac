// lut_screen.h —— S7-1b/S7-2-A：SA 内的「读屏」（无障碍元素树）
//
// 方向性（与用户敲定的设计一致）：**感知（世界→Agent）与执行（Agent→世界）是两个方向**，
// 所以 ReadScreen 是独立能力，不塞进 ExecuteAction；以后的 ReadCamera/ReadAudio/ReadSensor 走同一条线。
//
// 进程级单例：首次调用时 RegisterAbilityListener + Connect（拿 channel），之后复用；
// 断开放在 SA 的 Release 里。任何失败都返回带 error 字段的 JSON（不抛异常、不崩 SA）。
//
// 隐私纪律：屏幕原文**默认不写日志**（日志只记计数与错误码）；返回内容只给通过准入的调用方。
#ifndef OHOS_LUTSA_LUT_SCREEN_H
#define OHOS_LUTSA_LUT_SCREEN_H

#include <cstdint>
#include <string>
#include <vector>

namespace tmac_sa {

// ── 对外（IPC 用）：读当前屏幕并转成有界 JSON ────────────────────────────────
// maxNodes <= 0 → 用默认上限；超过硬上限会被夹住（避免把无障碍服务拖住）。
std::string ReadScreenJson(int maxNodes);

// ── S7-2-A：结构化快照（给 agent 模块用；避免在 agent 里解析 JSON）──────────
// 与 JSON 版共用同一条连接、同一把锁、同一套有界遍历，只是换个输出形态。
struct ScreenElement {
    int64_t a11yId = -1;
    int32_t winId = -1;
    std::string type;
    std::string text;
    bool clickable = false;
    bool visible = false;
    int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;   // 屏幕坐标系（与元素树同一空间）
};

struct ScreenWindow {
    int32_t id = -1;
    int32_t type = -1;
    int32_t layer = -1;
};

struct ScreenSnapshot {
    bool ok = false;              // 连接 + 取根是否成功
    std::string error;            // ok=false 的原因（如 register_failed_ret_1005）
    int32_t connected = 0;
    int32_t user = 0;
    long elapsedMs = 0;
    int visited = 0;              // 有界遍历访问到的节点总数
    int withText = 0;             // 其中带文本的
    int clickable = 0;            // 其中可点的
    bool truncated = false;       // 是否触到预算上限
    std::vector<ScreenWindow> windows;
    std::vector<ScreenElement> elements;   // 只含「带文本或可点」的节点（与 JSON 版一致）
    // 根元素几何（S7-2-B：给 agent 当"屏幕锚点"用——按比例滑动，不写死坐标）
    bool hasRoot = false;
    int64_t rootA11yId = -1;
    int32_t rootX1 = 0, rootY1 = 0, rootX2 = 0, rootY2 = 0;
};

// 读一次结构化快照（失败也返回结构体：ok=false + error 说明原因；不抛异常）
ScreenSnapshot ReadScreenSnapshot(int maxNodes);

// 断开无障碍连接（幂等）；供 SA 的 Release 调用。
void ReleaseScreen();

// 只读状态串（给 GetMetrics 用，不泄露屏幕内容）：connected / absent / error:xxx
std::string ScreenState();

}  // namespace tmac_sa
#endif  // OHOS_LUTSA_LUT_SCREEN_H
