# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OTP descriptors.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from logging import getLogger
from typing import TYPE_CHECKING, TextIO

if TYPE_CHECKING:
    from .map import OtpMap


class OTPPartitionDesc:
    """OTP Partition descriptor generator."""

    ATTRS = {
        'size': None,
        'offset': None,
        'digest_offset': None,
        'hw_digest': '',
        'sw_digest': '',
        'secret': '',
        'variant': 'buffer',
        'write_lock': 'wlock',
        'read_lock': 'rlock',
        'integrity': '',
        'iskeymgr': '',
        'iskeymgr_creator': '',
        'iskeymgr_owner': '',
        'wide': ''
    }

    def __init__(self, otpmap: 'OtpMap'):
        self._log = getLogger('otp.partdesc')
        self._otpmap = otpmap

    def save(self, hjname: str, scriptname: str, cfp: TextIO) -> None:
        """Generate a C file with a static description for the partitions."""
        # pylint: disable=f-string-without-interpolation
        attrs = {n: getattr(self, f'_convert_to_{k}') if k else lambda x: x
                 for n, k in self.ATTRS.items() if k is not None}
        print(f'/* Generated from {hjname} with {scriptname} */', file=cfp)
        print(file=cfp)
        print('/* clang-format off */', file=cfp)
        print('/* NOLINTBEGIN */', file=cfp)
        print('static const OtOTPPartDesc OtOTPPartDescs[] = {', file=cfp)
        for part in self._otpmap.enumerate_partitions():
            print(f'    [OTP_PART_{part.name}] = {{', file=cfp)
            print(f'        .size = {part.size}u,', file=cfp)
            print(f'        .offset = {part.offset}u,', file=cfp)
            if part.digest_offset is not None:
                print(f'        .digest_offset = {part.digest_offset}u,',
                      file=cfp)
            else:
                print(f'        .digest_offset = UINT16_MAX,',   # noqa: F541
                      file=cfp)
            for attr in attrs:
                value = getattr(part, attr, None)
                if value is None:
                    continue
                convs = attrs[attr](value)
                if not isinstance(convs, list):
                    convs = [convs]
                for conv in convs:
                    if isinstance(conv, tuple):
                        attr_name = conv[0]
                        attr_val = conv[1]
                    else:
                        attr_name = attr
                        attr_val = conv
                    if isinstance(attr_val, bool):
                        attr_val = str(attr_val).lower()
                    print(f'        .{attr_name} = {attr_val},', file=cfp)
            print(f'    }},', file=cfp)  # noqa: F541
        print('};', file=cfp)
        print('', file=cfp)
        print('#define OTP_PART_COUNT ARRAY_SIZE(OtOTPPartDescs)', file=cfp)
        print(file=cfp)
        print('/* NOLINTEND */', file=cfp)
        print('/* clang-format on */', file=cfp)
        # pylint: enable=f-string-without-interpolation

    @classmethod
    def _convert_to_bool(cls, value) -> str:
        return str(value).lower()

    @classmethod
    def _convert_to_buffer(cls, value) -> tuple[str, bool]:
        return {
            'unbuffered': ('buffered', False),
            'buffered': ('buffered', True),
            'lifecycle': ('buffered', True),
        }[value.lower()]

    @classmethod
    def _convert_to_wlock(cls, value) -> bool:
        return value == 'digest'

    @classmethod
    def _convert_to_rlock(cls, value) -> list[tuple[str, bool]]:
        value = value.lower()
        if value == 'csr':
            return [('read_lock_csr', True), ('read_lock', True)]
        if value == 'digest':
            return 'read_lock', True
        if value == 'none':
            return 'read_lock', False
        assert False, 'Unknown RLOCK type'


class OTPRegisterDef:
    """OTP Partition register generator."""

    def __init__(self, otpmap: 'OtpMap'):
        self._log = getLogger('otp.reg')
        self._otpmap = otpmap

    def save(self, hjname: str, scriptname: str, cfp: TextIO) -> None:
        """Generate a C file with register definition for the partitions."""
        reg_offsets = []
        reg_sizes = []
        part_names = []
        for part in self._otpmap.enumerate_partitions():
            part_names.append(f'OTP_PART_{part.name}')
            offset = part.offset
            reg_sizes.append((f'{part.name}_SIZE', part.size))
            for itname, itdict in part.items.items():
                size = itdict['size']
                if not itname.startswith(f'{part.name}_'):
                    name = f'{part.name}_{itname}'.upper()
                else:
                    name = itname
                reg_offsets.append((name, offset))
                reg_sizes.append((f'{name}_SIZE', size))
                offset += size
        print(f'/* Generated from {hjname} with {scriptname} */')
        print(file=cfp)
        print('/* clang-format off */', file=cfp)
        for reg, off in reg_offsets:
            print(f'REG32({reg}, {off}u)', file=cfp)
        print(file=cfp)
        regwidth = max(len(r[0]) for r in reg_sizes)
        for reg, size in reg_sizes:
            print(f'#define {reg:{regwidth}s} {size}u', file=cfp)
        print(file=cfp)
        pcount = len(part_names)
        part_names.extend((
            '_OTP_PART_COUNT',
            'OTP_ENTRY_DAI = _OTP_PART_COUNT',
            'OTP_ENTRY_KDI',
            '_OTP_ENTRY_COUNT'))
        print('typedef enum {', file=cfp)
        for pname in part_names:
            print(f'    {pname},', file=cfp)
        print('} OtOTPPartitionType;', file=cfp)
        print(file=cfp)
        print('static const char *PART_NAMES[] = {', file=cfp)
        for pname in part_names[:pcount]:
            print(f'    OTP_NAME_ENTRY({pname}),', file=cfp)
        print('};', file=cfp)
        print('/* clang-format on */', file=cfp)
        print(file=cfp)
