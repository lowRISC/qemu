#!/usr/bin/env python3

# Copyright (c) 2025 lowRISC contributors.
# SPDX-License-Identifier: Apache2

"""Wrapper script for 'clang-tidy', the LLVM C linter.

   :author: Alice Ziuziakowska <a.ziuziakowska@lowrisc.org>
"""

# pylint: disable=invalid-name
# pylint: enable=invalid-name

import argparse
import re
import subprocess
import sys
from glob import glob
from multiprocessing import cpu_count
from os import execvp, scandir
from os.path import abspath, dirname, isfile, join, normpath, relpath, splitext
from traceback import format_exc

LLVM_MAJOR = 21
"""LLVM major version to check for"""

OT_SCRIPTS = dirname(abspath(__file__))
"""Directory this script is in"""

QEMU_ROOT = dirname(dirname(OT_SCRIPTS))
"""QEMU root directory"""

DEF_BUILD_DIR = relpath(join(QEMU_ROOT, "build"))
"""Default build directory is 'build' in QEMU root"""

DEF_TIDY_YML = relpath(join(OT_SCRIPTS, "clang-tidy.yml"))
"""Default 'clang-tidy' configuration file"""


def ci_files() -> list[str]:
    """Get list of files that are checked in CI"""
    lst_files_list = []
    ci_files_list = []
    directory = join(OT_SCRIPTS, "clang-tidy.d")
    for entry in scandir(directory):
        if entry.is_file() and splitext(entry)[1] == ".lst":
            lst_files_list.append(entry)
    for entry in lst_files_list:
        with open(entry, "rt") as f:
            patterns = f.readlines()
            for pattern in patterns:
                # glob patterns are relative to QEMU root
                ci_files_list.extend(glob(pattern.strip(), root_dir=QEMU_ROOT))
    # convert QEMU relative paths to CWD relative paths
    return [relpath(normpath(join(QEMU_ROOT, f))) for f in ci_files_list]


def uint(value: str) -> int:
    """Argparse function to parse integer greater than 0"""
    v = int(value)
    if v <= 0:
        raise argparse.ArgumentTypeError("value is an integer <= 0")
    return v


def eprint_tty(*args, **kwargs) -> None:
    """Wrapper for print to stderr with colour for tty"""
    print("".join(["\x1b[33m", *args, "\x1b[0m"]), **kwargs, file=sys.stderr)


def eprint_plain(*args, **kwargs) -> None:
    """Wrapper for print to stderr"""
    print(*args, **kwargs, file=sys.stderr)


eprint = eprint_tty if sys.stderr.isatty() else eprint_plain


def check_compile_commands(build_dir: str) -> None:
    """
    Check for 'compile_commands.json' in given build directory
    """
    if not isfile(join(build_dir, "compile_commands.json")):
        eprint(f"missing 'compile_commands.json' in build directory "
               f"'{build_dir}'.\n"
               f"please specify the correct QEMU build directory with "
               f"'-b'/'--build-dir',\n"
               f"or make sure to run 'configure'")
        raise FileNotFoundError("missing 'compile_commands.json' "
                                "in build directory")


def check_cc(build_dir: str) -> None:
    """Check that the configured C compiler for the build is clang"""
    cc_found = False
    # look in <build_dir>/config.status and try to find CC variable
    with open(join(build_dir, "config.status"), "rt") as f:
        lines = f.readlines()
        for line in lines:
            match = re.match(r"^CC='(.*)'$", line)
            if match is not None:
                ccs = [f"clang-{LLVM_MAJOR}", "clang"]
                if match.group(1) not in ccs:
                    eprint(f"QEMU build is configured with non-clang C "
                           f"compiler '{match.group(1)}'.\nunknown warning and "
                           f"argument errors will appear, please re-'configure'"
                           f" with '--cc=clang...'")
                cc_found = True
                break
    if not cc_found:
        # no CC option in config.status
        eprint("no 'cc' argument given to configure.\n"
               "if the host C compiler is not clang, unknown warning and "
               "argument errors will appear.\nplease re-'configure' with "
               "'--cc=clang...'")


def check_and_get_clang_tidy() -> str:
    """Check for and return an appropriate 'clang-tidy'"""
    cmds = [f"clang-tidy-{LLVM_MAJOR}", "clang-tidy"]
    for cmd in cmds:
        try:
            p = subprocess.run([cmd, "--version"],
                               capture_output=True, text=True, check=True)
            for line in p.stdout.split("\n"):
                match = re.match(r"^.*LLVM version ([0-9]+)\..*$", line)
                if match is not None:
                    if int(match.group(1)) != LLVM_MAJOR:
                        eprint(f"'{cmd}' is build with LLVM major version "
                               f"{match.group(1)}, "
                               f"expected version {LLVM_MAJOR}")
                        break
                    return cmd
            else:
                eprint(f"could not parse version of '{cmd}'")
        except FileNotFoundError:
            pass
        except subprocess.CalledProcessError:
            eprint(f"'{cmd}' exited with non-zero exit code")
    raise FileNotFoundError("No suitable 'clang-tidy' found")


def check_and_get_run_clang_tidy() -> str:
    """Check for and return an appropriate 'run-clang-tidy'"""
    # 'run-clang-tidy' does not have a version flag,
    # and will try to run with no flags given,
    # so run with '--help' and check if it exists
    cmds = [f"run-clang-tidy-{LLVM_MAJOR}", "run-clang-tidy"]
    for cmd in cmds:
        try:
            _ = subprocess.run([cmd, "--help"], capture_output=True, check=True)
            return cmd
        except FileNotFoundError:
            pass
        except subprocess.CalledProcessError:
            eprint(f"'{cmd}' exited with non-zero exit code")
    raise FileNotFoundError("No suitable 'run-clang-tidy' found")


def main():
    """Wrapper entry point"""
    debug = True
    try:
        parser = argparse.ArgumentParser()

        parser.add_argument("-b", "--build-dir", default=DEF_BUILD_DIR,
                            help=(f"QEMU build directory path. Default is "
                                  f"'{DEF_BUILD_DIR}'"))

        parser.add_argument("-j", "--jobs", nargs="?", type=uint,
                            default=1, const=cpu_count(),
                            help=("Number of tidy jobs to run in parallel. "
                                  "Default is number of threads if value "
                                  "is unspecified"))

        parser.add_argument("-C", "--config-file", default=DEF_TIDY_YML,
                            help=(f"'clang-tidy' configuration file path. "
                                  f"Default is '{DEF_TIDY_YML}'"))

        parser.add_argument("-c", "--ci-files", action="store_true",
                            help="Run on all files checked by CI")

        parser.add_argument("files", nargs="*", default=[], action="extend",
                            help="Files to run tidy on")

        parser.add_argument("-d", "--debug", action="store_true",
                            help="Enable debug mode")

        parser.add_argument("-v", "--verbose", action="store_true",
                            help="Enable verbose output")

        args = parser.parse_args()

        debug = args.debug

        # normalise paths
        args.build_dir = normpath(args.build_dir)
        args.config_file = normpath(args.config_file)
        args.files = [normpath(f) for f in args.files]

        # append ci files
        if args.ci_files:
            args.files.extend(ci_files())

        if not args.files:
            parser.error("no input files provided. specify files, or use "
                         "'-c'/'--ci-files' to run on files checked in CI.")

        check_compile_commands(args.build_dir)
        check_cc(args.build_dir)

        cmd = ""
        cmd_args = []

        # use a 'run-clang-tidy' if running in parallel.
        # this wrapper has slightly different command line arguments
        if args.jobs == 1:
            cmd = check_and_get_clang_tidy()
            cmd_args = [
                cmd,
                "-p", args.build_dir,
                "--config-file", args.config_file,
            ]
        else:
            cmd = check_and_get_run_clang_tidy()
            cmd_args = [
                cmd,
                "-quiet",
                "-p", args.build_dir,
                "-config-file", args.config_file,
                "-j", str(args.jobs),
            ]

        if args.verbose:
            fmt_args = " ".join(cmd_args[1:])
            print(f"running '{cmd} {fmt_args} ... ({len(args.files)} files)'")

        cmd_args.extend(args.files)
        execvp(cmd, cmd_args)

    # pylint: disable=broad-except
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == "__main__":
    main()
