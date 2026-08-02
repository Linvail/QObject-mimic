from waflib.Configure import conf
from waflib import Logs
import os


@conf
def configure_win_msvc_common(ctx, env_name):
    ctx.load("msvc")

    ctx.env.append_value(
        "DEFINES",
        [
            "WIN32",
            "_WINDOWS",
            "_UNICODE",
            "UNICODE",
            "_CRT_SECURE_NO_DEPRECATE",
            "_CRT_NON_CONFORMING_SWPRINTFS",
            "_ENABLE_ATOMIC_ALIGNMENT_FIX",
        ],
    )

    # Modern compiler flags: Standard compliance, UTF-8 parsing, and high warnings.
    ctx.env.append_value(
        "CXXFLAGS",
        [
            "/EHsc",
            "/std:c++17",
            "/permissive-",  # Enforce standard conformance.
            "/utf-8",  # Force UTF-8 source file encoding.
            "/W4",  # Warning level 4.
        ],
    )
    ctx.env.append_value("CFLAGS", ["/utf-8", "/W3"])

    # Target Console Subsystem.
    ctx.env.append_value("LINKFLAGS", ["/SUBSYSTEM:CONSOLE"])

    ctx.env.MSVC_BIN_DIR = os.path.dirname(ctx.env.CC[0])

    ctx.env.ENV_VALID = True

    """
    For debug build
    """
    base_env = ctx.env
    ctx.setenv("%s-debug" % env_name, base_env)

    ctx.env.append_value("DEFINES", ["_DEBUG", "DEBUG"])
    for flag in ("CFLAGS", "CXXFLAGS"):
        ctx.env.append_value(flag, ["/MDd", "/Zi", "/Ob0", "/Od", "/RTC1"])
        ctx.env.append_value(flag, ["/FS"])

    ctx.env.append_value("LINKFLAGS", ["/DEBUG"])

    """
    For release build (with optimizations + debug symbols enabled)
    """
    ctx.setenv("%s-release" % env_name, base_env)

    ctx.env.append_value("DEFINES", ["NDEBUG"])

    for flag in ("CFLAGS", "CXXFLAGS"):
        ctx.env.append_value(
            flag,
            ["/MD", "/O2", "/Ob2", "/Zi"],  # Generate PDB debug symbols in Release.
        )
        ctx.env.append_value(flag, ["/FS"])

    # Enable linker optimizations along with debug symbols generation.
    ctx.env.append_value("LINKFLAGS", ["/DEBUG", "/OPT:REF", "/OPT:ICF"])


@conf
def configure_win64_msvc(ctx, root):
    prev_variant = ctx.variant

    env_name = "win64-msvc"
    Logs.info("*** Configuring %s" % env_name)
    ctx.setenv(env_name, root)

    ctx.env.MSVC_TARGETS = ["x64"]
    ctx.configure_win_msvc_common(env_name)

    ctx.variant = prev_variant


@conf
def configure_win32_msvc(ctx, root):
    prev_variant = ctx.variant

    env_name = "win32-msvc"
    Logs.info("*** Configuring %s" % env_name)
    ctx.setenv(env_name, root)

    # Use x64 host compiler to compile x86 target.
    ctx.env.MSVC_TARGETS = ["amd64_x86"]
    ctx.configure_win_msvc_common(env_name)

    ctx.variant = prev_variant


@conf
def add_AddressSanitizer_on_Windows(bld):

    bld.env.append_unique(
        "CXXFLAGS",
        [
            "/fsanitize=address",
        ],
    )
    bld.env.append_unique("LINKFLAGS", ["/INCREMENTAL:NO"])
    bld.env.append_unique("LINKFLAGS", ["/NODEFAULTLIB:LIBCMT"])

    asan_dlls = []
    if "x64" in bld.env.MSVC_TARGETS[0]:
        asan_dlls.append("clang_rt.asan_dynamic-x86_64.dll")
    elif "x86" in bld.env.MSVC_TARGETS[0]:
        asan_dlls.append("clang_rt.asan_dynamic-i386.dll")

    if asan_dlls:
        asan_runtime_dir = bld.env.MSVC_BIN_DIR
        asan_nodes = []
        for dll in asan_dlls:
            node = bld.root.find_node(f"{asan_runtime_dir}/{dll}")
            if node:
                asan_nodes.append(node)
            else:
                Logs.warn(
                    f"AddressSanitizer DLL not found: {dll} in {asan_runtime_dir}. Please ensure the DLL is present."
                )

        bld.install_files("${PREFIX}/bin", asan_nodes)
