# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""QEMU test executer for OpenTitan unit test sequencer.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import Namespace
from collections import defaultdict
from os.path import isfile
from typing import Any, Optional

import re

from ot.util.file import guess_file_type
from ot.util.misc import EasyDict

from ..executer import Executer
from .wrapper import QEMUWrapper


class QEMUExecuter(Executer):
    """Test execution engine using a QEMU virtual machine
    """

    LOG_SHORTCUTS = {
        'A': 'in_asm',
        'E': 'exec',
        'G': 'guest_errors',
        'H': 'help',
        'I': 'int',
        'M': 'mmu',
        'R': 'cpu_reset',
        'U': 'unimp',
    }
    """Shortcut names for QEMU log sources."""

    WRAPPER = QEMUWrapper
    """QEMU wrapper."""

    def _build_fw_args(self, args: Namespace) \
            -> tuple[str, Optional[str], list[str], Optional[str]]:
        rom_exec = bool(args.rom_exec)
        roms = args.rom or []
        # rom can be specified as a string or a list of strings
        if isinstance(roms, str):
            roms = [roms]
        multi_rom = (len(roms) + int(rom_exec)) > 1
        # generate pre-application ROM option
        fw_args: list[str] = []
        machine = args.machine
        variant = args.variant
        chiplet_count = 1
        if variant:
            machine = f'{machine},variant={variant}'
            try:
                chiplet_count = sum(int(x)
                                    for x in re.split(r'[A-Za-z]', variant)
                                    if x)
            except ValueError:
                self._log.warning('Unknown variant syntax %s', variant)
        rom_counts: list[int] = [0]
        for chip_id in range(chiplet_count):
            rom_count = 0
            for rom in roms:
                if rom in ('-', '_'):
                    # special marker to disable a specific ROM
                    rom_count += 1
                    continue
                rom_path = self._tfm.interpolate(rom)
                if not isfile(rom_path):
                    raise ValueError(f'Unable to find ROM file {rom_path}')
                rom_ids = []
                if args.first_soc:
                    if chiplet_count == 1:
                        rom_ids.append(f'{args.first_soc}.')
                    else:
                        rom_ids.append(f'{args.first_soc}{chip_id}.')
                rom_ids.append('rom')
                if multi_rom:
                    rom_ids.append(f'{rom_count}')
                rom_id = ''.join(rom_ids)
                rom_opt = f'ot-rom_img,id={rom_id},file={rom_path}'
                fw_args.extend(('-object', rom_opt))
                rom_count += 1
            rom_counts.append(rom_count)
        rom_count = max(rom_counts)
        xtype = None
        if args.exec:
            exec_path = self._virtual_tests.get(args.exec)
            if not exec_path:
                exec_path = self.abspath(args.exec)
            xtype = guess_file_type(exec_path)
            if xtype == 'spiflash':
                fw_args.extend(('-drive',
                                f'if=mtd,id=spiflash,bus=0,format=raw,'
                                f'file={exec_path}'))
            elif xtype == 'bin':
                if args.embedded_flash is None:
                    raise ValueError(f'{xtype} test type not supported without '
                                     f'embedded-flash option')
            else:
                if xtype != 'elf':
                    raise ValueError(f'No support for test type: '
                                     f'{xtype.upper()}')
                if rom_exec:
                    # generate ROM option(s) for the application itself
                    for chip in range(chiplet_count):
                        rom_id_parts = []
                        if args.first_soc:
                            if chiplet_count == 1:
                                rom_id_parts.append(f'{args.first_soc}.')
                            else:
                                rom_id_parts.append(f'{args.first_soc}{chip}.')
                        rom_id_parts.append('rom')
                        if multi_rom:
                            rom_id_parts.append(f'{rom_count}')
                        rom_id = ''.join(rom_id_parts)
                        rom_opt = f'ot-rom_img,id={rom_id},file={exec_path}'
                        fw_args.extend(('-object', rom_opt))
                    rom_count += 1
                else:
                    if args.embedded_flash is None:
                        if not roms:
                            fw_args.extend(('-kernel', exec_path))
                        else:
                            fw_args.extend(('-device',
                                            f'loader,file={exec_path}'))
        else:
            exec_path = None
        return machine, xtype, fw_args, exec_path

    def _build_log_sources(self, args: Namespace) -> list[str]:
        if not args.log:
            return []
        log_args = []
        for arg in args.log:
            if arg.lower() == arg:
                log_args.append(arg)
                continue
            for upch in arg:
                try:
                    logname = self.LOG_SHORTCUTS[upch]
                except KeyError as exc:
                    raise ValueError(f"Unknown log name '{upch}'") from exc
                log_args.append(logname)
        return ['-d', ','.join(log_args)]

    def _build_vcp_args(self, args: Namespace) -> \
            tuple[list[str], dict[str, tuple[str, int]]]:
        device = args.device
        devdesc = device.split(':')
        host = devdesc[0]
        try:
            port = int(devdesc[1])
            if not 0 < port < 65536:
                raise ValueError(f'Invalid serial TCP port: {port}')
        except IndexError as exc:
            raise ValueError(f'TCP port not specified: {device}') from exc
        except TypeError as exc:
            raise ValueError(f'Invalid TCP serial device: {device}') from exc
        mux = f'mux={"on" if args.muxserial else "off"}'
        vcps = args.vcp or [self.DEFAULT_SERIAL_PORT]
        vcp_args = ['-display', 'none']
        vcp_map = {}
        for vix, vcp in enumerate(vcps):
            vcp_map[vcp] = (host, port+vix)
            vcp_args.extend(('-chardev',
                             f'socket,id={vcp},host={host},port={port+vix},'
                             f'{mux},server=on,wait=on'))
            if vcp == self.DEFAULT_SERIAL_PORT:
                vcp_args.extend(('-serial', 'chardev:serial0'))
        return vcp_args, vcp_map

    def _build_command(self, args: Namespace,
                       opts: Optional[list[str]] = None) -> EasyDict[str, Any]:
        """Build QEMU command line from argparser values.

           :param args: the parsed arguments
           :param opts: any QEMU-specific additional options
           :return: a dictionary defining how to execute the command
        """
        if args.qemu is None:
            raise ValueError('QEMU path is not defined')
        machine, xtype, fw_args, xexec = self._build_fw_args(args)
        qemu_args = [args.qemu, '-M', machine]
        for otcfg in args.otcfg or []:
            qemu_args.extend(('-readconfig', self.abspath(otcfg)))
        qemu_args.extend(fw_args)
        temp_files = defaultdict(set)
        if all((args.otp, args.otp_raw)):
            raise ValueError('OTP VMEM and RAW options are mutually exclusive')
        if args.otp:
            if not isfile(args.otp):
                raise ValueError(f'No such OTP file: {args.otp}')
            otp_file = self._tfm.create_otp_image(args.otp)
            temp_files['otp'].add(otp_file)
            qemu_args.extend(('-drive',
                              f'if=pflash,file={otp_file},format=raw'))
        elif args.otp_raw:
            otp_raw_path = self.abspath(args.otp_raw)
            qemu_args.extend(('-drive',
                              f'if=pflash,file={otp_raw_path},format=raw'))
        if args.flash:
            if xtype == 'spiflash':
                raise ValueError('Cannot use a flash file with a flash test')
            if not isfile(args.flash):
                raise ValueError(f'No such flash file: {args.flash}')
            if any((args.exec, args.boot)):
                raise ValueError('Flash file argument is mutually exclusive '
                                 'with bootloader or rom extension')
            flash_path = self.abspath(args.flash)
            if args.embedded_flash is None:
                raise ValueError('Embedded flash bus not defined')
            qemu_args.extend(('-drive', f'if=mtd,id=eflash,'
                                        f'bus={args.embedded_flash},'
                                        f'file={flash_path},format=raw'))
        elif any((xexec, args.boot)):
            if xexec and not isfile(xexec):
                raise ValueError(f'No such exec file: {xexec}')
            if args.boot and not isfile(args.boot):
                raise ValueError(f'No such bootloader file: {args.boot}')
            if args.embedded_flash is not None:
                no_flash_header = args.no_flash_header
                flash_file = self._tfm.create_eflash_image(xexec, args.boot,
                                                           no_flash_header)
                temp_files['flash'].add(flash_file)
                qemu_args.extend(('-drive', f'if=mtd,id=eflash,'
                                            f'bus={args.embedded_flash},'
                                            f'file={flash_file},format=raw'))
        if args.qemu_log:
            qemu_args.extend(('-D', self.abspath(args.qemu_log)))
        for trace in args.trace or []:
            qemu_args.append('-trace')
            if isfile(trace):
                qemu_args.append(f'events={self.abspath(trace)}')
            else:
                qemu_args.append(trace)
        qemu_args.extend(self._build_log_sources(args))
        if args.singlestep:
            qemu_args.extend(('-accel', 'tcg,one-insn-per-tb=on'))
        if 'icount' in args:
            if args.icount is not None:
                qemu_args.extend(('-icount', f'shift={args.icount}'))
        try:
            start_delay = float(getattr(args, 'start_delay') or
                                self.DEFAULT_START_DELAY)
        except ValueError as exc:
            raise ValueError(f'Invalid start up delay {args.start_delay}') \
                from exc
        start_delay *= args.timeout_factor
        trigger = getattr(args, 'trigger', '')
        validate = getattr(args, 'validate', '')
        if trigger and validate:
            raise ValueError(f"{getattr(args, 'exec', '?')}: 'trigger' and "
                             f"'validate' are mutually exclusive")
        asan = getattr(args, 'asan', False)
        logfile = getattr(args, 'log_file', None)
        vcp_args, vcp_map = self._build_vcp_args(args)
        qemu_args.extend(vcp_args)
        qemu_args.extend(args.global_opts or [])
        if opts:
            qemu_args.extend((str(o) for o in opts))
        return EasyDict(command=qemu_args, vcp_map=vcp_map,
                        tmpfiles=temp_files, start_delay=start_delay,
                        trigger=trigger, validate=validate, asan=asan,
                        logfile=logfile)
