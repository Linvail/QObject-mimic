from waflib.Configure import conf
from waflib import Logs

@conf
def configure_win_msvc_common(ctx, env_name):
    # TODO: Choose from different MSVC versions.
    ctx.load('msvc')

    ctx.env.append_value('DEFINES', [
        'WIN32',
        '_WINDOWS',
        '_UNICODE',
        'UNICODE',
        '_CRT_SECURE_NO_DEPRECATE',
        '_CRT_NON_CONFORMING_SWPRINTFS',
        '_ENABLE_ATOMIC_ALIGNMENT_FIX'
    ])

    # Modern compiler flags: Standard compliance, UTF-8 parsing, and high warnings.
    ctx.env.append_value('CXXFLAGS', [
        '/EHsc',
        '/std:c++17',
        '/permissive-',  # Enforce standard conformance.
        '/utf-8',        # Force UTF-8 source file encoding.
        '/W4'            # Warning level 4.
    ])
    ctx.env.append_value('CFLAGS', [
        '/utf-8',
        '/W3'
    ])

    # Target Console Subsystem.
    ctx.env.append_value('LINKFLAGS', ['/SUBSYSTEM:CONSOLE'])

    '''
    For debug build
    '''
    base_env = ctx.env
    ctx.setenv('%s-debug' % env_name, base_env)

    ctx.env.append_value('DEFINES', ['_DEBUG', 'DEBUG'])
    for flag in ('CFLAGS', 'CXXFLAGS'):
        ctx.env.append_value(flag, ['/MDd', '/Zi', '/Ob0', '/Od', '/RTC1'])
        ctx.env.append_value(flag, ['/FS'])

    ctx.env.append_value('LINKFLAGS', ['/DEBUG:FASTLINK'])

    '''
    For release build (with optimizations + debug symbols enabled)
    '''
    ctx.setenv('%s-release' % env_name, base_env)

    ctx.env.append_value('DEFINES', ['NDEBUG'])

    for flag in ('CFLAGS', 'CXXFLAGS'):
        ctx.env.append_value(flag, [
            '/MD',
            '/O2',
            '/Ob2',
            '/Zi'      # Generate PDB debug symbols in Release.
        ])
        ctx.env.append_value(flag, ['/FS'])

    # Enable linker optimizations along with debug symbols generation.
    ctx.env.append_value('LINKFLAGS', [
        '/DEBUG',
        '/OPT:REF',
        '/OPT:ICF'
    ])

    ctx.env.ENV_VALID = True


@conf
def configure_win64_msvc(ctx):
    prev_variant = ctx.variant

    env_name = "Windows-x64-Windows-msvc"
    ctx.setenv(env_name, ctx.env)

    ctx.env.MSVC_TARGETS = ['x64']
    ctx.configure_win_msvc_common(env_name)

    ctx.variant = prev_variant


@conf
def configure_win32_msvc(ctx):
    prev_variant = ctx.variant

    env_name = "Windows-x86-Windows-msvc"
    ctx.setenv(env_name, ctx.env)

    # Use x64 host compiler to compile x86 target.
    ctx.env.MSVC_TARGETS = ['amd64_x86']
    ctx.configure_win_msvc_common(env_name)

    ctx.variant = prev_variant
