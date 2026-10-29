#!/usr/bin/env python3
"""Adds the Rover-Bot board to an xiaozhi-esp32 source tree (idempotent).

Usage:  python tools/add_rover_board.py            (run from the project root)
Copy the folder main/boards/rover-bot/ into the tree first.
"""
import re, sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
kconfig = root / "main" / "Kconfig.projbuild"
cmake = root / "main" / "CMakeLists.txt"

if not (root / "main" / "boards" / "rover-bot" / "rover_bot.cc").exists():
    sys.exit("main/boards/rover-bot/ is missing - copy it into the tree first")

k = kconfig.read_text(encoding="utf-8")
if "BOARD_TYPE_ROVER_BOT" not in k:
    anchor = "    config BOARD_TYPE_OTTO_ROBOT\n"
    new = ('    config BOARD_TYPE_ROVER_BOT\n'
           '        bool "Rover-Bot (ESP32-S3 + camera + 2 N20 motors + 3 servos)"\n'
           '        depends on IDF_TARGET_ESP32S3\n'
           '    config ROVER_TFT_FLIP\n'
           '        bool "Rover-Bot TFT: rotate picture 180 degrees"\n'
           '        depends on BOARD_TYPE_ROVER_BOT\n'
           '    config ROVER_TFT_RGB_ORDER\n'
           '        bool "Rover-Bot TFT: RGB colour order (default BGR)"\n'
           '        depends on BOARD_TYPE_ROVER_BOT\n'
           '    config ROVER_TFT_INVERT\n'
           '        bool "Rover-Bot TFT: invert colours"\n'
           '        depends on BOARD_TYPE_ROVER_BOT\n')
    if anchor not in k:
        sys.exit("Kconfig anchor not found - upstream layout changed, add the entry by hand")
    k = k.replace(anchor, new + anchor, 1)
    kconfig.write_text(k, encoding="utf-8")
    print("patched Kconfig.projbuild")

c = cmake.read_text(encoding="utf-8")
if "CONFIG_BOARD_TYPE_ROVER_BOT" not in c:
    anchor = "elseif(CONFIG_BOARD_TYPE_OTTO_ROBOT)\n"
    new = ('elseif(CONFIG_BOARD_TYPE_ROVER_BOT)\n'
           '    set(BOARD_DIR "rover-bot")\n'
           '    set(BUILTIN_TEXT_FONT font_noto_sans_basic_14_1)\n'
           '    set(BUILTIN_ICON_FONT font_material_symbols_14_1)\n')
    if anchor not in c:
        sys.exit("CMakeLists anchor not found - add the board block by hand")
    c = c.replace(anchor, new + anchor, 1)
    cmake.write_text(c, encoding="utf-8")
    print("patched main/CMakeLists.txt")
print("Rover-Bot board registered.")
