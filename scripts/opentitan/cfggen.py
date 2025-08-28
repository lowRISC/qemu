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
from typing import NamedTuple, Optional, TextIO
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
from ot.util.misc import camel_to_snake_case, to_bool


OtParamRegex = str
"""Definition of a parameter to seek and how to shorten it."""


class OtClock(NamedTuple):
    """Clock definition."""

    name: str
    """Clock signal name."""

    frequency: int
    """Clock frequency in Hz."""

    aon: bool
    """Whether the clock is always on."""

    ref: bool
    """Whether the clock is a reference clock."""


class OtDerivedClock(NamedTuple):
    """Clock derived from a top level clock definition."""

    name: str
    """Clock signal name."""

    source: str
    """Clock source signal name."""

    div: int
    """Divider."""


class OtClockGroup(NamedTuple):
    """Clock logicial group definition."""

    name: str
    """Group name."""

    sources: list[str]
    """Clock source signal names."""

    sw_cg: bool
    """Whether clock group can be managed by SW."""

    hint: bool
    """Whether clock group can be hinted by SW."""


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
        self._keymgr: dict[str, str] = {}
        self._keymgr_name: Optional[str] = None
        self._top_clocks: dict[str, OtClock] = {}
        self._sub_clocks: dict[str, OtDerivedClock] = {}
        self._clock_groups: dict[str, OtClockGroup] = {}
        self._mod_clocks: dict[str, list[str]] = {}
        self._top_name: Optional[str] = None

    @property
    def top_name(self) -> Optional[str]:
        """Return the name of the top as defined in a configuration file."""
        return self._top_name

    def load_top_config(self, toppath: str) -> None:
        """Load data from HJSON top configuration file."""
        assert not _HJSON_ERROR
        with open(toppath, 'rt') as tfp:
            cfg = hjload(tfp, object_pairs_hook=dict)
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
            if modtype == 'lc_ctrl':
                self._load_top_values(module, self._lc, False,
                                      r'RndCnstLcKeymgrDiv(.*)')
                continue
            if modtype.startswith('keymgr'):
                self._keymgr_name = modtype
                self._load_top_values(module, self._keymgr, False,
                                      r'RndCnst((?:.*)Seed)')
                continue
        clocks = cfg.get('clocks', {})
        for clock in clocks.get('srcs', []):
            name = clock['name']
            aon = to_bool(clock['aon'], False)
            ref = to_bool(clock['ref'], False)
            freq = int(clock['freq'])
            self._top_clocks[name] = OtClock(name, freq, aon, ref)
        for clock in clocks.get('derived_srcs', []):
            name = clock['name']
            src = clock['src']
            aon = to_bool(clock['aon'], False)
            freq = int(clock['freq'])
            div = int(clock['div'])
            src_clock = self._top_clocks.get(src)
            if not src_clock:
                raise ValueError(f'Invalid top clock {src} '
                                 f'referenced from {name}')
            if src_clock.frequency // div != freq:
                raise ValueError(f'Incoherent derived clock {name} frequency: '
                                 f'{src_clock.frequency}/{div} != {freq}')
            if aon and not src_clock.aon:
                raise ValueError(f'Incoherent derived clock {name} AON')
            self._sub_clocks[name] = OtDerivedClock(name, src, div)
        clock_names = set(self._top_clocks.keys())
        clock_names.update(set(self._sub_clocks.keys()))
        for group in clocks.get('groups', []):
            ext = group['src'] == 'ext'
            if ext:
                continue
            name = group['name']
            hint = group['sw_cg'] == 'hint'
            sw_cg = not hint and to_bool(group['sw_cg'], False)
            clk_srcs = []
            for clk_name, clk_src in group.get('clocks', {}).items():
                if not hint:
                    exp_name = f'clk_{clk_src}_{name}'
                    if clk_name != exp_name:
                        raise ValueError(f'Unexpected clock {clk_name} in group'
                                         f' {name} (exp: {exp_name})')
                    clk_srcs.append(clk_src)
                else:
                    exp_prefix = f'clk_{clk_src}_'
                    if not clk_name.startswith(exp_prefix):
                        raise ValueError(f'Unexpected clock {clk_name} in group'
                                         f' {name}')
                    src_name = clk_name[len(exp_prefix):]
                    clk_srcs.append(src_name)
                    if src_name in self._sub_clocks:
                        raise ValueError(f'Refinition of clock {src_name}')
                    self._sub_clocks[src_name] = OtDerivedClock(src_name,
                                                                clk_src, 1)
            self._clock_groups[name] = OtClockGroup(name, clk_srcs, sw_cg,
                                                    hint)
        modules = cfg.get('module', [])
        mod_clocks = {}
        for module in modules:
            type_ = module['type']
            if type_ in ('ast', 'clkmgr'):
                continue
            name = module['name']
            clk_srcs = module.get('clock_srcs', {})
            clk_grp = module.get('clock_group', '')
            clocks = []
            for clk in clk_srcs.values():
                if isinstance(clk, dict):
                    clocks.append(f'{clk["group"]}.{clk["clock"]}')
                else:
                    clocks.append(f'{clk_grp}.{clk}')
            mod_clocks[name] = clocks
        self._mod_clocks = mod_clocks

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
        digests = {
            'cnsty_digest': 'digest',
            'flash_data_key': 'flash_data',
            'flash_addr_key': 'flash_addr',
            'sram_data_key': 'sram',
        }
        avail_digests = otpconst.get_digests()
        for digest, prefix in digests.items():
            if digest not in avail_digests:
                continue
            pair = otpconst.get_digest_pair(digest, prefix)
            self._otp.update(pair)
        idx = 0
        while True:
            try:
                defaults = otpconst.get_partition_inv_defaults(idx)
                if defaults:
                    self._otp[f'inv_default_part_{idx}'] = defaults
                idx += 1
            except ValueError:
                break

    def save(self, variant: str, socid: Optional[str], count: Optional[int],
             ofp: Optional[TextIO]) -> None:
        """Save QEMU configuration file using a INI-like file format,
           compatible with the `-readconfig` option of QEMU.
        """
        cfg = ConfigParser()
        self._generate_roms(cfg, socid, count or 1)
        self._generate_otp(cfg, variant, socid)
        self._generate_life_cycle(cfg, socid)
        self._generate_key_mgr(cfg, socid)
        self._generate_ast(cfg, variant, socid)
        self._generate_clkmgr(cfg, socid)
        self._generate_pwrmgr(cfg, socid)
        cfg.write(ofp)

    def show_clocks(self, ofp: Optional[TextIO]) -> None:
        """List clock inputs for each module."""
        mod_max_len = max(map(len, self._mod_clocks.keys()))
        for modname, modclocks in sorted(self._mod_clocks.items()):
            print(f'{modname:{mod_max_len}s}', ', '.join(modclocks), file=ofp)

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

    def _generate_key_mgr(self, cfg: ConfigParser,
                          socid: Optional[str] = None) -> None:
        nameargs = [f'ot-{self._keymgr_name}']
        if socid:
            nameargs.append(socid)
        kmname = '.'.join(nameargs)
        kmdata = {}
        for kname, value in self._keymgr.items():
            self.add_pair(kmdata, kname, value)
            kmdata = dict(sorted(kmdata.items()))
            cfg[f'ot_device "{kmname}"'] = kmdata

    def _generate_ast(self, cfg: ConfigParser, variant: str,
                         socid: Optional[str] = None) -> None:
        nameargs = [f'ot-ast-{variant}']
        if socid:
            nameargs.append(socid)
        clkname = '.'.join(nameargs)
        clkdata = {}
        topclockstr = ','.join(f'{c.name}:{c.frequency}'
                               for c in self._top_clocks.values())
        aonclockstr = ','.join(c.name for c in self._top_clocks.values()
                               if c.aon)
        self.add_pair(clkdata, 'topclocks', topclockstr)
        self.add_pair(clkdata, 'aonclocks', aonclockstr)
        cfg[f'ot_device "{clkname}"'] = clkdata

    def _generate_clkmgr(self, cfg: ConfigParser,
                         socid: Optional[str] = None) -> None:
        nameargs = ['ot-clkmgr']
        if socid:
            nameargs.append(socid)
        clkname = '.'.join(nameargs)
        clkdata = {}
        refclocks = [c for c in self._top_clocks.values() if c.ref]
        if len(refclocks) > 1:
            raise ValueError(f'Multiple reference clocks detected: '
                             f'{", ".join(refclocks)}')
        if refclocks:
            clkrefname = refclocks[0].name
            clfrefval = self._top_clocks.get(clkrefname)
            if not clfrefval:
                raise ValueError(f'Invalid reference clock {clkrefname}')
        else:
            clkrefname = None
            clfrefval = None
        topclockdefs = []
        for clkname, clkval in self._top_clocks.items():
            if clfrefval:
                clkratio = clkval.frequency // clfrefval.frequency
            else:
                clkratio = 1
            topclockdefs.append(f'{clkname}:{clkratio}')
        topclockstr = ','.join(topclockdefs)
        subclockstr = ','.join(f'{c.name}:{c.source}:{c.div}'
                               for c in self._sub_clocks.values())
        groupstr = ','.join(f'{g.name}:{"+".join(sorted(g.sources))}'
                            for g in self._clock_groups.values())
        swcfgstr = ','.join(g.name for g in self._clock_groups.values()
                            if g.sw_cg)
        hintstr = ','.join(g.name for g in self._clock_groups.values()
                            if g.hint)
        self.add_pair(clkdata, 'topclocks', topclockstr)
        if clkrefname:
            self.add_pair(clkdata, 'refclock', clkrefname)
        self.add_pair(clkdata, 'subclocks', subclockstr)
        self.add_pair(clkdata, 'groups', groupstr)
        self.add_pair(clkdata, 'swcfg', swcfgstr)
        self.add_pair(clkdata, 'hint', hintstr)
        cfg[f'ot_device "{clkname}"'] = clkdata

    def _generate_pwrmgr(self, cfg: ConfigParser,
                         socid: Optional[str] = None) -> None:
        nameargs = ['ot-pwrmgr']
        if socid:
            nameargs.append(socid)
        pwrname = '.'.join(nameargs)
        pwrdata = {}
        clockstr = ','.join(c.name for c in self._top_clocks.values()
                               if not c.aon)
        self.add_pair(pwrdata, 'clocks', clockstr)
        cfg[f'ot_device "{pwrname}"'] = pwrdata


def main():
    """Main routine"""
    debug = True
    top_map = {
        'darjeeling': 'dj',
        'earlgrey': 'eg',
    }
    actions = ['config', 'clock']
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
        mods = argparser.add_argument_group(title='Actions')
        mods.add_argument('-a', '--action', choices=actions,
                          action='append', default=[],
                          help=f'Action(s) to perform, default: {actions[0]}')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        log = configure_loggers(args.verbose, 'cfggen', 'otp')[0]

        if _HJSON_ERROR:
            argparser.error(f'Missing HJSON module: {_HJSON_ERROR}')

        cfg = OtConfiguration()

        topcfg = args.topcfg
        ot_dir = args.opentitan
        if not args.action:
            args.action.append(actions[0])
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
        top_dir = joinpath(ot_dir, 'hw', top)

        lcfilename = 'lc_ctrl_state_pkg.sv'
        lcpath = args.lifecycle
        if not lcpath:
            lc_constant_locations = [
                joinpath(top_dir, f'rtl/autogen/dev/{lcfilename}'),
                joinpath(top_dir, f'rtl/autogen/{lcfilename}'),
                joinpath(ot_dir, f'hw/ip/lc_ctrl/rtl/{lcfilename}'),
            ]
            for maybe_lcpath in lc_constant_locations:
                if isfile(maybe_lcpath):
                    lcpath = maybe_lcpath
                    break
        if not lcpath:
            argparser.error(f"Unknown location for '{lcfilename}'")
        if not isfile(lcpath):
            argparser.error(f"No such file '{lcpath}'")
        log.debug(f"'{lcfilename}' location: '%s'", lcpath)

        ocfilename = 'otp_ctrl_part_pkg.sv'
        ocpath = args.otpconst
        if not ocpath:
            otp_constant_locations = [
                joinpath(top_dir, f'ip_autogen/otp_ctrl/rtl/{ocfilename}'),
                joinpath(ot_dir, f'hw/ip/otp_ctrl/rtl/{ocfilename}'),
            ]
            for maybe_ocpath in otp_constant_locations:
                if isfile(maybe_ocpath):
                    ocpath = maybe_ocpath
                    break
        if not ocpath:
            argparser.error(f"Unknown location for '{ocfilename}'")
        if not isfile(ocpath):
            argparser.error(f"No such file '{ocpath}'")
        log.debug(f"'{ocfilename}' location: '%s'", ocpath)

        cfg.load_lifecycle(lcpath)
        cfg.load_otp_constants(ocpath)
        with open(args.out, 'wt') if args.out else sys.stdout as ofp:
            if 'config' in args.action:
                cfg.save(topvar, args.socid, args.count, ofp)
            if 'clock' in args.action:
                cfg.show_clocks(ofp)

    except (IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
