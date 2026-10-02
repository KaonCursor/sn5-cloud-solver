#!/usr/bin/env python3
"""Offline g++ build of libyaml-cpp.a without cmake: rebuild only changed translation units.

Objects in .prebuilt/obj keyed by the content hash of each source plus the headers g++
reported for it (-MMD); the archive is rebuilt when any object changed.
"""

import concurrent.futures
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".prebuilt"
OBJ = OUT / "obj"
FLAGS = ["g++", "-std=c++17", "-O1", "-g0", "-w", "-Iinclude", "-Isrc"]


def key(source, depfile):
    digest = hashlib.sha256(json.dumps(FLAGS).encode() + (ROOT / source).read_bytes())
    if depfile.exists():
        for dep in sorted(set(depfile.read_text().replace("\\\n", " ").split()[1:])):
            path = Path(dep) if dep.startswith("/") else ROOT / dep
            if not path.exists():
                return None
            digest.update(dep.encode() + path.read_bytes())
    return digest.hexdigest()


def compile_one(source, manifest):
    obj = OBJ / (Path(source).stem + ".o")
    dep = obj.with_suffix(".d")
    current = key(source, dep)
    if current and manifest.get(source) == current and obj.exists():
        return source, current, False
    done = subprocess.run([*FLAGS, "-MMD", "-MF", str(dep), "-c", source, "-o", str(obj)],
                          cwd=ROOT, capture_output=True, text=True)
    if done.returncode:
        sys.stderr.write(done.stderr[-4000:])
        raise SystemExit(f"compile failed: {source}")
    return source, key(source, dep), True


def main():
    OBJ.mkdir(parents=True, exist_ok=True)
    record = OUT / "build-manifest.json"
    manifest = json.loads(record.read_text()) if record.exists() else {}
    sources = sorted(p.relative_to(ROOT).as_posix() for p in (ROOT / "src").rglob("*.cpp"))
    changed = 0
    with concurrent.futures.ThreadPoolExecutor(2) as pool:
        for source, digest, rebuilt in pool.map(lambda s: compile_one(s, manifest), sources):
            manifest[source] = digest
            changed += rebuilt
    library = OUT / "libyaml-cpp.a"
    if changed or not library.exists():
        library.unlink(missing_ok=True)
        subprocess.run(["ar", "rcs", str(library),
                        *[str(OBJ / (Path(s).stem + ".o")) for s in sources]], check=True)
    record.write_text(json.dumps(manifest, indent=0, sort_keys=True))
    print(f"yaml-cpp build: {changed} translation units rebuilt")


if __name__ == "__main__":
    main()
