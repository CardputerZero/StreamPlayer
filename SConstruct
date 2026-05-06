from pathlib import Path
import os
import platform
import shutil
import socket
import sys


arch = platform.machine()
version = "v0.0.1"
static_lib = "static_lib"
update = False


def write_config(lines):
    if os.path.exists("build/config/config_tmp.mk"):
        return
    os.makedirs("build/config", exist_ok=True)
    with open("build/config/config_tmp.mk", "w") as f:
        f.writelines(lines)


if "CardputerZero" in os.environ:
    toolchain_gcc = shutil.which("aarch64-linux-gnu-gcc")
    toolchain_lines = []
    if toolchain_gcc:
        toolchain_lines.append(f'CONFIG_TOOLCHAIN_PATH="{Path(toolchain_gcc).parent}"\n')

    sysroot_dir = Path.cwd() / static_lib
    multiarch_include = sysroot_dir / "usr" / "include" / "aarch64-linux-gnu"
    multiarch_lib = sysroot_dir / "usr" / "lib" / "aarch64-linux-gnu"
    toolchain_flags = [
        f"-B{multiarch_lib}",
        f"-isystem{multiarch_include}",
        f"-L{multiarch_lib}",
        f"-Wl,-rpath-link,{multiarch_lib}",
    ]

    write_config([
        "CONFIG_V9_5_LV_USE_LINUX_FBDEV=y\n",
        "CONFIG_V9_5_LV_DRAW_SW_ASM_NEON=y\n",
        "CONFIG_V9_5_LV_USE_DRAW_SW_ASM=1\n",
        "CONFIG_V9_5_LV_USE_EVDEV=y\n",
        'CONFIG_TOOLCHAIN_PREFIX="aarch64-linux-gnu-"\n',
        f'CONFIG_TOOLCHAIN_SYSROOT="{os.path.join(sys.path[0], static_lib)}"\n',
        f'CONFIG_TOOLCHAIN_FLAGS="{" ".join(toolchain_flags)}"\n',
    ] + toolchain_lines)
elif arch == "aarch64":
    write_config([
        "CONFIG_V9_5_LV_USE_LINUX_FBDEV=y\n",
        "CONFIG_V9_5_LV_DRAW_SW_ASM_NEON=y\n",
        "CONFIG_V9_5_LV_USE_DRAW_SW_ASM=1\n",
        "CONFIG_V9_5_LV_USE_EVDEV=y\n",
    ])
else:
    write_config(["CONFIG_V9_5_LV_USE_SDL=y\n"])


if "CardputerZero" in os.environ:
    multiarch_include = Path.cwd() / static_lib / "usr" / "include" / "aarch64-linux-gnu"
    multiarch_lib = Path.cwd() / static_lib / "usr" / "lib" / "aarch64-linux-gnu"

    if multiarch_include.exists():
        for key in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH"):
            old_value = os.environ.get(key)
            os.environ[key] = str(multiarch_include) if not old_value else f"{multiarch_include}:{old_value}"

    if multiarch_lib.exists():
        old_value = os.environ.get("LIBRARY_PATH")
        os.environ["LIBRARY_PATH"] = str(multiarch_lib) if not old_value else f"{multiarch_lib}:{old_value}"


local_path = Path(os.getcwd())
sdk_path = local_path.parent.parent / "SDK"
os.environ["SDK_PATH"] = str(sdk_path)
os.environ["EXT_COMPONENTS_PATH"] = str(sdk_path.parent / "ext_components")


env = SConscript(
    str(sdk_path / "tools" / "scons" / "project.py"),
    variant_dir=os.getcwd(),
    duplicate=0,
)


if not os.path.exists(static_lib):
    update = True
else:
    try:
        with open(str(Path(static_lib) / "version"), "r") as f:
            if version != f.read().strip():
                update = True
    except Exception:
        update = True

if update:
    with open(env["PROJECT_TOOL_S"]) as f:
        exec(f.read())
    down_url = "https://github.com/dianjixz/M5CardputerZero-UserDemo/releases/download/{}/sdk_bsp.tar.gz".format(version)
    down_path = check_wget_down(down_url, "static_lib_{}.tar.gz".format(version))
    if os.path.exists(static_lib):
        shutil.rmtree(static_lib)
    shutil.move(down_path, static_lib)
