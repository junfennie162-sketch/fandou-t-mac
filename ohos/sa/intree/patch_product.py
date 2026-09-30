#!/usr/bin/env python3
"""把 vendor/ohemu/lutsa 组件挂到产品 config.json 的 subsystems 列表（幂等）。

OH 的产品配置是「带注释的 JSON」，这里先剥注释再解析，写回标准 JSON（其解析器兼容）。
"""
import json
import re
import sys


def strip_comments(text: str) -> str:
    out = []
    in_str = False
    esc = False
    i = 0
    while i < len(text):
        ch = text[i]
        if in_str:
            out.append(ch)
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
            out.append(ch)
            i += 1
            continue
        if ch == "/" and i + 1 < len(text) and text[i + 1] == "/":
            while i < len(text) and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < len(text) and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = len(text) if j < 0 else j + 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def main() -> int:
    path = sys.argv[1]
    raw = open(path, encoding="utf-8").read()
    cfg = json.loads(strip_comments(raw))

    subs = cfg.setdefault("subsystems", [])
    entry = next((s for s in subs if s.get("subsystem") == "ohemu"), None)
    comp = {"component": "lutsa", "features": []}
    if entry is None:
        subs.append({"subsystem": "ohemu", "components": [comp]})
        action = "新增 ohemu 子系统条目"
    else:
        comps = entry.setdefault("components", [])
        if any(c.get("component") == "lutsa" for c in comps):
            action = "已存在，跳过"
        else:
            comps.append(comp)
            action = "并入既有 ohemu 子系统条目"

    open(path, "w", encoding="utf-8").write(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n")
    print(f"  {action}: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
