from waflib.Configure import conf
from waflib.Build import BuildContext
from waflib import Options, Utils, Context, Logs
import os, sys

platforms = ['Windows', 'Linux']
arch = ['x86', 'x64', 'arm64']


@conf
def get_all_projects(ctx):
    return Utils.to_list(getattr(Context.g_module, 'projects', []))


@conf
def get_all_modes(ctx):
    return Utils.to_list(getattr(Context.g_module, 'modes', []))


def select_target_platform(ctx):
    """
    Select target platform if not specified.
    """
    host = Utils.unversioned_sys_platform()
    if host == 'win32':
        return 'Windows'
    elif host == 'linux':
        return 'Linux'
    else:
        ctx.fatal('Unknown platform: %s' % host)


def select_target_arch(ctx):
    """
    Select target architecture if not specified.
    """
    host = Utils.unversioned_sys_platform()
    if host == 'win32':
        return 'x64'
    elif host == 'linux':
        return 'x64'
    else:
        ctx.fatal('Unknown platform: %s' % host)


def _project(ctx):
    if hasattr(ctx, '_cached_project'):
        return ctx._cached_project

    projects = ctx.get_all_projects()
    default_project = projects[0]
    project = getattr(Options.options, 'project', None) or default_project

    if project == '?':
        Logs.info( 'Valid projects (* = default):' )
        for p in sorted(projects):
            tag = '*' if p == default_project else ' '
            Logs.info( '%s %s' % (tag, p) )
        sys.exit()

    if project not in projects:
        ctx.fatal('Invalid project \'%s\'.  Valid projects are %s' % (project, ','.join(projects)))

    ctx._cached_project = project
    return project


def _mode(ctx):
    if hasattr(ctx, '_cached_mode'):
        return ctx._cached_mode

    modes = ctx.get_all_modes()

    default_mode = modes[0]
    mode = getattr(Options.options, 'mode', None) or default_mode

    if mode == '?':
        Logs.info( 'Valid modes (* = default):' )
        for m in sorted(modes):
            tag = '*' if m == default_mode else ' '
            Logs.info( '%s %s' % (tag, m) )
        sys.exit()

    if mode not in modes:
        ctx.fatal('Invalid mode \'%s\'.  Valid modes are %s' % (mode, ','.join(modes)))

    ctx._cached_mode = mode
    return mode

def _variant(ctx):
    if 'conf_check_' in ctx.top_dir:
        # Configuration time build contexts use a special directory named
        # conf_check_<hash> to build small test programs to figure out if
        # header files or other features are included.  We do not have and
        # cannot require a product and mode to be set for these so check for
        # a build in this directory and return an empty variant in this case.
        # We do not have a better way to know that the build context is being
        # created at configure time.
        return ''

    return os.path.join(ctx.project, ctx.mode, ctx.target_platform, ctx.target_arch)


def _target_platform(ctx):
    if hasattr(ctx, '_cached_platform'):
        return ctx._cached_platform

    platform = getattr(Options.options, 'platform', None) or select_target_platform(ctx)
    if platform == '?':
        Logs.info('Valid platforms (* = default):')
        default_platform = select_target_platform(ctx)
        for p in sorted(platforms):
            tag = '*' if p == default_platform else ' '
            Logs.info('%s %s' % (tag, p))
        sys.exit()

    if platform not in platforms:
        ctx.fatal('Invalid platform \'%s\'. Valid platforms are %s' % (platform, ','.join(platforms)))

    ctx._cached_platform = platform
    return platform


def _target_arch(ctx):
    if hasattr(ctx, '_cached_arch'):
        return ctx._cached_arch

    arch_val = getattr(Options.options, 'arch', None) or select_target_arch(ctx)
    if arch_val == '?':
        Logs.info('Valid architectures (* = default):')
        default_arch = select_target_arch(ctx)
        for a in sorted(arch):
            tag = '*' if a == default_arch else ' '
            Logs.info('%s %s' % (tag, a))
        sys.exit()

    if arch_val not in arch:
        ctx.fatal('Invalid architecture \'%s\'. Valid architectures are %s' % (arch_val, ','.join(arch)))

    ctx._cached_arch = arch_val
    return arch_val


def options(opt):

    group = opt.add_option_group('Variant builder')

    projects = get_all_projects(opt)
    modes = get_all_modes(opt)


    if projects:
        project_help = 'Project name (choices: %s, default: %s, type ? for list)' % (', '.join(projects), projects[0])
    else:
        project_help = 'Project name (type ? for list)'

    if modes:
        mode_help = 'Build mode (choices: %s, default: %s, type ? for list)' % (', '.join(modes), modes[0])
    else:
        mode_help = 'Build mode (type ? for list)'

    platforms_help = 'Target platform (choices: %s, default: %s, type ? for list)' % (', '.join(platforms), platforms[0])
    arch_help = 'Target architecture (choices: %s, default: %s, type ? for list)' % (', '.join(arch), arch[0])

    group.add_option('--project', help=project_help)
    group.add_option('--mode', help=mode_help)
    group.add_option('--platform', help=platforms_help)
    group.add_option('--arch', help=arch_help)


    BuildContext.project = property(_project)
    BuildContext.mode = property(_mode)
    BuildContext.variant = property(_variant)  # type: ignore
    BuildContext.target_platform = property(_target_platform)
    BuildContext.target_arch = property(_target_arch)
