#!/usr/bin/env python3
"""
Dev-time ITN comparison helper (NOT shipped with product).

Requires: pip install wetext

Usage: type build\no_itn\1.md | python scripts/itn_compare.py
       cat build/no_itn/1.md | python scripts/itn_compare.py

This script reads raw ASR output (with numbers in spoken form, e.g., "二零二六")
from stdin and prints the wetext-normalized (ITN) version to stdout.

Use to compare wetext ITN against built-in FST ITN once an FST is sourced and
integrated. If built-in FST output matches wetext output on real lecture
numbers/dates, it is ready for production use.

Example:
  Input:  "这是二零二六年六月二十五日,讲课内容如下..."
  Output: "这是2026年6月25日,讲课内容如下..."
"""

from wetext import Normalizer
import sys

def main():
    n = Normalizer(lang="zh", operator="itn")
    text = sys.stdin.read()
    print(n.normalize(text))

if __name__ == "__main__":
    main()
