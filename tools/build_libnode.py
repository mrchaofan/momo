#!/usr/bin/env python3
"""从源码构建 libnode(Node.js 共享库)并安装到 third_party/prebuilt/node/(docs/09 §2)。

Node 官方不发布 libnode 二进制, 所以按 cmake/deps.lock.json 的 node 条目从源码构建:
  下载源码包 → SHA256 校验 → configure(--shared --without-npm --ninja)→ ninja libnode
  → 安装头文件(Node 的 tools/install.py --headers-only)与 libnode.<模块版本>.dylib → 写戳记。

用法:
  python3 tools/build_libnode.py               # 已安装且版本一致时跳过
  python3 tools/build_libnode.py --force
  python3 tools/build_libnode.py --jobs 8 --work-dir /tmp/libnode

要求: Python 3.10+(Node 的 configure 要求)、ninja、Xcode 命令行工具。
产物:
  third_party/prebuilt/node/include/node/   node.h、uv.h、v8*.h、libplatform/ …
  third_party/prebuilt/node/lib/libnode.<ver>.dylib(install name @rpath/…)
  third_party/prebuilt/node/.momo_dep.json  戳记(版本/平台/源码 sha256/configure 参数)
"""

import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK_PATH = os.path.join(ROOT, "cmake", "deps.lock.json")
DOWNLOAD_DIR = os.path.join(ROOT, "third_party", ".downloads")
DEST = os.path.join(ROOT, "third_party", "prebuilt", "node")
STAMP_NAME = ".momo_dep.json"


def log(msg):
    print(f"[build_libnode] {msg}", flush=True)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find_python():
    """Node 的 configure 需要 Python 3.10+;当前解释器不够新时在 PATH 与 uv 管理的解释器里找。"""
    if sys.version_info >= (3, 10):
        return sys.executable
    for minor in (14, 13, 12, 11, 10):
        exe = shutil.which(f"python3.{minor}")
        if exe:
            return exe
    uv = shutil.which("uv") or os.path.expanduser("~/.local/bin/uv")
    if os.path.exists(uv):
        for minor in (12, 13, 11):
            r = subprocess.run([uv, "python", "find", f"3.{minor}"], capture_output=True, text=True)
            if r.returncode == 0 and r.stdout.strip():
                return r.stdout.strip()
    sys.exit("build_libnode: Node's configure needs Python 3.10+ (none found)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true", help="rebuild even if installed")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    parser.add_argument("--work-dir", default=os.path.join(ROOT, "third_party", ".build", "node"),
                        help="source tree and build output (large: ~6 GB)")
    args = parser.parse_args()

    if sys.platform != "darwin":
        sys.exit("build_libnode: only macOS is supported for now")
    with open(LOCK_PATH, encoding="utf-8") as f:
        dep = json.load(f)["deps"]["node"]
    version, source = dep["version"], dep["source_archive"]
    configure_flags = dep["configure_flags"]
    stamp = {"name": "node", "version": version, "platform": "macos-arm64",
             "sha256": source["sha256"], "configure_flags": configure_flags}

    stamp_path = os.path.join(DEST, STAMP_NAME)
    if not args.force and os.path.exists(stamp_path):
        with open(stamp_path, encoding="utf-8") as f:
            if json.load(f) == stamp:
                log(f"node {version} already installed")
                return

    # 1. 源码包(缓存于 third_party/.downloads/)。
    os.makedirs(DOWNLOAD_DIR, exist_ok=True)
    archive = os.path.join(DOWNLOAD_DIR, os.path.basename(source["url"]))
    if not os.path.exists(archive) or sha256_of(archive) != source["sha256"]:
        log(f"downloading {source['url']}")
        urllib.request.urlretrieve(source["url"], archive + ".part")
        os.replace(archive + ".part", archive)
    digest = sha256_of(archive)
    if digest != source["sha256"]:
        sys.exit(f"build_libnode: sha256 mismatch for {archive}: {digest}")

    # 2. 解压(源码树按版本区分;已有则复用, 增量构建)。
    os.makedirs(args.work_dir, exist_ok=True)
    src = os.path.join(args.work_dir, f"node-v{version}")
    if not os.path.isdir(src):
        log(f"extracting to {src}")
        with tarfile.open(archive) as tar:
            tar.extractall(args.work_dir, filter="data") if sys.version_info >= (3, 12) else tar.extractall(args.work_dir)

    # 3. configure + ninja libnode。
    python = find_python()
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(python) + os.pathsep + env.get("PATH", "")
    log(f"configure {' '.join(configure_flags)}")
    subprocess.run([python, "configure", *configure_flags], cwd=src, env=env, check=True)
    log(f"ninja libnode (-j{args.jobs}), this takes a while")
    subprocess.run(["ninja", "-C", "out/Release", f"-j{args.jobs}", "libnode"], cwd=src, env=env, check=True)

    # 4. 安装: 头文件 + dylib。
    if os.path.isdir(DEST):
        shutil.rmtree(DEST)
    os.makedirs(os.path.join(DEST, "lib"))
    subprocess.run([python, "tools/install.py", "install", "--headers-only", "--silent",
                    "--dest-dir", DEST, "--prefix", "/"], cwd=src, env=env, check=True)
    # Node 的头文件安装不含 V8 inspector 头(libnode 导出其符号): momo 的调试器需要。
    for header in glob.glob(os.path.join(src, "deps", "v8", "include", "v8-inspector*.h")):
        shutil.copy2(header, os.path.join(DEST, "include", "node"))
    libs = glob.glob(os.path.join(src, "out", "Release", "libnode.*.dylib"))
    if len(libs) != 1:
        sys.exit(f"build_libnode: expected one libnode.*.dylib, found {libs}")
    lib = os.path.join(DEST, "lib", os.path.basename(libs[0]))
    shutil.copy2(libs[0], lib)
    subprocess.run(["install_name_tool", "-id", f"@rpath/{os.path.basename(lib)}", lib], check=True)
    subprocess.run(["codesign", "--force", "--sign", "-", lib], check=True, capture_output=True)

    with open(stamp_path, "w", encoding="utf-8") as f:
        json.dump(stamp, f, indent=2)
    log(f"installed {lib}")


if __name__ == "__main__":
    main()
