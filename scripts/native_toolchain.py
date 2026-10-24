# Adds the MinGW toolchain shipped with PlatformIO to PATH when the host does
# not already provide gcc/g++ (typical on Windows). On Linux/macOS hosts a
# system compiler is used and this script does nothing.
import os
import shutil
import sys

Import("env")

if env.subst("$PIOPLATFORM") == "native" and sys.platform == "win32":
    # The MinGW runtime DLLs are not on PATH when PlatformIO launches the test
    # binary, so link them statically.
    env.Append(LINKFLAGS=["-static"])
    if shutil.which("gcc") is None:
        mingw_bin = os.path.join(
            os.path.expanduser("~"), ".platformio", "packages",
            "toolchain-gccmingw32", "bin")
        if os.path.isdir(mingw_bin):
            os.environ["PATH"] = mingw_bin + os.pathsep + os.environ.get("PATH", "")
            print("native test toolchain: %s" % mingw_bin)
        else:
            print("native test toolchain not found: install a C++ compiler or "
                  "run `pio pkg install -g -p toolchain-gccmingw32`")
