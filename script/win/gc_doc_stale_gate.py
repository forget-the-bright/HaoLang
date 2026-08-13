# A01: GC doc stale gate (no full suite)
# Forbid present-tense lies in key docs.
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def fail(msg: str, fails: list[str]) -> None:
    print(f"FAIL {msg}")
    fails.append(msg)


def main() -> int:
    fails: list[str] = []
    contract = (ROOT / "docs" / "IR与GC契约.md").read_text(encoding="utf-8")
    if "span 下一切口" in contract:
        fail("IR与GC契约.md still has 'span 下一切口'", fails)
    else:
        print("OK   contract: no span 下一切口")
    if "余 span" in contract:
        fail("IR与GC契约.md still has '余 span'", fails)
    else:
        print("OK   contract: no 余 span")
    if re.search(r"真\s*size-class\s*SPAN|真\s*SPAN\s*freelist|>32→FULL|>32->FULL", contract):
        fail("IR与GC契约.md still oversells SPAN/FULL", fails)
    else:
        print("OK   contract: no SPAN/FULL oversell")

    dbg = (
        ROOT
        / "docs"
        / "方法论"
        / "调试能力&语言架构设计方法论与约束规范.md"
    ).read_text(encoding="utf-8")
    if re.search(r"memory\(argmem", dbg):
        fail("调试方法论 still has memory(argmem", fails)
    else:
        print("OK   debug meth: no argmem")

    gov = (ROOT / "docs" / "文档治理.md").read_text(encoding="utf-8")
    if "F / GC-NEXT" in gov:
        fail("文档治理.md still lists GC-NEXT as open deferred debt", fails)
    else:
        print("OK   文档治理: GC-NEXT not open debt")

    for rel in ("README.md", "记忆文档.md"):
        text = (ROOT / rel).read_text(encoding="utf-8")
        if "余 span" in text:
            fail(f"{rel} still has '余 span'", fails)
        else:
            print(f"OK   {rel}: no 余 span")
        if "GC-NEXT 下一切口" in text or "GC-NEXT·span" in text:
            fail(f"{rel} still opens GC-NEXT/span as next", fails)
        else:
            print(f"OK   {rel}: no GC-NEXT next-slot")

    meth_readme = (ROOT / "docs" / "方法论" / "README.md").read_text(encoding="utf-8")
    if "GC-NEXT concurrent sweep" in meth_readme:
        fail("方法论/README.md still has GC-NEXT concurrent sweep", fails)
    else:
        print("OK   方法论/README: no stale GC-NEXT concurrent sweep")

    # GC-ALIGN-5：禁止用户面现状谎
    user_docs = [
        ("docs/IR与GC契约.md", contract),
        (
            "docs/方法论/高级语言GC设计全覆盖检查清单.md",
            (ROOT / "docs" / "方法论" / "高级语言GC设计全覆盖检查清单.md").read_text(
                encoding="utf-8"
            ),
        ),
        ("记忆文档.md", (ROOT / "记忆文档.md").read_text(encoding="utf-8")),
        ("docs/坑债.md", (ROOT / "docs" / "坑债.md").read_text(encoding="utf-8")),
    ]
    for rel, text in user_docs:
        if "跨线程 VERIFY 未交付" in text:
            fail(f"{rel} still claims 跨线程 VERIFY 未交付", fails)
        else:
            print(f"OK   {rel}: no 跨线程 VERIFY 未交付")
        if "弱/软引用仍 P2" in text or "弱/软仍 P2" in text:
            fail(f"{rel} still claims 弱/软仍 P2", fails)
        else:
            print(f"OK   {rel}: no 弱/软仍 P2")

    # GC-MSPAN：禁止用户面现状谎「页级 mspan 非目标」
    for rel, text in user_docs:
        # 允许历史句含「当时」「推翻」「已交付」「作废」
        for m in re.finditer(r".{0,40}页级 mspan\s*非目标.{0,40}", text):
            snip = m.group(0)
            if not re.search(r"当时|推翻|已交付|作废|→\s*\*?0\.73", snip):
                fail(f"{rel} still claims 页级 mspan 非目标 as present: {snip.strip()}", fails)
                break
        else:
            print(f"OK   {rel}: no present-tense 页级 mspan 非目标")

    if fails:
        print(f"GC_DOC_STALE_GATE FAIL ({len(fails)})")
        return 1
    print("GC_DOC_STALE_GATE OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
