#!/usr/bin/env python3

# Copyright (c) 2025 Rivos, Inc.
# Copyright (c) 2025 lowRISC contributors.
# SPDX-License-Identifier: Apache2

"""Generate device definitions for OpenTitan device.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType
from enum import StrEnum, auto
from io import StringIO
from logging import getLogger
from os.path import basename, dirname, join as joinpath, normpath, splitext
from time import localtime
from traceback import format_exception
from typing import Any, NamedTuple, Optional, TextIO, Union

import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

# ruff: noqa: E402
from ot.util.eval import safe_eval
from ot.util.log import configure_loggers
from ot.util import mbb
from ot.util.misc import (HexInt, camel_to_snake_case, camel_to_snake_uppercase,
                          classproperty, flatten, redent, retrieve_git_version,
                          to_bool)
from ot.util.arg import ArgError

try:
    _HJSON_ERROR = None
    from hjson import load as hjload
except ImportError as hjson_exc:
    _HJSON_ERROR = str(hjson_exc)


class AutoAccess(StrEnum):
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


class AutoField(NamedTuple):
    """Register field."""

    name: str
    desc: Optional[str]
    offset: int
    width: int
    access: AutoAccess
    reset: int = 0

    def __str__(self) -> str:
        """Return a compact representation of a field."""
        return (f'{self.__class__.__name__}({self.offset}:{self.name}:'
                f'{self.width} {self.access})')


class AutoRegister(NamedTuple):
    """Device register."""

    name: str
    desc: Optional[str]
    address: int
    access: AutoAccess
    fields: list[AutoField] = []
    reset: Optional[int] = None
    count: int = 1
    shared_fields: bool = False


class AutoReg:
    """Register definition generator.

       :param devname: the name of the generated QEMU device
       :param ignore_prefix: an optional prefix to discard useless register
                             definitions. Registers and fields starting with
                             this prefix are ignored.
       :param resets: a list of reset functions to generate, if any.
    """

    RESETS = ['enter', 'hold', 'exit']

    WINDOW_LIMIT = 64
    """Default window item count limit."""

    def __init__(self, devname: str, ignore_prefix: Optional[str] = None,
                 resets: Optional[list[str]] = None):
        self._log = getLogger('autoreg')
        self._devname = devname
        self._ignore_prefix = ignore_prefix
        self._git_version = 'unknown'
        self._resets = resets or []
        self._parameters: dict[str, Union[int, bool]] = {}
        self._regwidth = 0  # bits
        self._registers: list[AutoRegister] = []
        self._copyright = '@todo Add copyright attributions'
        self._reg_groups: dict[str, set[str]] = {}  # group, reg names
        self._group_offsets: dict[str, int] = {}
        self._irq_count = 0
        self._alert_count = 0
        self._nibcount = 0

    @classproperty
    def generators(cls):
        """Provide a list of supported generators."""
        # pylint: disable=no-self-argument
        actions: list[str] = []
        prefix = 'generate_'
        for item in dir(cls):
            if not item.startswith(prefix):
                continue
            actions.append(item.removeprefix(prefix))
        actions.sort()
        return actions

    def load(self, hfp: TextIO, win_lim: Optional[int] = None) -> None:
        """Load a register map definition from an OT HJSON file."""
        if hfp.name:
            git_version = retrieve_git_version(hfp.name)
            if git_version:
                self._git_version = git_version
                self._log.info('Git info for %s: %s', basename(hfp.name),
                               self._git_version)
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
        registers: list[AutoRegister] = []
        registers.extend(self._parse_interrupts(hjson, address, addr_inc))
        address = len(registers) * addr_inc
        registers.extend(self._parse_alerts(hjson, address))
        address = len(registers) * addr_inc
        regnames: set[str] = set(r.name for r in registers)
        regdefs = hjson['registers']
        if bus_if:
            self._log.info("Using registers from bus '%s'", bus_if)
            regdefs = regdefs[bus_if]
        for item in regdefs:
            if 'skipto' in item:
                address = HexInt.parse(item['skipto'])
                continue
            if 'multireg' in item:
                item = item['multireg']
                # compact field can either be a string, a boolean, an integer
                # and may be present or not
                force_compact = item.get('compact')
                reg = self._parse_register(address, item)
                count = safe_eval(item['count'], self._parameters)
                if force_compact is not None:
                    compact = to_bool(force_compact)
                else:
                    compact = len(reg.fields) == 1
                if compact:
                    # single register with multiple fields with the same prefix
                    if len(reg.fields) > 1:
                        # this case is not yet supported
                        raise NotImplementedError(f'Too many compact fields for'
                                                  f' {reg.name}')
                    field = reg.fields[0]
                    fwidth = field.offset + field.width
                    fcount = min(count, self._regwidth // fwidth)
                    if fcount > 1:
                        reg = reg._replace(fields=[
                            field._replace(name=f'{field.name}_{pos}',
                                           offset=pos)
                            for pos in range(fcount)
                        ])
                    else:
                        reg = reg._replace(fields=[])
                    count = count // fcount
                if count > 1:
                    # multiple registers with the same name prefix
                    reg = reg._replace(count=count)
                    regnames.add((f'{reg.name}_{ix}' for ix in range(count)))
                registers.append(reg)
                address += addr_inc * count
                continue
            if 'window' in item:
                item = item['window']
                reg = self._parse_register(address, item)
                count = safe_eval(item['items'], self._parameters)
                if count > (win_lim or self.WINDOW_LIMIT):
                    self._log.warning(
                        'Ignoring oversized window %s with %u items',
                        reg.name, count)
                    continue
                fields = reg.fields
                if not reg.fields:
                    validbits = item.get('validbits')
                    if validbits:
                        field = AutoField("data", desc=None, offset=0,
                                          width=int(validbits),
                                          access=reg.access)
                        fields = [field]
                reg = reg._replace(count=count, fields=fields)
                regnames.add((f'{reg.name}_{ix}' for ix in range(count)))
                registers.append(reg)
                address += addr_inc * count
                continue
            reg = self._parse_register(address, item)

            if reg:
                if reg.name in regnames:
                    prefix = reg.name.split('_')[0]
                    if prefix in ('INTR', 'ALERT'):
                        # Comportable registers may already be defined, discard
                        continue
                    raise ValueError(f'Register {reg.name} redefined')
                regnames.add(reg.name)
                registers.append(reg)
                address += addr_inc
        registers.sort(key=lambda r: r.address)
        self._registers = registers
        self._reg_groups[''] = {reg.name for reg in registers}
        self._group_offsets[''] = 0
        max_addr = max(reg.address for reg in registers)
        self._nibcount = (max_addr.bit_length() + 3) // 4

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

    def segment(self, segdescs: list[str]) -> None:
        """Create register segmentation."""
        renum_names: dict[str, str] = {}
        for segdesc in segdescs:
            try:
                regname, segment = segdesc.split(':')
            except ValueError as exc:
                raise ArgError(f'Invalid segment declaration: '
                               f'{segdesc}') from exc
            regname = regname.upper()
            if regname in renum_names:
                raise ArgError(f'Multiple segmentation on {regname}')
            if segment in renum_names.values():
                raise ArgError(f'Multiple definition of {segment}')
            renum_names[regname] = segment.upper()
        reg_names = {reg.name for reg in self._registers}
        missing = set(renum_names) - reg_names
        if missing:
            raise ArgError(f'Unknown registers: {", ".join(missing)}')
        upregs: list[AutoRegister] = []
        addr_inc = self._regwidth // 8
        address = 0
        group = self._reg_groups.popitem()[0]
        del self._group_offsets[group]
        for reg in self._registers:
            new_group = renum_names.get(reg.name)
            if new_group:
                self._log.info('Reset address base on %s', reg.name)
                group = new_group
                address = 0
            if group not in self._reg_groups:
                self._reg_groups[group] = []
                self._group_offsets[group] = reg.address
            self._reg_groups[group].append(reg.name)
            upregs.append(reg._replace(address=address))
            address += addr_inc * reg.count
        self._registers = upregs

    def generate_register(self, tfp: TextIO) -> None:
        """Generate the register map.

           :param tfp: output file stream
        """
        self._generate_registers(tfp)

    def _parse_interrupts(self, hjson: dict[str, Any], address: int,
                          addr_inc: int) -> list[AutoRegister]:
        fields: list[AutoField] = []
        if hjson.get('no_auto_intr_regs', False):
            self._log.info('Comportable interrupt generation disabled')
            return []
        for fpos, item in enumerate(hjson.get('interrupt_list', [])):
            access = 'ro' if item.get('type') == 'status' else 'rw1c'
            fieldargs = [item['name'].upper(), item['desc'], fpos, 1,
                         AutoAccess(access), 0]
            fields.append(AutoField(*fieldargs))
        self._irq_count = len(fields)
        if not fields:
            return []
        registers: list[AutoRegister] = []
        # pylint: disable=use-dict-literal
        regargs = dict(
            name='INTR_STATE',
            desc='Interrupt state',
            address=address,
            access=self._build_reg_access(fields),
            fields=fields,
            reset=0,
            count=1,
            shared_fields=True
        )
        registers.append(AutoRegister(**regargs))
        address += addr_inc
        regargs = dict(
            name='INTR_ENABLE',
            desc='Interrupt enable',
            address=address,
            access=AutoAccess('rw'),
        )
        registers.append(AutoRegister(**regargs))
        address += addr_inc
        regargs = dict(
            name='INTR_TEST',
            desc='Interrupt test',
            address=address,
            access=AutoAccess('wo'),
        )
        registers.append(AutoRegister(**regargs))
        return registers

    def _parse_alerts(self, hjson: dict[str, Any], address: int) \
            -> list[AutoRegister]:
        fields: list[AutoField] = []
        if hjson.get('no_auto_alert_regs', False):
            self._log.info('Comportable alert generation disabled')
            return []
        wo_acc = AutoAccess('wo')
        for fpos, item in enumerate(hjson.get('alert_list', [])):
            fieldargs = [item['name'].upper(), item['desc'], fpos, 1, wo_acc, 0]
            fields.append(AutoField(*fieldargs))
        self._alert_count = len(fields)
        if not fields:
            return []
        # pylint: disable=use-dict-literal
        regargs = dict(
            name='ALERT_TEST',
            desc='Alert test',
            address=address,
            access=wo_acc,
            fields=fields,
            reset=0
        )
        return [AutoRegister(**regargs)]

    def _parse_register(self, address: int, reg: dict[str, Any]) \
            -> Optional[AutoRegister]:
        name = reg.get('name')
        if not name:
            return None
        if self._ignore_prefix and name.startswith(self._ignore_prefix):
            return None
        desc = reg.get('desc')
        self._log.debug('Register: %s', name)
        fields = self._parse_reg_fields(reg)
        fields.sort(key=lambda f: f.offset)
        reset = self._build_reg_reset(fields)
        access = self._build_reg_access(fields)
        rname = camel_to_snake_case(name).upper()
        regargs = [rname, desc, address, access, fields, reset]
        return AutoRegister(*regargs)

    def _parse_reg_fields(self, reg: dict[str, Any]) -> list[AutoField]:
        fields: list[AutoField] = []
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
            if isinstance(bits, str) and ':' in bits:
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
                access = getattr(AutoAccess,
                                 field.get('swaccess', reg_swaccess).upper())
            except KeyError as exc:
                raise RuntimeError(f'Cannot find swaccess for {name}') from exc
            resval = field.get('resval')
            mubi = field.get('mubi', False)
            if isinstance(resval, bool):
                if mubi:
                    mubi_symbol = f'MB{width}_{str(resval).upper()}'
                    reset = getattr(mbb, mubi_symbol, None)
                    if reset is None:
                        raise ValueError(f'Unknown value for {mubi_symbol}')
                else:
                    reset = int(resval)
            else:
                reset = HexInt.parse(resval, accept_int=True) \
                        if resval else HexInt(0)
            fname = camel_to_snake_case(name).upper()
            fieldargs = [fname, desc, offset, width, access, reset]
            fields.append(AutoField(*fieldargs))
        bitmask = (1 << self._regwidth) - 1
        if bitfield & ~bitmask:
            raise ValueError(f'Bitfield {name} out of range: '
                             f'0x{bitfield:0{nibcount}x}, mask '
                             f'0x{bitmask:0{nibcount}x}')
        return fields

    def _parse_bits(self, value: Union[int, str]) -> int:
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
            'int': HexInt.parse,
            'int unsigned': HexInt.parse,
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
        return AutoAccess.UNDEF

    def _generate_registers(self, tfp: TextIO) -> None:
        raise NotImplementedError("Abstract base class")


class QEMUAutoReg(AutoReg):
    """Generator for QEMU skeleton files."""

    def generate_all(self, tfp: TextIO) -> None:
        """Generate a QEMU device skeleton file based on the register map.

           :param tfp: output file stream
        """
        self.generate_header(tfp)
        self.generate_param(tfp)
        self.generate_register(tfp)
        self.generate_mask(tfp)
        self.generate_regname(tfp)
        self.generate_struct(tfp)
        self.generate_io(tfp)
        self.generate_device(tfp)

    def generate_header(self, tfp: TextIO) -> None:
        """Generate the skeleton for the header of a device driver.

           :param tfp: output file stream
        """
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
         *
         * Based on OpenTitan {self._git_version}
         */

        #include "qemu/osdep.h"
        #include "qemu/log.h"
        #include "hw/qdev-properties.h"
        #include "hw/registerfields.h"
        #include "hw/sysbus.h"
        #include "trace.h"

        '''  # noqa: E501
        print(redent(code), file=tfp)

    def generate_struct(self, tfp: TextIO) -> None:
        """Generate the skeleton for the device state and class structs.

           :param tfp: output file stream
        """
        dname = self._devname
        hdname = dname.title().replace('_', '')
        header = f'''
        struct {hdname}State {{
            SysBusDevice parent_obj;

            MemoryRegion mmio;
        '''
        extras = []
        indent = header.rsplit('\n', 1)[-1]
        if len(self._reg_groups) > 1:
            for group in self._reg_groups:
                extras.append(f'    MemoryRegion mmio_{group.lower()};'
                              f'\n{indent}')
        if self._irq_count or self._alert_count:
            extras.append(f'\n{indent}')
        if self._irq_count:
            extras.append(f'    IbexIRQ irqs[NUM_IRQS];\n{indent}')
        if self._alert_count:
            extras.append(f'    IbexIRQ alerts[NUM_ALERTS];\n{indent}')
        extras.append(f'\n{indent}')
        if len(self._reg_groups) == 1:
            extras.append(f'    uint{self._regwidth}_t *regs;\n{indent}')
        else:
            for group in self._reg_groups:
                sregs = f'regs_{group.lower()}'
                extras.append(f'    uint{self._regwidth}_t *{sregs};\n{indent}')
        extra = ''.join(extras)
        footer = f'''
            char *ot_id;
        }};

        struct {hdname}Class {{
            SysBusDeviceClass parent_class;
            ResettablePhases parent_phases;
        }};

        '''
        code = f'{header}{extra}{footer}'
        print(redent(code), file=tfp)

    def generate_param(self, tfp: TextIO) -> None:
        """Generate the parameter constants.

           :param tfp: output file stream
        """
        cat_params: dict[str, dict[str, int]] = {}
        max_length = 0
        name_first = ('NUM', )
        parameters = dict(self._parameters)
        if self._irq_count:
            parameters['NUM_IRQS'] = self._irq_count
        if self._alert_count:
            parameters['NUM_ALERTS'] = self._alert_count
        for name, value in parameters.items():
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
                if isinstance(value, bool):
                    # no use case for this kind of parameter
                    continue
                imod = 'u' if isinstance(value, int) and value >= 0 else ''
                print(f'#define {name:<{max_length}s} {value}{imod}', file=tfp)
        print(file=tfp)

    def generate_reset(self, tfp: TextIO) -> None:
        """Generate reset initialization values.

           :param tfp: output file stream
        """
        for group, regs in self._build_reg_groups():
            for reg in regs:
                self._generate_reg_reset(group, reg, tfp)

    def generate_mask(self, tfp: TextIO) -> None:
        """Generate the write mask associated with the register map.

           :param tfp: output file stream
        """
        for _, regs in self._build_reg_groups():
            self._generate_mask(regs, tfp)
        print(file=tfp)

    def generate_io(self, tfp: TextIO) -> None:
        """Generate the skeleton for the MMIO read and write functions.

           :param tfp: output file stream
        """
        groups = self._build_reg_groups()
        for group, regs in groups:
            self._generate_io(group, regs, False, tfp)
        for group, regs in groups:
            self._generate_io(group, regs, True, tfp)

    def generate_regname(self, tfp: TextIO) -> None:
        """Generate the array of register names.

           :param tfp: output file stream
        """
        print(f'#define R{self._regwidth}_OFF(_r_) ((_r_) / '
              f'sizeof(uint{self._regwidth}_t))\n', file=tfp)
        groups = self._build_reg_groups()
        for group, regs in groups:
            self._generate_reg_wrappers(group, regs, tfp)
        print('#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)',
              file=tfp)
        for pos, (group, regs) in enumerate(groups):
            self._generate_regname_array(group, regs, tfp)
            if pos < len(groups) - 1:
                print('', file=tfp)
        print('#undef REG_NAME_ENTRY\n', file=tfp)

    def generate_device(self, tfp: TextIO) -> None:
        """Generate the skeleton for the device initialization functions.

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

    @classmethod
    def tweak_field_name(cls, regname: str, fieldname: str) -> str:
        """Attempt to simplify field names to avoid useless duplication
           of the register name in the field name.
        """
        last = regname.split('_')[-1]
        if regname != fieldname:
            if last != fieldname:
                return fieldname
        if last in ('ENABLE', 'REGWEN'):
            return 'EN'
        return 'VAL'

    def _build_reg_groups(self) -> list[tuple[str, list[AutoRegister]]]:
        """Build list of register grouped by segment groups

           :return: a list of (group, ordered(AutoRegister))
        """
        return [(group, [reg for reg in self._registers
                         if reg.name in regnames])
                for group, regnames in self._reg_groups.items()]

    def _generate_registers(self, tfp: TextIO) -> None:
        print('/* clang-format off */', file=tfp)
        for reg in self._registers:
            if reg.count == 1:
                self._generate_register(reg, tfp)
                continue
            address = reg.address
            addr_inc = self._regwidth // 8
            for rpos in range(reg.count):
                rreg = reg._replace(name=f'{reg.name}_{rpos}',
                                    address=address, count=1,
                                    fields=reg.fields if rpos == 0 else [],
                                    shared_fields=rpos == 0)
                self._generate_register(rreg, tfp)
                address += addr_inc
        print('/* clang-format on */\n', file=tfp)

    def _generate_register(self, reg: AutoRegister, tfp: TextIO) -> None:
        print(f'REG{self._regwidth}({reg.name}, '
              f'0x{reg.address:0{self._nibcount}x}u)',
              file=tfp)
        if not reg.shared_fields:
            if len(reg.fields) > 1:
                for field in reg.fields:
                    fdname = self.tweak_field_name(reg.name, field.name)
                    print(f'    FIELD({reg.name}, {fdname}, '
                          f'{field.offset}u, '
                          f'{field.width}u)', file=tfp)
            elif reg.fields:
                field = reg.fields[0]
                if field.width != self._regwidth:
                    # do not emit a field if the field covers the entire reg
                    fdname = self.tweak_field_name(reg.name, field.name)
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
                if field.width != self._regwidth:
                    # do not emit a field if the field covers the entire reg
                    if name != field.name:
                        shname = f'{name}_{field.name}'
                    else:
                        shname = name
                    print(f'    SHARED_FIELD({shname}, '
                          f'{field.offset}u, '
                          f'{field.width}u)', file=tfp)

    def _generate_reg_reset(self, group: str, reg: AutoRegister,
                            tfp: TextIO) -> None:
        if reg.reset:
            regs = f'regs_{group.lower()}' if group else 'regs'
            print(f'    s->{regs}[R_{reg.name}] = 0x{reg.reset:x}u;',
                  file=tfp)

    def _generate_mask(self, regs: list[AutoRegister], tfp: TextIO) -> None:
        for reg in regs:
            if len(reg.fields) < 2:
                continue
            bitmask = sum(((1 << bf.width) - 1) << bf.offset
                          for bf in reg.fields)
            bitfield = (1 << self._regwidth) - 1
            if bitfield & ~bitmask == 0:
                continue
            if not reg.shared_fields:
                masks: list[str] = [
                    f'{reg.name}_{field.name}_MASK' for field in reg.fields
                    if field.access not in (AutoAccess.RO,)
                ]
            else:
                name = '_'.join(reg.name.rsplit('_', 1)[:-1]) or reg.name
                masks: list[str] = [
                    f'{name}_{field.name}_MASK' for field in reg.fields
                    if field.access not in (AutoAccess.RO,)
                ]
            if not masks:
                continue
            maskstr = ' | \\\n     '.join(masks)
            if not reg.shared_fields:
                name = reg.name
            else:
                name = '_'.join(reg.name.rsplit('_', 1)[:-1]) or reg.name
            print(f'#define {name}_WMASK \\\n    ({maskstr})', file=tfp)
            # special cast for interrupt testing with mixed fields
            if (reg.name == 'INTR_STATE' and reg.shared_fields and
               reg.access == AutoAccess.UNDEF):
                masks: list[str] = [
                    f'{name}_{field.name}_MASK' for field in reg.fields
                ]
                maskstr = ' | \\\n     '.join(masks)
                print(f'#define INTR_TEST_WMASK \\\n    ({maskstr})',
                      file=tfp)

    def _generate_io(self, group: str, regs: list[AutoRegister], write: bool,
                     tfp: TextIO) -> None:
        unfold_regs: list[AutoRegister] = []
        for reg in regs:
            if reg.count > 1:
                addr_inc = self._regwidth // 8
                for rpos in range(reg.count):
                    unfold_regs.append(
                        reg._replace(name=f'{reg.name}_{rpos}',
                                     address=reg.address + addr_inc * rpos,
                                     count=1))
            else:
                unfold_regs.append(reg)
        nregs = {r.name: r for r in unfold_regs}
        reg_names = set(nregs)
        reg_defs = {r.name: (sum(((1 << bf.width) - 1) << bf.offset
                                 for bf in r.fields), r.access)
                    for r in unfold_regs}
        if write:
            noaccess_types = (AutoAccess.RO, AutoAccess.URO)
        else:
            noaccess_types = (AutoAccess.WO, )
        noaccess_names = {
            r.name for r in unfold_regs if r.access in noaccess_types
        }
        rc_names = {
            r.name for r in unfold_regs if r.access == AutoAccess.RC
        }
        reg_names -= noaccess_names
        if not write and rc_names:
            reg_names -= rc_names
        rwidth = self._regwidth
        dname = self._devname
        if group:
            lgroup = group.lower()
            larg = f'"{lgroup}", '
            sregs = f's->regs_{lgroup}'
        else:
            sregs = 's->regs'
            larg = ''
        dpad = ' ' * (len(dname) - 7)
        hsname = self._devname.title().replace('_', '')
        rnm = '_'.join(filter(None, ('REG', group, 'NAME')))
        vnm = f'val{rwidth}'
        if write:
            if rwidth < 64:
                vcast = f'uint{rwidth}_t val{rwidth} = (uint{rwidth}_t)val64;'
            else:
                vcast = ''
            code = f'''
            static void {dname}_regs_write(void *opaque, hwaddr addr,
                                    {dpad}uint64_t val64, unsigned size)
            {{
                {hsname}State *s = opaque;
                (void)size;
                {vcast}
                hwaddr reg = R{rwidth}_OFF(addr);

                uint32_t pc = ibex_get_current_pc();
                trace_{dname}_io_write(s->ot_id, {larg}(uint32_t)addr,
                                       {dpad}{rnm}(reg), val{rwidth}, pc);
            '''
        else:
            if rwidth < 64:
                vdef = f'uint{rwidth}_t val{rwidth};'
            else:
                vdef = ''
            code = f'''
            static uint64_t {dname}_regs_read(void *opaque, hwaddr addr,
                                       {dpad}unsigned size)
            {{
                {hsname}State *s = opaque;
                (void)size;
                {vdef}

                hwaddr reg = R{rwidth}_OFF(addr);
            '''
        print(redent(code), file=tfp)
        lines = []
        lines.append('    switch (reg) {')
        bitfield = (1 << rwidth) - 1
        if not write:
            for rname in sorted(reg_names, key=lambda r: nregs[r].address):
                lines.append(f'case R_{rname}:')
            lines.append(f'    {vnm} = {sregs}[reg];')
            lines.append('    break;')
        else:
            for rname in sorted(reg_names, key=lambda r: nregs[r].address):
                lines.append(f'case R_{rname}:')
                rbm, racc = reg_defs[rname]
                if bitfield & rbm:
                    lines.append(f'    {vnm} &= R_{rname}_WMASK;')
                if racc in (AutoAccess.RW1C, AutoAccess.R0W1C):
                    lines.append(f'    {sregs}[reg] &= ~{vnm}; '
                                 f'/* {racc.upper()} */')
                elif racc == AutoAccess.RW0C:
                    lines.append(f'    {sregs}[reg] &= {vnm}; /* RW0C */')
                elif racc == AutoAccess.RW1S:
                    lines.append(f'    {sregs}[reg] |= {vnm}; /* RW1S */')
                else:
                    if racc == AutoAccess.UNDEF:
                        self._log.warning(
                            "Register '%s' has multiple access types which is "
                            "not supported by this script, defaulting to RW.",
                            rname)
                        lines.append('    /* @todo handle multiple access type '
                                     'bitfield */')
                        lines.append(f'    {sregs}[reg] = {vnm};')
                lines.append('    break;')
        if noaccess_names:
            for rname in sorted(noaccess_names, key=lambda r: nregs[r].address):
                lines.append(f'case R_{rname}:')
            a = 'R' if write else 'W'
            lines.append(redent(f'''
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: {a}/O register 0x%02" HWADDR_PRIx " (%s)\\n",
                          __func__, s->ot_id, addr, {rnm}(reg));
            ''', 4, True))
            if not write:
                lines.append('    val32 = 0;')
            lines.append('    break;')
        lines.append('default:')
        lines.append(redent('''
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%" HWADDR_PRIx "\\n",
                      __func__, s->ot_id, addr);
        ''', 4, True))  # noqa: E501
        if not write:
            lines.append('    val32 = 0;')
        lines.append('    break;')
        lines.append('}')
        print('\n    '.join(lines), file=tfp)
        if write:
            code = '};\n'
        else:
            rval = f'(uint64_t)val{rwidth}' if rwidth < 64 else 'val64'
            code = f'''

                uint32_t pc = ibex_get_current_pc();
                trace_{dname}_io_read_out(s->ot_id, {larg}(uint32_t)addr,
                                          {dpad}{rnm}(reg), val{rwidth}, pc);

                return {rval};
            }}
            '''
        print(redent(code), '', file=tfp)

    def _generate_reg_wrappers(self, group: Optional[str],
                               regs: list[AutoRegister], tfp: TextIO) -> None:
        last = regs[-1]
        sep = f'_{group}_' if group else '_'
        lines = []
        if last.count > 1:
            lines.append(f'#define R_LAST{sep}REG '
                         f'(R_{last.name}_{last.count-1})')
        else:
            lines.append(f'#define R_LAST{sep}REG (R_{last.name})')
        lines.append(f'#define REGS{sep}COUNT (R_LAST{sep}REG + 1u)')
        lines.append(f'#define REGS{sep}SIZE  (REGS{sep}COUNT * '
                     f'sizeof(uint{self._regwidth}_t))')
        if len(self._reg_groups) > 1:
            base = self._group_offsets[group]
            lines.append(f'#define REGS{sep}BASE 0x{base:0{self._nibcount}x}u')
        lines.append(f'#define REG{sep}NAME(_reg_) \\')
        lines.append(f'    ((((_reg_) < REGS{sep}COUNT) && '
                     f'REG{sep}NAMES[_reg_]) ? REG{sep}NAMES[_reg_] : "?")')
        lines.append('')
        print('\n'.join(lines), file=tfp)

    def _generate_regname_array(self, group: Optional[str],
                                regs: list[AutoRegister], tfp: TextIO) -> None:
        lines = []
        reg_names = '_'.join(filter(None, ('REG', group, 'NAMES')))
        reg_count = '_'.join(filter(None, ('REG', group, 'COUNT')))
        lines.append(f'static const char *{reg_names}[{reg_count}] = {{')
        lines.append('    /* clang-format off */')
        for reg in regs:
            if reg.count > 1:
                for rpos in range(reg.count):
                    lines.append(f'    REG_NAME_ENTRY({reg.name}_{rpos}),')
            else:
                lines.append(f'    REG_NAME_ENTRY({reg.name}),')
        lines.append('    /* clang-format on */')
        lines.append('};')
        print('\n'.join(lines), file=tfp)

    def _generate_mr_ops(self, tfp: TextIO) -> None:
        for group in self._reg_groups:
            if group:
                dname = f'{self._devname}_{group.lower()}'
            else:
                dname = self._devname
            code = f'''
            static const MemoryRegionOps {dname}_ops = {{
                .read = &{dname}_regs_read,
                .write = &{dname}_regs_write,
                .endianness = DEVICE_LITTLE_ENDIAN,
                .impl = {{
                    .min_access_size = 4u,
                    .max_access_size = 4u,
                }},
            }};
            '''
            print(redent(code), file=tfp)

    def _generate_props(self, tfp: TextIO) -> None:
        dname = self._devname
        hdname = dname.title().replace('_', '')
        code = f'''
        static Property {dname}_properties[] = {{
            DEFINE_PROP_STRING(OT_COMMON_DEV_ID, {hdname}State, ot_id),
            DEFINE_PROP_END_OF_LIST(),
        }};
        '''
        print(redent(code), file=tfp)

    def _generate_reset(self, tfp: TextIO, reset_type: str) -> None:
        dname = self._devname
        hdname = dname.title().replace('_', '')
        uname = dname.upper()
        regio = StringIO()
        groups = self._build_reg_groups()
        for group, regs in groups:
            for reg in regs:
                self._generate_reg_reset(group, reg, regio)
        regcode = redent(regio.getvalue(), 12, strip_end=True).lstrip()

        code = f'''
        static void {dname}_reset_{reset_type}(Object *obj, ResetType type)
        {{
            {hdname}Class *c = {uname}_GET_CLASS(obj);
            {hdname}State *s = {uname}(obj);

            if (c->parent_phases.{reset_type}) {{
                c->parent_phases.{reset_type}(obj, type);
            }}
        '''
        indent = code.rsplit('\n', 1)[-1]
        lines = [f'\n{indent}']
        if reset_type == "enter":
            for group in self._reg_groups:
                regname = f'regs_{group.lower()}' if group else 'regs'
                lines.append(f'    memset(s->{regname}, 0, '
                             f'{regname.upper()}_SIZE);\n{indent}')
        lines.append(f'\n{indent}    {regcode}\n{indent}')
        lines.append('}\n')
        code = f'{code}{"".join(lines)}'
        print(redent(code), file=tfp)

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
        print(redent(code), file=tfp)

    def _generate_init(self, tfp: TextIO) -> None:
        dname = self._devname
        hdname = dname.title().replace('_', '')
        uname = dname.upper()
        lines: list[str] = [
            f'static void {dname}_init(Object *obj)',
            '{'
        ]
        multi = len(self._reg_groups) > 1
        lines.append(f'{hdname}State *s = {uname}(obj);')
        lines.append('')
        if not multi:
            lines.append(
                f'memory_region_init_io(&s->mmio, obj, &{dname}_regs_ops, s,')
            lines.append(
                f'                       TYPE_{uname}, REGS_SIZE);')
        else:
            # need to map up to the very last register
            aperture = self._registers[-1].address + self._regwidth // 8
            # apertures are defined as power-of-2
            aperture = 1 << (aperture - 1).bit_length()
            lines.append(f'#define {uname}_APERTURE 0x{aperture:x}u')
            lines.append('')
            lines.append(
                f'memory_region_init(&s->mmio, obj, TYPE_{uname} "-regs",')
            lines.append(
                f'                   {uname}_APERTURE);')
        lines.append('sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);')
        lines.append('')
        if multi:
            for group in self._reg_groups:
                lgroup = group.lower()
                lines.append(
                    f'memory_region_init_io(&s->mmio_{lgroup}, obj, '
                    f'&{dname}_{lgroup}_ops, s,'
                )
                lines.append(
                    f'                      TYPE_{uname} "-regs-{lgroup}", '
                    f'REGS_{group}_SIZE);'
                )
                lines.append(
                    f'memory_region_add_subregion(&s->mmio, REGS_{group}_BASE, '
                    f'&s->mmio_{lgroup});'
                )
                lines.append('')
        for group in self._reg_groups:
            regname = f'regs_{group.lower()}' if group else 'regs'
            lines.append(f's->{regname} = g_new0(uint{self._regwidth}_t, '
                         f'{regname.upper()}_COUNT);')
        lines.append('')
        if self._irq_count:
            lines.append('for (unsigned ix = 0; ix < NUM_IRQS; ix++) {')
            lines.append('    ibex_sysbus_init_irq(obj, &s->irqs[ix]);')
            lines.append('}')
        if self._alert_count:
            lines.append('')
            lines.append('for (unsigned ix = 0; ix < NUM_ALERTS; ix++) {')
            lines.append('    ibex_qdev_init_irq(obj, &s->alerts[ix], '
                         'OT_DEVICE_ALERT);')
            lines.append('}')
        lines.append('}')
        last_line = len(lines)
        for lno, line in enumerate(lines, 1):
            if lno in (1, 2, last_line) or line.startswith('#'):
                print(line, file=tfp)
            else:
                print(f'    {line}'.rstrip(), file=tfp)
        print(file=tfp)

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
        print(redent(code), file=tfp)

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
        print(redent(code), file=tfp)


class BmTestAutoReg(AutoReg):
    """Generator for Baremetal Test skeleton files."""

    def generate_all(self, tfp: TextIO) -> None:
        """Generate a QEMU device skeleton file based on the register map.

           :param tfp: output file stream
        """
        self.generate_header(tfp)
        self.generate_struct(tfp)
        self.generate_register(tfp)

    def generate_header(self, tfp: TextIO) -> None:
        """Generate the skeleton for the header of a device driver.

           :param tfp: output file stream
        """
        year = localtime().tm_year
        code = f'''
        // Copyright (c) {year} {self.copyright}
        // Licensed under the Apache License, Version 2.0, see LICENSE for details.
        // SPDX-License-Identifier: Apache-2.0

        #![no_std]

        use tock_registers::interfaces::{{ReadWriteable, Readable, Writeable}};
        use tock_registers::registers::{{ReadOnly, ReadWrite, WriteOnly}};
        use tock_registers::{{register_bitfields, register_structs}};
        '''  # noqa: E501
        # @todo ideally RW/RO/WO use should depend on the actual register
        # definitions. This is not yet supported.
        print(redent(code), file=tfp)

    def generate_struct(self, tfp: TextIO) -> None:
        """Generate the skeleton for the device state and class structs.

           :param tfp: output file stream
        """
        dname = self._devname
        hdname = dname.title().replace('_', '')
        code = f'''
        pub struct {hdname} {{
            regs: &'static mut {hdname}Regs,
        }}
        '''
        print(redent(code), file=tfp)

    def _generate_registers(self, tfp: TextIO) -> None:
        dname = self._devname
        hdname = dname.title().replace('_', '')
        print('register_structs! {', file=tfp)
        print(f'    pub {hdname}Regs {{', file=tfp)
        shared_fields: dict[str, tuple[list[AutoField], set[str]]] = {}
        bitfields: dict[str, list[AutoField]] = {}
        for reg in self._registers:
            radix = reg.name.rsplit('_', 1)[0]
            if reg.shared_fields:
                if radix in shared_fields:
                    raise RuntimeError(f"Redefinition of shared field for "
                                       f"'{radix}'")
                shared_fields[radix] = (reg.fields, {reg.name})
                bitfields[radix] = reg.fields
            elif radix in shared_fields:
                shared_fields[radix][1].add(reg.name)
            else:
                flds = reg.fields
                if flds and (len(flds) > 1 or flds[0].width != self._regwidth):
                    bitfields[reg.name] = reg.fields
        reg = None
        address = 0
        rsvcnt = 0
        nibcount = self._nibcount
        for reg in self._registers:
            if reg.address > address:
                rsvcnt += 1
                self._generate_reserved_register(address, f'_reserved{rsvcnt}',
                                                 nibcount, tfp)
            self._generate_register(reg, shared_fields, nibcount, reg.count,
                                    tfp)
            address = reg.address + (reg.count * self._regwidth) // 8
        print(f'        (0x{address:0{nibcount}x} => @END),', file=tfp)
        print('    }\n}\n', file=tfp)
        print(f'register_bitfields! [u{self._regwidth},', file=tfp)
        for name, fields in bitfields.items():
            print(f'    {name} [', file=tfp)
            for fld in fields:
                print(f'        {fld.name} OFFSET({fld.offset}) '
                      f'NUMBITS({fld.width}) [],', file=tfp)
            print('    ],', file=tfp)
        print('];\n', file=tfp)

    def _generate_register(self, reg: AutoRegister,
                           shfields: dict[str, tuple[list[AutoField],
                                                     set[str]]],
                           nibcount: int, repcount: int, tfp: TextIO) -> None:
        regtype = {
            AutoAccess.RO: 'ReadOnly',
            AutoAccess.WO: 'WriteOnly',
        }.get(reg.access, 'ReadWrite')
        regdesc = ''
        radix = reg.name.rsplit('_', 1)[0]
        if radix in shfields and reg.name in shfields[radix][1]:
            fields = shfields[radix][0]
            regdescname = radix
        else:
            fields = reg.fields
            regdescname = reg.name
        if fields and (len(fields) > 1 or fields[0].width != self._regwidth):
            regdesc = f', {regdescname.upper()}::Register'
        if repcount > 1:
            print(f'        (0x{reg.address:0{nibcount}x} => '
                  f'{reg.name.lower()}: '
                  f'[{regtype}<u{self._regwidth}{regdesc}>; {repcount}]),',
                  file=tfp)
        else:
            print(f'        (0x{reg.address:0{nibcount}x} => '
                  f'{reg.name.lower()}: '
                  f'{regtype}<u{self._regwidth}{regdesc}>),', file=tfp)

    def _generate_reserved_register(self, address: int, name: str,
                                    nibcount: int, tfp: TextIO):
        print(f'        (0x{address:0{nibcount}x} => {name},', file=tfp)


def main():
    """Main routine"""
    debug = True
    mod = sys.modules[__name__]
    desc = mod.__doc__.split('.', 1)[0].strip()
    argparser = ArgumentParser(description=f'{desc}.')
    base = 'AutoReg'
    autoregs = {
        name.removesuffix(base).lower(): tit
        for name, tit in [(m, getattr(mod, m)) for m in dir(mod)]
        if isinstance(tit, type) and issubclass(tit, AutoReg) and name != base
    }
    default_outkind = 'qemu'
    outkinds = sorted(autoregs, key=lambda n: '' if n == default_outkind else n)
    assert len(outkinds) > 0
    generators = sorted(set(flatten([areg.generators
                                     for areg in autoregs.values()])))
    try:
        files = argparser.add_argument_group(title='Files')
        files.add_argument('-c', '--config', type=FileType('rt'),
                           metavar='HJSON', required=True,
                           help='input HJSON definition file')
        files.add_argument('-o', '--output', type=FileType('wt'),
                           metavar='FILE',
                           help='output source file')
        params = argparser.add_argument_group(title='Parameters')
        files.add_argument('-C', '--copyright',
                           help='define copyright string')
        params.add_argument('-g', '--generate', action='append',
                            choices=generators,
                            help='what to generate')
        params.add_argument('-k', '--out-kind', choices=outkinds,
                            default=outkinds[0],
                            help=f'output file format '
                                 f'(default: {outkinds[0]})')
        params.add_argument('-n', '--name', help='device name')
        params.add_argument('-p', '--ignore', metavar='PREFIX',
                            help='ignore register/fields starting with prefix')
        params.add_argument('-S', '--segment', metavar='REGNAME:SEGMENT',
                            action='append', default=[],
                            help='Creage a segment on specified register '
                                 '(may be repeated)')
        params.add_argument('-r', '--reset', action='append',
                            choices=AutoReg.RESETS,
                            help='generate reset code')
        params.add_argument('-w', '--win-limit', type=HexInt.parse,
                            metavar='ITEMS', default=AutoReg.WINDOW_LIMIT,
                            help=f'Max window size, discard larger '
                                 f'(default: {AutoReg.WINDOW_LIMIT} items)')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')

        args = argparser.parse_args()
        debug = args.debug

        if _HJSON_ERROR:
            argparser.error(f'Missing HJSON module: {_HJSON_ERROR}')

        log = configure_loggers(args.verbose, 'autoreg')[0]

        reggen = autoregs[args.out_kind]
        name = args.name or f'ot_{splitext(basename(args.config.name))[0]}'
        areg = reggen(name, args.ignore, args.reset)
        if args.copyright:
            areg.copyright = args.copyright
        areg.load(args.config, args.win_limit)
        if args.generate:
            areg.segment(args.segment)
            for gen in args.generate:
                generate = getattr(areg, f'generate_{gen}', None)
                if not generate:
                    argparser.error(f'{gen} is not supported for '
                                    f'{args.out_kind}')
                generate(args.output or sys.stdout)
        else:
            log.warning('No generation requested')

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
