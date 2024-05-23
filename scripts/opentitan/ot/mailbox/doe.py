# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Device proxy for OpenTitan devices and peripherals

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from logging import getLogger
from struct import calcsize as scalc, pack as spack, unpack as sunpack

from .sysmbox import SysMbox, SysMboxError


class DOEHeader:
    """Container/helper for DOE headers.

       :param vid: vendor identifier
       :param objtype: object type
       :param dwlength: count of 32-bit words in the DOE packet (incl. the
                        header 2x 32-bit words)
    """

    FORMAT = '<HBxI'
    """Encoding format."""

    SIZE = scalc(FORMAT)
    """Size of encoded content in bytes."""

    DWSIZE = SIZE//4
    """Size of encoded content in double word."""

    def __init__(self, vid: int, objtype: int, dwlength: int = 0):
        if not isinstance(vid, int) or not 0 <= vid <= 0xffff:
            raise ValueError('Invalid VID')
        if not isinstance(objtype, int) or not 0 <= objtype <= 0xff:
            raise ValueError('Invalid objtype')
        if not isinstance(dwlength, int) or not 0 <= dwlength <= 0x3ff:
            raise ValueError('Invalid dwlength')
        self._vid = vid
        self._objtype = objtype
        self._dwlength = dwlength

    def __str__(self) -> str:
        return f'vid:0x{self._vid:04x}, objtype:0x{self._objtype:02x}'

    def __repr__(self) -> str:
        return f'{self.__class__.__name__} ' \
               f'0x{self._vid:04x},0x{self._objtype:02x}'

    def set_dwlength(self, dwlength: int) -> None:
        """Set the actual 32-bit word length of the DOE packet."""
        if not isinstance(dwlength, int) or not 0 <= dwlength <= 0x3ff:
            raise ValueError('Invalid dwlength')
        self._dwlength = dwlength

    @property
    def vid(self):
        """Report the stored vendor identifier."""
        return self._vid

    @property
    def objtype(self):
        """Report the stored object type."""
        return self._objtype

    @property
    def dwlength(self):
        """Report the count of double words (i.e. 32 bit words) including
           the two double words from this very header in the DOE packet.
        """
        return self._dwlength

    @classmethod
    def decode(cls, buf: bytes) -> 'DOEHeader':
        """Decode a byte buffer into a DOE header."""
        if len(buf) < cls.SIZE:
            raise ValueError('Too short a buffer')
        vid, objtype, length = sunpack(cls.FORMAT, buf)
        length &= 0x3ff
        return cls(vid, objtype, length)

    def encode(self) -> bytes:
        """Encode this DOE header into a byte sequence."""
        buf = spack(self.FORMAT, self._vid, self._objtype, self._dwlength)
        return buf


class DOEMailbox:
    """Mailbox with a DOE header.

       :param mailbox: a system mailbox instance
       :param vid: vendor ID for the DOE header
    """

    def __init__(self, mailbox: SysMbox, vid: int):
        self._mbox = mailbox
        self._vid = vid
        self._log = getLogger('mbox.doe')

    def abort(self):
        """Tell the mailbox to abort the request / clear any error."""
        self._mbox.abort()

    @property
    def busy(self) -> bool:
        """Report whether the mailbox is busy."""
        return self._mbox.busy

    @property
    def on_error(self) -> bool:
        """Report whether the mailbox is on error."""
        return self._mbox.on_error

    @property
    def object_ready(self) -> bool:
        """Report whether the mailbox contains a response."""
        return self._mbox.object_ready

    def write(self, oid: int, msg: bytes) -> None:
        """Send a DOE message."""
        hdr = DOEHeader(self._vid, oid)
        trailing = len(msg) & 0x3
        if trailing:
            msg = b''.join((msg, bytes(4-trailing)))
        dwlen = len(msg)//4 + DOEHeader.DWSIZE
        hdr.set_dwlength(dwlen)
        req = b''.join((hdr.encode(), msg))
        self._log.debug('len %d', len(req))
        count = self._mbox.write(req)
        if count != len(req):
            raise SysMboxError(f'Cannot write full buffer {count}/{len(req)}')
        if self._mbox.on_error:
            raise SysMboxError('JTAG mailbox on error')

    def read(self) -> tuple[int, bytes]:
        """Receive a DOE message."""
        resp = self._mbox.read()
        if not resp:
            raise SysMboxError('No response from host')
        if len(resp) < DOEHeader.SIZE:
            raise SysMboxError('Truncated response from host')
        self._log.debug('len %d', len(resp))
        hdr = DOEHeader.decode(resp[:DOEHeader.SIZE])
        if hdr.vid != self._vid:
            raise SysMboxError(f'Unexpected vendor ID 0x{hdr.vid:04x}')
        plen = (hdr.dwlength - DOEHeader.DWSIZE) * 4
        payload = resp[DOEHeader.SIZE:]
        if plen != len(payload):
            raise SysMboxError(f'Unexpected payload length '
                               f'{plen}/{len(payload)}')
        oid = hdr.objtype
        return oid, payload

    def exchange(self, oid: int, msg: bytes) -> bytes:
        """Exchange a message/response on the mailbox."""
        self.write(oid, msg)
        roid, payload = self.read()
        if roid != oid:
            raise SysMboxError(f'Unexpected object type: {oid}/{roid}')
        return payload
