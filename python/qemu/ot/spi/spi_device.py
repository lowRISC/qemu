# Copyright (c) 2023-2024, Rivos, Inc.
# All rights reserved.

"""SPI device proxy.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import hexlify
from logging import getLogger
from select import POLLIN, poll as spoll
from socket import (create_connection, socket, IPPROTO_TCP, TCP_NODELAY,
                    SHUT_RDWR, timeout as LegacyTimeoutError)
from struct import calcsize as scalc, pack as spack
from time import sleep, time as now
from typing import Optional


class SpiDevice:
    """SPI device proxy that implements the SPI device protocol over TCP."""

    TIMEOUT = 2.0
    """Default allowed timeout to complete an exchange with the remote target.
    """

    POLL_TIMEOUT = 0.05
    """Maximum time to wait on a blocking operation.
    """

    VERSION = 0
    """Protocol version."""

    CS_HEADER_FMT = '3sBBxH'
    """CS header format."""

    CS_HEADER_SIZE = scalc(CS_HEADER_FMT)
    """Size of a CS header."""

    BUSY = 1 << 0
    """Status register BUSY bit."""

    WEL = 1 << 1
    """Status register write enabled bit."""

    READ_BUFFER_SIZE = 0x400
    """1 KiB HW buffer."""

    MAILBOX_SIZE = 0x400
    """1 KiB mailbox."""

    COMMANDS = {
        'PAGE_PROGRAM': 0x02,
        'READ_DATA': 0x3,
        'WRITE_DISABLE': 0x04,
        'READ_STATUS': 0x05,
        'WRITE_ENABLE': 0x06,
        'FAST_READ': 0x0b,
        'READ_SFDP': 0x5a,
        'READ_JEDEC_ID': 0x9f,
        'ENTER_ADDR4': 0xb7,
        'POWER_DOWN': 0xb9,
        'EXIT_ADDR4': 0xe9,
        'CHIP_ERASE': 0xc7,
        'BLOCK_ERASE': 0xd8,
        'SECTOR_ERASE': 0x20,
        'RESET1': 0x66,
        'RESET2': 0x99,
        'CHECK_ANSWER': 0xca,
    }
    """Supported *25 SPI data flash device commands."""

    def __init__(self):
        self._log = getLogger('spidev')
        self._socket: Optional[socket] = None
        self._mode = 0
        self._4ben = False
        self._rev_rx = False
        self._rev_tx = False

    def connect(self, host: str, port: int) -> None:
        """Open a TCP connection to the remote host.

           @param host the host name
           @paran port the TCP port
        """
        if self._socket:
            raise RuntimeError('Cannot open multiple comm port at once')
        try:
            self._socket = create_connection((host, port), timeout=self.TIMEOUT)
        except OSError:
            self._log.fatal('Cannot connect to %s:%d', host, port)
            raise
        # use poll
        self._socket.settimeout(None)
        self._socket.setsockopt(IPPROTO_TCP, TCP_NODELAY, 1)

    def quit(self) -> None:
        """Close the communication socket."""
        if self._socket:
            self._socket.shutdown(SHUT_RDWR)
            self._socket.close()
            self._socket = None

    def transmit(self, cmd: Optional[int] = None,
                 in_payload: Optional[bytes | bytearray | int] = None,
                 out_len: int = 0, release: bool = True) -> bytes:
        """SPI data transfer.

           :param cmd: the command to send
           :param in_payload: the payload to send
           :param out_len: the count of meaningful bytes to receive
           :param release: whether to release /CS line (to manage transactions)
        """
        if isinstance(in_payload, int):
            in_payload = bytes([0xff] * in_payload)
        elif in_payload is not None:
            assert isinstance(in_payload, (bytes, bytearray))
        else:
            in_payload = bytes()
        assert isinstance(out_len, int) and 0 <= out_len <= 0xffff
        if cmd is not None:
            assert 0 <= cmd <= 0xff
            tx_payload = b''.join((bytes([cmd]), in_payload, bytes(out_len)))
        else:
            tx_payload = b''.join((in_payload, bytes(out_len)))
        if self._rev_tx:
            tx_payload = bytes(SpiDevice.rev8(x) for x in tx_payload)
        rx_payload = self._exchange(tx_payload, release)
        if self._rev_rx:
            rx_payload = bytes(SpiDevice.rev8(x) for x in rx_payload)
        assert len(rx_payload) == len(tx_payload)
        return rx_payload[-out_len:]

    def read_status_register(self) -> int:
        """Read out the flash status register."""
        resp = self.transmit(self.COMMANDS['READ_STATUS'], out_len=1)
        return resp[0]

    def is_busy(self, sreg: Optional[int] = None) -> bool:
        """Check if the flash device is busy."""
        if sreg is None:
            sreg = self.read_status_register()
        return bool(sreg & self.BUSY)

    def is_wel(self, sreg: Optional[int] = None) -> bool:
        """Check if the flash device is write-enabled."""
        if sreg is None:
            sreg = self.read_status_register()
        return bool(sreg & self.WEL)

    def wait_idle(self, timeout: float = 1.0, pace: float = 0.0005):
        """Wait for the flash device to become idle.

           :param timeout: raise a TimeoutError if flash does not become
                           available after this delay
           :param pace: delay between each flash device poll request.
        """
        timeout += now()
        while True:
            status = self.read_status_register()
            if not status & self.BUSY:
                break
            if now() > timeout:
                raise TimeoutError('Flash stuck to busy')
            sleep(pace)

    def read_jedec_id(self) -> bytes:
        """Read out the flash device JEDEC ID."""
        jedec = bytearray()
        self.transmit(self.COMMANDS['READ_JEDEC_ID'], release=False)
        while True:
            manuf = self.transmit(out_len=1, release=False)[0]
            if manuf != 0x7f:
                jedec.append(manuf)
                break
        jedec.extend(self.transmit(out_len=2))
        return jedec

    def read_sfdp(self, address: int = 0) -> bytes:
        """Read out the flash device SFTP descriptor."""
        payload = spack('>I', address)
        return self.transmit(self.COMMANDS['READ_SFDP'], payload, 256)

    def enable_write(self):
        """Enable write."""
        self.transmit(self.COMMANDS['WRITE_ENABLE'])

    def disable_write(self):
        """Disable write."""
        self.transmit(self.COMMANDS['WRITE_DISABLE'])

    def enable_4b_address(self, enable: bool):
        """Enable or disable 4-byte addressing mode."""
        self.transmit(self.COMMANDS['ENTER_ADDR4' if enable else 'EXIT_ADDR4'])
        self._4ben = enable

    def page_program(self, address: int, buffer: bytes):
        """Program a page (usually 256 bytes) into the flash device.

           :param address: address of the first byte to program
           :param buffer: the page content
        """
        addr = spack('>I', address)
        if not self.is_4b_addr:
            if address >= (1 << 24):
                raise ValueError('Cannot encode address')
            addr = addr[1:]
        self.transmit(self.COMMANDS['PAGE_PROGRAM'], b''.join((addr, buffer)))

    def power_down(self):
        """Power down the device (may trigger a QEMU shurtdown)."""
        self.transmit(self.COMMANDS['POWER_DOWN'])

    def chip_erase(self):
        """Erase the content of the whole chip."""
        self.transmit(self.COMMANDS['CHIP_ERASE'])

    def block_erase(self, address: int):
        """Erase a 64 KiB block."""
        address &= ~(64 << 10)
        addr = spack('>I', address)
        if not self.is_4b_addr:
            if address >= (1 << 24):
                raise ValueError('Cannot encode address')
            addr = addr[1:]
        self.transmit(self.COMMANDS['BLOCK_ERASE'], addr)

    def sector_erase(self, address: int):
        """Erase a 4 KiB block."""
        address &= ~(4 << 10)
        addr = spack('>I', address)
        if not self.is_4b_addr:
            if address >= (1 << 24):
                raise ValueError('Cannot encode address')
            addr = addr[1:]
        self.transmit(self.COMMANDS['SECTOR_ERASE'], addr)

    def reset(self):
        """Reset the flash device."""
        # self.transmit(self.COMMANDS['RESET1'])
        self.transmit(self.COMMANDS['RESET2'])

    def read(self, address: int, length: int, fast: bool = False) -> bytes:
        """Read out from the flash device.

           :param address: the address of the first byte to read
           :param length: how many bytes to read
           :param fast: whether to use the fast SPI read command
        """
        if fast:
            cmd = self.COMMANDS['FAST_READ']
            dummy = True
        else:
            cmd = self.COMMANDS['READ_DATA']
            dummy = False
        addr = spack('>I', address)
        if not self.is_4b_addr:
            if address >= (1 << 24):
                raise ValueError('Cannot encode address')
            addr = addr[1:]
        if dummy:
            addr.append(0)
        return self.transmit(cmd, addr, length)

    @staticmethod
    def rev8(num: int) -> int:
        """Bit-wise reversal for a byte."""
        num = ((num >> 1) & 0x55) | ((num & 0x55) << 1)
        num = ((num >> 2) & 0x33) | ((num & 0x33) << 2)
        num = ((num >> 4) & 0x0f) | (num << 4)
        return num & 0xff

    def enable_rx_lsb(self, enable: bool):
        """Change RX bit ordering."""
        self._rev_rx = enable
        if enable:
            self._mode |= 1 << 3
        else:
            self._mode &= ~(1 << 3)

    def enable_tx_lsb(self, enable: bool):
        """Change TX bit ordering."""
        self._rev_tx = enable
        if enable:
            self._mode |= 1 << 2
        else:
            self._mode &= ~(1 << 2)

    def check_answer(self) -> None:
        """Check is the remote peer has data to read.
           This is a proprietary (i.e. non *25 series standard) command.

           Caller should call wait_idle() till busy bit is released.
        """
        self.transmit(self.COMMANDS['CHECK_ANSWER'])

    @property
    def is_4b_addr(self) -> bool:
        """Report wether 4-byte addressing mode is active."""
        return self._4ben

    def _build_cs_header(self, size: int, release: bool = True) -> bytes:
        mode = self._mode & 0xf
        if not release:
            mode |= 0x80
        header = spack(self.CS_HEADER_FMT, b'/CS', self.VERSION, mode,
                       size)
        self._log.debug('Header: %s', hexlify(header).decode())
        return header

    def _send(self, buf: bytes, release: bool = True):
        data = b''.join((self._build_cs_header(len(buf), release), buf))
        self._log.debug('TX[%d]: %s %s', len(buf), hexlify(buf).decode(),
                        '/' if release else '...')
        self._socket.send(data)

    def _receive(self, size: int) -> bytes:
        buf = bytearray()
        rem = size
        timeout = now() + self.TIMEOUT
        poller = spoll()
        poller.register(self._socket, POLLIN)
        while rem:
            for _ in poller.poll(self.POLL_TIMEOUT):
                try:
                    data = self._socket.recv(rem)
                    buf.extend(data)
                    rem -= len(data)
                    if rem == 0:
                        break
                except (TimeoutError, LegacyTimeoutError):
                    self._log.error('Unexpected timeout w/ poll')
                    raise
            if rem == 0:
                break
            if now() > timeout:
                raise TimeoutError('Failed to receive response')
        else:
            raise TimeoutError(f'{"No" if len(buf) == 0 else "Truncated"} '
                               f'response from host')
        self._log.debug("RX[%d]: %s", len(buf), hexlify(buf).decode())
        return bytes(buf)

    def _exchange(self, requ: bytes, release: bool = True) -> bytes:
        txlen = len(requ)
        self._send(requ, release)
        resp = self._receive(txlen)
        rxlen = len(resp)
        if rxlen != txlen:
            raise RuntimeError(f'Response truncated {rxlen}/{txlen}')
        return resp
