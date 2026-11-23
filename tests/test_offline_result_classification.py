#!/usr/bin/env python3
"""Regression for offline suite reporting: skipped subtests are not full passes."""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location("offline_suite", Path(__file__).with_name("run_offline_suite.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
classify = module.classify_result

assert classify(0, "OK (skipped=0)\n") == ("PASS", 0)
assert classify(0, "OK (skipped=2)\n") == ("SKIP", 2)
assert classify(0, "SKIP: hardware unavailable\n") == ("SKIP", 0)
assert classify(1, "OK (skipped=2)\n") == ("FAIL", 2)
assert classify(None, "timeout") == ("FAIL", 0)
assert classify(0, "all tests passed\n") == ("PASS", 0)
print("PASS: offline result classification preserves failures and skipped subtests")
