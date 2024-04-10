# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""System Mailbox.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import hexlify
from logging import getLogger
from time import sleep, time as now


class SysMboxError(RuntimeError):
    """System mailbox error."""


class SysMbox:
    """Mailbox requester API
    """
    def __init__(self):
        self._log = getLogger('mbox.sys')

    @property
    def busy(self) -> bool:
        """Report whether the mailbox is busy."""
        raise NotImplementedError('ABC')

    @property
    def on_error(self) -> bool:
        """Report whether the mailbox is on error."""
        raise NotImplementedError('ABC')

    @property
    def object_ready(self) -> bool:
        """Report whether the mailbox contains a response."""
        raise NotImplementedError('ABC')

    def go(self) -> None:
        """Tell the mailbox to process the request."""
        # pylint: disable=invalid-name
        raise NotImplementedError('ABC')

    def abort(self) -> None:
        """Tell the mailbox to abort the request / clear any error."""
        raise NotImplementedError('ABC')

    def write_word(self, word: int) -> None:
        """Write a single request word into the mailbox.

           It is the caller responsability to check the mailbox is ready to
           receive a new word.
        """
        raise NotImplementedError('ABC')

    def read_word(self, ack: bool = True) -> int:
        """Read a single response word from the mailbox.

           It is the caller responsability to check the mailbox contains a new
           word.
        """
        raise NotImplementedError('ABC')

    def acknowledge_read(self) -> None:
        """Acknowledge the read out of the last retrieved word."""
        raise NotImplementedError('ABC')

    def write(self, request: bytes) -> int:
        """Send a request to the mailbox."""
        if self.busy:
            raise SysMboxError('Mailbox is busy')
        if self.on_error:
            raise SysMboxError('Mailbox is on error')
        trailing = len(request) & 0x3
        if trailing:
            request = b''.join((request, bytes(4-trailing)))
        self._log.info('TX: (%d) %s', len(request), hexlify(request).decode())
        reqlen = 0
        while request:
            chunk, request = request[:4], request[4:]
            self.write_word(int.from_bytes(chunk, 'little'))
            reqlen += 4
        self.go()
        return reqlen

    def read(self) -> bytes:
        """Receive a response from the mailbox."""
        timeout = now() + 2.0
        while True:
            if self.on_error:
                self.abort()
                raise SysMboxError('Mailbox is on error')
            if self.object_ready:
                break
            if now() > timeout:
                raise TimeoutError('No response from mailbox')
            sleep(0.1)
        response = bytearray()
        while self.object_ready:
            chunk = self.read_word()
            response.extend(chunk.to_bytes(4, 'little'))
        self._log.info('RX: (%d) %s', len(response), hexlify(response).decode())
        return bytes(response)

    def exchange(self, request: bytes) -> bytes:
        """Performs a full half-duplex response-request on the mailbox."""
        self.write(request)
        return self.read()
