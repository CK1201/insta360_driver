#!/usr/bin/env python3
"""Prepare a project-local Android toolchain and private Maven configuration."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import urllib.request
import urllib.parse
import zipfile


ROOT = Path(__file__).resolve().parent.parent
TOOLS_URL = "https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-zip", type=Path, required=True)
    parser.add_argument("--credentials-only", action="store_true")
    parser.add_argument("--proxy", help="HTTP proxy URL for restricted download networks")
    args = parser.parse_args()
    sdkmanager_proxy = []
    if args.proxy:
        proxy = urllib.parse.urlparse(args.proxy)
        if proxy.scheme != "http" or not proxy.hostname or not proxy.port:
            parser.error("--proxy requires http://HOST:PORT")
        urllib.request.install_opener(urllib.request.build_opener(
            urllib.request.ProxyHandler({"http": args.proxy, "https": args.proxy})))
        sdkmanager_proxy = ["--proxy=http", f"--proxy_host={proxy.hostname}", f"--proxy_port={proxy.port}"]
        gradle_home = ROOT / ".local/gradle"
        gradle_home.mkdir(parents=True, exist_ok=True)
        (gradle_home / "gradle.properties").write_text(
            f"systemProp.http.proxyHost={proxy.hostname}\nsystemProp.http.proxyPort={proxy.port}\n"
            f"systemProp.https.proxyHost={proxy.hostname}\nsystemProp.https.proxyPort={proxy.port}\n"
            "systemProp.http.nonProxyHosts=localhost|127.*|androidsdk.insta360.com|maven.aliyun.com\n")
    with zipfile.ZipFile(args.sdk_zip) as package:
        text = package.read("Android-SDK-2.2.6/settings.gradle.kts").decode()
    patterns = {
        "sdk.repository.url": r'url\s*=\s*uri\("(https://androidsdk[^\"]+)"\)',
        "sdk.repository.username": r'username\s*=\s*"([^\"]+)"',
        "sdk.repository.password": r'password\s*=\s*"([^\"]+)"',
    }
    values = {}
    for key, pattern in patterns.items():
        match = re.search(pattern, text)
        if not match:
            raise SystemExit("SDK archive does not contain the expected Maven settings")
        values[key] = match.group(1)
    credentials = ROOT / "android/sdk-credentials.properties"
    credentials.write_text("".join(f"{key}={value}\n" for key, value in values.items()))
    credentials.chmod(0o600)
    print("Private SDK Maven settings prepared; values are not printed.", flush=True)
    if args.credentials_only:
        return
    sdk = ROOT / ".local/android-sdk"
    manager = sdk / "cmdline-tools/latest/bin/sdkmanager"
    if not manager.is_file():
        archive = ROOT / ".local/downloads/commandlinetools.zip"
        archive.parent.mkdir(parents=True, exist_ok=True)
        if not archive.is_file():
            print("Downloading Android command-line tools to the project cache.", flush=True)
            with urllib.request.urlopen(TOOLS_URL, timeout=60) as source, archive.open("wb") as target:
                shutil.copyfileobj(source, target)
        destination = sdk / "cmdline-tools/latest"
        destination.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive) as package:
            for member in package.infolist():
                path = Path(member.filename)
                if member.is_dir() or path.parts[0] != "cmdline-tools":
                    continue
                relative = Path(*path.parts[1:])
                if ".." in relative.parts:
                    raise SystemExit("Invalid command-line-tools archive path")
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(package.read(member))
                mode = member.external_attr >> 16
                if mode & 0o111:
                    target.chmod(0o755)
    print("Preparing Android API 35 build tools; installation stays in .local/.", flush=True)
    subprocess.run([str(manager), f"--sdk_root={sdk}", *sdkmanager_proxy, "--licenses"], input="y\n" * 100,
                   text=True, stdout=subprocess.DEVNULL, check=True)
    subprocess.run([str(manager), f"--sdk_root={sdk}", *sdkmanager_proxy, "platforms;android-35", "build-tools;35.0.0"],
                   check=True)
    (ROOT / "android/local.properties").write_text(f"sdk.dir={sdk}\n")
    print("Android toolchain ready. Build with scripts/build_android.sh.", flush=True)


if __name__ == "__main__":
    main()
