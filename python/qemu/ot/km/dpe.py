#!/usr/bin/env python3

# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""QEMU OT tool to generate Key Manager DPE keys.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from argparse import Namespace
from binascii import unhexlify
from configparser import RawConfigParser
from io import BufferedReader
from logging import getLogger
from os.path import dirname, join as joinpath, normpath
from textwrap import fill
from typing import BinaryIO, Optional, TextIO
import sys

QEMU_PYPATH = joinpath(dirname(dirname(dirname(normpath(__file__)))),
                       'python', 'qemu')
sys.path.append(QEMU_PYPATH)

from ..otp import OtpImage, OtpLifecycleExtension, OtpMap
from ..rom.image import ROMImage
from ..util.arg import ArgError

# ruff: noqa: E402
_CRYPTO_EXC: Optional[Exception] = None
try:
    from Crypto.Hash import KMAC256
except ModuleNotFoundError:
    try:
        # see pip3 install -r requirements.txt
        from Cryptodome.Hash import KMAC256
    except ModuleNotFoundError as exc:
        _CRYPTO_EXC = exc


class KeyManagerDpe:
    """Key Manager DPE.
    """

    SLOT_COUNT = 4
    ADVANCE_DATA_WIDTH = 208
    GENERATE_DATA_WIDTH = 100
    SW_BINDINGS_WIDTH = 32  # 256 bits
    SALT_WIDTH = 32  # 256 bits
    KMAC_LEN = 48  # 384 bits

    OUTPUTS = ('HW', 'SW')
    TARGETS = ('AES', 'KMAC', 'OTBN', 'NONE')

    def __init__(self, otp_img: OtpImage, rom_count: int):
        if _CRYPTO_EXC:
            raise _CRYPTO_EXC
        self._log = getLogger('keymgr')
        self._otp_img = otp_img
        self._roms = [ROMImage(x) for x in range(rom_count)]
        self._config = RawConfigParser()
        self._step: int = 0
        self._command: str = ''
        self._slots: list[bytes] = [b'' for _ in range(self.SLOT_COUNT)]
        self._lc_ctrl_km_divs: dict[str, bytes] = {}
        self._seeds: dict[str, bytes] = {}

    @classmethod
    def from_args(cls, args: Namespace) -> 'KeyManagerDpe':
        """Generate a key based on the provided arguments

           :param args: ArgumentPaser argument
           :return KeyManagerDpe: KeyManagerDpe instance
        """
        swbindings = []
        if not args.swbindings:
            raise ArgError('Missing SW bindings for advance')
        for sval in reversed(args.swbindings):
            if len(sval) & 1:
                sval = f'0{sval}'
            try:
                val = unhexlify(sval)
            except ValueError as exc:
                raise ArgError(f'Invalid SW binding: {sval}: {exc}') from exc
            if len(val) > cls.SW_BINDINGS_WIDTH:
                raise ArgError('Invalid SW bindings length')
            swbindings.append(val)
        # first step needs no bindings
        swbindings.append(None)

        ssalt = args.salt
        if len(ssalt) & 1:
            ssalt = f'0{ssalt}'
        try:
            salt = unhexlify(ssalt)
        except ValueError as exc:
            raise ArgError(f'Invalid SW binding: {ssalt}: {exc}') from exc

        salt_len = len(salt)
        if salt_len > cls.SALT_WIDTH:
            raise ArgError('Invalid salt length')

        if not 0<= args.key_version < 1<<32:
            raise ArgError('Invalid key version value')

        kmd = cls.create(args.otp_map, args.ecc, args.vmem, args.raw,
                         args.lifecycle, args.config, args.rom, args.rom_size)

        # if swbindings is not empty, use advance mode
        while swbindings:
            swb = swbindings.pop()
            kmd.advance(0, 0, swb)

        res = kmd.generate(0, args.target, args.gen_out, salt, args.key_version)

        if args.rust_const:
            const_name = args.rust_const.upper()
            res_array = ', '.join((f'0x{v:02x}' for v in res))
            res_array = fill(res_array, width=100,
                             initial_indent='    ', subsequent_indent='    ')
            outstr = (f'const {const_name}: [u8; {len(res)}] = '
                      f'[\n{res_array}\n];')
        else:
            outstr = kmd.hexstr(res)

        if args.output:
            mode = 'at' if args.rust_const else 'wt'
            with open(args.output, mode) as ofp:
                print(outstr, file=ofp)
        else:
            print(outstr)

        return kmd

    @classmethod
    def create(cls, otpmap: BinaryIO, ecc: int, vmem: Optional[BinaryIO],
               raw: Optional[BinaryIO], lifecycle: TextIO, config: TextIO,
               roms: list[ROMImage], rom_sizes: list[int]) -> 'KeyManagerDpe':
        """Create a KeyManagerDpe instance from dependencies."""
        otp_map = OtpMap()
        otp_map.load(otpmap)
        otpmap.close()

        otp_img = OtpImage(ecc)
        if vmem:
            otp_img.load_vmem(vmem, 'otp')
            vmem.close()
        elif raw:
            otp_img.load_raw(raw)
            raw.close()
        otp_img.dispatch(otp_map)

        lcext = OtpLifecycleExtension()
        lcext.load(lifecycle)
        otp_img.load_lifecycle(lcext)
        lifecycle.close()

        kmd = cls(otp_img, len(roms))

        kmd.load_config(config)
        config.close()

        if len(rom_sizes) < len(roms):
            rom_sizes.extend([None] * (len(roms) - len(rom_sizes)))
        for rom, (rfp, rom_size) in enumerate(zip(roms, rom_sizes)):
            kmd.load_rom(rom, rfp, rom_size)
            rfp.close()

        return kmd

    def load_config(self, config_file: TextIO) -> None:
        """Load QEMU 'readconfig' file.

           :param config_file: the config file text stream
        """
        self._config.read_file(config_file)
        loaded: set[str] = set()
        for section in self._config.sections():
            prefix = 'ot_device '
            if not section.startswith(prefix):
                continue
            devname = section.removeprefix(prefix).strip(' "')
            devdescs = devname.split('.')
            devtype = devdescs[0]
            if devtype == 'ot-lc_ctrl' and devtype not in loaded:
                loaded.add(devtype)
                for opt in 'invalid production test_unlocked dev rma'.split():
                    sval = self._config.get(section, opt)
                    if not sval:
                        raise ValueError(f'Unable to load {opt} LC KM div')
                    sval = sval.strip('"')
                    val = unhexlify(sval)
                    # TODO: check why we need to reverse the div
                    val = bytes(reversed(val))
                    self._lc_ctrl_km_divs[opt] = val
                continue
            if devtype == 'ot-keymgr_dpe' and devtype not in loaded:
                loaded.add(devtype)
                for opt in self._config.options(section):
                    sval = self._config.get(section, opt).strip('"')
                    seed_name = opt.replace('_seed', '')
                    val = unhexlify(sval)
                    # TODO: check why we need to reverse the seeds
                    val = bytes(reversed(val))
                    self._seeds[seed_name] = val
                continue
            if devtype == 'ot-rom_ctrl':
                devinst = devdescs[-1]
                devload = f'{devtype}.{devinst}'
                if devload in loaded:
                    continue
                loaded.add(devload)
                prefix = 'rom'
                if not devinst.startswith(prefix):
                    raise ValueError(f'Invalid ROM instance name: {devinst}')
                devidx = int(devinst.removeprefix(prefix))
                try:
                    rom_img = self._roms[devidx]
                except IndexError:
                    self._log.warning('No ROM image loaded for device %s',
                                      devinst)
                    continue
                for opt in self._config.options(section):
                    val = self._config.get(section, opt).strip('"')
                    setattr(rom_img, opt, unhexlify(val))
                continue

    def load_rom(self, rom_idx: int, rfp: BufferedReader,
                 size: Optional[int] = None) -> None:
        """Load QEMU 'readconfig' file.

           :param rom_idx: the ROM index
           :param rfp: the ROM data stream
           :param size: the size of the ROM image
        """
        rom_img = self._roms[rom_idx]
        rom_img.load(rfp, size)

    def initialize(self, dst: int):
        """Initialize the KeyManager DPE statemachine.

           :param dst: the destination slot index
        """
        return self.advance(0, dst)

    def advance(self, src: int, dst: int, swbindings: Optional[bytes] = None):
        """Advance the KeyManager DPE statemachine to the next step.

           :param src: the source slot index
           :param dst: the destination slot index
           :param swbindings: the software bindings for the current step
        """
        assert 0 <= src < self.SLOT_COUNT, f'Invalid source slot {src}'
        assert 0 <= dst < self.SLOT_COUNT, f'Invalid destination slot {dst}'
        try:
            advfn = getattr(self, f'_advance_{self._step}')
        except AttributeError:
            self._log.error('Unknown advance step %s', self._step)
            raise
        exp_swbindings = self._step not in (0,)
        if exp_swbindings:
            if swbindings is None:
                raise ValueError(f'Missing SW bindings for step {self._step}')
            swb_len = len(swbindings)
            if swb_len < self.SW_BINDINGS_WIDTH:
                swbindings = b''.join((swbindings,
                                       bytes(self.SW_BINDINGS_WIDTH - swb_len)))
        if not exp_swbindings and swbindings is not None:
            raise ValueError(f'Unexpected SW bindings for step {self._step}')
        self._log.debug('Advance #%d', self._step)
        self._slots[dst] = advfn(src, swbindings)
        if self._step < 3:
            self._step += 1
        return self._slots[dst]

    def generate(self, src: int, target: str, output: Optional[str],
                 salt: bytes, key_version: int) -> bytes:
        """Generate an output key.

           :param src: the source slot index
           :param target: the target device for the key.
           :param output: the type of output key to generate.
           :param salt: the salt to use for the key generation.
           :param key_version: the version of the key to generate.
           :return: the generated key.
        """
        assert 0 <= src < self.SLOT_COUNT, f'Invalid source slot {src}'
        if not output:
            output = 'SW' if target == 'NONE' else 'HW'
        outmap = {'SW': 'soft', 'HW': 'hard'}
        output_seed = self._seeds[f'{outmap[output]}_output']
        self._log.debug('Output Key Seed: %s', self.bnstr(output_seed))
        dest = 'NONE' if target == 'SW' else target
        dest_seed = self._seeds[dest.lower()]
        self._log.debug('Destination Seed: %s', self.bnstr(dest_seed))
        salt_len = len(salt)
        if salt_len < self.SALT_WIDTH:
            salt = b''.join((salt, bytes(self.SALT_WIDTH - salt_len)))
        self._log.debug('Salt: %s', self.bnstr(salt))
        key_ver = key_version.to_bytes(4, 'little')
        self._log.debug('Key Version: %s', self.bnstr(key_ver))
        buf_parts = [
            output_seed,
            dest_seed,
            salt,
            key_ver,
        ]
        resp = self._kmac_generate(src, buf_parts)
        return resp

    @classmethod
    def bnstr(cls, data: bytes) -> str:
        """Convert a byte array to a big-endian hex string."""
        return bytes(reversed(data)).hex()

    @classmethod
    def hexstr(cls, data: bytes) -> str:
        """Convert a byte array to a hex string."""
        return data.hex()

    def _advance_0(self, *_) -> bytes:
        share0 = self._otp_img.get_field('SECRET2', 'CREATOR_ROOT_KEY_SHARE0')
        share1 = self._otp_img.get_field('SECRET2', 'CREATOR_ROOT_KEY_SHARE1')
        self._log.debug('Creator Root Key share 0: %s', self.bnstr(share0))
        self._log.debug('Creator Root Key share 1: %s', self.bnstr(share1))
        return bytes((s0 ^ s1 for s0, s1 in zip(share0, share1)))

    def _advance_1(self, src: int, swbindings: bytes) -> bytes:
        if len(self._roms) < 2:
            raise ValueError('Missing ROM')
        creator_seed = self._otp_img.get_field('SECRET2', 'CREATOR_SEED')
        self._log.debug('Creator Seed: %s', self.bnstr(creator_seed))
        rom0_digest = self._roms[0].digest
        rom1_digest = self._roms[1].digest
        self._log.debug('ROM0 digest: %s', self.bnstr(rom0_digest))
        self._log.debug('ROM1 digest: %s', self.bnstr(rom1_digest))
        lc_div = self._get_lc_div()
        lc_ctrl_km_div = self._lc_ctrl_km_divs[lc_div]
        self._log.debug('KeyManager div: %s', self.hexstr(lc_ctrl_km_div))
        device_id = self._otp_img.get_field('HW_CFG0', 'DEVICE_ID')
        self._log.debug('Device ID: %s', self.bnstr(device_id))
        revision_seed = self._seeds['revision']
        self._log.debug('Revision Seed: %s', self.bnstr(revision_seed))
        self._log.debug('Software Binding: %s', self.hexstr(swbindings))
        buf_parts = [
            creator_seed,
            self._roms[0].digest,
            self._roms[1].digest,
            lc_ctrl_km_div,
            device_id,
            revision_seed,
            swbindings
        ]
        resp = self._kmac_advance(src, buf_parts)
        return resp

    def _advance_2(self, src: int, swbindings: bytes):
        owner_seed = self._otp_img.get_field('SECRET3', 'OWNER_SEED')
        self._log.debug('Owner Seed: %s', self.bnstr(owner_seed))
        self._log.debug('Software Binding: %s', self.hexstr(swbindings))
        buf_parts = [
            owner_seed,
            swbindings
        ]
        resp = self._kmac_advance(src, buf_parts)
        return resp

    def _advance_3(self, src: int, swbindings: bytes):
        self._log.debug('Software Binding: %s', self.hexstr(swbindings))
        resp = self._kmac_advance(src, [swbindings])
        return resp

    def _kmac_advance(self, src: int, buffers: list[bytes]) -> bytes:
        return self._kmac(src, buffers, self.ADVANCE_DATA_WIDTH)

    def _kmac_generate(self, src: int, buffers: list[bytes]) -> bytes:
        return self._kmac(src, buffers, self.GENERATE_DATA_WIDTH)

    def _kmac(self, src: int, buffers: list[bytes], data_width: int) -> bytes:
        buffer = bytearray()
        for buf in buffers:
            buffer.extend(buf)
        buflen = len(buffer)
        assert buflen <= data_width, \
            f'Invalid data width: {buflen}'
        if buflen < data_width:
            buffer.extend(bytes(data_width - buflen))
        kmac_key = self._slots[src][:32]
        self._log.debug('KMAC key: %s (%d)',
                        self.bnstr(kmac_key), len(kmac_key))
        kmac = KMAC256.new(key=kmac_key, mac_len=self.KMAC_LEN)
        kmac.update(buffer)
        resp = kmac.digest()
        self._log.info('KMAC resp: %s (%d)', self.bnstr(resp), len(resp))
        return resp

    def _get_lc_div(self):
        lc_part = self._otp_img.get_partition('LIFE_CYCLE')
        lc_state = lc_part.decode_field('LC_STATE')
        self._log.debug('LC state: %s', lc_state)
        if lc_state.startswith('TESTUNLOCKED'):
            return 'test_dev_rma'
        if lc_state in ('DEV', 'RMA'):
            return 'test_dev_rma'
        if lc_state.startswith('PROD'):
            return 'production'
        return 'invalid'
