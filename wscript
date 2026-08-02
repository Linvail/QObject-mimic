import platform
from waflib import Configure

Configure.autoconfig = True

top = "."
default_prefix = "/usr"

if platform.system() == "Windows":
    out = "out/windows"
elif platform.system() == "Linux":
    out = "out/linux"

projects = ["Tests"]
modes = ["debug", "release"]


if platform.system() == "Windows":
    project_toolchains = {
        "Tests": ["win64-msvc", "win32-msvc"],
    }
elif platform.system() == "Linux":
    project_toolchains = {
        "Tests": ["linux64-clang", "linux64-gcc", "linux-2-win64-clang"],
    }
