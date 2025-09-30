"""Verilator wrapper."""

# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

from atexit import register
from logging import getLogger
from os.path import isdir
from shutil import rmtree
from tempfile import mkdtemp
from typing import Optional


class VtorFileManager:
    """File manager.

       Handle temporary directories and files.

       :param keep_temp: do not automatically discard generated files on exit
       :param tmp_dir: store the temporary files in the specified directory
    """

    def __init__(self, keep_temp: bool = False, tmp_dir: Optional[str] = None):
        self._log = getLogger('vtor.file')
        self._keep_temp = keep_temp
        self._base_tmp_dir = tmp_dir
        self._tmp_dir: Optional[str] = None
        register(self._cleanup)

    @property
    def tmp_dir(self) -> str:
        """Provides the main temporary directory for executing Verilator."""
        if not self._tmp_dir:
            self._tmp_dir = mkdtemp(prefix='verilator_ot_dir_',
                                    dir=self._base_tmp_dir)
        return self._tmp_dir

    def _cleanup(self):
        if self._tmp_dir and isdir(self._tmp_dir):
            if self._keep_temp:
                self._log.warning('Preserving temp dir %s', self._tmp_dir)
                return
            self._log.debug('Removing temp dir %s', self._tmp_dir)
            rmtree(self._tmp_dir)
            self._tmp_dir = None
