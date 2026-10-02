#!/usr/bin/env python3
"""Run tasks/<id>/solution.{rs,py} against tasks/<id>/cases.json."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def run_rust(task: Path, cases: list[dict], timeout: float) -> list[tuple[bool, str]]:
    env = dict(os.environ, PATH=f"{Path.home() / '.cargo/bin'}:{os.environ['PATH']}")
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "solution"
        build = subprocess.run(
            ["rustc", "-O", "--edition", "2021", "-o", str(binary), str(task / "solution.rs")],
            capture_output=True, text=True, env=env,
        )
        if build.returncode != 0:
            return [(False, "compile error:\n" + build.stderr)] * len(cases)
        results = []
        for case in cases:
            try:
                proc = subprocess.run(
                    [str(binary)], input=case["args"][0], capture_output=True,
                    text=True, timeout=timeout,
                )
            except subprocess.TimeoutExpired:
                results.append((False, "timeout"))
                continue
            ok = proc.stdout.split() == case["expected"].split()
            results.append((ok, "" if ok else f"got {proc.stdout!r}"))
        return results


def run_python(task: Path, entrypoint: str, cases: list[dict]) -> list[tuple[bool, str]]:
    spec = importlib.util.spec_from_file_location("solution", task / "solution.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    func = getattr(module, entrypoint)
    results = []
    for case in cases:
        try:
            actual = json.loads(json.dumps(func(*case["args"], **case.get("kwargs", {}))))
        except Exception as error:  # noqa: BLE001
            results.append((False, f"raised {error!r}"))
            continue
        ok = actual == case["expected"]
        results.append((ok, "" if ok else f"got {actual!r}"))
    return results


def main() -> int:
    task = Path(sys.argv[1])
    payload = json.loads((task / "cases.json").read_text())
    cases = payload["cases"]
    if payload["language"] == "rust":
        results = run_rust(task, cases, timeout=5.0)
    else:
        results = run_python(task, payload["entrypoint"], cases)
    passed = 0
    for index, (case, (ok, detail)) in enumerate(zip(cases, results), 1):
        label = case.get("name", f"case {index}")
        passed += ok
        print(f"{'PASS' if ok else 'FAIL'} {index}: {label}" + ("" if ok else f" ({detail})"))
    print(f"{passed}/{len(cases)} cases passed")
    return 0 if passed == len(cases) else 1


if __name__ == "__main__":
    sys.exit(main())
