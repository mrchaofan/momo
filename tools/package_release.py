#!/usr/bin/env python3
"""把一个已完成的 Release 构建打成发布产物(docs/09 §6), 命名与布局参照 Electron 的 release。

用法:
  python3 tools/package_release.py --build-dir out/release --out out/dist
  python3 tools/package_release.py --build-dir out/release --out out/dist --platform-independent

产物(<v> = framework/packages/momo/package.json 的 version, <p> = darwin-arm64 / win32-x64, 即 Node 的
process.platform-process.arch):
  momo-v<v>-<p>.zip            运行时。macOS: Momo.app;Windows: Momo.exe + libnode.dll + icudtl.dat + bundle.js。
                               另有 version 文件。解压后即 `momo build --runtime=` 接受的运行时。
  momo-headless-v<v>-<p>.zip   momo_headless(无窗口运行 bundle 的测试运行器)+ libnode
  momo-ext-sdk-v<v>-<p>.zip    原生扩展 SDK: include/{momo, node}、lib/cmake/MomoExtSdk/;Windows 另有
                               Release/node.lib(libnode.dll 的导入库)
  --platform-independent 时另出(与平台无关, 发布流程里只由一个平台产出):
  momo-framework-v<v>.tgz      @momo/framework 的 npm 包(npm pack)
  momo-cli-v<v>.tgz            @momo/cli 的 npm 包: momo bundle / build / ext。版本必须与 @momo/framework 相同。

各包的内容由 CMake 的安装组件(runtime / headless / sdk)决定, 本脚本只负责 暂存 → 签名 → 压缩。
macOS 上的二进制是 ad-hoc 签名, 没有 Developer ID 签名与公证(应用开发者对自己的 .app 签名)。
SHASUMS256.txt 由发布流程在汇总全部平台的产物后生成(.github/workflows/release.yml)。
仅依赖 Python 3.9 标准库。
"""

import argparse
import json
import os
import shutil
import stat
import subprocess
import sys
import zipfile

from fetch_deps import ROOT, host_platform

FRAMEWORK_PACKAGE = os.path.join(ROOT, "framework", "packages", "momo")
CLI_PACKAGE = os.path.join(ROOT, "framework", "packages", "cli")


def log(msg):
    print(f"[package_release] {msg}", flush=True)


def release_platform():
    """Node 的 process.platform-process.arch(Electron 产物的命名方式)。"""
    system, arch = host_platform().split("-")
    node_platform = {"macos": "darwin", "windows": "win32"}.get(system)
    if node_platform is None:
        sys.exit(f"package_release: unsupported platform {sys.platform}")
    return f"{node_platform}-{arch}"


def write_zip(source_dir, archive):
    """把 source_dir 的内容(不含该目录本身)压成 zip;保留可执行位与符号链接, 条目按路径排序。"""
    entries = []
    for parent, dirs, files in os.walk(source_dir):
        # 指向目录的符号链接作为链接保存, 不展开。
        for name in list(dirs):
            if os.path.islink(os.path.join(parent, name)):
                dirs.remove(name)
                files.append(name)
        entries.extend(os.path.join(parent, name) for name in files)
    if not entries:
        sys.exit(f"package_release: nothing to package in {source_dir}")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(entries):
            name = os.path.relpath(path, source_dir).replace(os.sep, "/")
            if os.path.islink(path):
                info = zipfile.ZipInfo.from_file(path, name)
                info.external_attr = (stat.S_IFLNK | 0o755) << 16
                z.writestr(info, os.readlink(path))
            else:
                z.write(path, name)
    log(f"{os.path.basename(archive)}  {os.path.getsize(archive) / 1e6:.1f} MB")


def install_component(build_dir, config, component, prefix):
    shutil.rmtree(prefix, ignore_errors=True)
    subprocess.run(["cmake", "--install", build_dir, "--config", config, "--component", component,
                    "--prefix", prefix], check=True, stdout=subprocess.DEVNULL)
    if not os.path.isdir(prefix) or not os.listdir(prefix):
        sys.exit(f"package_release: install component '{component}' produced nothing "
                 f"(is {build_dir} a finished build with Skia and libnode?)")


def package_version(package_dir):
    with open(os.path.join(package_dir, "package.json"), encoding="utf-8") as f:
        return json.load(f)["version"]


def npm_pack(package_dir, stage, archive):
    """npm pack 一个包, 产物改名为 archive。"""
    npm = shutil.which("npm")
    if npm is None:
        sys.exit("package_release: npm not found (needed for the npm packages)")
    packed = subprocess.run([npm, "pack", "--json", "--pack-destination", stage], cwd=package_dir,
                            check=True, capture_output=True, text=True)
    os.replace(os.path.join(stage, json.loads(packed.stdout)[0]["filename"]), archive)
    log(f"{os.path.basename(archive)}  {os.path.getsize(archive) / 1e6:.1f} MB")


def adhoc_sign(path):
    """安装时改写了 rpath, 原签名失效;arm64 上未签名的二进制无法运行。"""
    if sys.platform == "darwin":
        subprocess.run(["codesign", "--force", "--deep", "--sign", "-", path], check=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", required=True, help="已构建完成的 CMake 构建目录")
    parser.add_argument("--out", required=True, help="产物输出目录")
    parser.add_argument("--config", default="Release", help="多配置生成器(Visual Studio)的配置名")
    parser.add_argument("--platform-independent", action="store_true",
                        help="另出与平台无关的产物(框架与 CLI 的 npm 包)")
    args = parser.parse_args()

    build_dir, out = os.path.abspath(args.build_dir), os.path.abspath(args.out)
    version = package_version(FRAMEWORK_PACKAGE)
    if package_version(CLI_PACKAGE) != version:
        sys.exit(f"package_release: @momo/cli is {package_version(CLI_PACKAGE)} but @momo/framework is {version}; "
                 "the two packages are released together under one version")
    target = release_platform()
    stage = os.path.join(out, ".stage")
    os.makedirs(out, exist_ok=True)

    # 运行时。
    runtime = os.path.join(stage, "runtime")
    install_component(build_dir, args.config, "runtime", runtime)
    if sys.platform == "darwin":
        adhoc_sign(os.path.join(runtime, "Momo.app"))
    with open(os.path.join(runtime, "version"), "w", encoding="utf-8") as f:
        f.write(version)
    write_zip(runtime, os.path.join(out, f"momo-v{version}-{target}.zip"))

    # headless 运行器。
    headless = os.path.join(stage, "headless")
    install_component(build_dir, args.config, "headless", headless)
    adhoc_sign(os.path.join(headless, "momo_headless"))
    write_zip(headless, os.path.join(out, f"momo-headless-v{version}-{target}.zip"))

    # 原生扩展 SDK(Windows 的包里有 libnode 的导入库, 所以按平台出)。
    sdk = os.path.join(stage, "sdk")
    install_component(build_dir, args.config, "sdk", sdk)
    write_zip(sdk, os.path.join(out, f"momo-ext-sdk-v{version}-{target}.zip"))

    if args.platform_independent:
        npm_pack(FRAMEWORK_PACKAGE, stage, os.path.join(out, f"momo-framework-v{version}.tgz"))
        npm_pack(CLI_PACKAGE, stage, os.path.join(out, f"momo-cli-v{version}.tgz"))

    shutil.rmtree(stage)


if __name__ == "__main__":
    main()
