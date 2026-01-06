#!/usr/bin/env python3

# Copyright (c) 2023-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""QEMU OT tool to manage OTP files.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import ArgumentParser, FileType
from binascii import unhexlify
from io import IOBase
from os.path import (basename, dirname, isfile, join as joinpath, normpath,
                     split as splitpath)
from re import match as re_match
from traceback import format_exception
from typing import Optional, Union
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

# ruff: noqa: E402
_EXC: Optional[Exception] = None
try:
    from ot.lc_ctrl.tools import LifeCycleTokenEngine, LifeCycleTokenPair
except ModuleNotFoundError as exc:
    _EXC = exc
from ot.otp import (OtpImage, OtpLifecycleExtension, OtpMap, OtpPartition,
                    OtpPartitionDesc, OtpRegisterDef)
from ot.top import OpenTitanTop
from ot.util.log import configure_loggers
from ot.util.misc import HexInt, to_bool


def parse_lc_token(tkdesc: str) -> tuple[str, 'LifeCycleTokenPair']:
    """Parse a Rust file for a LC token and return its name and VALUE

       :param tkdesc: argument parser input string
       :return: a 2-uple of OTP map token name and LifeCycleTokenPair
    """
    token_desc, token_val = tkdesc.split('=', 1)
    token_name = token_desc.upper()
    tkeng = LifeCycleTokenEngine()
    try:
        tktext = unhexlify(token_val)
        if len(tktext) != tkeng.TOKEN_LENGTH:
            raise ValueError()
    except ValueError as exc:
        raise ValueError(f"No valid token '{token_name}'") from exc
    return token_name, tkeng.build_from_text(tktext)


def get_top_name(*filepaths: list[Union[str, IOBase]]) -> Optional[str]:
    """Try to retrieve the top name from a list of configuration files.

       :param filepaths: list of file path or file objects
       :return: the name of the identified top, if found
    """
    for filepath in filepaths:
        if not isinstance(filepath, str):
            filepath = getattr(filepath, 'name', None)
            if not filepath:
                continue
        if not isfile(filepath):
            continue
        fdir = dirname(filepath)
        top_names = set(OpenTitanTop.names)
        while fdir:
            fdir, tail = splitpath(fdir)
            top_prefix = 'top_'
            if tail.startswith(top_prefix):
                candidate = tail.removeprefix(top_prefix)
                if candidate in top_names:
                    return candidate
        return None


def main():
    """Main routine"""
    debug = True
    genfmts = 'LCVAL LCTPL PARTS REGS'.split()
    outkinds = ('qemu', 'bmtest')
    try:
        desc = sys.modules[__name__].__doc__.split('.', 1)[0].strip()
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
        files.add_argument('-o', '--output', metavar='FILE',
                           type=FileType('wt'),
                           help='output filename (default to stdout)')
        files.add_argument('-r', '--raw',
                           help='QEMU OTP raw image file')
        files.add_argument('-x', '--export', metavar='VMEM',
                           help='Export data to a VMEM file')
        files.add_argument('-X', '--export-verbose', metavar='VMEM',
                           help='Export data to a VMEM file with field info')
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
        params.add_argument('-f', '--filter', action='append',
                            metavar='PART:FIELD',
                            help='filter which OTP fields are shown')
        params.add_argument('--force-absorb', action='store_true',
                            help='force absorption')
        params.add_argument('--no-version', action='store_true',
                            help='do not report the OTP image version')
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
        commands.add_argument('-g', '--generate', choices=genfmts,
                              help='generate code, see doc for options')
        commands.add_argument('-F', '--fix-ecc', action='store_true',
                              help='rebuild ECC')
        commands.add_argument('-G', '--fix-digest', action='append',
                              metavar='PART', default=[],
                              help='rebuild HW digest')
        commands.add_argument('--change', action='append',
                              metavar='PART:FIELD=VALUE', default=[],
                              help='change the content of an OTP field')
        commands.add_argument('--empty', metavar='PARTITION', action='append',
                              default=[],
                              help='reset the content of a whole partition, '
                                   'including its digest if any')
        commands.add_argument('--erase', action='append', metavar='PART:FIELD',
                              default=[],
                              help='clear out an OTP field')
        commands.add_argument('--clear-bit', action='append', default=[],
                              help='clear a bit at specified location')
        commands.add_argument('--set-bit', action='append',  default=[],
                              help='set a bit at specified location')
        commands.add_argument('--toggle-bit', action='append',  default=[],
                              help='toggle a bit at specified location')
        commands.add_argument('--write', action='append',
                              metavar='ADDR/HEXBYTES', default=[],
                              help='write bytes at specified location')
        commands.add_argument('--patch-token', action='append',
                              metavar='NAME=VALUE', default=[],
                              help='change a LC hashed token, using Rust file')
        commands.add_argument('--out-kind', choices=outkinds,
                              default=outkinds[0],
                              help=f'select output format for code generation'
                                   f' (default: {outkinds[0]})')
        commands.add_argument('--top-name',
                              help='optional top name for code generation '
                                   '(default: auto)')
        extra = argparser.add_argument_group(title='Extras')
        extra.add_argument('-v', '--verbose', action='count',
                           help='increase verbosity')
        extra.add_argument('-d', '--debug', action='store_true',
                           help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        log = configure_loggers(args.verbose, 'otptool', 'otp')[0]

        otp = OtpImage(args.ecc)

        check_update = False

        if not (args.vmem or args.raw):
            if any((args.show, args.digest, args.ecc_recover, args.clear_bit,
                    args.set_bit, args.toggle_bit, args.write, args.change,
                    args.erase, args.fix_digest)):
                argparser.error('At least one raw or vmem file is required')

        if not args.vmem and args.kind:
            argparser.error('VMEM kind only applies for VMEM input files')

        if args.update:
            if not args.raw:
                argparser.error('No RAW file specified for update')
            if args.vmem:
                argparser.error('RAW update mutually exclusive with VMEM')

        if args.filter and not args.show:
            argparser.error('Filter only apply to the show command')

        bit_actions = ('clear', 'set', 'toggle')
        alter_bits: list[list[tuple[int, int]]] = []
        for slot, bitact in enumerate(bit_actions):
            bitdefs = getattr(args, f'{bitact}_bit')
            alter_bits.append([])
            for bitdef in bitdefs:
                try:
                    soff, sdata = bitdef.split('/', 1)
                    if soff[1] == 'h':
                        offset = HexInt.parse(soff.replace('h', 'x'))
                        offset *= 2
                    else:
                        offset = HexInt.parse(soff)
                    bit = HexInt.parse(sdata)
                except ValueError as exc:
                    argparser.error(f"Invalid bit specifier '{bitdef}', should "
                                    f"be <offset>/<bit_num> format: {exc}")
                alter_bits[slot].append((offset, bit))

        otpmap: Optional[OtpMap] = None
        lcext: Optional[OtpLifecycleExtension] = None
        partdesc: Optional[OtpPartitionDesc] = None

        if not args.otp_map:
            if args.generate in ('PARTS', 'REGS'):
                argparser.error('Generator requires an OTP map')
            for feat in ('show', 'digest', 'empty', 'change', 'erase',
                         'fix_digest', 'patch_token', 'export_verbose'):
                if not getattr(args, feat):
                    continue
                argparser.error('Specified option requires an OTP map')
        else:
            otpmap = OtpMap()
            otpmap.load(args.otp_map, args.force_absorb)

        if args.lifecycle:
            lcext = OtpLifecycleExtension()
            lcext.load(args.lifecycle)
        elif args.generate in ('LCVAL', 'LCTPL'):
            argparser.error('Cannot generate LC array w/o a lifecycle file')

        output = sys.stdout if not args.output else args.output

        try:
            if not args.generate:
                pass
            elif args.generate == 'PARTS':
                partdesc = OtpPartitionDesc(otpmap)
                partdesc.save(args.out_kind, basename(args.otp_map.name),
                              basename(sys.argv[0]), output)
            elif args.generate == 'REGS':
                topname = args.top_name
                if not topname:
                    topname = get_top_name(args.otp_map)
                regdef = OtpRegisterDef(otpmap)
                regdef.save(args.out_kind, topname, basename(args.otp_map.name),
                            basename(sys.argv[0]), output)
            elif args.generate == 'LCVAL':
                lcext.save(args.out_kind, output, True)
            elif args.generate == 'LCTPL':
                lcext.save(args.out_kind, output, False)
        except NotImplementedError:
            argparser.error(f'Cannot generate {args.generate} in '
                            f'{args.out_kind}')

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
                ot_opts = ('iv', 'constant', 'digest', 'generate', 'otp_map',
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

            for field_desc in args.erase:
                try:
                    part, field = field_desc.split(':')
                    if not isinstance(part, str):
                        raise ValueError()
                except ValueError:
                    argparser.error('Invalid field specifier, should follow '
                                    '<PART>:<FIELD> syntax')
                otp.erase_field(part, field)
                check_update = True

            for chg_desc in args.change:
                try:
                    fdesc, value = chg_desc.split('=')
                except ValueError:
                    argparser.error('Invalid change specifier, should follow '
                                    '<PART>:<FIELD>=<VALUE> syntax')
                try:
                    part, field = fdesc.split(':')
                    if not isinstance(part, str):
                        raise ValueError()
                except ValueError:
                    argparser.error('Invalid field specifier, should follow '
                                    '<PART>:<FIELD> syntax')
                smo = re_match(r'^(?P<quote>[\"\'])(?P<value>.*)(?P=quote)$',
                               value)
                if smo:
                    # single- or double- quoted string value
                    value = smo.group('value')
                else:
                    if value.startswith('0x'):
                        value = HexInt.parse(value)
                    else:
                        try:
                            value = to_bool(value, permissive=False)
                        except ValueError:
                            pass
                    if isinstance(value, str):
                        try:
                            value = bytes(reversed(unhexlify(value)))
                        except ValueError:
                            argparser.error(f'Unknown value type: {value}')
                otp.change_field(part, field, value)
                check_update = True

            if args.config:
                otp.load_config(args.config)

            if args.iv:
                otp.set_digest_iv(args.iv)

            if args.constant:
                otp.set_digest_constant(args.constant)

            token_parts: set[OtpPartition] = set()

            if args.patch_token and _EXC:
                if debug:
                    print(''.join(format_exception(_EXC, chain=False)),
                          file=sys.stderr)
                argparser.error(f'Missing PYTHONPATH: {_EXC}')
            for tkdesc in args.patch_token:
                if '=' not in tkdesc:
                    argparser.error(f"Invalid token syntax: '{tkdesc}'")
                token, value = parse_lc_token(tkdesc)
                token_name = f'{token}_TOKEN'
                token_parts = otpmap.find_field(token_name)
                if len(token_parts) == 0:
                    argparser.error(f"No such token '{token}' in OTP map")
                if len(token_parts) > 1:
                    argparser.error(
                        f"Token '{token}' found in multiple partitions "
                        f"{', '.join(p.name for p in token_parts)}")
                part = token_parts[0]
                # no sure why we need to revert the bytes here
                # could be due to any other inversion in any other piece of SW
                # ...
                token_hash = bytes(reversed(value.hashed))
                otp.change_field(part.name, token_name, token_hash)
                if part.is_locked:
                    token_parts.add(part.name)
                check_update = True
            for part in token_parts:
                otp.build_digest(part.name, True)

            if args.digest or args.fix_digest:
                if not otp.has_present_constants:
                    if args.raw and otp.version == 1:
                        msg = '; OTP v1 image does not track them'
                    else:
                        msg = ''
                    # can either be defined on the CLI or in an existing QEWU
                    # image
                    argparser.error(f'Present scrambler constants are required '
                                    f'to handle the partition digest{msg}')
            build_digest_parts = {p for p in args.fix_digest if p != 'tokens'}
            if 'tokens' in args.fix_digest:
                build_digest_parts.update((p.name for p in token_parts))
            for part in build_digest_parts:
                otp.build_digest(part, True)
                check_update = True

            if lcext:
                otp.load_lifecycle(lcext)

            if args.show:
                otp.decode(not args.no_decode, args.wide, output,
                           not args.no_version, args.filter)

            if args.digest:
                otp.verify(True)

            for pos, bitact in enumerate(bit_actions):
                if alter_bits[pos]:
                    getattr(otp, f'{bitact}_bits')(alter_bits[pos])
                check_update = True

            for write in args.write:
                try:
                    saddr, sdata = write.split('/', 1)
                    if saddr[1] == 'h':
                        addr = HexInt.parse(saddr.replace('h', 'x'))
                        addr *= 2
                    else:
                        addr = HexInt.parse(saddr)
                    data = unhexlify(sdata)
                except ValueError as exc:
                    argparser.error(f'Invalid write syntax: {write} {exc}')
                otp.change_data(addr, data)

            if args.fix_ecc:
                otp.fix_ecc()
                check_update = True

            if args.raw and (args.vmem or args.update):
                if not args.update and any(alter_bits):
                    otp.verify_ecc(False)
                # when both RAW and VMEM are selected, QEMU RAW image file
                # should be generated
                with open(args.raw, 'wb') as rfp:
                    otp.save_raw(rfp)

            if args.raw and not args.vmem:
                if not args.update and check_update:
                    log.warning('OTP content modified, image file not updated')

            xp_name = args.export or args.export_verbose
            if xp_name:
                if args.export and args.export_verbose:
                    argparser.error('export options are mutually exclusive')
                with open(xp_name, 'wt') if xp_name != '-' else \
                        sys.stdout as xfp:
                    otp.save_vmem(xfp, bool(args.export_verbose))

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
