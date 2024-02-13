#!/usr/bin/env python3

# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OpenTitan OTP HJSON helper.

   :note: this script is deprecated and should be merged into otptool.py

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""


from argparse import ArgumentParser, FileType
try:
    # try to use HJSON if available
    from hjson import load as jload
except ImportError as _exc:
    raise ImportError("HJSON Python module required") from _exc
from logging import getLogger
from os import linesep
from pprint import pprint
from sys import exit as sysexit, modules, stderr
from traceback import format_exc
from typing import BinaryIO

from ot.util.log import configure_loggers


class OtpHelper:

    def __init__(self):
        self._log = getLogger('otp.help')
        self._log = {}
        self._params = {}
        self._registers = {}
        self._regwens = {}
        self._readonlys = []
        self._writeonlys = []

    def load(self, hfp: BinaryIO) -> None:
        self._data = jload(hfp, object_pairs_hook=dict)
        for param in self._data.get('param_list', []):
            if param['type'] == 'int':
                self._params[param['name']] = int(param['default'])
        for regdef in self._data['registers']['core']:
            multireg = regdef.get('multireg')
            if multireg:
                countparam = multireg.get('count')
                if countparam in self._params:
                    count = self._params[countparam]
                regdef = multireg
            else:
                count = 1
            name = regdef.get('name')
            if not name:
                continue
            regs = [name] if count == 1 else [f'{name}_{p}' for p in range(count)]
            regwen = regdef.get('regwen')
            if regwen:
                if regwen not in self._regwens:
                    self._regwens[regwen] = []
                for regname in regs:
                    self._regwens[regwen].append(regname)
            swaccess = regdef.get('swaccess')
            if swaccess == 'ro':
                for regname in regs:
                    self._readonlys.append(regname)
            elif swaccess == 'wo':
                for regname in regs:
                    self._writeonlys.append(regname)

    def make_read(self, c_code: bool = False, indent=4):
        if not self._writeonlys:
            return
        if not c_code:
            pprint(self._writeonlys)
            return
        lns = []
        lns.append('/* READ HELPERS */')
        lns.append('switch(reg) {')
        idt = ' ' * indent
        if self._writeonlys:
            for reg in self._writeonlys:
                lns.append(f'case R_{reg}:')
            lns.append('    qemu_log_mask(LOG_GUEST_ERROR, "%s: W/O register 0x%03" HWADDR_PRIx " (%s)\\n",')
            lns.append('                  __func__, addr, REG_NAME(reg));')
            lns.append('    return;')
        lns.append('default:')
        lns.append('    break;')
        lns.append('}')
        print('\n'.join((f'{idt}{l}' for l in lns)))

    def make_write(self, c_code: bool = False, indent=4):
        if not self._regwens and not self._readonlys:
            return
        if not c_code:
            pprint(self._regwens)
            return
        lns = []
        lns.append('/* WRITE HELPERS */')
        lns.append('switch(reg) {')
        idt = ' ' * indent
        for regwen, regs in self._regwens.items():
            for reg in regs:
                lns.append(f'case R_{reg}:')
            lns.append(f'    if (s->regs[R_{regwen}] & R_{regwen}_REGWEN_MASK) {{')
            lns.append(f'        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s is not enabled, %s is protected\\n",')
            lns.append(f'                      __func__, REG_NAME(R_{regwen}), REG_NAME(reg));')
            lns.append('        return;')
            lns.append('    }')
            lns.append('    break;')
        if self._readonlys:
            for reg in self._readonlys:
                lns.append(f'case R_{reg}:')
            lns.append('    qemu_log_mask(LOG_GUEST_ERROR, "%s: R/O register 0x%03" HWADDR_PRIx " (%s)\\n",')
            lns.append('                  __func__, addr, REG_NAME(reg));')
            lns.append('    return;')
        lns.append('default:')
        lns.append('    break;')
        lns.append('}')
        print('\n'.join((f'{idt}{l}' for l in lns)))


def main():
    """Main routine"""
    debug = True
    try:
        argparser = ArgumentParser(description=modules[__name__].__doc__)
        argparser.add_argument('-c', '--config', metavar='JSON',
                               type=FileType('rt', encoding='utf-8'),
                               help='path to configuration file')
        argparser.add_argument('-v', '--verbose', action='count',
                               help='increase verbosity')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')

        args = argparser.parse_args()
        debug = args.debug

        configure_loggers('otp', args.verbose, name_width=12, lineno=True)

        otp = OtpHelper()
        otp.load(args.config)
        otp.make_read(True)
        otp.make_write(True)
        sysexit(0)
    # pylint: disable=broad-except
    except Exception as exc:
        print(f'{linesep}Error: {exc}', file=stderr)
        if debug:
            print(format_exc(chain=False), file=stderr)
        sysexit(1)
    except KeyboardInterrupt:
        sysexit(2)


if __name__ == '__main__':
    main()
