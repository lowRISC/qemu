#!/usr/bin/env python3

# Copyright (c) 2024 Rivos, Inc.
# Copyright (c) 2025 lowRISC contributors.
# SPDX-License-Identifier: Apache2

"""OpenTitan QEMU configuration file generator.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser
from configparser import ConfigParser
from logging import getLogger
from os.path import abspath, dirname, isdir, isfile, join as joinpath, normpath
from traceback import format_exc
from typing import Optional
import re
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

try:
    _HJSON_ERROR = None
    from hjson import load as hjload
except ImportError as hjson_exc:
    _HJSON_ERROR = str(hjson_exc)
    def hjload(*_, **__):  # noqa: E301
        """dummy func if HJSON module is not available"""
        return {}

from ot.otp.const import OtpConstants
from ot.otp.lifecycle import OtpLifecycle
from ot.util.log import configure_loggers
from ot.util.misc import camel_to_snake_case


OtParamRegex = str
"""Definition of a parameter to seek and how to shorten it."""


class OtConfiguration:
    """QEMU configuration file generator."""

    def __init__(self):
        self._log = getLogger('cfggen.cfg')
        self._lc_states: tuple[str, str] = ('', '')
        self._lc_transitions: tuple[str, str] = ('', '')
        self._socdbg: tuple[str, str] = ('', '')
        self._ownership: tuple[str, str] = ('', '')
        self._roms: dict[Optional[int], dict[str, str]] = {}
        self._otp: dict[str, str] = {}
        self._lc: dict[str, str] = {}
        self._top_name: Optional[str] = None

    @property
    def top_name(self) -> Optional[str]:
        """Return the name of the top as defined in a configuration file."""
        return self._top_name

    def load_top_config(self, toppath: str) -> None:
        """Load data from HJSON top configuration file."""
        assert not _HJSON_ERROR
        with open(toppath, 'rt') as tfp:
            cfg = hjload(tfp)
        self._top_name = cfg.get('name')
        for module in cfg.get('module') or []:
            modtype = module.get('type')
            if modtype == 'rom_ctrl':
                self._load_top_values(module, self._roms, True,
                                      r'RndCnstScr(.*)')
                continue
            if modtype == 'otp_ctrl':
                self._load_top_values(module, self._otp, False,
                                      r'RndCnst(.*)Init')
                continue

    def load_lifecycle(self, lcpath: str) -> None:
        """Load LifeCycle data from RTL file."""
        lcext = OtpLifecycle()
        with open(lcpath, 'rt') as lfp:
            lcext.load(lfp)
        states = lcext.get_configuration('LC_STATE')
        if not states:
            raise ValueError('Cannot obtain LifeCycle states')
        for raw in {s for s in states if int(s, 16) == 0}:
            del states[raw]
        ostates = list(states)
        self._lc_states = ostates[0], ostates[-1]
        self._log.info("States first: '%s', last '%s'",
                       states[self._lc_states[0]], states[self._lc_states[1]])
        trans = lcext.get_configuration('LC_TRANSITION_CNT')
        if not trans:
            raise ValueError('Cannot obtain LifeCycle transitions')
        for raw in {s for s in trans if int(s, 16) == 0}:
            del trans[raw]
        otrans = list(trans)
        self._lc_transitions = otrans[0], otrans[-1]
        self._log.info('Transitions first: %d, last %d',
                       int(trans[self._lc_transitions[0]]),
                       int(trans[self._lc_transitions[1]]))
        self._lc.update(lcext.get_tokens(False, False))
        socdbg = lcext.get_configuration('SOCDBG')
        if socdbg:
            for raw in {s for s in socdbg if int(s, 16) == 0}:
                del socdbg[raw]
            osoc = list(socdbg)
            self._socdbg = osoc[0], osoc[-1]
            self._log.info("Socdbg first: '%s', last '%s'",
                           socdbg[self._socdbg[0]], socdbg[self._socdbg[1]])
        ownership = lcext.get_configuration('OWNERSHIP')
        if ownership:
            for raw in {s for s in ownership if int(s, 16) == 0}:
                del ownership[raw]
            osoc = list(ownership)
            self._ownership = osoc[0], osoc[-1]
            self._log.info("Ownership first: '%s', last '%s'",
                           ownership[self._ownership[0]],
                           ownership[self._ownership[1]])

    def load_otp_constants(self, otppath: str) -> None:
        """Load OTP data from RTL file."""
        otpconst = OtpConstants()
        with open(otppath, 'rt') as cfp:
            otpconst.load(cfp)
        self._otp.update(otpconst.get_digest_pair('cnsty_digest', 'digest'))
        self._otp.update(otpconst.get_digest_pair('sram_data_key', 'sram'))

    def save(self, variant: str, socid: Optional[str], count: Optional[int],
             outpath: Optional[str]) \
            -> None:
        """Save QEMU configuration file using a INI-like file format,
           compatible with the `-readconfig` option of QEMU.
        """
        cfg = ConfigParser()
        self._generate_roms(cfg, socid, count or 1)
        self._generate_otp(cfg, variant, socid)
        self._generate_life_cycle(cfg, socid)
        if outpath:
            with open(outpath, 'wt') as ofp:
                cfg.write(ofp)
        else:
            cfg.write(sys.stdout)

    @classmethod
    def add_pair(cls, data: dict[str, str], kname: str, value: str) -> None:
        """Helper to create key, value pair entries."""
        if value:
            data[f'  {kname}'] = f'"{value}"'

    def _load_top_values(self, module: dict, odict: dict, multi: bool,
                         *regexes: tuple[OtParamRegex, ...]) -> None:
        modname = module.get('name')
        if not modname:
            return
        for params in module.get('param_list', []):
            if not isinstance(params, dict):
                continue
            for regex in regexes:
                pmo = re.match(regex, params['name'])
                if not pmo:
                    continue
                value = params.get('default')
                if not value:
                    continue
                if value.startswith('0x'):
                    value = value[2:]
                kname = camel_to_snake_case(pmo.group(1))
                if multi:
                    imo = re.search(r'(\d+)$', modname)
                    idx = int(imo.group(1)) if imo else None
                    if idx not in odict:
                        odict[idx] = {}
                    odict[idx][kname] = value
                else:
                    odict[kname] = value

    def _generate_roms(self, cfg: ConfigParser, socid: Optional[str] = None,
                       count: int = 1) -> None:
        for cnt in range(count):
            for rom, data in self._roms.items():
                nameargs = ['ot-rom_ctrl']
                if socid:
                    if count > 1:
                        nameargs.append(f'{socid}{cnt}')
                    else:
                        nameargs.append(socid)
                if rom is not None:
                    nameargs.append(f'rom{rom}')
                romname = '.'.join(nameargs)
                romdata = {}
                for kname, val in sorted(data.items()):
                    self.add_pair(romdata, kname, val)
                cfg[f'ot_device "{romname}"'] = romdata

    def _generate_otp(self, cfg: ConfigParser, variant: str,
                      socid: Optional[str] = None) -> None:
        nameargs = [f'ot-otp-{variant}']
        if socid:
            nameargs.append(socid)
        otpname = '.'.join(nameargs)
        otpdata = {}
        for kname, val in self._otp.items():
            self.add_pair(otpdata, kname, val)
        otpdata = dict(sorted(otpdata.items()))
        cfg[f'ot_device "{otpname}"'] = otpdata

    def _generate_life_cycle(self, cfg: ConfigParser,
                             socid: Optional[str] = None) -> None:
        nameargs = ['ot-lc_ctrl']
        if socid:
            nameargs.append(socid)
        lcname = '.'.join(nameargs)
        lcdata = {}
        self.add_pair(lcdata, 'lc_state_first', self._lc_states[0])
        self.add_pair(lcdata, 'lc_state_last', self._lc_states[1])
        self.add_pair(lcdata, 'lc_trscnt_first', self._lc_transitions[0])
        self.add_pair(lcdata, 'lc_trscnt_last', self._lc_transitions[1])
        self.add_pair(lcdata, 'ownership_first', self._ownership[0])
        self.add_pair(lcdata, 'ownership_last', self._ownership[1])
        self.add_pair(lcdata, 'socdbg_first', self._socdbg[0])
        self.add_pair(lcdata, 'socdbg_last', self._socdbg[1])
        for kname, value in self._lc.items():
            self.add_pair(lcdata, kname, value)
        lcdata = dict(sorted(lcdata.items()))
        cfg[f'ot_device "{lcname}"'] = lcdata


def main():
    """Main routine"""
    debug = True
    top_map = {
        'darjeeling': 'dj',
        'earlgrey': 'eg',
    }
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        files = argparser.add_argument_group(title='Files')
        files.add_argument('opentitan', nargs='?', metavar='OTDIR',
                           help='OpenTitan root directory')
        files.add_argument('-T', '--top', choices=top_map.keys(),
                           help='OpenTitan top name')
        files.add_argument('-o', '--out', metavar='CFG',
                           help='Filename of the config file to generate')
        files.add_argument('-c', '--otpconst', metavar='SV',
                           help='OTP Constant SV file (default: auto)')
        files.add_argument('-l', '--lifecycle', metavar='SV',
                           help='LifeCycle SV file (default: auto)')
        files.add_argument('-t', '--topcfg', metavar='HJSON',
                           help='OpenTitan top HJSON config file '
                                '(default: auto)')
        mods = argparser.add_argument_group(title='Modifiers')
        mods.add_argument('-s', '--socid',
                          help='SoC identifier, if any')
        mods.add_argument('-C', '--count', default=1, type=int,
                          help='SoC count (default: 1)')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        log = configure_loggers(args.verbose, 'cfggen', 'otp')[0]

        if _HJSON_ERROR:
            argparser.error('Missing HJSON module: {_HJSON_ERROR}')

        cfg = OtConfiguration()

        topcfg = args.topcfg
        ot_dir = args.opentitan
        if not topcfg:
            if not args.opentitan:
                argparser.error('OTDIR is required is no top file is specified')
            if not isdir(ot_dir):
                argparser.error('Invalid OpenTitan root directory')
            ot_dir = abspath(ot_dir)
            if not args.top:
                argparser.error('Top name is required if no top file is '
                                'specified')
            top = f'top_{args.top}'
            topvar = top_map[args.top]
            topcfg = joinpath(ot_dir, f'hw/{top}/data/autogen/{top}.gen.hjson')
            if not isfile(topcfg):
                argparser.error(f"No such file '{topcfg}'")
            log.info("Top config: '%s'", topcfg)
            cfg.load_top_config(topcfg)
        else:
            if not isfile(topcfg):
                argparser.error(f'No such top file: {topcfg}')
            cfg.load_top_config(topcfg)
            ltop = cfg.top_name
            if not ltop:
                argparser.error('Unknown top name')
            log.info("Top: '%s'", cfg.top_name)
            ltop = ltop.lower()
            topvar = {k.lower(): v for k, v in top_map.items()}.get(ltop)
            if not topvar:
                argparser.error(f'Unsupported top name: {cfg.top_name}')
            top = f'top_{ltop}'
            if not ot_dir:
                check_dir = f'hw/{top}/data'
                cur_dir = dirname(topcfg)
                while cur_dir:
                    check_path = joinpath(cur_dir, check_dir)
                    if isdir(check_path):
                        ot_dir = cur_dir
                        break
                    cur_dir = dirname(cur_dir)
                if not ot_dir:
                    argparser.error('Cannot find OT root directory')
            elif not isdir(ot_dir):
                argparser.error('Invalid OpenTitan root directory')
            ot_dir = abspath(ot_dir)
            log.info("OT directory: '%s'", ot_dir)
        log.info("Variant: '%s'", topvar)
        if not args.lifecycle:
            lcpath = joinpath(ot_dir, 'hw/ip/lc_ctrl/rtl/lc_ctrl_state_pkg.sv')
        else:
            lcpath = args.lifecycle
        if not isfile(lcpath):
            argparser.error(f"No such file '{lcpath}'")

        if not args.otpconst:
            ocpath = joinpath(ot_dir, 'hw', top,
                              'ip_autogen/otp_ctrl/rtl/otp_ctrl_part_pkg.sv')
        else:
            ocpath = args.otpconst
        if not isfile(lcpath):
            argparser.error(f"No such file '{ocpath}'")

        cfg = OtConfiguration()
        cfg.load_lifecycle(lcpath)
        cfg.load_otp_constants(ocpath)
        cfg.save(topvar, args.socid, args.count, args.out)

    except (IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
