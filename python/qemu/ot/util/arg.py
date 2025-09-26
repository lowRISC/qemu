# Copyright (c) 2024-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""ArgumentParser extension.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

# pylint: disable=unused-import
# ruff: noqa: F401
from argparse import ArgumentParser as _ArgumentParser, FileType
from sys import stderr


class ArgumentParser(_ArgumentParser):
    """Report usage error first, before printing out the usage. This enables
       catching the first line as the main error message.
    """

    def error(self, message):
        print(f'{self.prog}: error: {message}\n', file=stderr)
        self.print_usage(stderr)
        self.exit(2)
