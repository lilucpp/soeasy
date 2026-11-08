#!/usr/bin/env python3
import sys
import os

log_path = sys.argv[1] if len(sys.argv) > 1 else "build/Testing/Temporary/LastTest.log"
if not os.path.exists(log_path):
    print(f"::error::CTest log not found at {log_path}")
    sys.exit(0)

with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
    lines = f.readlines()

failed_found = False
for i, line in enumerate(lines):
    if any(k in line for k in ["***Failed", "***Timeout", "***Exception"]):
        failed_found = True
        snippet = "".join(lines[max(0, i - 10):min(len(lines), i + 30)])
        encoded = snippet.replace("%", "%25").replace("\r", "").replace("\n", "%0A")
        title = line.strip().replace(":", " - ")
        print(f"::error title={title}::{encoded}")

if not failed_found:
    tail_snippet = "".join(lines[-40:])
    encoded = tail_snippet.replace("%", "%25").replace("\r", "").replace("\n", "%0A")
    print(f"::error title=CTestFailedTail::{encoded}")
