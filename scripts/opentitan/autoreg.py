#!/usr/bin/env python3

# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""QEMU OT tool to generate register definition for OT device.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType
from enum import StrEnum, auto
from io import StringIO
from logging import getLogger
from os.path import dirname, join as joinpath, normpath
from time import localtime
from traceback import format_exception
from textwrap import dedent, indent
from typing import Any, NamedTuple, Optional, TextIO, Union

import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

# ruff: noqa: E402
from ot.util.eval import safe_eval
from ot.util.log import configure_loggers
from ot.util.misc import (HexInt, camel_to_snake_case, camel_to_snake_uppercase,
                          classproperty)

try:
    _HJSON_ERROR = None
    from hjson import load as hjload
except ImportError as hjson_exc:
    _HJSON_ERROR = str(hjson_exc)


class QEMUAccess(StrEnum):
    """Type of access to a register or a field."""
    UNDEF = auto()
    RO = auto()
    RW = auto()
    WO = auto()
    RC = auto()
    RW0C = auto()
    RW1C = auto()
    RW1S = auto()
    R0W1C = auto()
    URO = auto()  # "unused read/only"


class QEMUField(NamedTuple):
    """A register field."""

    name: str
    desc: Optional[str]
    offset: int
    width: int
    access: QEMUAccess
    reset: int


class QEMURegister(NamedTuple):
    """A register."""

    name: str
    desc: Optional[str]
    address: int
    access: QEMUAccess
    fields: list[QEMUField]
    reset: Optional[int]
    shared_fields: bool = False


class QEMUAutoReg:
    """QEMU register definition generator.

       :param devname: the name of the generated QEMU device
       :param ignore_prefix: an optional prefix to discard useless register
                             definitions. Registers and fields starting with
                             this prefix are ignored.
       :param resets: a list of reset functions to generate, if any.
    """

    RESETS = ['enter', 'hold', 'exit']

    def __init__(self, devname: str, ignore_prefix: Optional[str] = None,
                 resets: Optional[list[str]] = None):
        self._log = getLogger('autoreg')
        self._devname = devname
        self._ignore_prefix = ignore_prefix
        self._resets = resets or []
        self._parameters: dict[str, Union[int, bool]] = {}
        self._regwidth = 0
        self._registers: list[QEMURegister] = []
        self._copyright = '@todo Add copyright attributions'

    @classproperty
    def generators(cls):
        """Provide a list of supported generators."""
        # pylint: disable=no-self-argument
        actions: list[str] = []
        for item in cls.__dict__:
            if not item.startswith('generate_'):
                continue
            actions.append(item[len('generate_'):])
        actions.sort()
        return actions

    @classmethod
    def redent(cls, text: str, spc: int = 0, strip_end: bool = False) -> str:
        """Utility function to re-indent code string.

           :param text: the text to re-indent
           :param spc: the number of leading empty space chars to prefix lines
           :param strip_end: whether to strip trailing whitespace and newline
        """
        text = dedent(text.lstrip('\n'))
        text = indent(text, ' ' * spc)
        if strip_end:
            text = text.rstrip(' ').rstrip('\n')
        return text

    def load(self, hfp: TextIO) -> None:
        """Load a register map definition from an OT HJSON file."""
        hjson = hjload(hfp, object_pairs_hook=dict)
        # heuristics
        # registers -seem- to either be a list of registers, or a map of buses
        # which contain the list of registers; need to check if some bus name
        # is defined for the registers that need to be parsed
        bus_if = None
        for bus in hjson.get('bus_interfaces', []):
            if bus.get('protocol') != 'tlul':
                continue
            if bus.get('direction') != 'device':
                continue
            bus_if = bus.get('name')
            break
        self._parameters.update(self._parse_parameters(hjson.get('param_list',
                                                       [])))
        self._regwidth = int(hjson.get('regwidth', 32))
        addr_inc = self._regwidth // 8
        address = 0
        registers: list[QEMURegister] = []
        registers.extend(self._parse_interrupts(hjson, address, addr_inc))
        address = len(registers) * addr_inc
        registers.extend(self._parse_alerts(hjson, address))
        address = len(registers) * addr_inc
        regnames: set[str] = set()
        regdefs = hjson['registers']
        if bus_if:
            self._log.info("Using registers from bus '%s'", bus_if)
            regdefs = regdefs[bus_if]
        for item in regdefs:
            if 'skipto' in item:
                address = HexInt.parse(item['skipto'])
                continue
            if 'multireg' in item:
                regs = self._parse_registers(address, item['multireg'])
                for reg in regs:
                    if reg.name in regnames:
                        raise ValueError(f'Register {reg.name} redefined')
                    regnames.add(reg.name)
                registers.extend(regs)
                address += addr_inc * len(regs)
                continue
            reg = self._parse_register(address, item)
            if reg:
                if reg.name in regnames:
                    prefix = reg.name.split('_')
                    if prefix in ('INTR', 'ALERT'):
                        # Comportable registers may already be defined, discard
                        continue
                    raise ValueError(f'Register {reg.name} redefined')
                regnames.add(reg.name)
                registers.append(reg)
                address += addr_inc
        registers.sort(key=lambda r: r.address)
        self._registers = registers

    @property
    def copyright(self) -> str:
        """Return current copyright string.
        """
        return self._copyright

    @copyright.setter
    def copyright(self, copyright_str: str) -> None:
        """Set copyright string.
        """
        self._copyright = copyright_str

    def generate_all(self, tfp: TextIO) -> None:
        """Generate a QEMU device template file based on the register map.

           :param tfp: output file stream
        """
        for gen in 'header register mask regname struct io device'.split():
            getattr(self, f'generate_{gen}')(tfp)

    def generate_header(self, tfp: TextIO) -> None:
        """Generate the template for the header of a device driver.

           :param tfp: output file stream
        """
        self._generate_header(tfp)

    def generate_struct(self, tfp: TextIO) -> None:
        """Generate the template for the device state and class stucts.

           :param tfp: output file stream
        """
        self._generate_struct(tfp)

    def generate_register(self, tfp: TextIO) -> None:
        """Generate the register map.

           :param tfp: output file stream
        """
        max_addr = max(reg.address for reg in self._registers)
        nibcount = (max_addr.bit_length() + 3) // 4
        print('/* clang-format off */', file=tfp)
        for reg in self._registers:
            self._generate_register(reg, nibcount, tfp)
        print('/* clang-format on */\n', file=tfp)

    def generate_param(self, tfp: TextIO) -> None:
        """Generate the parameter constants.

           :param tfp: output file stream
        """
        self._generate_parameters(tfp)

    def generate_reset(self, tfp: TextIO) -> None:
        """Generate one or more reset functions, based on the selected reset
           list defined at instantiation time.

           :param tfp: output file stream
        """
        nibcount = self._regwidth // 4
        for reg in self._registers:
            self._generate_reg_reset(reg, nibcount, tfp)

    def generate_mask(self, tfp: TextIO) -> None:
        """Generate the write mask associated with the register map.

           :param tfp: output file stream
        """
        for reg in self._registers:
            self._generate_mask(reg, tfp)

    def generate_io(self, tfp: TextIO) -> None:
        """Generate the template for the MMIO read and write functions.

           :param tfp: output file stream
        """
        self._generate_io(self._registers, False, tfp)
        self._generate_io(self._registers, True, tfp)

    def generate_regname(self, tfp: TextIO) -> None:
        """Generate the array of register names.

           :param tfp: output file stream
        """
        self._generate_reg_wrappers(self._registers, tfp)
        self._generate_regname_array(self._registers, tfp)

    def generate_device(self, tfp: TextIO) -> None:
        """Generate the template for the device initializationfunctions.

           :param tfp: output file stream
        """
        self._generate_mr_ops(tfp)
        self._generate_props(tfp)
        for reset_type in self._resets:
            self._generate_reset(tfp, reset_type)
        self._generate_realize(tfp)
        self._generate_init(tfp)
        self._generate_class_init(tfp)
        self._generate_type(tfp)

    def _parse_interrupts(self, hjson:dict[str, Any], address: int,
                          addr_inc: int) -> list[QEMURegister]:
        fields: list[QEMUField] = []
        if hjson.get('no_auto_intr_regs', False):
            self._log.info('Comportable interrupt generation disabled')
            return []
        for fpos, item in enumerate(hjson.get('interrupt_list', [])):
            access = 'ro' if item.get('type') == 'status' else 'rw1c'
            fieldargs = [item['name'].upper(), item['desc'], fpos, 1,
                         QEMUAccess(access), 0]
            fields.append(QEMUField(*fieldargs))
        if not fields:
            return []
        registers: list[QEMURegister] = []
        access = self._build_reg_access(fields)
        regargs = ['INTR_STATE', 'Interrupt state', address, access, fields, 0,
                   True]
        registers.append(QEMURegister(*regargs))
        address += addr_inc
        ro_acc = QEMUAccess('ro')
        regargs = ['INTR_ENABLE', 'Interrupt enable', address, ro_acc, [], 0]
        registers.append(QEMURegister(*regargs))
        address += addr_inc
        wo_acc = QEMUAccess('wo')
        regargs = ['INTR_TEST', 'Interrupt test', address, wo_acc, [], 0]
        registers.append(QEMURegister(*regargs))
        return registers

    def _parse_alerts(self, hjson:dict[str, Any], address: int) \
            -> list[QEMURegister]:
        fields: list[QEMUField] = []
        if hjson.get('no_auto_alert_regs', False):
            self._log.info('Comportable alert generation disabled')
            return []
        wo_acc = QEMUAccess('wo')
        for fpos, item in enumerate(hjson.get('alert_list', [])):
            fieldargs = [item['name'].upper(), item['desc'], fpos, 1, wo_acc, 0]
            fields.append(QEMUField(*fieldargs))
        if not fields:
            return []
        regargs = ['ALERT_TEST', 'Alert test', address, wo_acc, fields, 0]
        return [QEMURegister(*regargs)]

    def _parse_register(self, address: int, reg: dict[str, Any]) \
            -> Optional[QEMURegister]:
        name = reg.get('name')
        if not name:
            return None
        if self._ignore_prefix and name.startswith(self._ignore_prefix):
            return None
        desc = reg.get('desc')
        self._log.info('Register: %s', name)
        fields = self._parse_reg_fields(reg)
        fields.sort(key=lambda f: f.offset)
        reset = self._build_reg_reset(fields)
        access = self._build_reg_access(fields)
        rname = camel_to_snake_case(name).upper()
        regargs = [rname, desc, address, access, fields, reset]
        return QEMURegister(*regargs)

    def _parse_registers(self, address: int, reg: dict[str, Any]) \
            -> list[QEMURegister]:
        count = safe_eval(reg['count'], self._parameters)
        freg = self._parse_register(address, reg)
        regs = []
        self._log.info('Generating multireg %s[%d]', freg.name, count)
        for rpos in range(count):
            fields = freg.fields if rpos == 0 else []
            regs.append(
                freg._replace(name=f'{freg.name}_{rpos}', address=address,
                              shared_fields=(rpos == 0), fields=fields))
            address += 4
        return regs

    def _parse_reg_fields(self, reg: dict[str, Any]) -> list[QEMUField]:
        fields: list[QEMUField] = []
        bitfield = 0
        reg_swaccess = reg.get('swaccess')
        rname = reg['name']
        nibcount = self._regwidth // 4
        for field in reg.get('fields', []):
            name = field.get('name')
            if not name:
                name = rname
                if fields:
                    raise ValueError(f'Multiple anonymous bitfields '
                                     f'for reg {name}')
            if self._ignore_prefix and name.startswith(self._ignore_prefix):
                continue
            desc = field.get('desc')
            bits = field.get('bits')
            if ':' in bits:
                hibit, lobit = (self._parse_bits(x) for x in bits.split(':'))
                offset = lobit
                width = hibit-lobit+1
            else:
                offset = self._parse_bits(bits)
                width = 1
            bitmask = ((1 << width) - 1) << offset
            if bitmask & bitfield:
                raise ValueError(f'Bitfield {name} overlap: '
                                 f'0x{bitfield:0{nibcount}x}, mask '
                                 f'0x{bitmask:0{nibcount}x}')
            bitfield |= bitmask
            try:
                access = getattr(QEMUAccess,
                    field.get('swaccess', reg_swaccess).upper())
            except KeyError as exc:
                raise RuntimeError(f'Cannot find swaccess for {name}') from exc
            resval = field.get('resval')
            reset = HexInt.parse(resval, accept_int=True) \
                if resval else HexInt(0)
            fname = camel_to_snake_case(name).upper()
            fieldargs = [fname, desc, offset, width, access, reset]
            fields.append(QEMUField(*fieldargs))
        bitmask = (1 << self._regwidth) - 1
        if bitfield &~ bitmask:
            raise ValueError(f'Bitfield {name} out of range: '
                             f'0x{bitfield:0{nibcount}x}, mask '
                             f'0x{bitmask:0{nibcount}x}')
        return fields

    def _parse_bits(self, value: str) -> int:
        try:
            # simple integer
            return int(value)
        except ValueError:
            eval_value = safe_eval(value, self._parameters)
            self._log.info("parametric value '%s' evaluated as '%s'",
                           value, eval_value)
            return eval_value

    def _parse_parameters(self, params: list[dict[str, Any]]) \
            -> dict[str, Union[int, bool]]:
        parameters: dict[str, Union[int, bool]] = {}
        converters = {
            'int': int,
            'int unsigned': int,
            'bit': bool
        }
        for param in params:
            name = param.get('name')
            type_ = param.get('type')
            value = param.get('default')
            if not all((name, type_, value is not None)):
                continue
            conv = converters.get(type_)
            if not conv:
                self._log.warning("Type '%s' for param '%s' is not supported",
                                  type_, name)
                continue
            parameters[name] = conv(value)
        return parameters

    def _build_reg_reset(self, fields):
        reset = 0
        for field in fields:
            reset |= field.reset << field.offset
        return reset

    def _build_reg_access(self, fields):
        access = set(f.access for f in fields)
        if len(access) == 1:
            return access.pop()
        return QEMUAccess.UNDEF

    def _generate_register(self, reg: QEMURegister, nibcount: int,
                           tfp: TextIO) -> None:
        print(f'REG{self._regwidth}({reg.name}, 0x{reg.address:0{nibcount}x}u)',
              file=tfp)
        if not reg.shared_fields:
            if len(reg.fields) > 1:
                for field in reg.fields:
                    print(f'    FIELD({reg.name}, {field.name}, '
                          f'{field.offset}u, '
                          f'{field.width}u)', file=tfp)
            elif reg.fields:
                field = reg.fields[0]
                if reg.name != field.name:
                    fdname = field.name
                else:
                    fdname = field.name.rsplit('_', 1)[-1]
                print(f'    FIELD({reg.name}, {fdname}, {field.offset}u, '
                      f'{field.width}u)', file=tfp)
        else:
            name = '_'.join(reg.name.rsplit('_', 1)[:-1])
            if len(reg.fields) > 1:
                for field in reg.fields:
                    print(f'    SHARED_FIELD({name}_{field.name}, '
                          f'{field.offset}u, '
                          f'{field.width}u)', file=tfp)
            elif reg.fields:
                field = reg.fields[0]
                if name != field.name:
                    shname = f'{name}_{field.name}'
                else:
                    shname = name
                print(f'    SHARED_FIELD({shname}, '
                        f'{field.offset}u, '
                        f'{field.width}u)', file=tfp)

    def _generate_parameters(self, tfp: TextIO) -> None:
        cat_params: dict[str, dict[str, int]] = {}
        max_length = 0
        name_first = ('NUM', )
        for name, value in self._parameters.items():
            ucname = camel_to_snake_uppercase(name)
            parts = ucname.split('_')
            pre = parts[0]
            suf = parts[-1]
            if pre in name_first:
                cat = pre
            else:
                cat = suf
            if cat not in cat_params:
                cat_params[cat] = {}
            cat_params[cat][ucname] = value
            max_length = max(max_length, len(ucname))
        for cat in name_first:
            if cat in cat_params:
                cat_params[cat] = dict(sorted(cat_params[cat].items()))
        cat_fmt = {
            'OFFSET': 'x'
        }
        for cat, fmt in cat_fmt.items():
            if not fmt.endswith('x'):
                continue
            if cat not in cat_params:
                continue
            maxval = max(cat_params[cat].values())
            nibble = (maxval.bit_length() + 3) // 4
            cat_params[cat] = {n: f'0x{v:0{nibble}{fmt}}u'
                               for n, v in cat_params[cat].items()}
        for cat in sorted(cat_params):
            params = cat_params[cat]
            for name, value in params.items():
                imod = 'u' if isinstance(value, int) and value >= 0 else ''
                print(f'#define {name:<{max_length}s} {value}{imod}',
                      file=tfp)
            print(file=tfp)

    def _generate_reg_reset(self, reg: QEMURegister, nibcount: int,
                            tfp: TextIO) -> None:
        if reg.reset:
            print(f'    s->regs[R_{reg.name}] = 0x{reg.reset:0{nibcount}x}u;',
                  file=tfp)

    def _generate_mask(self, reg: QEMURegister, tfp: TextIO) -> None:
        if len(reg.fields) < 2:
            return
        bitmask = sum(((1 << bf.width) - 1) << bf.offset for bf in reg.fields)
        bitfield = (1 << self._regwidth) - 1
        if bitfield & ~bitmask == 0:
            return
        if not reg.shared_fields:
            masks: list[str] = [
                f'R_{reg.name}_{field.name}_MASK' for field in reg.fields
                if field.access not in (QEMUAccess.RO,)
            ]
        else:
            name = '_'.join(reg.name.rsplit('_', 1)[:-1])
            masks: list[str] = [
                f'{name}_{field.name}_MASK' for field in reg.fields
                if field.access not in (QEMUAccess.RO,)
            ]
        if not masks:
            return
        maskstr = ' | \\\n     '.join(masks)
        if not reg.shared_fields:
            name = reg.name
        else:
            name = '_'.join(reg.name.rsplit('_', 1)[:-1])
        print(f'#define {name}_WMASK \\\n    ({maskstr})\n', file=tfp)

    def _generate_io(self, regs: list[QEMURegister], write: bool,
                    tfp: TextIO) -> None:
        nregs = {r.name: r for r in regs}
        reg_names = set(nregs)
        reg_defs = {r.name: (sum(((1 << bf.width) - 1) << bf.offset
                                 for bf in r.fields), r.access) for r in regs}
        if write:
            noaccess_types = (QEMUAccess.RO, QEMUAccess.URO)
        else:
            noaccess_types = (QEMUAccess.WO, )
        noaccess_names = {
            r.name for r in regs if r.access in noaccess_types
        }
        rc_names = {
            r.name for r in regs if r.access == QEMUAccess.RC
        }
        reg_names -= noaccess_names
        if not write and rc_names:
            reg_names -= rc_names
        rwidth = self._regwidth
        dname = self._devname
        hdname = dname.title().replace('_', '')
        vnm = f'val{rwidth}'
        if write:
            if rwidth < 64:
                vcast = f'uint{rwidth}_t val{rwidth} = (uint{rwidth}_t)val64;'
            else:
                vcast = ''
            code = f'''
            static void {dname}_regs_write(void *opaque, hwaddr addr,
                                           uint64_t val64, unsigned size)
            {{
                {hdname}State *s = opaque;
                (void)size;
                {vcast}
                hwaddr reg = R{rwidth}_OFF(addr);

                uint32_t pc = ibex_get_current_pc();
                trace_{dname}_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg),
                                       val{rwidth}, pc);
            '''
        else:
            if rwidth < 64:
                vdef = f'uint{rwidth}_t val{rwidth};'
            else:
                vdef = ''
            code = f'''
            static uint64_t {dname}_regs_read(void *opaque, hwaddr addr,
                                              unsigned size)
            {{
                {hdname}State *s = opaque;
                (void)size;
                {vdef}

                hwaddr reg = R{rwidth}_OFF(addr);
            '''
        print(self.redent(code), file=tfp)
        lines = []
        lines.append('    switch (reg) {')
        bitfield = (1 << rwidth) - 1
        if not write:
            for rname in sorted(reg_names, key=lambda r: nregs[r].address):
                lines.append(f'case R_{rname}:')
            lines.append(f'    {vnm} = s->regs[reg];')
            lines.append('    break;')
            for rname in sorted(rc_names, key=lambda r: nregs[r].address):
                lines.append(f'case R_{rname}:')
            lines.append(f'    {vnm} = s->regs[reg];')
            lines.append('    s->regs[reg] = 0;')
            lines.append('    break;')
        else:
            for rname in sorted(reg_names, key=lambda r: nregs[r].address):
                lines.append(f'case R_{rname}:')
                rbm, racc = reg_defs[rname]
                if bitfield & rbm:
                    lines.append(f'    {vnm} &= R_{rname}_WMASK;')
                if racc in (QEMUAccess.RW1C, QEMUAccess.R0W1C):
                    lines.append(f'    s->regs[reg] &= ~{vnm}; '
                                 f'/* {racc.upper()} */')
                elif racc == QEMUAccess.RW0C:
                    lines.append(f'    s->regs[reg] &= {vnm}; /* RW0C */')
                elif racc == QEMUAccess.RW1S:
                    lines.append(f'    s->regs[reg] |= {vnm}; /* RW1S */')
                else:
                    if racc == QEMUAccess.UNDEF:
                        self._log.warning(
                            "Register '%s' has multiple access types which is "
                            "not supported by this script, defaulting to RW.",
                            rname)
                        lines.append('    /* @todo handle multiple access type '
                                     'bitfield */')
                    lines.append(f'    s->regs[reg] = {vnm};')
                lines.append('    break;')
        if noaccess_names:
            for rname in sorted(noaccess_names, key=lambda r: nregs[r].address):
                lines.append(f'case R_{rname}:')
            a = 'R' if write else 'W'
            lines.append(self.redent(f'''
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: {a}/O register 0x%02" HWADDR_PRIx " (%s)\\n",
                          __func__, s->ot_id, addr, REG_NAME(reg));
            ''', 4, True))
            lines.append('    break;')
        lines.append('default:')
        lines.append('    break;')
        lines.append('}')
        print('\n    '.join(lines), file=tfp)
        if write:
            code = '};\n'
        else:
            rval = '(uint64_t)val{rwidth}' if rwidth < 64 else 'val64'
            code = f'''

                uint32_t pc = ibex_get_current_pc();
                trace_{dname}_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg),
                                        val{rwidth}, pc);

                return {rval};
            }}
            '''
        print(self.redent(code), '', file=tfp)

    def _generate_reg_wrappers(self, regs: list[QEMURegister],
                               tfp: TextIO) -> None:
        last = regs[-1].name
        lines = []
        lines.append(f'#define R_LAST_REG (R_{last})')
        lines.append('#define REGS_COUNT (R_LAST_REG + 1u)')
        lines.append(f'#define REGS_SIZE  (REGS_COUNT * '
                     f'sizeof(uint{self._regwidth}_t))')
        lines.append('#define REG_NAME(_reg_) \\')
        lines.append('    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? '
                     'REG_NAMES[_reg_] : "?")')
        lines.append('')
        print('\n'.join(lines), file=tfp)

    def _generate_regname_array(self, regs: list[QEMURegister],
                                tfp: TextIO) -> None:
        lines = []
        lines.append(
            '#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)')
        lines.append('static const char *REG_NAMES[REGS_COUNT] = {')
        lines.append('    /* clang-format off */')
        for reg in regs:
            lines.append(f'    REG_NAME_ENTRY({reg.name}),')
        lines.append('    /* clang-format on */')
        lines.append('};')
        lines.append('')
        print('\n'.join(lines), file=tfp)

    def _generate_header(self, tfp: TextIO) -> None:
        dname = self._devname
        pname = dname.title().replace('_', ' ')
        year = localtime().tm_year
        code = f'''
        /*
         * QEMU {pname} device
         *
         * Copyright (c) {year} {self.copyright}
         *
         * Author(s):
         *
         * Permission is hereby granted, free of charge, to any person obtaining a copy
         * of this software and associated documentation files (the "Software"), to deal
         * in the Software without restriction, including without limitation the rights
         * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
         * copies of the Software, and to permit persons to whom the Software is
         * furnished to do so, subject to the following conditions:
         *
         * The above copyright notice and this permission notice shall be included in
         * all copies or substantial portions of the Software.
         *
         * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
         * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
         * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
         * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
         * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
         * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
         * THE SOFTWARE.
         */

        #include "qemu/osdep.h"
        #include "qemu/log.h"
        #include "hw/qdev-properties.h"
        #include "hw/registerfields.h"
        #include "hw/sysbus.h"
        #include "trace.h"

        '''
        print(self.redent(code), file=tfp)

    def _generate_struct(self, tfp: TextIO) -> None:
        dname = self._devname
        hdname = dname.title().replace('_', '')
        code = f'''
        struct {hdname}State {{
            SysBusDevice parent_obj;

            MemoryRegion mmio;

            char *ot_id;
        }};

        struct {hdname}Class {{
            SysBusDeviceClass parent_class;
            ResettablePhases parent_phases;
        }};

        '''
        print(self.redent(code), file=tfp)

    def _generate_mr_ops(self, tfp: TextIO) -> None:
        dname = self._devname
        code = f'''
        static const MemoryRegionOps {dname}_ops = {{
            .read = &{dname}_io_read,
            .write = &{dname}_io_write,
            .endianness = DEVICE_LITTLE_ENDIAN,
            .impl = {{
                .min_access_size = 4u,
                .max_access_size = 4u,
            }},
        }};
        '''
        print(self.redent(code), file=tfp)

    def _generate_props(self, tfp: TextIO) -> None:
        dname = self._devname
        hdname = dname.title().replace('_', '')
        code = f'''
        static Property {dname}_properties[] = {{
            DEFINE_PROP_STRING(OT_COMMON_DEV_ID, {hdname}State, ot_id),
            DEFINE_PROP_END_OF_LIST(),
        }};
        '''
        print(self.redent(code), file=tfp)

    def _generate_reset(self, tfp: TextIO, reset_type: str) -> None:
        dname = self._devname
        hdname = dname.title().replace('_', '')
        uname = dname.upper()

        regio = StringIO()
        nibcount = self._regwidth // 4
        for reg in self._registers:
            self._generate_reg_reset(reg, nibcount, regio)
        regcode = self.redent(regio.getvalue(), 12, strip_end=True).lstrip()

        code = f'''
        static void {dname}_reset_{reset_type}(Object *obj, ResetType type)
        {{
            {hdname}Class *c = {uname}_GET_CLASS(obj);
            {hdname}State *s = {uname}(obj);

            if (c->parent_phases.{reset_type}) {{
                c->parent_phases.{reset_type}(obj, type);
            }}
        '''
        if reset_type == "enter":
            code += '''
            memset(s->regs, 0, REG_SIZE);

            '''
            spacer = ' ' * 8
            code += f'{regcode}\n{spacer}}}\n'
        else:
            code += '}\n'
        print(self.redent(code), file=tfp)

    def _generate_realize(self, tfp: TextIO) -> None:
        dname = self._devname
        hdname = dname.title().replace('_', '')
        uname = dname.upper()
        code = f'''
        static void {dname}_realize(DeviceState *dev, Error **errp)
        {{
            {hdname}State *s = {uname}(dev);
            (void)errp;
        }}
        '''
        print(self.redent(code), file=tfp)

    def _generate_init(self, tfp: TextIO) -> None:
        dname = self._devname
        hdname = dname.title().replace('_', '')
        uname = dname.upper()
        code = f'''
        static void {dname}_init(Object *obj)
        {{
            {hdname}State *s = {uname}(obj);

            memory_region_init_io(&s->mmio, obj, &{hdname}_regs_ops, s,
                                TYPE_{uname}, REGS_SIZE);
            sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

            s->regs = g_new0(uint{self._regwidth}_t, REGS_COUNT);
        }}
        '''
        print(self.redent(code), file=tfp)

    def _generate_class_init(self, tfp: TextIO) -> None:
        dname = self._devname
        hdname = dname.title().replace('_', '')
        uname = dname.upper()
        resets = ', '.join((f'&{dname}_reset_{r}' if r in self._resets
                            else 'NULL' for r in self.RESETS))
        code = f'''
        static void {dname}_class_init(ObjectClass *klass, void *data)
        {{
            DeviceClass *dc = DEVICE_CLASS(klass);
            (void)data;

            dc->realize = &{dname}_realize;
            device_class_set_props(dc, {dname}_properties);
            set_bit(DEVICE_CATEGORY_MISC, dc->categories);

            ResettableClass *rc = RESETTABLE_CLASS(klass);
            {hdname}Class *mc = {uname}_CLASS(klass);
            resettable_class_set_parent_phases(rc, {resets},
                                               &mc->parent_phases);
        }}
        '''
        print(self.redent(code), file=tfp)

    def _generate_type(self, tfp: TextIO) -> None:
        dname = self._devname
        hdname = dname.title().replace('_', '')
        uname = dname.upper()
        code = f'''
        static const TypeInfo {dname}_info = {{
            .name = TYPE_{uname},
            .parent = TYPE_SYS_BUS_DEVICE,
            .instance_size = sizeof({hdname}State),
            .instance_init = &{dname}_init,
            .class_size = sizeof({hdname}Class),
            .class_init = &{dname}_class_init,
        }};

        static void {dname}_register_types(void)
        {{
            type_register_static(&{dname}_info);
        }}

        type_init({dname}_register_types);
        '''
        print(self.redent(code), file=tfp)


def main():
    """Main routine"""
    debug = True
    desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
    argparser = ArgumentParser(description=f'{desc}.')
    try:

        files = argparser.add_argument_group(title='Files')
        files.add_argument('-c', '--config', type=FileType('rt'),
                           metavar='HJSON', required=True,
                           help='input HJSON definition file')
        files.add_argument('-o', '--output', type=FileType('wt'),
                           metavar='FILE',
                           help='output C file')
        params = argparser.add_argument_group(title='Parameters')
        files.add_argument('-C', '--copyright',
                           help='define copyright string')
        params.add_argument('-g', '--generate', action='append',
                           choices=QEMUAutoReg.generators, required=True,
                           help='what to generate')
        params.add_argument('-n', '--name', default='foo',
                           help='device name')
        params.add_argument('-p', '--ignore', metavar='PREFIX',
                           help='ignore register/fields starting with prefix')
        params.add_argument('-r', '--reset', action='append',
                           choices=QEMUAutoReg.RESETS,
                           help='generate reset code')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')

        args = argparser.parse_args()
        debug = args.debug

        if _HJSON_ERROR:
            argparser.error(f'Missing HJSON module: {_HJSON_ERROR}')

        configure_loggers(args.verbose, 'autoreg')

        areg = QEMUAutoReg(args.name, args.ignore, args.reset)
        if args.copyright:
            areg.copyright = args.copyright
        areg.load(args.config)
        for gen in args.generate or []:
            getattr(areg, f'generate_{gen}')(args.output or sys.stdout)

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
