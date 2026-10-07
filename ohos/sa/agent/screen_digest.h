// screen_digest.h —— S7-2-A：把「读屏快照」压成决策可用的摘要
//
// 职责边界：本模块**只做整理**（文本/类型/可点/几何），不做决策、不做动作。
// 铁律：agent 里所有坐标都必须来自这里（ElementRef 的 box），**禁止硬编码坐标**。
#ifndef OHOS_LUTSA_AGENT_SCREEN_DIGEST_H
#define OHOS_LUTSA_AGENT_SCREEN_DIGEST_H

#include <cstdint>
#include <string>
#include <vector>

#include "lut_screen.h"   // ScreenSnapshot / ScreenElement（tmac_sa 命名空间）

namespace tmac_sa {
namespace agent {

// 感知到的一个元素引用（坐标一律取自元素树，不猜）
struct ElementRef {
    int64_t a11yId = -1;
    int32_t winId = -1;
    std::string type;
    std::string text;
    bool clickable = false;
    bool visible = false;
    int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;

    bool HasBox() const { return (x2 - x1) > 0 && (y2 - y1) > 0; }
    int CenterX() const { return (x1 + x2) / 2; }
    int CenterY() const { return (y1 + y2) / 2; }
    int Width() const { return x2 - x1; }
    int Height() const { return y2 - y1; }
    std::string Describe() const;   // "Text#83 \"上滑解锁\" box=[493,168,546,183] click=0"
};

// 一帧屏幕摘要（有界：elements 只含「带文本或可点」的节点，与 ReadScreen 一致）
struct Digest {
    bool ok = false;
    std::string error;              // ok=false 的原因（透传自快照）
    int windows = 0;
    std::string windowIds;          // "4 2 3 7 9"
    int visited = 0, withText = 0, clickable = 0;
    bool truncated = false;
    std::vector<int32_t> windowTypes;   // 窗口类型（用于识别锁屏/通知层等）
    std::vector<ElementRef> elements;
    // 屏幕锚点（来自感知到的根元素；用于"按屏幕比例"的动作，避免硬编码坐标）
    bool hasRoot = false;
    int64_t rootA11yId = -1;
    int32_t rootX1 = 0, rootY1 = 0, rootX2 = 0, rootY2 = 0;
    bool HasRoot() const { return hasRoot && (rootX2 - rootX1) > 0 && (rootY2 - rootY1) > 0; }

    std::string Summary() const;    // 一行摘要（进 trace；不含过多原文）
    // 查找：文本包含匹配（区分大小写由调用方决定）/ 类型匹配；找不到返回 nullptr
    const ElementRef *FindByText(const std::string &sub) const;
    const ElementRef *FindByType(const std::string &type) const;
    bool HasWindowType(int32_t type) const;
};

Digest BuildDigest(const ScreenSnapshot &snap);

}  // namespace agent
}  // namespace tmac_sa
#endif  // OHOS_LUTSA_AGENT_SCREEN_DIGEST_H
