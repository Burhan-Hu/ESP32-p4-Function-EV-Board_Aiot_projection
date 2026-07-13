#!/usr/bin/env python3
"""Wrapper to run idf.py from MSYS2/Git Bash on Windows.

The ESP-IDF idf.py exits early when MSYSTEM is set (MSys/Mingw is no longer
supported). This wrapper spawns idf.py in a child environment that excludes
MSYSTEM and includes the tool paths exported by idf_tools.py.

Usage: python run_idf.py <idf.py arguments>
"""
import os
import re
import subprocess
import sys

IDF_PATH = r"E:/Tools/ESP-IDF/Espressif/frameworks/esp-idf-v5.5.2"
IDF_PYTHON_ENV_PATH = r"E:\Tools\ESP-IDF\Espressif\python_env\idf5.5_py3.11_env"
IDF_TOOLS_PATH = r"E:/Tools/ESP-IDF/Espressif"
PYTHON = os.path.join(IDF_PYTHON_ENV_PATH, "Scripts", "python.exe")
IDF_PY = os.path.join(IDF_PATH, "tools", "idf.py")
IDF_TOOLS_PY = os.path.join(IDF_PATH, "tools", "idf_tools.py")


def get_tool_env():
    env = os.environ.copy()
    # Remove the MSYS marker so idf.py's guard does not short-circuit.
    env.pop("MSYSTEM", None)
    env["IDF_PATH"] = IDF_PATH
    # Force UTF-8 output to avoid 'gbk' codec errors on Chinese Windows.
    env["PYTHONIOENCODING"] = "utf-8"
    env["PYTHONUTF8"] = "1"
    env["IDF_PYTHON_ENV_PATH"] = IDF_PYTHON_ENV_PATH
    env["IDF_TOOLS_PATH"] = IDF_TOOLS_PATH

    # Ask idf_tools.py for the toolchain paths.
    out = subprocess.check_output(
        [PYTHON, IDF_TOOLS_PY, "export"],
        env=env,
        text=True,
        stderr=subprocess.STDOUT,
    )
    old_path = env.get("PATH", "")
    for part in re.split(r";(?=export )", out):
        m = re.match(r"export\s+([A-Za-z_][A-Za-z0-9_]*)=\"(.*?)\"", part.strip())
        if not m:
            continue
        key, val = m.group(1), m.group(2)
        if key.upper() == "PATH":
            # idf_tools.py emits '...;%PATH%' which is meant for cmd expansion.
            # Replace the literal placeholder with the original PATH.
            parts = [p for p in val.split(";") if p]
            expanded = []
            for p in parts:
                if p.upper() == "%PATH%":
                    if old_path:
                        expanded.append(old_path)
                else:
                    expanded.append(p)
            env[key] = ";".join(expanded)
        else:
            env[key] = val
    return env


def main():
    env = get_tool_env()
    args = [PYTHON, IDF_PY] + sys.argv[1:]
    print("Running:", " ".join(args))
    sys.exit(subprocess.call(args, env=env))


if __name__ == "__main__":
    main()
