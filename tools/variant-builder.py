from waflib.Configure import conf
from waflib.Build import BuildContext
from waflib import Options, Utils, Context, Logs
import os, sys


@conf
def get_all_projects(ctx):
    """
    projects is supposed to be a list of all projects in the build system.
    """
    return Utils.to_list(getattr(Context.g_module, "projects", []))


@conf
def get_all_toolchains(ctx):
    """
    project_toolchains is supposed to be a dict mapping to a list of toolchains for each project.
    For example:

    project_toolchains = {
        "Hello_World": ["win32-msvc", "win64-msvc"]
    }

    """
    toolchains_table = getattr(ctx, "project_toolchains_cache", {})

    if not toolchains_table:
        toolchains_table = getattr(Context.g_module, "project_toolchains", {})
        if not isinstance(toolchains_table, dict):
            ctx.fatal(
                "project_toolchains must be a dictionary mapping project names to lists of toolchains."
            )

        ctx.project_toolchains_cache = toolchains_table

    return toolchains_table


@conf
def get_all_modes(ctx):
    """
    modes is supposed to be a list of all build modes in the build system.
    """
    return Utils.to_list(getattr(Context.g_module, "modes", []))


def select_target_platform(ctx):
    """
    Select target platform if not specified.
    """
    host = Utils.unversioned_sys_platform()
    if host == "win32":
        return "Windows"
    elif host == "linux":
        return "Linux"
    else:
        ctx.fatal("Unknown platform: %s" % host)


def select_target_arch(ctx):
    """
    Select target architecture if not specified.
    """
    host = Utils.unversioned_sys_platform()
    if host == "win32":
        return "x64"
    elif host == "linux":
        return "x64"
    else:
        ctx.fatal("Unknown platform: %s" % host)


def _project(ctx):
    if hasattr(ctx, "_cached_project"):
        return ctx._cached_project

    projects = ctx.get_all_projects()
    default_project = projects[0]
    project = getattr(Options.options, "project", None) or default_project

    if project == "?":
        Logs.info("Valid projects (* = default):")
        for p in sorted(projects):
            tag = "*" if p == default_project else " "
            Logs.info("%s %s" % (tag, p))
        sys.exit()

    if project not in projects:
        ctx.fatal(
            "Invalid project '%s'.  Valid projects are %s"
            % (project, ",".join(projects))
        )

    ctx._cached_project = project
    return project


def _toolchain(ctx):
    if hasattr(ctx, "_cached_toolchain"):
        return ctx._cached_toolchain

    toolchains_table = ctx.get_all_toolchains()
    project_toolchains = []
    try:
        project_toolchains += Utils.to_list(toolchains_table[ctx.project])
    except (KeyError, TypeError):
        ctx.fatal(f"No toolchains defined for project '{ctx.project}'")

    specified_toolchain = getattr(Options.options, "toolchain", None)
    selected_toolchain = None
    if not specified_toolchain:
        if len(project_toolchains) > 0:
            selected_toolchain = project_toolchains[0]
        else:
            ctx.fatal(f"No toolchains defined for project '{ctx.project}'")
    else:
        if specified_toolchain == "?":
            Logs.info("Valid toolchains for project '%s' (* = default):" % ctx.project)
            for t in sorted(project_toolchains):
                tag = "*" if t == project_toolchains[0] else " "
                Logs.info("%s %s" % (tag, t))
            sys.exit()
        elif specified_toolchain not in project_toolchains:
            ctx.fatal(
                "Invalid toolchain '%s' for project '%s'. Valid toolchains are %s"
                % (specified_toolchain, ctx.project, ",".join(project_toolchains))
            )
        else:
            selected_toolchain = specified_toolchain

    return selected_toolchain


def _mode(ctx):
    if hasattr(ctx, "_cached_mode"):
        return ctx._cached_mode

    modes = ctx.get_all_modes()

    default_mode = modes[0]
    mode = getattr(Options.options, "mode", None) or default_mode

    if mode == "?":
        Logs.info("Valid modes (* = default):")
        for m in sorted(modes):
            tag = "*" if m == default_mode else " "
            Logs.info("%s %s" % (tag, m))
        sys.exit()

    if mode not in modes:
        ctx.fatal("Invalid mode '%s'.  Valid modes are %s" % (mode, ",".join(modes)))

    ctx._cached_mode = mode
    return mode


def _variant(ctx):
    if "conf_check_" in ctx.top_dir:
        # Configuration time build contexts use a special directory named
        # conf_check_<hash> to build small test programs to figure out if
        # header files or other features are included.  We do not have and
        # cannot require a product and mode to be set for these so check for
        # a build in this directory and return an empty variant in this case.
        # We do not have a better way to know that the build context is being
        # created at configure time.
        return ""

    return os.path.join(ctx.project, ctx.toolchain, ctx.mode)


def options(opt):

    group = opt.add_option_group("Variant builder")

    projects = get_all_projects(opt)
    modes = get_all_modes(opt)

    if projects:
        project_help = "Project name (choices: %s, default: %s)" % (
            ", ".join(projects),
            projects[0],
        )
    else:
        project_help = "Project name (type ? for list)"

    if modes:
        mode_help = "Build mode (choices: %s, default: %s)" % (
            ", ".join(modes),
            modes[0],
        )
    else:
        mode_help = "Build mode (type ? for list)"

    group.add_option("--project", help=project_help)
    group.add_option("--toolchain", help="Toolchain name (type ? for list)")
    group.add_option("--mode", help=mode_help)

    BuildContext.project = property(_project)
    BuildContext.mode = property(_mode)
    BuildContext.toolchain = property(_toolchain)

    BuildContext.variant = property(_variant)  # type: ignore
