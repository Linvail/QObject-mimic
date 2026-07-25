
from waflib import Logs
import platform

def options(opt):
    opt.load('compiler_c compiler_cxx')

def configure(ctx):
    if platform.system() == "Windows":
        ctx.load('toolchain-windows', tooldir='tools')
        ctx.configure_win64_msvc()
        ctx.configure_win32_msvc()
    elif platform.system() == "Linux":
        ctx.load('toolchain-linux', tooldir='tools')
        ctx.configure_Linux_x64_gcc()
        ctx.configure_Linux_x64_clang()
        ctx.configure_Windows_x64_Linux_clang()
