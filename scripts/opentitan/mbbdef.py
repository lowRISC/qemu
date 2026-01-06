#!/usr/bin/env python3

# Copyright (c) 2024, Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Find and report multi-bit boolean definitions from HJSON configuration file.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser
from logging import getLogger
from os import walk
from os.path import basename, dirname, join as joinpath, normpath, splitext
from pprint import pprint
from traceback import format_exc
from typing import Iterator, TextIO
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from ot.util.log import configure_loggers


try:
    from hjson import load as jload
except ImportError as imp_exc:
    raise ImportError('hjson module is required') from imp_exc


@staticmethod
def flatten(lst: list) -> list:
    """Flatten nested list.
    """
    return [item for sublist in lst for item in sublist]


class MbbChecker:
    """Simple parser to retrieve MultiBitBool field definition from OpenTitan
       HW definition file.
    """

    def __init__(self):
        self._log = getLogger('mbb')
        self.emit_ipname = True

    def parse(self, hdir: str) -> None:
        """Find and parse HW definition files.

           :param hdir: HW top-level definition directory to scan for HW files.
        """
        for dirpath, _, filenames in walk(hdir):
            leafdir = basename(dirpath)
            if leafdir != 'data':
                continue
            radix = basename(dirname(dirpath))
            deffile = f'{radix}.hjson'
            if deffile not in filenames:
                continue
            filename = joinpath(dirpath, deffile)
            with open(filename, 'rt', encoding='utf8') as hfp:
                try:
                    self._parse(hfp)
                except Exception:
                    self._log.error('Failed to parse %s', filename)
                    raise

    def _parse(self, hfp: TextIO) -> None:
        values = jload(hfp, object_pairs_hook=dict)
        filename = hfp.name
        registers = values.get('registers')
        self.emit_ipname = True
        self._log.info('Parsing %s', filename)
        for regs in self._enumerate_registers(registers):
            try:
                ipname = splitext(basename(filename))[0].upper()
                self._parse_register(ipname, regs)
            except Exception:
                pprint(regs)
                raise

    def _enumerate_registers(self, values: [list, dict]) -> Iterator[dict]:
        if isinstance(values, list):
            for sub in values:
                yield from self._enumerate_registers(sub)
        if isinstance(values, dict):
            if 'fields' in values:
                yield values
            else:
                for regs in values.values():
                    yield from self._enumerate_registers(regs)

    def _parse_register(self, ipname: str,  reg: dict) -> None:
        regname = reg.get('name')
        self._log.debug('Parsing %s.%s', ipname, regname)
        reg_swaccess = reg.get('swaccess')
        emit_regname = True
        for field in reg.get('fields', []):
            fld_swaccess = field.get('swaccess', reg_swaccess).lower()
            if fld_swaccess in ('ro', 'wo', 'rw'):
                continue
            mubi = bool(field.get('mubi', False))
            if not mubi:
                continue
            if self.emit_ipname:
                print(ipname)
                self.emit_ipname = False
            if emit_regname:
                print(' *', regname)
                emit_regname = False
                print('   -', field.get('name'), fld_swaccess)


def main():
    """Main routine"""
    debug = False
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        argparser.add_argument('ot', nargs='+', metavar='dir',
                               help='HJSON top-level directory')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'mbb')

        for hjson in args.ot:
            mbb = MbbChecker()
            mbb.parse(hjson)

    # pylint: disable=broad-except
    except Exception as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
