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
from os.path import (abspath, basename, dirname, isdir, isfile,
                     join as joinpath, normpath)
from traceback import format_exc
from typing import NamedTuple, Optional, TextIO
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

from ot.lc_ctrl.const import LcCtrlConstants
from ot.otp.const import OtpConstants
from ot.otp.secret import OtpSecretConstants
from ot.util.arg import ArgError
from ot.util.log import configure_loggers
from ot.util.misc import alphanum_key, to_bool


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
    """QEMU configuration file generator.

       This may seem complicated and duplicated, since it is. This script tries
       to deal with the many way to define constants that have been used in many
       implementations of OpenTitan and that have changed over time. YMMV.
    """

    MODULES = {
        'rom_ctrl': (True, r'RndCnstScr(.*)'),
        'otp_ctrl': (False, r'RndCnst(.*)Init'),
        'lc_ctrl': (False, r'RndCnstLcKeymgrDiv(.*)'),
        'keymgr':  (False, r'RndCnst((?:.*)Seed)',
                           # The CDI keymgr seed does not match 'RndCnst.*Seed'
                           r'RndCnst(Cdi)'),
        'keymgr_dpe': (False, r'RndCnst((?:.*)Seed)'),
    }
    """Secrets to extract from OT modules, defined as regexes where the first
       capturing group of the first matching string in the module defines the
       secret name to store.
    """

    TRANSLATIONS = {
        'keymgr': {'cdi': 'cdi_seed'}
    }
    """Secret name translations from OT definitions to QEMU property names."""

    def __init__(self):
        self._log = getLogger('cfggen.cfg')
        self._otpconst = OtpConstants()
        self._lcconst = LcCtrlConstants()
        self._constants: dict[str, dict[Optional[int], dict[str, str]]] = {}
        self._top_clocks: dict[str, OtClock] = {}
        self._sub_clocks: dict[str, OtDerivedClock] = {}
        self._clock_groups: dict[str, OtClockGroup] = {}
        self._mod_clocks: dict[str, list[str]] = {}
        self._top_name: Optional[str] = None
        self._exclusions: dict[str, set[str]] = {}

    @property
    def top_name(self) -> Optional[str]:
        """Return the name of the top as defined in a configuration file."""
        return self._top_name

    def load_config(self, toppath: str) -> None:
        """Load data from HJSON configuration file."""
        assert not _HJSON_ERROR
        with open(toppath, 'rt') as tfp:
            cfg = hjload(tfp, object_pairs_hook=dict)
        self._top_name = cfg.get('name')
        topbase = basename(toppath)

        for module in cfg.get('module') or []:
            modtype = module.get('type')
            moddefs = self.MODULES.get(modtype)
            if not moddefs:
                continue
            multi, regexes = moddefs[0], moddefs[1:]
            consts = {}
            OtpSecretConstants.load_values(module, consts, multi, *regexes)
            if not consts:
                continue
            for cname, tname in self.TRANSLATIONS.get(modtype, {}).items():
                if cname in consts:
                    consts[tname] = consts.pop(cname)
            self._log.debug('Constants for %s loaded from %s', modtype, topbase)
            exist = modtype in self._constants
            if multi:
                if not exist:
                    self._constants[modtype] = consts
                else:
                    self._constants[modtype].update(consts)
            else:
                if exist:
                    raise ValueError(f'Redefinition of {modtype}')
                self._constants[modtype] = {None: consts}

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

    def load_lifecycle(self, svpath: str) -> None:
        """Load LifeCycle data from RTL file.

           :param svpath: System Verilog file with OTP constants
        """
        with open(svpath, 'rt') as cfp:
            self._log.debug('Loading LC constants from %s', svpath)
            self._lcconst.load_sv(cfp)

    def load_otp_constants(self, svpath: str) -> None:
        """Load OTP data from RTL file.

           :param svpath: System Verilog file with OTP constants
        """
        with open(svpath, 'rt') as cfp:
            self._log.debug('Loading OTP constants from %s', svpath)
            self._otpconst.load_sv(cfp)

    def load_constants(self, hjpath: Optional[str]) -> None:
        """Load definitions from HJSON file.

           :param hjpath: HJSON file with constants
        """
        if not hjpath:
            return
        assert not _HJSON_ERROR
        self._log.debug('Loading secrets from %s', hjpath)
        hjbase = basename(hjpath)
        with open(hjpath, 'rt') as tfp:
            cfg = hjload(tfp, object_pairs_hook=dict)
        for module in cfg.get('module') or []:
            modtype = module.get('type')
            moddefs = self.MODULES.get(modtype)
            if not moddefs:
                continue
            multi, regexes = moddefs[0], moddefs[1:]
            consts = {}
            OtpSecretConstants.load_values(module, consts, multi, *regexes)
            if not consts:
                continue
            for cname, tname in self.TRANSLATIONS.get(modtype, {}).items():
                if cname in consts:
                    consts[tname] = consts.pop(cname)
            self._log.debug('Constants for %s loaded from %s',
                            modtype, hjbase)
            exist = modtype in self._constants
            if multi:
                if not exist:
                    self._constants[modtype] = consts
                else:
                    self._constants[modtype].update(consts)
            else:
                if exist:
                    raise ValueError(f'Redefinition of {modtype}')
                self._constants[modtype] = {None: consts}
        self._otpconst.load_secrets(cfg)

    def prepare(self) -> None:
        """Prepare generation of data, aggregating several sources.
        """
        digests = {
            'cnsty_digest': 'digest',
            'flash_data_key': 'flash_data',
            'flash_addr_key': 'flash_addr',
            'sram_data_key': 'sram',
        }
        avail_digests = self._otpconst.get_digests()
        otp_ctrl = self._constants['otp_ctrl'][None]
        for digest, prefix in digests.items():
            if digest not in avail_digests:
                continue
            pair = self._otpconst.get_digest_pair(digest, prefix)
            otp_ctrl.update(pair)
        idx = 0
        while True:
            try:
                defaults = self._otpconst.get_partition_inv_defaults(idx)
                if defaults:
                    otp_ctrl[f'inv_default_part_{idx}'] = defaults
                idx += 1
            except ValueError:
                break
        lc_ctrl = self._constants['lc_ctrl'][None]
        lc_ctrl.update(self._lcconst.tokens)

    def exclude(self, exclusions: list[str]) -> None:
        """Add property exclusions.

           :param exclusions: property defined as <device>.<name_prefix> to
                              exclude from generation
        """
        for exclude in exclusions:
            try:
                dev, prop = exclude.split('.')
            except ValueError as exc:
                raise ArgError(f'Invalid exclusion format: {exclude}') from exc
            if dev not in self._exclusions:
                self._exclusions[dev] = set()
            self._exclusions[dev].add(prop)

    def save(self, variant: str, socid: Optional[str], count: Optional[int],
             ofp: Optional[TextIO]) -> None:
        """Save QEMU configuration file using a INI-like file format,
           compatible with the `-readconfig` option of QEMU.
        """
        cfg = ConfigParser()
        self._generate_roms(cfg, socid, count or 1)
        self._generate_otp(cfg, variant, socid)
        self._generate_lc_ctrl(cfg, socid)
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

    def add_pair(self, devname: str, data: dict[str, str], kname: str,
                 value: str) -> None:
        """Helper to create key, value pair entries."""
        for exc in self._exclusions.get(devname, []):
            if kname.startswith(exc):
                self._log.debug('Discarding %s.%s property', devname, kname)
                return
        if value:
            data[f'  {kname}'] = f'"{value}"'

    def _generate_roms(self, cfg: ConfigParser, socid: Optional[str] = None,
                       count: int = 1) -> None:
        for cnt in range(count):
            for rom, data in self._constants['rom_ctrl'].items():
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
                    self.add_pair(romname, romdata, kname, val)
                cfg[f'ot_device "{romname}"'] = romdata

    def _generate_otp(self, cfg: ConfigParser, variant: str,
                      socid: Optional[str] = None) -> None:
        nameargs = [f'ot-otp-{variant}']
        if socid:
            nameargs.append(socid)
        otpname = '.'.join(nameargs)
        otpdata = {}
        otp_ctrl = self._constants['otp_ctrl'][None]
        for kname, val in otp_ctrl.items():
            self.add_pair(otpname, otpdata, kname, val)

        otpdata = dict(sorted(otpdata.items(),
                              key=lambda x: alphanum_key(x[0])))
        cfg[f'ot_device "{otpname}"'] = otpdata

    def _generate_lc_ctrl(self, cfg: ConfigParser,
                          socid: Optional[str] = None) -> None:
        nameargs = ['ot-lc_ctrl']
        if socid:
            nameargs.append(socid)
        lcname = '.'.join(nameargs)
        lcdata = {}
        for name, states in self._lcconst.states.items():
            self.add_pair(lcname, lcdata, f'{name}_first', states[0])
            self.add_pair(lcname, lcdata, f'{name}_last', states[1])
        lc_ctrl = self._constants['lc_ctrl'][None]
        for kname, value in lc_ctrl.items():
            self.add_pair(lcname, lcdata, kname, value)
        lcdata = dict(sorted(lcdata.items()))
        cfg[f'ot_device "{lcname}"'] = lcdata

    def _generate_key_mgr(self, cfg: ConfigParser,
                          socid: Optional[str] = None) -> None:
        keymgr = None
        for keymgr_name in ('keymgr', 'keymgr_dpe'):
            if keymgr_name in self._constants:
                keymgr = self._constants[keymgr_name][None]
                break
        else:
            return
        nameargs = [f'ot-{keymgr_name}']
        if socid:
            nameargs.append(socid)
        kmname = '.'.join(nameargs)
        kmdata = {}
        for kname, value in keymgr.items():
            self.add_pair(kmname, kmdata, kname, value)
            kmdata = dict(sorted(kmdata.items()))
            cfg[f'ot_device "{kmname}"'] = kmdata

    def _generate_ast(self, cfg: ConfigParser, variant: str,
                         socid: Optional[str] = None) -> None:
        nameargs = [f'ot-ast-{variant}']
        if socid:
            nameargs.append(socid)
        astname = '.'.join(nameargs)
        astdata = {}
        topclockstr = ','.join(f'{c.name}:{c.frequency}'
                               for c in self._top_clocks.values())
        aonclockstr = ','.join(c.name for c in self._top_clocks.values()
                               if c.aon)
        self.add_pair(astname, astdata, 'topclocks', topclockstr)
        self.add_pair(astname, astdata, 'aonclocks', aonclockstr)
        cfg[f'ot_device "{astname}"'] = astdata

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
        for ckname, ckval in self._top_clocks.items():
            if clfrefval:
                clkratio = ckval.frequency // clfrefval.frequency
            else:
                clkratio = 1
            topclockdefs.append(f'{ckname}:{clkratio}')
        topclockstr = ','.join(topclockdefs)
        subclockstr = ','.join(f'{c.name}:{c.source}:{c.div}'
                               for c in self._sub_clocks.values())
        groupstr = ','.join(f'{g.name}:{"+".join(sorted(g.sources))}'
                            for g in self._clock_groups.values())
        swcgstr = ','.join(g.name for g in self._clock_groups.values()
                            if g.sw_cg)
        hintstr = ','.join(g.name for g in self._clock_groups.values()
                            if g.hint)
        self.add_pair(clkname, clkdata, 'topclocks', topclockstr)
        if clkrefname:
            self.add_pair(clkname, clkdata, 'refclock', clkrefname)
        self.add_pair(clkname, clkdata, 'subclocks', subclockstr)
        self.add_pair(clkname, clkdata, 'groups', groupstr)
        self.add_pair(clkname, clkdata, 'swcg', swcgstr)
        self.add_pair(clkname, clkdata, 'hint', hintstr)
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
        self.add_pair(pwrname, pwrdata, 'clocks', clockstr)
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
        files.add_argument('-S', '--secrets', metavar='HJSON',
                           help='Secret HJSON file (default: auto)')
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
        mods.add_argument('-x', '--exclude', action='append',
                          metavar='DEVICE.NAME', default=[],
                          help='Discard any property from DEVICE that starts '
                               'with NAME (may be repeated)')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        log = configure_loggers(args.verbose, 'cfggen', 'lc', 'otp')[0]

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
            cfg.load_config(topcfg)
        else:
            if not isfile(topcfg):
                argparser.error(f'No such top file: {topcfg}')
            cfg.load_config(topcfg)
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
                # master branch development environment
                joinpath(top_dir, f'rtl/autogen/testing/{lcfilename}'),
                # (obsolete) master branch development environment
                joinpath(top_dir, f'rtl/autogen/dev/{lcfilename}'),
                # (obsolete) master branch
                joinpath(top_dir, f'rtl/autogen/{lcfilename}'),
                # earlgrey_1.0.0 branch
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
                # master branch
                joinpath(top_dir, f'ip_autogen/otp_ctrl/rtl/{ocfilename}'),
                # earlgrey_1.0.0 branch
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

        secpath = args.secrets
        if secpath:
            if not isfile(secpath):
                argparser.error('No such secret file: {secpath}')
        else:
            sec_constant_locations = [
                # master branch development environment
                joinpath(top_dir,
                         f'data/autogen/{top}.secrets.testing.gen.hjson'),
                # (obsolete) master branch development environment
                joinpath(top_dir, f'data/autogen/{top}.secrets.dev.gen.hjson'),
            ]
            for maybe_secpath in sec_constant_locations:
                if isfile(maybe_secpath):
                    secpath = maybe_secpath
                    break

        cfg.load_lifecycle(lcpath)
        cfg.load_otp_constants(ocpath)
        cfg.load_constants(secpath)
        cfg.prepare()
        cfg.exclude(args.exclude)

        with open(args.out, 'wt') if args.out else sys.stdout as ofp:
            if 'config' in args.action:
                cfg.save(topvar, args.socid, args.count, ofp)
            if 'clock' in args.action:
                cfg.show_clocks(ofp)

    except (ArgError, IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(format_exc(chain=False), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
