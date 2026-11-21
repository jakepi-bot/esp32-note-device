"""PlatformIO hook: recompile the app description on every build.

`esp_app_desc_t` carries the C compiler's `__DATE__` and `__TIME__`, which the
`esp_app_format` component bakes into one object file. Neither build system
rebuilds that file when nothing about it changed, so `diag`'s `build.date` and
`build.time` named the build that first compiled the object rather than the
image being flashed: an incremental PlatformIO build reused `15:16:59` through
every rebuild after it.

Removing the object before the build starts makes the description name this
build. `build.elf_sha256` is still what identifies an image, since two builds in
the same second share a stamp; the date and time are for a human ("how old is
this firmware?"), so they should not lie.

`-t upload` alone does nothing here (no build), and a fresh build directory has
no object to remove.
"""

import glob
import os

Import("env")  # noqa: F821 - provided by SCons

_build_dir = env.subst("$BUILD_DIR")  # noqa: F821

# SCons compiles it as `.o`, ninja as `.obj`; both write a dependency file beside
# it, which goes with it.
for _object in glob.glob(os.path.join(_build_dir, "**", "esp_app_desc.c.o*"), recursive=True):
    os.remove(_object)
