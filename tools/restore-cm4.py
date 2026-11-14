#!/usr/bin/env python3
"""Restore this snapshot into the matching STM32CubeMP1 example."""
from pathlib import Path
from datetime import datetime
import argparse
import shutil
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser()
parser.add_argument("--cube", type=Path, default=(
    Path.home() / "STM32MPU_workspace/STM32MPU-Ecosystem-v6.2.1"
    / "Developer-Package/STM32Cube_FW_MP1_V1.7.0"
))
parser.add_argument("--apply", action="store_true")
args = parser.parse_args()

repo = Path(__file__).resolve().parent.parent
cube = args.cube.expanduser().resolve()
app = cube / "Projects/STM32MP157C-DK2/Applications/OpenAMP/OpenAMP_TTY_echo"
project = app / "STM32CubeIDE/CM4/.project"

if not project.is_file():
    raise SystemExit(f"找不到官方 CM4 專案：{project}")

if ET.parse(project).getroot().findtext("name") != "OpenAMP_TTY_echo_CM4":
    raise SystemExit("目標專案名稱不符，停止。")

pairs = [
    ("m4-firmware/source/main.c", "Src/main.c"),
    ("m4-firmware/source/stm32mp1xx_hal_msp.c", "Src/stm32mp1xx_hal_msp.c"),
    ("m4-firmware/include/main.h", "Inc/main.h"),
    ("m4-firmware/include/stm32mp1xx_hal_conf.h", "Inc/stm32mp1xx_hal_conf.h"),
    ("m4-firmware/vl53l0x/port/mp1_vl53l0x.c", "Src/mp1_vl53l0x.c"),
    ("m4-firmware/vl53l0x/port/mp1_vl53l0x.h", "Inc/mp1_vl53l0x.h"),
    ("m4-firmware/cubeide/CM4/.project", "STM32CubeIDE/CM4/.project"),
    ("m4-firmware/cubeide/CM4/.cproject", "STM32CubeIDE/CM4/.cproject"),
]

vendor = repo / "m4-firmware/vl53l0x/vendor"
files = sorted(p for p in vendor.rglob("*") if p.suffix in (".c", ".h"))

if len(files) != 18:
    raise SystemExit(f"原廠感測器來源應有 18 個，目前為 {len(files)}。")

for source in files:
    folder = "Src" if source.suffix == ".c" else "Inc"
    pairs.append((str(source.relative_to(repo)), f"{folder}/{source.name}"))

if len({dst for _, dst in pairs}) != len(pairs):
    raise SystemExit("目的檔案名稱重複，停止。")

for source, _ in pairs:
    path = repo / source
    if not path.is_file() or path.stat().st_size == 0:
        raise SystemExit(f"缺少來源：{path}")

print(f"目標：{app}")
print(f"來源：{repo}")
print(f"將還原 {len(pairs)} 個檔案。")

if not args.apply:
    print("檢查通過，未寫入。關閉 CubeIDE 後加上 --apply 才會套用。")
    raise SystemExit(0)

backup = (
    repo / "build/cm4-backups"
    / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
)
backup.mkdir(parents=True, exist_ok=False)

for _, dest in pairs:
    original = app / dest
    if original.exists():
        target = backup / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, target)

for source, dest in pairs:
    target = app / dest
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(repo / source, target)

print(f"原檔備份：{backup}")
print("已還原。重新開啟 CubeIDE，重新整理、清除並建置 CM4 專案。")
