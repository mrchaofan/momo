#!/usr/bin/env python3
"""按 cmake/deps.lock.json 下载、校验并解压预编译三方依赖(docs/09 §2)。

用法:
  python3 tools/fetch_deps.py                 # 当前平台的全部依赖
  python3 tools/fetch_deps.py skia v8         # 指定依赖
  python3 tools/fetch_deps.py --platform windows-x64
  python3 tools/fetch_deps.py --mirror DIR    # 离线镜像: 从 DIR/<archive 文件名> 取包, 不联网

产物:
  third_party/.downloads/<archive>            下载缓存(断点续传, SHA256 校验)
  third_party/prebuilt/<name>/                解压结果 + .momo_dep.json 戳记(版本/平台/sha256)

下载策略: 分块并行 Range 请求(很多代理按连接限速, 并行可叠加带宽), 每块独立重试, 进度落盘可续传。
仅依赖 Python 3.9 标准库。
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import platform as py_platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK_PATH = os.path.join(ROOT, "cmake", "deps.lock.json")
DOWNLOAD_DIR = os.path.join(ROOT, "third_party", ".downloads")
PREBUILT_DIR = os.path.join(ROOT, "third_party", "prebuilt")
STAMP_NAME = ".momo_dep.json"

CHUNK_SIZE = 1 << 20  # 1 MiB
DEFAULT_JOBS = 16
MAX_RETRIES = 10


def log(msg):
    print(f"[fetch_deps] {msg}", flush=True)


def host_platform():
    system = sys.platform
    machine = py_platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "x64"
    if system == "darwin":
        return f"macos-{arch}"
    if system.startswith("win"):
        return f"windows-{arch}"
    return f"linux-{arch}"


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# 下载


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def resolve_final_url(url, headers):
    """跟随重定向拿到最终 URL 及请求它所需的头(签名 URL 有效期通常 ~1h, 足够一次下载)。"""
    opener = urllib.request.build_opener(_NoRedirect)
    current = url
    for _ in range(10):
        req = urllib.request.Request(current, method="HEAD", headers=headers)
        try:
            with opener.open(req, timeout=60) as resp:
                return current, headers, int(resp.headers.get("Content-Length", "0"))
        except urllib.error.HTTPError as e:
            if e.code in (301, 302, 303, 307, 308):
                current = e.headers["Location"]
                headers = {}  # 不把鉴权头带到 CDN
                continue
            raise
    raise RuntimeError(f"too many redirects: {url}")


def download(url, dest, size, headers, jobs):
    final_url, final_headers, length = resolve_final_url(url, dict(headers))
    if size and length and size != length:
        raise RuntimeError(f"size mismatch: lock={size} server={length}")
    size = size or length
    if not size:
        raise RuntimeError("unknown content length")

    part = dest + ".part"
    progress_path = dest + ".progress.json"
    done = set()
    if os.path.exists(part) and os.path.exists(progress_path):
        with open(progress_path) as f:
            saved = json.load(f)
        if saved.get("size") == size and saved.get("chunk") == CHUNK_SIZE:
            done = set(saved.get("done", []))
    if not os.path.exists(part) or not done:
        with open(part, "wb") as f:
            f.truncate(size)
        done = set()

    chunks = [(i, i * CHUNK_SIZE, min(size, (i + 1) * CHUNK_SIZE) - 1)
              for i in range((size + CHUNK_SIZE - 1) // CHUNK_SIZE)]
    todo = [c for c in chunks if c[0] not in done]
    lock = threading.Lock()
    received = [sum(min(size, (i + 1) * CHUNK_SIZE) - i * CHUNK_SIZE for i in done)]
    started = time.time()

    def save_progress():
        with open(progress_path, "w") as f:
            json.dump({"size": size, "chunk": CHUNK_SIZE, "done": sorted(done)}, f)

    def fetch_chunk(chunk):
        index, start, end = chunk
        for attempt in range(MAX_RETRIES):
            try:
                req = urllib.request.Request(
                    final_url, headers={**final_headers, "Range": f"bytes={start}-{end}"})
                with urllib.request.urlopen(req, timeout=120) as resp:
                    data = resp.read()
                if len(data) != end - start + 1:
                    raise RuntimeError(f"short chunk {index}: {len(data)}")
                with lock:
                    with open(part, "r+b") as f:
                        f.seek(start)
                        f.write(data)
                    done.add(index)
                    received[0] += len(data)
                    save_progress()
                return
            except Exception as e:  # noqa: BLE001  网络错误一律重试
                if attempt == MAX_RETRIES - 1:
                    raise RuntimeError(f"chunk {index} failed: {e}") from e
                time.sleep(min(30, 2 ** attempt))

    log(f"downloading {os.path.basename(dest)} ({size / 1e6:.1f} MB, {len(todo)} chunks left, {jobs} connections)")
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(fetch_chunk, c) for c in todo]
        last = 0.0
        for fut in concurrent.futures.as_completed(futures):
            fut.result()
            now = time.time()
            if now - last > 5:
                last = now
                speed = received[0] / max(1e-3, now - started) / 1e3
                log(f"  {received[0] / size * 100:5.1f}%  ~{speed:.0f} KB/s")
    os.replace(part, dest)
    os.remove(progress_path)


def obtain_archive(name, entry, mirror, jobs):
    archive_name = os.path.basename(entry["url"].split("?")[0])
    if entry["url"].startswith("https://ghcr.io/"):
        archive_name = f"{name}-{entry['sha256'][:12]}.{entry['archive']}"
    os.makedirs(DOWNLOAD_DIR, exist_ok=True)
    dest = os.path.join(DOWNLOAD_DIR, archive_name)
    if os.path.exists(dest) and sha256_of(dest) == entry["sha256"]:
        log(f"{name}: cached {archive_name}")
        return dest
    if mirror:
        src = os.path.join(mirror, archive_name)
        if not os.path.exists(src):
            raise RuntimeError(f"{name}: not found in mirror: {src}")
        shutil.copyfile(src, dest)
    else:
        download(entry["url"], dest, entry.get("size", 0), entry.get("http_headers", {}), jobs)
    digest = sha256_of(dest)
    if digest != entry["sha256"]:
        os.remove(dest)
        raise RuntimeError(f"{name}: sha256 mismatch: got {digest}, want {entry['sha256']}")
    log(f"{name}: sha256 ok")
    return dest


# ---------------------------------------------------------------------------
# 解压与后处理


def safe_extract(archive, kind, staging):
    """解压到全新空目录, 拒绝路径穿越与绝对路径。"""
    def check(member_name):
        target = os.path.realpath(os.path.join(staging, member_name))
        if not target.startswith(os.path.realpath(staging) + os.sep):
            raise RuntimeError(f"unsafe path in archive: {member_name}")

    if kind == "zip":
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                check(info.filename)
            z.extractall(staging)
    elif kind in ("tar.gz", "tgz"):
        with tarfile.open(archive, "r:gz") as t:
            for m in t.getmembers():
                check(m.name)
                if m.issym() or m.islnk():
                    check(os.path.join(os.path.dirname(m.name), m.linkname))
            t.extractall(staging)
    else:
        raise RuntimeError(f"unsupported archive type: {kind}")


def homebrew_relocate(root):
    """Homebrew bottle 的 dylib 使用 @@HOMEBREW_*@@ 占位路径: 改为 @rpath 并 ad-hoc 重签名。"""
    dylibs = []
    for dirpath, _, files in os.walk(root):
        for f in files:
            p = os.path.join(dirpath, f)
            if f.endswith(".dylib") and not os.path.islink(p):
                dylibs.append(p)
    names = {os.path.basename(p) for p in dylibs}
    for path in dylibs:
        base = os.path.basename(path)
        out = subprocess.run(["otool", "-L", path], check=True, capture_output=True, text=True).stdout
        args = ["install_name_tool", "-id", f"@rpath/{base}"]
        for line in out.splitlines()[1:]:
            dep = line.strip().split(" (")[0]
            if "@@HOMEBREW" in dep or (os.path.basename(dep) in names and not dep.startswith("@rpath/")):
                args += ["-change", dep, f"@rpath/{os.path.basename(dep)}"]
        rpaths = subprocess.run(["otool", "-l", path], check=True, capture_output=True, text=True).stdout
        lines = rpaths.splitlines()
        for i, line in enumerate(lines):
            if "cmd LC_RPATH" in line:
                rp = lines[i + 2].strip().split("path ", 1)[1].split(" (offset")[0]
                if "@@HOMEBREW" in rp:
                    args += ["-delete_rpath", rp]
        args.append(path)
        os.chmod(path, 0o755)
        subprocess.run(args, check=True, capture_output=True)
        subprocess.run(["codesign", "--force", "--sign", "-", path], check=True, capture_output=True)
    log(f"relocated {len(dylibs)} dylib(s) to @rpath")


POST_EXTRACT = {"homebrew_relocate": homebrew_relocate}


def install(name, dep, entry, archive, platform_key):
    dest = os.path.join(PREBUILT_DIR, name)
    os.makedirs(PREBUILT_DIR, exist_ok=True)
    staging = tempfile.mkdtemp(prefix=f".{name}-", dir=PREBUILT_DIR)
    try:
        safe_extract(archive, entry["archive"], staging)
        content = os.path.join(staging, entry["strip_prefix"]) if entry.get("strip_prefix") else staging
        if not os.path.isdir(content):
            raise RuntimeError(f"{name}: strip_prefix not found: {entry['strip_prefix']}")
        post = entry.get("post_extract")
        if post:
            POST_EXTRACT[post](content)
        stamp = {"name": name, "version": dep["version"], "platform": platform_key,
                 "sha256": entry["sha256"]}
        with open(os.path.join(content, STAMP_NAME), "w") as f:
            json.dump(stamp, f, indent=2)
        if os.path.exists(dest):
            shutil.rmtree(dest)
        os.replace(content, dest)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    log(f"{name}: installed {dep['version']} → {os.path.relpath(dest, ROOT)}")


def is_installed(name, dep, entry, platform_key):
    stamp = os.path.join(PREBUILT_DIR, name, STAMP_NAME)
    if not os.path.exists(stamp):
        return False
    with open(stamp) as f:
        s = json.load(f)
    return (s.get("version") == dep["version"] and s.get("platform") == platform_key
            and s.get("sha256") == entry["sha256"])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("deps", nargs="*", help="依赖名(默认全部)")
    parser.add_argument("--platform", default=host_platform())
    parser.add_argument("--mirror", help="离线镜像目录")
    parser.add_argument("--jobs", type=int, default=DEFAULT_JOBS, help="并行连接数")
    parser.add_argument("--force", action="store_true", help="忽略已安装戳记, 重新解压")
    args = parser.parse_args()

    with open(LOCK_PATH) as f:
        lock = json.load(f)
    names = args.deps or list(lock["deps"].keys())
    failed = False
    for name in names:
        dep = lock["deps"].get(name)
        if dep is None:
            log(f"unknown dependency: {name}")
            failed = True
            continue
        entry = dep["platforms"].get(args.platform)
        if entry is None:
            log(f"{name}: no prebuilt for {args.platform}, skipped")
            continue
        if not args.force and is_installed(name, dep, entry, args.platform):
            log(f"{name}: up to date ({dep['version']})")
            continue
        try:
            archive = obtain_archive(name, entry, args.mirror, args.jobs)
            install(name, dep, entry, archive, args.platform)
        except Exception as e:  # noqa: BLE001
            log(f"{name}: FAILED: {e}")
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
