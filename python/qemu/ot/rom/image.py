# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OpenTitan ROM image support.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import hexlify
from io import BytesIO
from logging import getLogger
from os.path import basename
from typing import BinaryIO, Optional, Union

import re

from ..util.elf import ElfBlob
from ..util.file import guess_file_type
from ..util.prince import PrinceCipher

# ruff: noqa: E402
_CRYPTO_EXC: Optional[Exception] = None
try:
    from Crypto.Hash import cSHAKE256
except ModuleNotFoundError:
    try:
        # see pip3 install -r requirements.txt
        from Cryptodome.Hash import cSHAKE256
    except ModuleNotFoundError as exc:
        _CRYPTO_EXC = exc


class ROMImage:
    """ROM controller image helper to manipulate ROM images

       Support scrambling, descrambling, file format conversions.

       :param name: an optional name for logging purposes
    """

    ADDR_SUBST_PERM_ROUNDS = 2
    DATA_SUBST_PERM_ROUNDS = 2
    PRINCE_HALF_ROUNDS = 2
    DATA_BITS = 4 * 8
    ECC_BITS = 7
    WORD_BITS = DATA_BITS + ECC_BITS
    WORD_BYTES = (WORD_BITS + 7) // 8
    DIGEST_BYTES = 32
    DIGEST_WORDS = DIGEST_BYTES // 4

    SBOX4 = [12, 5, 6, 11, 9, 0, 10, 13, 3, 14, 15, 8, 4, 7, 1, 2]
    """PRESENT S-box permutation."""
    SBOX4_INV = [5, 14, 15, 8, 12, 1, 2, 13, 11, 4, 6, 3, 0, 7, 9, 10]
    """PRESENT S-box inverted permutation."""

    ECC_39_32 = [0x2606bd25, 0xdeba8050, 0x413d89aa, 0x31234ed1,
                 0xc2c1323b, 0x2dcc624c, 0x98505586]
    """HSIAO constants for "inverted" 32-bit data with 7-bit SEC-DED."""

    def __init__(self, name: Union[str, int, None] = None):
        logname = 'romimg'
        if isinstance(name, (int, str)):
            logname = f'{logname}.{name}'
        self._log = getLogger(logname)
        self._name = name
        self._clear_data = bytearray()
        self._scrambled_words: list[int] = []  # in logical address order
        self._key: Optional[int] = None
        self._nonce: Optional[int] = None
        self._addr_nonce = 0
        self._data_nonce = 0
        self._addr_width = 0
        self._khi = 0
        self._klo = 0
        self._digest: Optional[bytes] = None

    def load(self, rfp: BinaryIO, size: Optional[int] = None):
        """Load a ROM file. Can either be:

           - a so called 'HEX' file, which is assumed to be scrambled
           - a pure raw binary file
           - a RISC-V RV32 ELF file

           :param rfp: input binary stream
           :param size: optional size, required for binary and ELF file format
        """
        ftype = guess_file_type(rfp)
        loader = getattr(self, f'_load_{ftype}', None)
        if not loader:
            raise ValueError(f'Unsupported ROM file type: {ftype}')
        filename = basename(rfp.name) if rfp.name else '?'
        self._log.info('loading ROM image %s as %s file',
                       filename, ftype.upper())
        loader(rfp, size)

    def save(self, rfp: BinaryIO, ftype: str) -> None:
        """Save a ROM file.

           :param rfp: output binary stream
           :param ftype: the output file format. Only 'HEX' scrambled file
                         format is supported for now
        """
        saver = getattr(self, f'_save_{ftype}', None)
        if not saver:
            raise ValueError(f'Unsupported ROM file type: {ftype}')
        filename = basename(rfp.name) if isinstance(rfp.name, str) else '?'
        self._log.info('storing ROM image %s as %s file', filename, ftype)
        saver(rfp)

    @property
    def digest(self) -> bytes:
        """Return the current digest of the ROM image.

           Digest is computed on-the-fly if not already known.

           :return: the digest
        """
        if not self._digest:
            self._make_digest()
        return self._digest

    @property
    def key(self) -> Optional[int]:
        """Key observer."""
        return None if self._key is None else self._key.to_bytes(16, 'big')

    @key.setter
    def key(self, value: bytes) -> None:
        """Key modifier."""
        if not isinstance(value, bytes):
            raise TypeError('Key must be bytes')
        self._key = int.from_bytes(value, 'big')
        self._khi = self._key >> 64
        self._klo = self._key & 0xFFFFFFFFFFFFFFFF

    @property
    def nonce(self) -> Optional[int]:
        """Nonce observer."""
        return None if self._nonce is None else self._nonce.to_bytes(8, 'big')

    @nonce.setter
    def nonce(self, value: bytes):
        """Nonce modifier."""
        if not isinstance(value, bytes):
            raise TypeError('Nonce must be bytes')
        self._nonce = int.from_bytes(value, 'big')
        self._addr_nonce = 0
        self._data_nonce = 0

    @classmethod
    def ctz(cls, val: int) -> int:
        """Count trailing zero bit in an integer."""
        if val == 0:
            raise ValueError('CTZ undefined')
        pos = 0
        while (val & 1) == 0:
            val >>= 1
            pos += 1
        return pos

    @classmethod
    def bitswap(cls, in_: int, mask: int, shift: int) -> int:
        """Bit swapping helper function."""
        return ((in_ & mask) << shift) | ((in_ & ~mask) >> shift)

    @classmethod
    def bitswap64(cls, val: int) -> int:
        """Swap (reverse) 64-bit integer."""
        val = cls.bitswap(val, 0x5555555555555555, 1)
        val = cls.bitswap(val, 0x3333333333333333, 2)
        val = cls.bitswap(val, 0x0f0f0f0f0f0f0f0f, 4)
        val = cls.bitswap(val, 0x00ff00ff00ff00ff, 8)
        val = cls.bitswap(val, 0x0000ffff0000ffff, 16)
        val = (val << 32) | (val >> 32)

        return val

    @classmethod
    def sbox(cls, in_: int, width: int, sbox: int) -> int:
        """PRESENT S-box permutation."""
        assert width < 64

        full_mask = (1 << width) - 1
        width &= ~3
        sbox_mask = (1 << width) - 1

        out = in_ & (full_mask & ~sbox_mask)
        for ix in range(0, width, 4):
            nibble = (in_ >> ix) & 0xf
            out |= sbox[nibble] << ix

        return out

    @classmethod
    def flip(cls, in_: int, width: int) -> int:
        """Reverse N bit in an integer."""
        out = cls.bitswap64(in_)
        out >>= 64 - width

        return out

    @classmethod
    def perm(cls, in_: int, width: int, invert: bool) -> int:
        """PRESENT permutation."""
        assert width < 64

        full_mask = (1 << width) - 1
        width &= ~1
        bfly_mask = (1 << width) - 1

        out = in_ & (full_mask & ~bfly_mask)

        width >>= 1
        if not invert:
            for ix in range(width):
                bit = (in_ >> (ix << 1)) & 1
                out |= bit << ix
                bit = (in_ >> ((ix << 1) + 1)) & 1
                out |= bit << (width + ix)
        else:
            for ix in range(width):
                bit = (in_ >> ix) & 1
                out |= bit << (ix << 1)
                bit = (in_ >> (ix + width)) & 1
                out |= bit << ((ix << 1) + 1)

        return out

    @classmethod
    def subst_perm_enc(cls, in_: int, key: int, width: int, num_round: int) \
            -> int:
        """Substitute-permute rounds."""
        state = in_
        while num_round:
            num_round -= 1
            state ^= key
            state = cls.sbox(state, width, cls.SBOX4)
            state = cls.flip(state, width)
            state = cls.perm(state, width, False)
        state ^= key

        return state

    @classmethod
    def subst_perm_dec(cls, val: int, key: int, width: int, num_round: int) \
            -> int:
        """Substitute-permute rounds."""
        state = val
        while num_round:
            num_round -= 1
            state ^= key
            state = cls.perm(state, width, True)
            state = cls.flip(state, width)
            state = cls.sbox(state, width, cls.SBOX4_INV)
        state ^= key

        return state

    def addr_sp_enc(self, addr: int) -> int:
        """Encode a logical (CPU) address into a physical (ROM storage) address.
        """
        return self.subst_perm_enc(addr, self._addr_nonce, self._addr_width,
                                   self.ADDR_SUBST_PERM_ROUNDS)

    def addr_sp_dec(self, addr: int) -> int:
        """Decode a physical (ROM storage) address into a logical (CPU) address.
        """
        return self.subst_perm_dec(addr, self._addr_nonce, self._addr_width,
                                   self.ADDR_SUBST_PERM_ROUNDS)

    @classmethod
    def add_ecc_inv_39_32(cls, data: int) -> int:
        """Compute and add HSIAO SEC-DEC to a 32 bit value.

           :param data: 32-bit value
           :return: 39-bit value (upper bits contain the ECC)
        """
        ecc = 0
        inv = False
        for mask in reversed(cls.ECC_39_32):
            ecc <<= 1
            parity = (data & mask).bit_count() & 1
            ecc |= parity ^ int(inv)
            inv = not inv
        return (ecc << 32) | data

    @classmethod
    def data_sp_enc(cls, val: int) -> int:
        """Encode (scramble) data."""
        return cls.subst_perm_enc(val, 0, cls.WORD_BITS,
                                  cls.DATA_SUBST_PERM_ROUNDS)

    @classmethod
    def data_sp_dec(cls, val: int) -> int:
        """Decode (unscramble) data."""
        return cls.subst_perm_dec(val, 0, cls.WORD_BITS,
                                  cls.DATA_SUBST_PERM_ROUNDS)

    def _load_hex(self, rfp: BinaryIO, size: Optional[int]) -> None:
        words: list[int] = []  # 64-bit values
        for lpos, line in enumerate(rfp.readlines(), start=1):
            line = line.strip()
            if len(line) != 10:
                raise ValueError(f'Unsupported ROM HEX format at line {lpos}')
            try:
                words.append(int(line, 16))
            except ValueError as exc:
                raise ValueError(f'Invalid HEX data at line {lpos}: {exc}')
        self._handle_scrambled_data(words)

    def _save_hex(self, rfp: BinaryIO) -> None:
        # assume a scrambled output image
        if self._key is None:
            raise RuntimeError('Key not defined, cannot scramble HEX file')
        if self._nonce is None:
            raise RuntimeError('Nonce not defined, cannot scramble HEX file')
        if not self._clear_data:
            return
        # ensure scrambled data and digest are generated
        digest = self._make_digest()
        bndigest = bytes(reversed(digest))
        self._log.info('computed digest: %s', hexlify(bndigest).decode())
        scrwords = self._scrambled_words
        rfp.write('\n'.join(f'{scrwords[self.addr_sp_dec(pa)]:010X}'
                            for pa in range(len(scrwords))).encode())
        rfp.write(b'\n')

    def _load_svmem(self, rfp: BinaryIO, size: Optional[int]) -> None:
        # load VMEM handle both kinds of VMEM files
        self._load_vmem(rfp, size)

    def _load_vmem(self, rfp: BinaryIO, size: Optional[int]) -> None:
        words: list[int] = []  # 64-bit values
        scrambled: Optional[bool] = None
        next_addr = 0
        # note: address marker (@address) defines the index in the destination
        # memory. For ROM images, each index represents a 32-bit memory address.
        # Each location contains 32 bits of data, and optionally 7 bits of ECC.
        # ECC extension is only supported in scrambled files. The ECC is stored
        # in the MSB of each VMEM word.
        for lpos, line in enumerate(rfp.readlines(), start=1):
            line = line.strip()
            if not line.startswith(b'@'):
                continue
            parts = re.split(r'\s+', line[1:])
            address_str = parts[0]
            data_str = parts[1:]
            scrambled_data = all(len(d) == 10 for d in data_str)
            plain_data = all(len(d) == 8 for d in data_str)
            if not scrambled_data ^ plain_data:
                raise ValueError(f'Unknown VMEM format @ {lpos}')
            if scrambled is None:
                scrambled = scrambled_data
                self._log.info('Identified %s as %s VMEM format',
                               rfp.name or '?',
                               'scrambled' if scrambled else 'plain')
            elif scrambled != scrambled_data:
                raise ValueError(f'Incoherent VMEM format @ {lpos}')
            try:
                address = int(address_str, 16)
                words.extend(int(d, 16) for d in data_str)
            except (TypeError, ValueError) as exc:
                raise ValueError(f'Invalid data in VMEM format @ '
                                 f'{lpos}') from exc
            if address != next_addr:
                raise ValueError(f'Incoherent next address @ {lpos}')
            next_addr += len(data_str)
        if scrambled is None:
            self._log.error('No valid data found in VMEM file')
            return
        if scrambled:
            if size is not None and size != len(words) * 4:
                raise ValueError(f'ROM size ({size}) does not match the '
                                 f'scrambled content size ({len(words) * 4})')
            self._handle_scrambled_data(words)
        else:
            if not size:
                raise ValueError('ROM size not specified')
            clrdata = bytearray()
            for word in words:
                clrdata.extend(word.to_bytes(4, 'little'))
            data_len = len(clrdata)
            if data_len < size:
                clrdata.extend(bytes(size - data_len))
            self._clear_data = clrdata
            bndigest = bytes(reversed(self.digest))
            self._log.info('computed digest: %s', hexlify(bndigest).decode())

    def _load_bin(self, rfp: BinaryIO, size: Optional[int]) -> None:
        if not size:
            raise ValueError('ROM size not specified')
        if _CRYPTO_EXC:
            raise ModuleNotFoundError('Crypto module not found')
        data = bytearray(rfp.read())
        data_len = len(data)
        if data_len > size:
            raise ValueError(f'Specified ROM size is too small to fit '
                             f'{rfp.name or '?'}')
        if data_len < size:
            data.extend(bytes(size - data_len))
        self._clear_data = data
        bndigest = bytes(reversed(self.digest))
        self._log.info('computed digest: %s', hexlify(bndigest).decode())

    def _load_elf(self, rfp: BinaryIO, size: Optional[int]) -> None:
        elf = ElfBlob()
        elf.load(rfp)
        bin_io = BytesIO(elf.blob)
        self._load_bin(bin_io, size)

    def _handle_scrambled_data(self, data: list[int]) -> None:
        word_count = len(data)
        addr_bits = self.ctz(word_count)
        data_nonce_width = 64 - addr_bits
        if self._key is None:
            raise RuntimeError('Key not defined, cannot unscramble HEX file')
        if self._nonce is None:
            raise RuntimeError('Nonce not defined, cannot unscramble HEX file')
        self._addr_nonce = self._nonce >> data_nonce_width
        self._data_nonce = self._nonce & ((1 << data_nonce_width) - 1)
        self._addr_width = addr_bits
        self._log.debug('nonce_width: %d', data_nonce_width)
        self._log.debug('addr_width:  %d', self._addr_width)
        self._log.debug('addr_nonce:  %06x', self._addr_nonce)
        self._log.debug('data_nonce:  %012x', self._data_nonce)
        self._log.debug('key_hi:      %016x', self._khi)
        self._log.debug('key_lo:      %016x', self._klo)
        self._unscramble(data)
        bndigest = bytes(reversed(self._digest))
        self._log.info('stored digest: %s', hexlify(bndigest).decode())
        local_digest = self.digest
        bndigest = bytes(reversed(local_digest))
        self._log.info('local digest:  %s', hexlify(bndigest).decode())
        if local_digest != self._digest:
            self._log.error('Digest mismatch')

    def _get_keystream(self, addr: int):
        scramble = (self._data_nonce << self._addr_width) | addr
        stream = PrinceCipher.run(scramble, self._khi, self._klo,
                                  self.PRINCE_HALF_ROUNDS)
        return stream & ((1 << self.WORD_BITS) - 1)

    def _scramble_word(self, addr: int, value: int):
        keystream = self._get_keystream(addr)
        return self.data_sp_enc(keystream ^ value)

    def _unscramble_word(self, addr: int, value: int):
        keystream = self._get_keystream(addr)
        spd = self.data_sp_dec(value)
        return keystream ^ spd

    def _scramble(self, data: Union[bytes, bytearray]) -> None:
        data_len = len(data)
        if data_len & (data_len - 1):
            self._log.warning('Unexpected data length: %d, not a 2^N value',
                              data_len)
        word_count = data_len // 4
        addr_bits = self.ctz(word_count)
        data_nonce_width = 64 - addr_bits
        if self._nonce is None:
            raise RuntimeError('Nonce not defined, cannot scramble data')
        addr_nonce = self._nonce >> data_nonce_width
        data_nonce = self._nonce & ((1 << data_nonce_width) - 1)
        addr_width = addr_bits
        if not self._addr_nonce:
            self._addr_nonce = addr_nonce
        elif self._addr_nonce != addr_nonce:
            raise RuntimeError('Addr nonce discrepancy')
        if not self._data_nonce:
            self._data_nonce = data_nonce
        elif self._data_nonce != data_nonce:
            raise RuntimeError('Data nonce discrepancy')
        if not self._addr_width:
            self._addr_width = addr_width
        elif self._addr_width != addr_width:
            raise RuntimeError('Addr width discrepancy')
        self._log.debug('nonce_width: %d', data_nonce_width)
        self._log.debug('addr_width:  %d', self._addr_width)
        self._log.debug('addr_nonce:  %06x', self._addr_nonce)
        self._log.debug('data_nonce:  %012x', self._data_nonce)
        scrambled: list[int] = []
        word_count = len(data) // 4
        dig_addr = len(data) - self.DIGEST_BYTES
        for log_addr in range(word_count):
            assert 0 <= log_addr < word_count
            byte_addr = log_addr << 2
            clrdata = int.from_bytes(data[byte_addr:byte_addr + 4], 'little')
            if byte_addr < dig_addr:
                clrdata = self.add_ecc_inv_39_32(clrdata)
                assert 0 <= clrdata < (1 << self.WORD_BITS), "invalid data"
                scrdata = self._scramble_word(log_addr, clrdata)
                scrambled.append(scrdata)
            else:
                # digest is not scrambled and contains no ECC
                scrambled.append(clrdata)
        self._scrambled_words = scrambled

    def _unscramble(self, scr: list[int]) -> None:
        # do not attempt to detect or correct errors for now
        word_count = len(scr)  # each slot is a 32-bit data value + ECC
        if word_count & (word_count - 1):
            self._log.warning('Unexpected word count: %d, not a 2^N value',
                              word_count)
        scr_word_count = word_count - self.DIGEST_BYTES // 4
        self._log.debug('word_count: %d, scr_word_count %d',
                        word_count, scr_word_count)
        log_addr = 0
        dst: list[int] = [0] * word_count  # 32-bit values
        scrambled_words: list[int] = []
        while log_addr < scr_word_count:
            phy_addr = self.addr_sp_enc(log_addr)
            assert phy_addr < word_count, "unexpected physical address"
            scrdata = scr[phy_addr]
            scrambled_words.append(scrdata)
            clrdata = self._unscramble_word(log_addr, scrdata)
            dst[log_addr] = clrdata & 0xffffffff
            log_addr += 1
        # digest words are not scrambled
        wix = 0
        digest_parts: list[int] = []  # 32-bit values
        while wix < self.DIGEST_WORDS:
            phy_addr = self.addr_sp_enc(log_addr)
            assert phy_addr < word_count, "unexpected physical address"
            word = scr[phy_addr] & 0xffffffff
            scrambled_words.append(word)
            digest_parts.append(word)
            wix += 1
            log_addr += 1
        digest = b''.join((dp.to_bytes(4, 'little') for dp in digest_parts))
        data = bytearray()
        for val in dst:
            data.extend(val.to_bytes(4, 'little'))
        self._clear_data = data
        self._scrambled_words = scrambled_words
        self._digest = digest

    def _compute_digest(self) -> None:
        scrambled_data = bytearray()
        word_bytes = len(self._clear_data)
        scr_word_count = (word_bytes - self.DIGEST_BYTES) // 4
        for word in self._scrambled_words[:scr_word_count]:
            scrambled_data.extend(word.to_bytes(self.WORD_BYTES, 'little'))
        shake = cSHAKE256.new(custom=b'ROM_CTRL')
        shake.update(scrambled_data)
        self._digest = shake.read(self.DIGEST_BYTES)
        if len(self._scrambled_words) > scr_word_count:
            self._scrambled_words[:] = self._scrambled_words[:scr_word_count]
        digest_words = [int.from_bytes(self._digest[a:a+4], 'little')
                        for a in range(0, len(self._digest), 4)]
        self._scrambled_words.extend(digest_words)

    def _make_digest(self) -> bytes:
        if not self._scrambled_words:
            self._scramble(self._clear_data)
        self._compute_digest()
        return self._digest
