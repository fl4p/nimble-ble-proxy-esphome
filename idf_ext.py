"""idf.py extension: archive the build ELF after flash for coredump symbolication.

Thin shim; the implementation lives in the etc/idf-devtools submodule
(github.com/fl4p/idf-devtools). It wraps flash/app-flash to record the build
ELF via elf_archive.py; device name from $ELF_ARCHIVE_DEVICE, else the
serial-port basename. After an OTA (POST /update) archive by hand:
  python3 etc/idf-devtools/elf_archive.py archive <device> --method ota --build-dir <dir>

Without the submodule checked out (e.g. CI) this is a no-op.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'etc', 'idf-devtools'))

try:
    from elf_archive_ext import action_extensions  # noqa: E402,F401
except ImportError:
    def action_extensions(base_actions, project_path):  # noqa: ARG001
        return {}
