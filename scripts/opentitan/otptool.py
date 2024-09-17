#!/usr/bin/env python3

# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""QEMU OT tool to manage OTP files.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType
from os.path import basename
from sys import argv, exit as sysexit, modules, stderr, stdout, version_info
from traceback import format_exc
from typing import Optional

from ot.util.log import configure_loggers
from ot.util.misc import HexInt
from ot.otp import (OtpImage, OtpLifecycleExtension, OtpMap, OTPPartitionDesc,
                    OTPRegisterDef)


# requirement: Python 3.7+: dict entries are kept in creation order
if version_info[:2] < (3, 7):
    raise RuntimeError('Unsupported Python version')


def main():
    """Main routine"""
    debug = True
    try:
        desc = modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc}.')
        files = argparser.add_argument_group(title='Files')
        files.add_argument('-j', '--otp-map', type=FileType('rt'),
                           metavar='HJSON',
                           help='input OTP controller memory map file')
        files.add_argument('-m', '--vmem', type=FileType('rt'),
                           help='input VMEM file')
        files.add_argument('-l', '--lifecycle', type=FileType('rt'),
                           metavar='SV',
                           help='input lifecycle system verilog file')
        files.add_argument('-o', '--output', metavar='C', type=FileType('wt'),
                           help='output C file')
        files.add_argument('-r', '--raw',
                           help='QEMU OTP raw image file')
        params = argparser.add_argument_group(title='Parameters')
        # pylint: disable=unsubscriptable-object
        params.add_argument('-k', '--kind',
                            choices=OtpImage.vmem_kinds,
                            help=f'kind of content in VMEM input file, '
                                 f'default: {OtpImage.vmem_kinds[0]}')
        params.add_argument('-e', '--ecc', type=int,
                            default=OtpImage.DEFAULT_ECC_BITS,
                            metavar='BITS', help='ECC bit count')
        params.add_argument('-C', '--config', type=FileType('rt'),
                            help='read Present constants from QEMU config file')
        params.add_argument('-c', '--constant', type=HexInt.parse,
                            metavar='INT',
                            help='finalization constant for Present scrambler')
        params.add_argument('-i', '--iv', type=HexInt.parse, metavar='INT',
                            help='initialization vector for Present scrambler')
        params.add_argument('-w', '--wide', action='count', default=0,
                            help='use wide output, non-abbreviated content')
        params.add_argument('-n', '--no-decode', action='store_true',
                            default=False,
                            help='do not attempt to decode OTP fields')
        commands = argparser.add_argument_group(title='Commands')
        commands.add_argument('-s', '--show', action='store_true',
                              help='show the OTP content')
        commands.add_argument('-E', '--ecc-recover', action='store_true',
                              help='attempt to recover errors with ECC')
        commands.add_argument('-D', '--digest', action='store_true',
                              help='check the OTP HW partition digest')
        commands.add_argument('-U', '--update', action='store_true',
                              help='update RAW file after ECC recovery or bit '
                                   'changes')
        commands.add_argument('--empty', metavar='PARTITION', action='append',
                              default=[],
                              help='reset the content of a whole partition, '
                                   'including its digest if any')
        commands.add_argument('--clear-bit', action='append', default=[],
                              help='clear a bit at specified location')
        commands.add_argument('--set-bit', action='append',  default=[],
                              help='set a bit at specified location')
        commands.add_argument('--toggle-bit', action='append',  default=[],
                              help='toggle a bit at specified location')
        generate = commands.add_mutually_exclusive_group()
        generate.add_argument('-L', '--generate-lc', action='store_true',
                              help='generate lc_ctrl C arrays')
        generate.add_argument('-P', '--generate-parts', action='store_true',
                              help='generate partition descriptor C arrays')
        generate.add_argument('-R', '--generate-regs', action='store_true',
                              help='generate partition register C definitions')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        configure_loggers(args.verbose, 'otptool', 'otp')

        otp = OtpImage(args.ecc)

        if not (args.vmem or args.raw):
            if any((args.show, args.digest, args.ecc_recover, args.clear_bit,
                    args.set_bit, args.toggle_bit)):
                argparser.error('At least one raw or vmem file is required')

        if not args.vmem and args.kind:
            argparser.error('VMEM kind only applies for VMEM input files')

        if args.update:
            if not args.raw:
                argparser.error('No RAW file specified for update')
            if args.vmem:
                argparser.error('RAW update mutuallly exclusive with VMEM')

        bit_actions = ('clear', 'set', 'toggle')
        alter_bits: list[list[tuple[int, int]]] = []
        for slot, bitact in enumerate(bit_actions):
            bitdefs = getattr(args, f'{bitact}_bit')
            alter_bits.append([])
            for bitdef in bitdefs:
                try:
                    offset, bit = (HexInt.parse(x) for x in bitdef.split('/'))
                except ValueError as exc:
                    argparser.error(f"Invalid bit specifier '{bitdef}', should "
                                    f"be <offset>/<bit_num> format: {exc}")
                alter_bits[slot].append((offset, bit))

        otpmap: Optional[OtpMap] = None
        lcext: Optional[OtpLifecycleExtension] = None
        partdesc: Optional[OTPPartitionDesc] = None

        if not args.otp_map:
            if args.generate_parts or args.generate_regs:
                argparser.error('Generator requires an OTP map')
            if args.show:
                argparser.error('Cannot decode OTP values without an OTP map')
            if args.digest:
                argparser.error('Cannot verify OTP digests without an OTP map')
            if args.empty:
                argparser.error('Cannot empty OTP partition without an OTP map')
        else:
            otpmap = OtpMap()
            otpmap.load(args.otp_map)

        if args.lifecycle:
            lcext = OtpLifecycleExtension()
            lcext.load(args.lifecycle)

        output = stdout if not args.output else args.output

        if args.generate_parts:
            partdesc = OTPPartitionDesc(otpmap)
            partdesc.save(basename(args.otp_map.name), basename(argv[0]),
                          output)

        if args.generate_regs:
            regdef = OTPRegisterDef(otpmap)
            regdef.save(basename(args.otp_map.name), basename(argv[0]), output)

        if args.generate_lc:
            if not lcext:
                argparser.error('Cannot generate LC array w/o a lifecycle file')
            lcext.save(output)

        if args.vmem:
            otp.load_vmem(args.vmem, args.kind)
            if otpmap:
                otp.dispatch(otpmap)
            otp.verify_ecc(args.ecc_recover)

        if args.raw:
            # if no VMEM is provided, select the RAW file as an input file
            # otherwise it is selected as an output file
            if not args.vmem:
                with open(args.raw, 'rb') as rfp:
                    otp.load_raw(rfp)
                if otpmap:
                    otp.dispatch(otpmap)
                otp.verify_ecc(args.ecc_recover)

        if otp.loaded:
            if not otp.is_opentitan:
                ot_opts = ('iv', 'constant', 'digest', 'generate_parts',
                           'generate_regs', 'generate_lc', 'otp_map',
                           'lifecycle')
                if any(getattr(args, a) for a in ot_opts):
                    argparser.error('Selected option only applies to OpenTitan '
                                    'images')
                if args.show:
                    argparser.error('Showing content of non-OpenTitan image is '
                                    'not supported')
            if args.empty:
                for part in args.empty:
                    otp.empty_partition(part)
            if args.config:
                otp.load_config(args.config)
            if args.iv:
                otp.set_digest_iv(args.iv)
            if args.constant:
                otp.set_digest_constant(args.constant)
            if lcext:
                otp.load_lifecycle(lcext)
            if args.show:
                otp.decode(not args.no_decode, args.wide, stdout)
            if args.digest:
                if not otp.has_present_constants:
                    if args.raw and otp.version == 1:
                        msg = '; OTP v1 image does not track them'
                    else:
                        msg = ''
                    # can either be defined on the CLI or in an existing QEWU
                    # image
                    argparser.error(f'Present scrambler constants are required '
                                    f'to verify the partition digest{msg}')
                otp.verify(True)
            if args.raw and (args.vmem or args.update):
                for pos, bitact in enumerate(bit_actions):
                    if alter_bits[pos]:
                        getattr(otp, f'{bitact}_bits')(alter_bits[pos])
                if not args.update and any(alter_bits):
                    otp.verify_ecc(False)
                # when both RAW and VMEM are selected, QEMU RAW image file
                # should be generated
                with open(args.raw, 'wb') as rfp:
                    otp.save_raw(rfp)

    except (IOError, ValueError, ImportError) as exc:
        print(f'\nError: {exc}', file=stderr)
        if debug:
            print(format_exc(chain=False), file=stderr)
        sysexit(1)
    except KeyboardInterrupt:
        sysexit(2)


if __name__ == '__main__':
    main()
