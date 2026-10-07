// screen_digest.cpp —— S7-2-A：读屏快照 → 决策摘要
#include "agent/screen_digest.h"

#include <cinttypes>
#include <cstdio>

namespace tmac_sa {
namespace agent {

std::string ElementRef::Describe() const
{
    char buf[256];
    std::string t = text;
    if (t.size() > 40) {
        t.resize(40);
        t += "...";
    }
    snprintf(buf, sizeof(buf), "%s#%" PRId64 " \"%s\" box=[%d,%d,%d,%d] click=%d win=%d",
             type.c_str(), a11yId, t.c_str(), x1, y1, x2, y2, clickable ? 1 : 0, winId);
    return buf;
}

std::string Digest::Summary() const
{
    char buf[256];
    snprintf(buf, sizeof(buf), "windows=%d ids=[%s] visited=%d withText=%d clickable=%d%s",
             windows, windowIds.c_str(), visited, withText, clickable, truncated ? " truncated=1" : "");
    std::string out = buf;
    out += " elements=[";
    for (size_t i = 0; i < elements.size() && i < 8; ++i) {
        if (i) out += " | ";
        const ElementRef &e = elements[i];
        std::string t = e.text.empty() ? e.type : e.text;
        if (t.size() > 18) {
            t.resize(18);
            t += "..";
        }
        out += t;
    }
    if (elements.size() > 8) {
        out += " | ...";
    }
    out += "]";
    return out;
}

const ElementRef *Digest::FindByText(const std::string &sub) const
{
    if (sub.empty()) {
        return nullptr;
    }
    for (const auto &e : elements) {
        if (!e.text.empty() && e.text.find(sub) != std::string::npos) {
            return &e;
        }
    }
    return nullptr;
}

const ElementRef *Digest::FindByType(const std::string &type) const
{
    for (const auto &e : elements) {
        if (e.type == type) {
            return &e;
        }
    }
    return nullptr;
}

bool Digest::HasWindowType(int32_t type) const
{
    for (int32_t t : windowTypes) {
        if (t == type) {
            return true;
        }
    }
    return false;
}

Digest BuildDigest(const ScreenSnapshot &snap)
{
    Digest d;
    d.ok = snap.ok;
    d.error = snap.error;
    d.visited = snap.visited;
    d.withText = snap.withText;
    d.clickable = snap.clickable;
    d.truncated = snap.truncated;
    d.windows = static_cast<int>(snap.windows.size());
    d.hasRoot = snap.hasRoot;
    d.rootA11yId = snap.rootA11yId;
    d.rootX1 = snap.rootX1;
    d.rootY1 = snap.rootY1;
    d.rootX2 = snap.rootX2;
    d.rootY2 = snap.rootY2;
    for (const auto &w : snap.windows) {
        if (!d.windowIds.empty()) {
            d.windowIds += " ";
        }
        d.windowIds += std::to_string(w.id);
        d.windowTypes.push_back(w.type);
    }
    for (const auto &e : snap.elements) {
        ElementRef r;
        r.a11yId = e.a11yId;
        r.winId = e.winId;
        r.type = e.type;
        r.text = e.text;
        r.clickable = e.clickable;
        r.visible = e.visible;
        r.x1 = e.x1;
        r.y1 = e.y1;
        r.x2 = e.x2;
        r.y2 = e.y2;
        d.elements.push_back(r);
    }
    return d;
}

}  // namespace agent
}  // namespace tmac_sa
