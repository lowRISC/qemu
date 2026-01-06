# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""LifeCycle Controller utilities.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import hexlify
from logging import getLogger
from io import StringIO
from random import randbytes
from typing import NamedTuple

import re

try:
    from Crypto.Hash import cSHAKE128
except ModuleNotFoundError:
    # see pip3 install -r requirements.txt
    from Cryptodome.Hash import cSHAKE128


class LifeCycleTokenPair(NamedTuple):
    """Life Cycle token pair"""

    text: bytes
    hashed: bytes


class LifeCycleTokenEngine:
    """Life Controller token management.
    """

    TOKEN_LENGTH = 16
    """Token length in bytes."""

    TOKEN_CUSTOM = 'LC_CTRL'
    """Token customisation string."""

    TOKEN_VAR_NAME = 'LC_{var}_TOKEN{hashed}'
    """Variable name template used to store a token value."""

    def __init__(self):
        self._log = getLogger('lc.token')

    def generate(self) -> LifeCycleTokenPair:
        """Generate a random token pair (clear, hashed)

           :note: random quality is not suitable for production use
        """
        token_text = randbytes(self.TOKEN_LENGTH)
        token_hash = self.hash(token_text)
        return LifeCycleTokenPair(token_text, token_hash)

    def hash(self, token: bytes) -> bytes:
        """Hash a plain text token.

           :param token: token to hash
           :return: the hashed tocken
        """
        cshake = cSHAKE128.new(custom=self.TOKEN_CUSTOM.encode())
        # convert natural byte order to little order
        # (human readable, left to right, MSB to LSB) to LE
        token_le = bytes(reversed(token))
        cshake.update(token_le)
        hash_le = cshake.read(self.TOKEN_LENGTH)
        # convert back to natural byte order (from LE to human)
        return bytes(reversed(hash_le))

    def build_from_text(self, token: bytes) -> LifeCycleTokenPair:
        """Build a token pair from a clear token.

           :param token: clear token
           :return: the token pair
        """
        return LifeCycleTokenPair(token, self.hash(token))

    @classmethod
    def build_rust_def(cls, name: str, value: bytes) -> str:
        """Build Rust definition for a token.

           :param name: the token name radix
           :param value: the token value
           :return: the generated definition
        """
        code = StringIO()
        print(f'// {hexlify(value).decode()}', file=code)
        print(f'const {name.upper()}: [u8; {len(value)}] = [', file=code)
        value = bytes(value)
        for pos in range(0, len(value), 8):
            s = ', '.join((f'0x{c:02x}' for c in value[pos:pos+8]))
            print(f'   {s},', file=code)
        print('];', file=code)
        return code.getvalue()

    @classmethod
    def build_qemu_def(cls, name: str, value: bytes) -> str:
        """Build QEMU definition for a token.

           :param name: the token name radix
           :param value: the token value
           :return: the generated definition
        """
        code = StringIO()
        print(f'// {hexlify(value).decode()}', file=code)
        print(f'static const OtOTPTokenValue {name.upper()} = {{', file=code)
        print(f'    .hi = 0x{int.from_bytes(value[:8], "big"):016x}u,'
              f' .lo = 0x{int.from_bytes(value[8:], "big"):016x}u,',
              file=code)
        print('}};', file=code)
        return code.getvalue()

    def generate_code(self, lang: str, name: str, tkpair: LifeCycleTokenPair) \
            -> str:
        """Generate code to store a token pair.

           :param lang: the output type
           :param name: the token name radix
           :param value: the token value
           :return: the generated code
        """
        builder = getattr(self, f'build_{lang}_def', None)
        if not builder and not callable(builder):
            raise ValueError('Unsupported language')
        # pylint: disable=not-callable
        lines = (
            builder(self.TOKEN_VAR_NAME.format(var=name, hashed=''),
                    tkpair.text),
            builder(self.TOKEN_VAR_NAME.format(var=name, hashed='_HASHED'),
                    tkpair.hashed),
            ''
        )
        return '\n'.join(lines)

    def parse_rust(self, rust_code: str) -> dict[str, LifeCycleTokenPair]:
        """Parse Rust code and extract Token definitions.

           :param rust_code: rust code to parse
           :return: a dictionary of token pair, indexed by name
        """
        var_re = self.TOKEN_VAR_NAME.format(var='(.*)', hashed='(_HASHED)?')
        rcre = re.compile(r'const ' + var_re + r': *\[u8; +16\] *= '
                          r'*\[((?:[\s\n]+0x[0-9a-fA-F]{2},){16})(?:[\s\n]+)];')
        tokens: dict[str, list[bytes, bytes]] = {}
        for rmo in rcre.finditer(rust_code):
            token = rmo.group(1)
            hashed = bool(rmo.group(2))
            seq = re.sub(r'(0x|\s|,)', '', rmo.group(3))
            if token not in tokens:
                tokens[token] = [bytes(0), bytes(0)]
            pos = int(hashed)
            if tokens[token][pos]:
                raise ValueError(f'Redefinition of '
                                 f'{"hashed" if hashed else "plain"} {token}')
            tokens[token][pos] = int(seq, 16).to_bytes(self.TOKEN_LENGTH)
        tkpairs = {n: LifeCycleTokenPair(*v) for n, v in tokens.items()}
        return tkpairs
