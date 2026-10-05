// lut_screen.h —— S7-1b：SA 内的「读屏」（无障碍元素树）
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

#include <string>

namespace tmac_sa {

// 读当前屏幕并转成有界 JSON：窗口列表 + 带文本/可点元素 + 计数 + 采集耗时。
// maxNodes <= 0 → 用默认上限；超过硬上限会被夹住（避免把无障碍服务拖住）。
std::string ReadScreenJson(int maxNodes);

// 断开无障碍连接（幂等）；供 SA 的 Release 调用。
void ReleaseScreen();

// 只读状态串（给 GetMetrics 用，不泄露屏幕内容）：connected / absent / error:xxx
std::string ScreenState();

}  // namespace tmac_sa
#endif  // OHOS_LUTSA_LUT_SCREEN_H
