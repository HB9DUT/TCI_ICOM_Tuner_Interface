# Ermittelt die Firmware-Version aus dem Git-Tag (v2.4.0 -> 2.4.0) und übergibt
# sie per Umgebungsvariable an CMake (CMakeLists.txt).
#
# Läuft im Python von PlatformIO, nicht in CMake: Das cmake-Paket von PlatformIO
# ist auf Apple-Silicon-Macs ein x86_64-Programm und kann /usr/bin/git dort nicht
# starten (xcrun-Fehler), die Version wäre dann 0.0.0-dev.
#
# CMake übernimmt die Version nur beim Konfigurieren. Ändert sie sich (neuer
# Commit oder Tag), wird build.ninja gelöscht; PlatformIO konfiguriert dann neu.

import os
import subprocess

Import("env")

try:
    ver = subprocess.check_output(
        ["git", "describe", "--tags", "--always", "--dirty"],
        cwd=env.subst("$PROJECT_DIR"),
        stderr=subprocess.DEVNULL,
        text=True,
    ).strip()
except (OSError, subprocess.CalledProcessError):
    ver = ""
ver = ver[1:] if ver.startswith("v") else ver
if not ver:
    ver = "0.0.0-dev"

os.environ["FW_VERSION"] = ver

build_dir = env.subst("$BUILD_DIR")
stamp = os.path.join(build_dir, "fw_version.txt")
try:
    with open(stamp) as f:
        old = f.read().strip()
except OSError:
    old = None
if old != ver:
    ninja = os.path.join(build_dir, "build.ninja")
    if os.path.isfile(ninja):
        os.remove(ninja)
    os.makedirs(build_dir, exist_ok=True)
    with open(stamp, "w") as f:
        f.write(ver)

print("Firmware-Version: " + ver)
