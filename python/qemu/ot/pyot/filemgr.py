# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""File manager for QEMU unit test sequencer.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from atexit import register
from logging import getLogger
from os import close, environ, sep, unlink
from os.path import abspath, basename, exists, isdir, isfile, normpath
from shutil import rmtree
from tempfile import mkdtemp, mkstemp
from typing import Any, Optional

import re


class QEMUFileManager:
    """Simple file manager to generate and track temporary files.

       :param keep_temp: do not automatically discard generated files on exit
    """

    DEFAULT_OTP_ECC_BITS = 6

    def __init__(self, keep_temp: bool = False):
        self._log = getLogger('pyot.file')
        self._keep_temp = keep_temp
        self._in_fly: set[str] = set()
        self._otp_files: dict[str, tuple[str, int]] = {}
        self._env: dict[str, str] = {}
        self._transient_vars: set[str] = set()
        self._dirs: dict[str, str] = {}
        register(self._cleanup)

    @property
    def keep_temporary(self) -> bool:
        """Tell whether temporary files and directories should be preserved or
           not.

           :return: True if temporary items should not be suppressed
        """
        return self._keep_temp

    def set_qemu_src_dir(self, path: str) -> None:
        """set the QEMU "source" directory.

           :param path: the path to the QEMU source directory
        """
        self._env['QEMU_SRC_DIR'] = abspath(path)

    def set_qemu_bin_dir(self, path: str) -> None:
        """set the QEMU executable directory.

           :param path: the path to the QEMU binary directory
        """
        self._env['QEMU_BIN_DIR'] = abspath(path)

    def set_config_dir(self, path: str) -> None:
        """Assign the configuration directory.

           :param path: the directory that contains the input configuration
                        file
        """
        self._env['CONFIG'] = abspath(path)

    def set_udp_log_port(self, port: int) -> None:
        """Assign the UDP logger port.

           :param port: the UDP logger port
        """
        self._env['UDPLOG'] = f'{port}'

    def interpolate(self, value: Any) -> str:
        """Interpolate a ${...} marker with shell substitutions or local
           substitution.

           :param value: input value
           :return: interpolated value as a string
        """
        def replace(smo: re.Match) -> str:
            name = smo.group(1)
            val = self._env[name] if name in self._env \
                else environ.get(name, '')
            if not val:
                getLogger('pyot.file').warning("Unknown placeholder '%s'",
                                               name)
            return val
        svalue = str(value)
        nvalue = re.sub(r'\$\{(\w+)\}', replace, svalue)
        if nvalue != svalue:
            self._log.debug('Interpolate %s with %s', value, nvalue)
        return nvalue

    def define(self, aliases: dict[str, Any]) -> None:
        """Store interpolation variables into a local dictionary.

            Variable values are interpolated before being stored.

           :param aliases: an alias JSON (sub-)tree
        """
        def replace(smo: re.Match) -> str:
            name = smo.group(1)
            val = self._env[name] if name in self._env \
                else environ.get(name, '')
            return val
        for name in aliases:
            value = str(aliases[name])
            value = re.sub(r'\$\{(\w+)\}', replace, value)
            if exists(value):
                value = normpath(value)
            aliases[name] = value
            self._env[name.upper()] = value
            self._log.debug('Store %s as %s', name.upper(), value)

    def define_transient(self, aliases: dict[str, Any]) -> None:
        """Add short-lived aliases that are all discarded when cleanup_transient
           is called.

           :param aliases: a dict of aliases
        """
        for name in aliases:
            name = name.upper()
            # be sure not to make an existing non-transient variable transient
            if name not in self._env:
                self._transient_vars.add(name)
        self.define(aliases)

    def cleanup_transient(self) -> None:
        """Remove all transient variables."""
        for name in self._transient_vars:
            if name in self._env:
                del self._env[name]
        self._transient_vars.clear()

    def interpolate_dirs(self, value: str, default: str) -> str:
        """Resolve temporary directories, creating ones whenever required.

           :param value: the string with optional directory placeholders
           :param default: the default name to use if the placeholder contains
                           none
           :return: the interpolated string
        """
        def replace(smo: re.Match) -> str:
            name = smo.group(1)
            if name == '':
                name = default
            if name not in self._dirs:
                tmp_dir = mkdtemp(prefix='qemu_ot_dir_')
                self._dirs[name] = tmp_dir
            else:
                tmp_dir = self._dirs[name]
            if not tmp_dir.endswith(sep):
                tmp_dir = f'{tmp_dir}{sep}'
            return tmp_dir
        nvalue = re.sub(r'\@\{(\w*)\}/', replace, value)
        if nvalue != value:
            self._log.debug('Interpolate %s with %s', value, nvalue)
        return nvalue

    def delete_default_dir(self, name: str) -> None:
        """Delete a temporary directory, if has been referenced.

           :param name: the name of the directory reference
        """
        if name not in self._dirs:
            return
        if not isdir(self._dirs[name]):
            return
        try:
            self._log.debug('Removing tree %s for %s', self._dirs[name], name)
            rmtree(self._dirs[name])
            del self._dirs[name]
        except OSError:
            self._log.error('Cannot be removed dir %s for %s', self._dirs[name],
                            name)

    def create_eflash_image(self, app: Optional[str] = None,
                            bootloader: Optional[str] = None,
                            no_flash_header: bool = False) -> str:
        """Generate a temporary flash image file.

           :param app: optional path to the application or the rom extension
           :param bootloader: optional path to a bootloader
           :param no_flash_header: input binary file do not contain an OpenTitan
                                   application header (i.e. regular files)
           :return: the full path to the temporary flash file
        """
        # pylint: disable=import-outside-toplevel
        from ot.eflash.gen import FlashGen
        gen = FlashGen(FlashGen.CHIP_ROM_EXT_SIZE_MAX if bool(bootloader)
                       else 0, True)
        flash_fd, flash_file = mkstemp(suffix='.raw', prefix='qemu_ot_flash_')
        self._in_fly.add(flash_file)
        close(flash_fd)
        self._log.debug('Create %s', basename(flash_file))
        try:
            gen.open(flash_file)
            if app:
                with open(app, 'rb') as afp:
                    gen.store_rom_ext(0, afp, no_header=no_flash_header)
            if bootloader:
                with open(bootloader, 'rb') as bfp:
                    gen.store_bootloader(0, bfp, no_header=no_flash_header)
        finally:
            gen.close()
        return flash_file

    def create_otp_image(self, vmem: str) -> str:
        """Generate a temporary OTP image file.

           If a temporary file has already been generated for the input VMEM
           file, use it instead.

           :param vmem: path to the VMEM source file
           :return: the full path to the temporary OTP file
        """
        # pylint: disable=import-outside-toplevel
        if vmem in self._otp_files:
            otp_file, ref_count = self._otp_files[vmem]
            self._log.debug('Use existing %s', basename(otp_file))
            self._otp_files[vmem] = (otp_file, ref_count + 1)
            return otp_file
        from otptool import OtpImage
        otp = OtpImage()
        with open(vmem, 'rt', encoding='utf-8') as vfp:
            otp.load_vmem(vfp, 'otp')
        otp_fd, otp_file = mkstemp(suffix='.raw', prefix='qemu_ot_otp_')
        self._log.debug('Create %s', basename(otp_file))
        self._in_fly.add(otp_file)
        close(otp_fd)
        with open(otp_file, 'wb') as rfp:
            otp.save_raw(rfp)
        self._otp_files[vmem] = (otp_file, 1)
        return otp_file

    def delete_flash_image(self, filename: str) -> None:
        """Delete a previously generated flash image file.

           :param filename: full path to the file to delete
        """
        if not isfile(filename):
            self._log.warning('No such flash image file %s', basename(filename))
            return
        self._log.debug('Delete flash image file %s', basename(filename))
        unlink(filename)
        self._in_fly.discard(filename)

    def delete_otp_image(self, filename: str) -> None:
        """Delete a previously generated OTP image file.

           The file may be used by other tests, it is only deleted if it not
           useful anymore.

           :param filename: full path to the file to delete
        """
        if not isfile(filename):
            self._log.warning('No such OTP image file %s', basename(filename))
            return
        for vmem, (raw, count) in self._otp_files.items():
            if raw != filename:
                continue
            count -= 1
            if not count:
                self._log.debug('Delete OTP image file %s', basename(filename))
                unlink(filename)
                self._in_fly.discard(filename)
                del self._otp_files[vmem]
            else:
                self._log.debug('Keep OTP image file %s', basename(filename))
                self._otp_files[vmem] = (raw, count)
            break

    def _cleanup(self) -> None:
        """Remove a generated, temporary flash image file.
        """
        removed: set[str] = set()
        for tmpfile in self._in_fly:
            if not isfile(tmpfile):
                removed.add(tmpfile)
                continue
            if not self._keep_temp:
                self._log.debug('Delete %s', basename(tmpfile))
                try:
                    unlink(tmpfile)
                    removed.add(tmpfile)
                except OSError:
                    self._log.error('Cannot delete %s', basename(tmpfile))
        self._in_fly -= removed
        if self._in_fly:
            if not self._keep_temp:
                raise OSError(f'{len(self._in_fly)} temp. files cannot be '
                              f'removed')
            for tmpfile in self._in_fly:
                self._log.warning('Temporary file %s not suppressed', tmpfile)
        removed: set[str] = set()
        if not self._keep_temp:
            for tmpname, tmpdir in self._dirs.items():
                if not isdir(tmpdir):
                    removed.add(tmpname)
                    continue
                self._log.debug('Delete dir %s', tmpdir)
                try:
                    rmtree(tmpdir)
                    removed.add(tmpname)
                except OSError as exc:
                    self._log.error('Cannot delete %s: %s', tmpdir, exc)
            for tmpname in removed:
                del self._dirs[tmpname]
        if self._dirs:
            if not self._keep_temp:
                raise OSError(f'{len(self._dirs)} temp. dirs cannot be removed')
            for tmpdir in self._dirs.values():
                self._log.warning('Temporary dir %s not suppressed', tmpdir)
