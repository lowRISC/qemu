#!/usr/bin/env python3

# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Generate machine definitions for OpenTitan top.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType
from logging import getLogger
from os import listdir
from os.path import (abspath, basename, commonprefix, dirname, isdir, isfile,
                     join as joinpath)
from traceback import format_exception
from typing import Any, NamedTuple, TextIO
import re
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(abspath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

# ruff: noqa: E402
from ot.util.arg import ArgError
from ot.util.log import configure_loggers
from ot.util.misc import HexInt, camel_to_snake_uppercase, classproperty, redent

try:
    _HJSON_ERROR = None
    from hjson import load as hjload
except ImportError as hjson_exc:
    _HJSON_ERROR = str(hjson_exc)


class QEMUDevice(NamedTuple):
    """A device slot."""

    base: HexInt
    size: HexInt
    aspc: str


class QEMUSignal(NamedTuple):
    """Interrupt or Alert"""

    name: str
    index: int


class AutoTop:
    """Helper class to generate QEMU machine definition from Top definitions.
    """

    SV_ENUM_CRE = re.compile(
        r'(?s)typedef\s+enum\s+([\w\s]*){([^\}]*)}\s+(\w+);')
    SV_ENUM_ITEM_CRE = re.compile(r'(?s)([\w\d]+)(?:\s+=\s+(\d+))?,')
    SV_PARAM_CRE = re.compile(r"parameter\sint\sunsigned\s([A-Z0-9_]+)\s"
                              r"=\s\d+'h([0-9a-fA-F]+);")
    DEVICE_MAP = {
        'OTP_MACRO': 'OTP_BACKEND',
        'RV_CORE_IBEX': 'IBEX_WRAPPER',
        'RV_PLIC': 'PLIC',
        'RV_TIMER': 'TIMER',
    }
    """RTL-to-QEMU renamed devices."""

    def __init__(self, topname):
        self._log = getLogger('autotop')
        self._topname = topname
        self._ases: list[str] = []
        self._devices: dict[str, dict[str, QEMUDevice]] = {}
        self._interrupts: dict[str, list[tuple[str, int]]] = {}
        self._alerts: dict[str, list[tuple[str, int]]] = {}
        self._pinmux: dict[str, list[str]] = {}
        self._mbox_indices: dict[str, int] = {}

    def load(self, ot_dir: str) -> None:
        """Load configuration files.

           :param: OpenTitan top directory
        """
        ot_dir = abspath(ot_dir)
        top = f'top_{self._topname}'
        topdir = joinpath(ot_dir, f'hw/{top}')
        if not isdir(topdir):
            raise ArgError(f"No such top '{self._topname}'")
        topcfg = joinpath(topdir, 'data/autogen', f'{top}.gen.hjson')
        if not isfile(topcfg):
            raise ArgError(f"No config file for top '{self._topname}'")
        self._load_devices(topdir)
        with open(topcfg, 'rt') as hfp:
            hjson = hjload(hfp, object_pairs_hook=dict)
        self._load_all_interrupts(hjson)
        self._load_all_alerts(hjson)
        self._load_pinmux(hjson)

    @classmethod
    def device_name(cls, name: str, discard_aon: bool = False) -> str:
        """Generate the devices name.

           :param name: original name
           :param discard_aon: discard AON qualifier
        """
        pname = name.upper()
        if discard_aon:
            pname = pname.replace('_AON', '')
        return cls.DEVICE_MAP.get(pname, pname)

    @classproperty
    def outkinds(cls) -> list[str]:
        """Report which generation output formats are supported.
        """
        # pylint: disable=no-self-argument
        prefix = 'generate_'
        return [f.removeprefix(prefix) for f in dir(cls)
                if f.startswith(prefix)]

    def generate_qemu(self, prefix: str, tfp: TextIO) -> None:
        """Generate a QEMU template file or machine definition.

           :param prefix: prefix for C definition
           :param tfp: output file stream
        """
        lprefix = prefix.lower()
        self._generate_qemu_dev_enum(prefix, tfp)
        self._generate_qemu_pinmux(prefix, tfp)
        print(f'static const IbexDeviceDef {lprefix}_devices[] = {{', file=tfp)
        print('/* clang-format off */', file=tfp)
        for devname in sorted(self._devices, key=self._device_address):
            self._generate_qemu_devices(prefix, devname, tfp)
        print('/* clang-format on */', file=tfp)
        print('};', file=tfp)

    def generate_bmtest(self, _: str, tfp: TextIO) -> None:
        """Generate a Rust template file for machine definition.

           :param tfp: output file stream
        """
        self._generate_bmtest_base_addresses(tfp)
        self._generate_bmtest_interrupts(tfp)
        self._generate_bmtest_alerts(tfp)
        self._generate_bmtest_pinmux(tfp)

    def _load_devices(self, topdir: str) -> None:
        iptopdir = f'{topdir}/ip'
        for ipdir in listdir(iptopdir):
            ippath = joinpath(iptopdir, ipdir)
            if not isdir(ippath) or not ipdir.startswith('xbar_'):
                continue
            gendir = joinpath(ippath, 'data/autogen')
            if not isdir(gendir):
                continue
            for genfile in listdir(gendir):
                if (not genfile.endswith('.gen.hjson') or
                    not genfile.startswith('xbar_')):
                    continue
                xbarcfg = joinpath(gendir, genfile)
                if not isfile(xbarcfg):
                    continue
                self._load_xbar(xbarcfg)

    def _load_xbar(self, xbarcfg: str) -> None:
        """Load a Top xbar configuration file.

           :param xbarcfg: path to the HJSON to load
        """
        with open(xbarcfg, 'rt') as hfp:
            hjson = hjload(hfp, object_pairs_hook=dict)
        self._ases = hjson.get('addr_spaces')
        if not self._ases:
            raise ValueError('No address space defined')
        if len(self._ases) > 1:
            raise NotImplementedError('Multiple address spaces')
        aspc = self._ases[0]
        xbar = basename(xbarcfg).split('.')[0].split('_', 1)[-1]
        self._log.debug('Address space for %s: %s', xbar, ', '.join(self._ases))
        for node in hjson.get('nodes', []):
            if node['type'] != 'device':
                continue
            if node['xbar']:
                continue
            parts = node['name'].split('.', 1)
            name = parts[0]
            subname = parts[1] if len(parts) > 1 else 'regs'
            if subname in ('core', 'jtag', 'dmi', 'prim', 'cfg'):
                subname = 'regs'
            addr_ranges = node.get('addr_range')
            if not addr_ranges:
                continue
            self._log.debug('loading device %s (%s)', name, subname)
            for addr_range in addr_ranges:
                base_addrs = addr_range.get('base_addrs', {})
                if len(base_addrs) != 1:
                    raise NotImplementedError('Multiple base addresses')
                addr = HexInt(base_addrs[aspc], 16)
                size = HexInt(addr_range.get('size_byte', 0), 16)
                dev = QEMUDevice(addr, size, aspc)
                uname = self.device_name(name, True)
                if uname not in self._devices:
                    self._devices[uname] = {}
                self._devices[uname][subname] = dev

    def _load_all_interrupts(self, hjson: dict[str, Any]) -> None:
        interrupts = self._load_interrupts( hjson.get('interrupt', []))
        irq_num = 0
        for dev, name in interrupts:
            if dev not in self._interrupts:
                self._interrupts[dev] = []
            irq_num += 1  # IRQ #0 is not an IRQ
            self._interrupts[dev].append(QEMUSignal(name, irq_num))
        self._log.info('found %d interrupts', irq_num)

    def _load_interrupts(self, interrupts: dict[str, Any]) \
            -> list[tuple[str, str]]:
        int_list: list[tuple[str, str]] = []
        for interrupt in interrupts:
            dev = self.device_name(interrupt['module_name'], True)
            width = interrupt.get('width', 1)
            prefix = f'{dev.lower()}_'
            name = interrupt['name'].lower().removeprefix(prefix)
            if width == 1:
                int_list.append((dev, name))
            else:
                int_list.extend((dev, f'{name}{pos}') for pos in range(width))
        return int_list

    def _load_all_alerts(self, hjson: dict[str, Any]) -> None:
        alerts: list[tuple[str, str]] = []
        alerts.extend(self._load_alerts('', hjson.get('alert', [])))
        for cat, cat_alerts in hjson.get('incoming_alert', {}).items():
            alerts.extend(self._load_alerts(f'incoming_{cat}', cat_alerts))
        alert_num = 0
        for dev, name in alerts:
            if dev not in self._alerts:
                self._alerts[dev] = []
            self._alerts[dev].append(QEMUSignal(name, alert_num))
            alert_num += 1
        self._log.info('found %d alerts', alert_num)

    def _load_alerts(self, cat: str, alerts: dict[str, Any]) \
            -> None:
        alert_list: list[tuple[str, str]] = []
        if cat:
            cat = f'{cat}_'.upper()
        for alert in alerts:
            dev = self.device_name(alert['module_name'], True)
            prefix = f'{dev.lower()}_'
            lname = alert['name'].lower().removeprefix(prefix)
            alert_list.append((f'{cat}{dev}', lname))
        return alert_list

    def _load_pinmux(self, hjson: dict[str, Any]) -> None:
        pinmux = hjson.get('pinmux', {})
        ios = pinmux.get('ios', [])
        pinmap: dict[str, list[str]] = {
            'dio': [],
            'mio_in': [],
            'mio_out': [],
            'mio_inout': [],
        }
        for ioe in ios:
            name = ioe['name']
            cnx = ioe['connection']
            idx = ioe['idx']
            type_ = ioe['type']
            iname = f'{name}{idx}' if idx >= 0 else name
            if cnx == 'direct':
                pinmap['dio'].append(iname)
            elif cnx == 'muxed':
                if type_ == 'input':
                    pinmap['mio_in'].append(iname)
                elif type_ == 'output':
                    pinmap['mio_out'].append(iname)
                elif type_ == 'inout':
                    pinmap['mio_inout'].append(iname)
                else:
                    self._log.error('Unknown MIO type %s', type_)
            elif cnx == 'manual':
                # not yet implemented
                self._log.warning('Unmanaged connection type %s', cnx)
            else:
                self._log.error('Unknown connection type %s', cnx)
        self._pinmux = pinmap

    def _device_address(self, dev: str) -> int:
        try:
            return self._devices[dev]['regs'].base
        except KeyError:
            try:
                return self._devices[dev]['ctn'].base
            except KeyError:
                return -1

    def _get_mbox_index(self, udev: str) -> int:
        if udev not in self._mbox_indices:
            self._mbox_indices[udev] = len(self._mbox_indices)
        return self._mbox_indices[udev]

    def _generate_qemu_dev_enum(self, prefix: str, tfp: TextIO) -> None:
        lines = []
        uprefix = prefix.upper()
        tprefix = prefix.title().replace('_', '')
        print(f'enum {tprefix}Device {{', file=tfp)
        for dev in sorted(self._devices):
            lines.append(f'{uprefix}_DEV_{dev}')
        code = redent(',\n'.join(lines), 4)
        print(code, file=tfp)
        print('}\n', file=tfp)

    def _generate_qemu_pinmux(self, prefix: str, tfp: TextIO) -> None:
        tprefix = prefix.title().replace('_', '')
        for ioname, pinmux in self._pinmux.items():
            lines = []
            u_ioname = ioname.upper()
            uc_ioname = ioname.title().replace('_', '')
            print(f'enum {tprefix}Pinmux{uc_ioname} {{', file=tfp)
            max_val = 0
            for val, ion in enumerate(pinmux):
                us_ion = camel_to_snake_uppercase(ion)
                lines.append(f'{u_ioname}_{us_ion}, /* {val} */')
                max_val = max(val, max_val)
            lines.append(f'{u_ioname}_COUNT, /* {max_val + 1} */')
            code = redent('\n'.join(lines), 4)
            print(code, file=tfp)
            print('};\n', file=tfp)

    def _generate_qemu_devices(self, prefix: str, dev: str, tfp: TextIO) \
            -> None:
        lines: list[str] = []
        uprefix = prefix.upper()
        irq_defs = self._generate_qemu_irq_defs(uprefix, dev)
        alert_defs = self._generate_qemu_alert_defs(uprefix, dev)
        mmap_defs = self._generate_qemu_mmap_defs(dev)
        if dev not in self._devices:
            self._log.warning('%s not in devices', dev)
        lines.append(f'[{uprefix}_DEV_{dev}] = {{')
        mmo = re.match(r'(MBX(?:_[A-Z]+)?)\d*$', dev)
        if mmo:
            # mailboxes are defined with a macro
            mbxidx = self._get_mbox_index(dev)
            addr = self._devices[dev]['regs'].base
            irq = self._interrupts[dev][0]
            try:
                alert = self._alerts[dev][0]
            except KeyError:
                # temporary
                alert = QEMUSignal("missing", -1)
            mbox_map = {'MBX_JTAG' : 'DEBUG'}
            if mmo.group(1) not in mbox_map:
                lines.append(f'    {uprefix}_DEV_MBX({mbxidx}, {addr}u, '
                             f'"ot-mbx.sram", {irq.index}, {alert.index}),')
            else:
                mmem = mbox_map[dev]
                spacer = ' ' * len(f'    {uprefix}_DEV_MBX_DUAL(')
                lines.append(f'    {uprefix}_DEV_MBX_DUAL({mbxidx}, {addr}u, '
                             f'"ot-mbx.sram", {irq.index}, {alert.index},')
                lines.append(f'{spacer}'
                             f'{mmem}_MEMORY({uprefix}_{mmem}_{dev}_ADDR))')
        else:
            devbase = re.sub(r'\d+$', '', dev)
            lines.append(f'    .type = TYPE_OT_{devbase},')
            if mmap_defs:
                lines.append('    .memmap = MEMMAPENTRIES(')
                lines.append(redent(',\n'.join(mmap_defs), 8))
                lines.append('    ),')
            if irq_defs or alert_defs:
                defs = []
                defs.extend(irq_defs)
                defs.extend(alert_defs)
                lines.append('    .gpio = IBEXGPIOCONNDEFS(')
                lines.append(redent(',\n'.join(defs), 8))
                lines.append('    ),')
        lines.append('},')
        code = redent('\n'.join(lines), 4)
        print(code, file=tfp)

    def _generate_qemu_mmap_defs(self, device: str) -> list[str]:
        # sorting memory range the weird way (hack ahead)
        # we want the IBEX bus to be seen first (vs. debug or external buses)
        # and appear in the logical address order
        def _sort_mems(dev):
            bus, addr = dev.base >> 28, dev.base & ~(0xf << 28)
            if dev.aspc == 'hart':
                return ('', -bus, addr)
            return (dev.aspc, -bus, addr)

        mmaps = []
        for dev in sorted(self._devices.get(device, {}).values(),
                          key=_sort_mems):
            width = '08' if dev.aspc == 'hart' else ''
            mmaps.append(f'{{ .base = 0x{dev.base:{width}x}u }}')
        return mmaps

    def _generate_qemu_alert_defs(self, prefix: str, dev: str) -> list[str]:
        alerts = []
        for pos, alert in enumerate(self._alerts.get(dev, [])):
            alerts.append(f'{prefix}_GPIO_ALERT({pos}, {alert.index})')
        return alerts

    def _generate_qemu_irq_defs(self, prefix: str, dev: str) -> list[str]:
        irqs = []
        for pos, irq in enumerate(self._interrupts.get(dev, [])):
            irqs.append(f'{prefix}_GPIO_SYSBUS_IRQ({pos}, PLIC, '
                        f'{irq.index})')
        return irqs

    def _generate_bmtest_base_addresses(self, tfp):
        utop = self._topname.upper()
        print('pub mod base_addresses {', file=tfp)
        for dev in sorted(self._devices, key=self._device_address):
            devices = self._devices[dev]
            devcount = len(devices)
            dev = re.sub('^RV_', '', dev)
            dev = re.sub(f'_{utop}$', '', dev)
            dev = re.sub(f'^{utop}_', '', dev)
            for name, slot in devices.items():
                suffix = ''
                if devcount > 1:
                    if name in ('dbg', 'soc'):
                        continue
                    mbxmo = re.match(r'^MBX(.*)', dev)
                    if mbxmo:
                        dev = f'MBXHOST{mbxmo.group(1)}'
                        suffix = ''
                    else:
                        suffix = {
                            'regs': '_CTRL',
                            'mem': '_MEM',
                            'ram': '_MEM',
                            'rom': '_MEM',
                        }.get(name) or f'_{name.upper()}'
                elif dev.startswith('SOC_PROXY'):
                    dev = 'CTN'
                print(f'    pub const {dev}{suffix}: usize = {slot.base:#_x};',
                      file=tfp)
        print('}\n', file=tfp)

    def _generate_bmtest_interrupts(self, tfp):
        irqs: dict[str, int] = {}
        if not self._interrupts:
            return
        for dev, dev_irqs in self._interrupts.items():
            for irq in dev_irqs:
                irqname = irq.name.upper()
                prefix = commonprefix((irqname, dev))
                if prefix:
                    irqname = irqname.removeprefix(prefix).lstrip('_')
                irqs[f'{dev}_{irqname}'] = irq.index
        print('pub mod irq_num {', file=tfp)
        max_val = 0
        for name, val in sorted(irqs.items(), key=lambda irq: irq[1]):
            print(f'    pub const {name}: usize = {val};', file=tfp)
            max_val = max(val, max_val)
        print(f'    pub const COUNT: usize = {max_val + 1};', file=tfp)
        print('}\n', file=tfp)

    def _generate_bmtest_alerts(self, tfp) -> None:
        alerts: dict[str, int] = {}
        if not self._alerts:
            return
        for dev, dev_alerts in self._alerts.items():
            for alert in dev_alerts:
                alerts[f'{dev}_{alert.name.upper()}'] = alert.index
        print('pub mod alert_num {', file=tfp)
        max_val = 0
        for name, val in sorted(alerts.items(), key=lambda alert: alert[1]):
            print(f'    pub const {name}: usize = {val};', file=tfp)
            max_val = max(val, max_val)
        print(f'    pub const COUNT: usize = {max_val + 1};', file=tfp)
        print('}\n', file=tfp)

    def _generate_bmtest_pinmux(self, tfp) -> None:
        for ioname, pinmux in self._pinmux.items():
            if not pinmux:
                continue
            print(f'pub mod pinmux_{ioname} {{', file=tfp)
            max_val = 0
            for val, ion in enumerate(pinmux):
                us_ion = camel_to_snake_uppercase(ion)
                print(f'    pub const {us_ion}: usize = {val};', file=tfp)
                max_val = max(val, max_val)
            print(f'    pub const COUNT: usize = {max_val + 1};', file=tfp)
            print('}\n', file=tfp)


def main():
    """Main routine."""
    debug = True
    desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
    argparser = ArgumentParser(description=f'{desc}.')
    try:
        default_outkind = 'qemu'
        outkinds = sorted(AutoTop.outkinds,
                          key=lambda n: '' if n == default_outkind else n)
        assert len(outkinds) > 0
        top = argparser.add_argument_group(title='Top')
        top.add_argument('opentitan', nargs='?', metavar='OTDIR',
                           help='OpenTitan root directory')
        top.add_argument('-T', '--top', required=True,
                           help='OpenTitan top name')
        files = argparser.add_argument_group(title='Files')
        files.add_argument('-o', '--output', type=FileType('wt'),
                           metavar='FILE',
                           help='output file name')
        files.add_argument('-p', '--prefix', default='ot_dj_soc',
                           help='constant prefix (default: ot_dj_soc)')
        files.add_argument('-k', '--out-kind', choices=outkinds,
                            default=outkinds[0],
                            help=f'output file format '
                                 f'(default: {outkinds[0]})')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')

        args = argparser.parse_args()
        debug = args.debug

        if _HJSON_ERROR:
            argparser.error(f'Missing HJSON module: {_HJSON_ERROR}')

        configure_loggers(args.verbose, 'autotop')

        ot_dir = args.opentitan
        if not args.opentitan:
            argparser.error('OTDIR is required is no top file is specified')
        if not isdir(ot_dir):
            argparser.error('Invalid OpenTitan root directory')
        atop = AutoTop(args.top)
        atop.load(ot_dir)
        getattr(atop, f'generate_{args.out_kind}')(args.prefix,
                                                   args.output or sys.stdout)

    except ArgError as exc:
        argparser.error(str(exc))
    except (IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=sys.stderr)
        if debug:
            print(''.join(format_exception(exc, chain=False)),
                  file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(2)


if __name__ == '__main__':
    main()
