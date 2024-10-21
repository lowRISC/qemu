# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Device proxy for OpenTitan devices and peripherals

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import hexlify, unhexlify
from collections import deque
from enum import IntEnum
from logging import getLogger
from socket import create_connection, socket, SHUT_RDWR
from struct import calcsize as scalc, pack as spack, unpack as sunpack
from sys import modules
from threading import Event, Thread, get_ident
from time import sleep, time as now
from typing import Any, Callable, Iterator, NamedTuple, Optional, Union

from .mailbox.doe import DOEHeader

try:
    from serial import Serial, serial_for_url
except ImportError:
    Serial = None
    serial_for_url = None


class ProxyCommandError(Exception):
    """Signal a remote proxy command execution error.
    """
    ERRORS = {
        # No error
        0: 'PE_NO_ERROR',
        # Undefined errors
        1: 'PE_UNKNOWN',
        # Request errors
        0x101: 'PE_INVALID_COMMAND_LENGTH',
        0x102: 'PE_INVALID_COMMAND_CODE',
        0x103: 'PE_INVALID_REQUEST_ID',
        0x104: 'PE_INVALID_SPECIFIER_ID',
        0x105: 'PE_INVALID_DEVICE_ID',
        0x106: 'PE_INVALID_IRQ',
        0x107: 'PE_INVALID_REG_ADDRESS',
        # State error
        0x201: 'PE_DEVICE_IN_ERROR',
        # Local error
        0x401: 'PE_CANNOT_READ_DEVICE',
        0x402: 'PE_CANNOT_WRITE_DEVICE',
        0x403: 'PE_TRUNCATED_RESPONSE',
        0x404: 'PE_INCOMPLETE_WRITE',
        0x405: 'PE_OOM',
        # internal error
        0x801: 'PE_UNSUPPORTED_DEVICE',
        0x802: 'PE_DUPLICATED_UID'
    }
    """Proxy error codes."""

    def __init__(self, errcode: int, msg: Optional[str] = None):
        err = self.ERRORS.get(errcode, 'PE_UNKNOWN')
        errmsg = f'{err}: {msg}' if msg else err
        super().__init__(errcode, errmsg)

    @property
    def code(self) -> int:
        """Return the code of the exception."""
        return self.args[0]

    @classmethod
    def get_error_code(cls, name: str) -> Optional[int]:
        """Retrieve the error code from its name

           :param name: error name
           :return: the matching error code if any
        """
        errors = {v: k for k, v in cls.ERRORS.items()}
        if name not in errors:
            getLogger('proxy.proxy').error("Unknown error '{name}'")
            return None
        return errors[name]

    def __eq__(self, other: 'ProxyCommandError') -> bool:
        return self.code == other.code


class ProxyDeviceError(ProxyCommandError):
    """Locallly generated ProxyCommandError when remote device reports iself
       on error.
    """

    def __init__(self):
        super().__init__(0x201, 'Device On Error')


class MemoryRoot(NamedTuple):
    """A root memory region.
    """
    name: str
    address: int
    size: int


class InterruptGroup(NamedTuple):
    """Descriptor of a named group of interrupts.
       Interrupts follow the meaning of QEMU IRQ: an "asynchronous" wire.
       They are not only "interrupts" between a device and an interrupt
       controller.
    """
    out: bool  # output or input IRQ group
    num: int  # group identifier
    count: int  # number of IRQ lines in the group


class MemoryHitEvent(NamedTuple):
    """Event triggered when a memory watcher has been matched."""
    address: int
    value: int
    write: bool
    width: int
    role: int


InterruptHandler = Callable[['DeviceProxy', str, int, int, Any], None]
"""qemu_irq handler (device, group name, channel, value, *args)
"""

RequestHandler = Callable[[str, bytes, Any], None]
"""Remote request handler."""


MemoryWatcherHandler = Callable[[MemoryHitEvent, Any], None]
"""Memory Watcher handler."""


class DeviceProxy:
    """Remote device wrapper.

       Expose a generic API to manage a remove QEMU DeviceState (mostly
       SysBusDeviceState)

       :param proxy: the parent communication proxy
       :param name: an arbitrary name to identify this device
       :param devid: unique device identifier for the communication proxy
       :param base: base address of the device in CPU space
       :param offset: relative base address for the first register in the remote
                    device (usually 0)
       :paran regcount: count of 32-bit registers in the remote device
    """

    MB4_TRUE = 0x6
    """Multibit bool true value."""

    MB4_FALSE = 0x9
    """Multibit bool false value."""

    NO_ROLE = 0xf
    """Disabled role."""

    IRQ_NAME = 'sysbus-irq'
    """SysBus Interrupt IRQ identifier."""

    ALERT_NAME = 'ot-alert-sig'
    """OpenTitan Alert IRQ identifier."""

    def __init__(self, proxy: 'ProxyEngine', name: str, devid: int, addr: int,
                 count: int, offset: int):
        logname = self.__class__.__name__.lower().replace('proxy', '')
        self._log = getLogger(f'proxy.{logname}')
        if offset > 0xffff:
            raise ValueError(f'Invalid address 0x{offset:02x}')
        self._proxy = proxy
        self._name = name
        self._devid = devid
        self._addr = addr
        self._regcount = count
        self._offset = offset * 4
        self._end = offset + count * 4
        self._interrupts: Optional[dict[str, InterruptGroup]] = None
        self._interrupt_handler: Optional[InterruptHandler] = None
        self._interrupt_args: list[Any] = []
        self._new()

    @property
    def proxy(self) -> 'ProxyEngine':
        """Return the proxy engine."""
        return self._proxy

    @property
    def uid(self) -> int:
        """Provide the unique device identifier in this environment."""
        return self._devid

    def __str__(self) -> str:
        return f'{self.__class__.__name__} ({self._devid})'

    def read_word(self, role: int, addr: int) -> int:
        """Read a single 32-bit word from the device.

           :param role: the control access role identifier.
           :param addr: the address (in bytes) of the register to read from
           :return: the 32-bit value of the device register
        """
        if not self._offset <= addr < self._end or addr & 0x3:
            raise ValueError(f'Invalid address 0x{addr:02x}')
        request = spack('<HH', addr >> 2, self._make_sel(self._devid, role))
        response = self._proxy.exchange('RW', request)
        value, = sunpack('<I', response)
        return value

    def write_word(self, role: int, addr: int, value: int,
                   mask: int = 0xffffffff) -> None:
        """Write a single 32-bit word from the device.

           :param role: the control access role identifier.
           :param addr: the address (in bytes) of the register to write to
           :param value: the 32-bit value to write into the register
           :param mask: an optional mask. Only set bits should be updated.
                        default to all 32 bits. If the remote device does not
                        support setters, a read-modify-write sequence should be
                        used.
        """
        if not self._offset <= addr < self._end or addr & 0x3:
            raise ValueError(f'Invalid address 0x{addr:02x}')
        request = spack('<HHII', addr >> 2, self._make_sel(self._devid, role),
                        value, mask)
        self._proxy.exchange('WW', request)

    def read_buf(self, cmd: str, role: int, addr: int, dwcount: int) -> bytes:
        """Read a sequence of 32-bit words from the device.

           This function is barely used directly, it is usually called from a
           high level function that provides the command to use to perform the
           read operation.

           :param cmd: the command to use (see Protocol documentation)
           :param role: the control access role identifier.
           :param addr: the address (in bytes) of the 1st register to read from
           :param dwcount: the count of 32-bit words to read out
           :return: the read words as a sequence of bytes (LE encoding)
        """
        if not self._offset <= addr < self._end or addr & 0x3:
            raise ValueError(f'Invalid address 0x{addr:02x}')
        request = spack('<HHI', addr >> 2, self._make_sel(self._devid, role),
                        dwcount)
        response = self._proxy.exchange(cmd, request)
        return response

    def write_buf(self, cmd: str, role: int, addr: int, buffer: bytes) -> int:
        """Write a sequence of 32-bit words tp the device.

           This function is barely used directly, it is usually called from a
           high level function that provides the command to use to perform the
           write operation.

           :param cmd: the command to use (see Protocol documentation)
           :param role: the control access role identifier.
           :param addr: the address (in bytes) of the 1st register to write to
           :param buffer: the buffer of bytes to write to the device. Note that
                          if the buffer length is not a multiple of 4 bytes, it
                          is padded with trailing nul bytes
           :return: count of written bytes (should be equal to buf length)
        """
        if not self._offset <= addr < self._end or addr & 0x3:
            raise ValueError(f'Invalid address 0x{addr:02x}')
        if not isinstance(buffer, (bytes, bytearray)):
            raise ValueError('Invalid buffer type')
        trailing = len(buffer) & 0x3
        if trailing:
            buffer = b''.join((buffer, bytes(4-trailing)))
        request = b''.join((spack('<HH', addr >> 2,
                            self._make_sel(self._devid, role)), buffer))
        response = self._proxy.exchange(cmd, request)
        respsize = scalc('<I')
        if len(response) != respsize:
            raise ProxyCommandError(0x403, f'expected {respsize}, '
                                           f'received {len(response)}')
        value, = sunpack('<I', response)
        return value * 4

    def register_interrupt_handler(self, handler: InterruptHandler, *args) \
            -> None:
        """Register a handler to receive notification from the remote peer.

           :param handler: the handler function
           :param args: any argument to forward to the handler
        """
        if self._interrupt_handler:
            self._log.warning('Overridding previous IRQ handler')
        self._interrupt_handler = handler
        self._interrupt_args = args

    def intercept_interrupts(self, group: int, mask: int) -> None:
        """Request the remote proxy to intercept output interrupt for the device

           Interrupted are reported to the proxy rather than delivered them to
           their default destination. This means that the default receiver no
           longer receive the selected interrtupts till they are explicitly
           release, see #release_interrupts

           It is safe to intercept the same interrupts several time (in which
           case the remote proxy should ignore the requests for these IRQ
           channels and intercept the selected ones that were not intercepted)

           :param group: IRQ group identifier
           :param mask: a bitmask of interrupt (0..31) to intercept.
        """
        if not isinstance(group, int) or not 0 <= group <= 0xff:
            raise ValueError(f'Invalid group value {group}')
        if not isinstance(mask, int) or not 0 <= mask <= 0xffff_ffff:
            raise ValueError(f'Invalid interrupt mask {mask}')
        request = spack('<BxHI', group, self._make_sel(self._devid), mask)
        self._proxy.exchange('II', request)

    def release_interrupts(self, group: int, mask: int) -> None:
        """Release previously intercepted IRQ lines

           Interrupted are connected back to their initial destination device.

           :param group: IRQ group identifier
           :param mask: a bitmask of interrupt (0..31) to release.
        """
        if not isinstance(group, int) or not 0 <= group <= 0xff:
            raise ValueError(f'Invalid group value {group}')
        if not isinstance(mask, int) or not 0 <= mask <= 0xffff_ffff:
            raise ValueError(f'Invalid interrupt mask {mask}')
        request = spack('<BxHI', group, self._make_sel(self._devid), mask)
        self._proxy.exchange('IR', request)

    def capture_sysbus_irq(self, irq: int, enable: bool) -> None:
        """Capture or release one or more sysbus interrupts output channels.

           :param irq: the interrupt number to manage
           :param enable: whether to intercept or release IRQ channels.
        """
        self.capture_sysbus_irqs(1 << irq, enable)

    def capture_sysbus_irqs(self, irq_mask: int, enable: bool) -> None:
        """Capture or release one or more sysbus interrupts output channels.

           :param irq: a bitfield of interrupt channel to manage
           :param enable: whether to intercept or release IRQ channels.
        """
        if self._interrupts is None:
            self._enumerate_interrupts()
        try:
            gnum = self._interrupts[self.IRQ_NAME].num
        except KeyError as exc:
            raise ValueError(f'No {self.IRQ_NAME} interruption support for '
                             f'{self.__class__.__name__}') from exc
        if enable:
            self.intercept_interrupts(gnum, irq_mask)
        else:
            self.release_interrupts(gnum, irq_mask)

    def enumerate_interrupts(self, out: bool) -> Iterator[InterruptGroup]:
        """Enumerate supported interrupt lines.

           :param out: True to enumerate output IRQ lines, False for input ones
           :yield: enumerated tuples that contain the IRQ group name, and the
                   count of interrupt line in this group.
        """
        if self._interrupts is None:
            self._enumerate_interrupts()
        for name, group in self._interrupts.items():
            if group.out == out:
                yield name, group

    def signal_interrupt(self, group: str, irq: int, level: int | bool) -> None:
        """Set the level of an input interrupt line.

           :param group: the name of the group
           :param irq: the IRQ line to signal
           :param level: the new level for this IRQ line

           The group name may be retrieved with the #enumerate_interrupts API.
        """
        if self._interrupts is None:
            self._enumerate_interrupts()
        if group not in self._interrupts:
            raise ValueError(f"No such interrupt group '{group}'")
        grp = self._interrupts[group]
        if irq >= grp.count:
            raise ValueError(f"No such interrupt {irq} in '{group}' "
                             f"(<{grp.count})")
        level = int(level)
        if level >= (1 << 32):
            raise ValueError(f'Invalied interrupt level {level}')
        request = spack('<HHHHI', grp.num, self._make_sel(self._devid), irq, 0,
                        level)
        try:
            self._proxy.exchange('IS', request)
        except ProxyCommandError as exc:
            self._log.fatal('%s', exc)
            raise

    def notify_interrupt(self, group: int, channel: int, value: int) -> None:
        """Wired IRQ notification handler."""
        if not self._interrupt_handler:
            self._log.warning('Missed IRQ notification')
            return
        if self._interrupts is None:
            self._log.error('IRQ received w/o registration')
            return
        for gname, igroup in self._interrupts.items():
            if igroup.num == group:
                if channel >= igroup.count:
                    self._log.error('Interrupt %s out of bound: %d', gname,
                                    channel)
                    return
                self._interrupt_handler(self, gname, channel, value,
                                        *self._interrupt_args)
                break
        else:
            self._log.error('Unknow interrupt group %d, ignored', igroup)

    @classmethod
    def _make_sel(cls, device: int, role: int = 0xf) -> int:
        if not isinstance(device, int) or not 0 <= device <= 0xfff:
            raise ValueError('Invalid device identifier')
        if not isinstance(role, int) or not 0 <= role <= 0xf:
            raise ValueError('Invalid device identifier')
        return device | (role << 12)

    def _new(self):
        self._log.debug('Device %s @ 0x%08x: %s',
                        str(self), self._addr, self._name)

    def _enumerate_interrupts(self) -> None:
        # lazy initialization interrupt enumeration is rarely used
        request = spack('<HH', 0, self._make_sel(self._devid))
        try:
            irqgroups = self._proxy.exchange('IE', request)
        except ProxyCommandError as exc:
            self._log.fatal('%s', exc)
            raise
        grpfmt = '<HBB32s'
        grplen = scalc(grpfmt)
        if len(irqgroups) % grplen:
            raise ValueError('Unexpected response length')
        grpcount = len(irqgroups) // grplen
        self._log.info('Found %d remote interrupt groups for %s', grpcount,
                       str(self))
        self._interrupts = {}
        while irqgroups:
            grphdr = irqgroups[0:grplen]
            irqgroups = irqgroups[grplen:]
            gcount, gnum, gdir, gname = sunpack(grpfmt, grphdr)
            grpname = gname.rstrip(b'\x00').decode().lower()
            if grpname in self._interrupts:
                self._log.error("Multiple devices w/ identical identifier: "
                                "'%s'", grpname)
            else:
                group = InterruptGroup(bool(gdir & 0x80), gnum, gcount)
                self._interrupts[grpname] = group


class MbxHostProxy(DeviceProxy):
    """DOE Mailbox Host-side proxy.

       Specialized DeviceProxy that helps communication with a remote DOE HOST
       mailbox.

       :param args: forwarded as is to the parent, see #DevicePRoxy
       :param role: optional access role
    """

    DEVICE_ID = 'mbh'

    REGS = {
        'INTR_STATE':  0x00,
        'INTR_ENABLE':  0x04,
        'INTR_TEST':  0x08,
        'ALERT_TEST':  0x0c,
        'CONTROL':  0x10,
        'STATUS':  0x14,
        'ADDRESS_RANGE_REGWEN':  0x18,
        'ADDRESS_RANGE_VALID':  0x1c,
        'IN_BASE_ADDR':  0x20,
        'IN_LIMIT_ADDR':  0x24,
        'IN_WRITE_PTR':  0x28,
        'OUT_BASE_ADDR':  0x2c,
        'OUT_LIMIT_ADDR':  0x30,
        'OUT_READ_PTR':  0x34,
        'OUT_OBJECT_SIZE':  0x38,
        'INTR_MSG_ADDR':  0x3c,
        'INTR_MSG_DATA':  0x40,
    }
    """Supported registers."""

    IRQS = {
        'MSGREADY': 0,
        'ABORT': 1,
        'ERROR': 3,
    }
    """Supported IRQ channels."""

    def __init__(self, *args, role: Optional[int] = None):
        super().__init__(*args)
        self._role = 0xff  # ensure it should be defined through set_role
        if role is not None:
            self.set_role(role)

    def set_role(self, role: int):
        """Set the control access role to read/write remote registers."""
        if not isinstance(role, int) or role > 0xf:
            raise ValueError(f'Invalid role {role}')
        self._log.debug('%d', role)
        self._role = role

    @property
    def role(self) -> int:
        """Provide the current role."""
        return self._role

    @property
    def interrupt_state(self) -> int:
        """Report which interrupts are active.

           :return: interrupt state bitfield
        """
        return self.read_word(self._role, self.REGS['INTR_STATE'])

    def enable_interrupts(self, intrs: int) -> None:
        """Enable interrupts.

           :param intrs: the bitfield of interrupts to enable
        """
        self._log.debug('0x%08x', intrs)
        self.write_word(self._role, self.REGS['INTR_ENABLE'], intrs)

    def clear_interrupts(self, intrs: int) -> None:
        """Clear interrupts.

           :param intrs: the bitfield of interrupts to clear
        """
        self._log.debug('0x%08x', intrs)
        self.write_word(self._role, self.REGS['INTR_STATE'], intrs)

    def test_interrupts(self, intrs: int) -> None:
        """Test interrupts.

           :param intrs: the bitfield of interrupts to test
        """
        self._log.debug('0x%08x', intrs)
        self.write_word(self._role, self.REGS['INTR_TEST'], intrs)

    @classmethod
    def intrnames_to_int(cls, *intrnames) -> int:
        """Create a interrupt bitfield from interrupt names.
        """
        value = 0
        for intname in intrnames:
            irq = cls.IRQS.get(intname)
            if irq is None:
                raise ValueError(f"Unkwon interrupt '{intname}'")
            value |= 1 << irq
        return value

    @classmethod
    def is_interrupt(cls, intnum: int, intname: str) -> bool:
        """Tell whether the object ready interrupt is raised.

           :param intrs: the interrupt bitfield
           :return: True if the object ready interrupt is raised
        """
        irqval = cls.IRQS.get(intname)
        if irqval is None:
            raise ValueError(f"Unkwon interrupt '{intname}'")
        return intnum == irqval

    def is_aborted(self) -> bool:
        """Tell whether the system side has aborted the request.

           :return: true if abort is set
        """
        res = (bool)(self.read_word(self._role, self.REGS['CONTROL']) & 0b001)
        self._log.debug('%d', res)
        return res

    def is_on_error(self) -> bool:
        """Tell whether the mailbox has detected an error.

           :return: true if ERROR bit is set
        """
        res = (bool)(self.read_word(self._role, self.REGS['CONTROL']) & 0b010)
        self._log.debug('%d', res)
        return res

    def is_busy(self) -> bool:
        """Tell whether the mailbox is busy.

           :return: true if BUSY bit is set
        """
        res = (bool)(self.read_word(self._role, self.REGS['STATUS']) & 0b001)
        self._log.debug('%d', res)
        return res

    def is_object_ready(self) -> bool:
        """Tell whether the mailbox contains a response.

           :return: true if OBJECT READY bit is set
        """
        res = (bool)(self.read_word(self._role, self.REGS['STATUS']) &
                     (1 << 31))
        self._log.debug('%d', res)
        return res

    def is_sys_interrupt(self) -> bool:
        """Tell whether the system side interrupt has been raised.

           :return: true if INT bit is set
        """
        res = bool(self.read_word(self._role, self.REGS['STATUS']) & 0b010)
        self._log.debug('%d', res)
        return res

    def is_sys_interrupt_enabled(self) -> bool:
        """Tell whether the system side has requested to be signalled with an
           interrupt.

           :return: true if interrupt mode is enabled
        """
        res = (bool)(self.read_word(self._role, self.REGS['STATUS']) & 0b100)
        self._log.debug('%d', res)
        return res

    def acknowledge_abort(self) -> None:
        """Finalize abort handling.
        """
        self._log.debug('')
        self.write_word(self._role, self.REGS['CONTROL'], 0b01, 0b01)

    def set_error(self) -> bool:
        """Signal the request cannot be handled because of an error
        """
        self._log.debug('')
        self.write_word(self._role, self.REGS['CONTROL'], 0b10, 0b10)

    def lock_address_range(self) -> bool:
        """Lock address range (base and limit registers).
        """
        self._log.debug('')
        self.write_word(self._role, self.REGS['ADDRESS_RANGE_REGWEN'],
                        self.MB4_FALSE)

    def is_locked_address_range(self) -> bool:
        """Lock address range (base and limit registers).
        """
        res = self.read_word(self._role, self.REGS['ADDRESS_RANGE_REGWEN']) \
            != self.MB4_TRUE
        self._log.debug('%d', res)
        return res

    def validate_address_range(self, valid: bool):
        """Validate address range (base and limit registers).

           :param valid: whether to validate or invalidate the range
        """
        self._log.debug('%d', valid)
        self.write_word(self._role, self.REGS['ADDRESS_RANGE_VALID'],
                        int(valid))

    def set_base_address(self, write: bool, address: int) -> None:
        """Set the 32-bit base address to the mailbox host register.

           The caller is responsible to first check that a value can be written
           to this register.

           :param write: whether to set the write or read register
           :param address: the 32-bit address to write
        """
        reg = 'IN_BASE_ADDR' if write else 'OUT_BASE_ADDR'
        self._log.debug('%s: 0x%08x', reg, address)
        self.write_word(self._role, self.REGS[reg], address)

    def get_base_address(self, write: int) -> int:
        """Get the 32-bit base address of the mailbox host register.

           :param write: whether to get the write or read register
           :return: the 32-bit address to write
        """
        reg = 'IN_BASE_ADDR' if write else 'OUT_BASE_ADDR'
        res = self.read_word(self._role, self.REGS[reg])
        self._log.debug('%s: 0x%08x', reg, res)
        return res

    def set_limit_address(self, write: bool, address: int) -> None:
        """Set the 32-bit limit address to the mailbox host register.

           The caller is responsible to first check that a value can be written
           to this register.

           :param write: whether to set the write or read register
           :param address: the 32-bit address to write
        """
        reg = 'IN_LIMIT_ADDR' if write else 'OUT_LIMIT_ADDR'
        self._log.debug('%s: 0x%08x', reg, address)
        self.write_word(self._role, self.REGS[reg], address)

    def get_limit_address(self, write: int) -> int:
        """Get the 32-bit limit address of the mailbox host register.

           :param write: whether to get the write or read register
           :return: the 32-bit address to write
        """
        reg = 'IN_LIMIT_ADDR' if write else 'OUT_LIMIT_ADDR'
        res = self.read_word(self._role, self.REGS[reg])
        self._log.debug('%s: 0x%08x', reg, res)
        return res

    def get_mem_pointer(self, write: int) -> int:
        """Get the 32-bit memory address of the mailbox host pointer.

           :param write: whether to get the write or read pointer
           :return: the 32-bit address to write
        """
        reg = 'IN_WRITE_PTR' if write else 'OUT_READ_PTR'
        res = self.read_word(self._role, self.REGS[reg])
        self._log.debug('%s: 0x%08x', reg, res)
        return res

    def get_interrupt_info(self) -> tuple[int, int]:
        """Return the system-side interrupt information.

           :return: address and data values
        """
        data = self.read_buf('RS', self._role, self.REGS['INTR_MSG_ADDR'], 2)
        vals = sunpack('<II', data)
        self._log.debug('0x%08x 0x%08x', vals[0], vals[1])
        return vals

    def set_object_size(self, size: int) -> None:
        """Set the size (in word count) of the response written in memory.

           :param size: the size in word count
        """
        self._log.debug('%d', size)
        self.write_word(self._role, self.REGS['OUT_OBJECT_SIZE'], size)


class MbxSysProxy(DeviceProxy):
    """DOE Mailbox System-side proxy.

       Specialized DeviceProxy that helps communication with a remote DOE SYS
       mailbox.

       :param args: forwarded as is to the parent, see #DevicePRoxy
       :param role: optional access role
    """

    DEVICE_ID = 'mbs'

    REGS = {
        'INTR_MSG_ADDR': 0x00,
        'INTR_MSG_DATA': 0x04,
        'CONTROL': 0x08,
        'STATUS': 0x0c,
        'WRITE_DATA': 0x010,
        'READ_DATA': 0x014,
    }
    """Supported registers."""

    def __init__(self, *args, role: Optional[int] = None):
        super().__init__(*args)
        self._role = 0xff  # ensure it should be defined through set_role
        if role is not None:
            self.set_role(role)

    def set_role(self, role: int):
        """Set the control access role to read/write remote registers."""
        if not isinstance(role, int) or role > 0xf:
            raise ValueError(f'Invalid role {role}')
        self._role = role
        self._log.debug('%d', role)

    def abort(self) -> None:
        """Abort an outstanding DOE request."""
        self._log.debug('')
        self.write_word(self._role, self.REGS['CONTROL'], 0b001, 0b001)

    def go(self) -> None:
        """Tell the mailbox responder that a request is reading to be processed.
        """
        # pylint: disable=invalid-name
        self._log.debug('')
        self.write_word(self._role, self.REGS['CONTROL'], 1 << 31, 1 << 31)

    def enable_interrupt(self, enable: bool) -> None:
        """Tell the DOE mailbox the requester wants to be signalled with an
           interrupt when the response is available.

           :param enable: whether to activate or deactivate this feature
        """
        self._log.debug('%d', enable)
        self.write_word(self._role, self.REGS['CONTROL'],
                        0b0010 if enable else 0, 0b0010)

    def get_status(self) -> int:
        """Retrieve the DOE mailbox status 32-bit register."""
        res = self.read_word(self._role, self.REGS['STATUS'])
        self._log.debug('%d', res)
        return res

    def is_busy(self) -> bool:
        """Report whether the mailbox is busy, not accepting any request."""
        res = bool(self.get_status() & 0b0001)
        self._log.debug('%d', res)
        return res

    def is_interrupt(self) -> bool:
        """Report whether the mailbox has signalled the completion of a
           response.
        """
        res = bool(self.get_status() & 0b0010)
        self._log.debug('%d', res)
        return res

    def is_on_error(self) -> bool:
        """Report whether the mailbox is in error."""
        res = bool(self.get_status() & 0b0100)
        self._log.debug('%d', res)
        return res

    def is_object_ready(self) -> bool:
        """Report whether a request object has been flagged as ready to be
           handled.
        """
        res = bool(self.get_status() & (1 << 31))
        self._log.debug('%d', res)
        return res

    def clear_interrupt(self) -> None:
        """Clear the response completion flag."""
        self._log.debug('')
        self.write_word(self._role, self.REGS['STATUS'], 0b0010, 0b0010)

    def write(self, value: int) -> None:
        """Write a 32-bit word into the mailbox system input register.

           The caller is responsible to first check that a value can be pushed
           into this register.
        """
        self._log.debug('0x%08x', value)
        self.write_word(self._role, self.REGS['WRITE_DATA'], value)

    def read(self) -> int:
        """Read a 32-bit word from the mailbox system output register.

           The caller is responsible to first check that a value can be read
           from this register.
        """
        res = self.read_word(self._role, self.REGS['READ_DATA'])
        self._log.debug('0x%08x', res)
        return res

    def write_buffer(self, pcie: DOEHeader, buffer: bytes) -> int:
        """Write a DOE object to the remote mailbox.

           If the buffer length is not a multiple of 4 bytes, it is padded with
           NUL bytes.

           The length encoded into the pcie header is automatically updated,
           i.e. the caller is only in charge of filling in the vendor identifier
           and the object type.

           Be aware that in this mode, the DOE status is not checked, the
           WRITE_DATA destination register is written blindly. See #write_mbox
        """
        trailing = len(buffer) & 0x3
        if trailing:
            buffer = b''.join((buffer, bytes(4-trailing)))
        dwlen = len(buffer)//4 + DOEHeader.DWSIZE
        pcie.set_dwlength(dwlen)
        buffer = b''.join((pcie.encode(), buffer))
        self._log.debug('len %d', len(buffer))
        return self.write_buf('WS', self._role, self.REGS['WRITE_DATA'], buffer)

    def write_mbox(self, pcie: DOEHeader, buffer: bytes) -> None:
        """Write a DOE object and tell the remote mailbox the DOE object is
           complete and ready to be handled.

           The DOE status register is checked before each write to the
           destination register. Write stops whenever the busy or error bit is
           set.

           If the buffer length is not a multiple of 4 bytes, it is padded with
           NUL bytes.

           The length encoded into the pcie header is automatically updated,
           i.e. the caller is only in charge of filling in the vendor identifier
           and the object type.

           If the whole buffer is sucessfully written into the DOE mailbox, the
           GO bit is automatically flagged to signal the mailbox that the DOE
           object can be handled immediately.
        """
        trailing = len(buffer) & 0x3
        if trailing:
            buffer = b''.join((buffer, bytes(4-trailing)))
        dwlen = len(buffer)//4 + DOEHeader.DWSIZE
        pcie.set_dwlength(dwlen)
        buffer = b''.join((pcie.encode(), buffer))
        self._log.debug('len %d', len(buffer))
        count = self.write_buf('WX', self._role, self.REGS['WRITE_DATA'],
                               buffer)
        if count != len(buffer):
            raise ProxyCommandError(0x402, f'Cannot write full buffer {count}/'
                                    f'{len(buffer)}')

    def read_mbox(self, dwcount: int) -> tuple[DOEHeader, bytes]:
        """Read a DOE object from the remote mailbox.

           Each read word is automatically acknowledged on the remote mailbox,
           and read stops immediately in case of an error.

           :param dwcount: the maximum 32-bit words to read out. The actual DOE
                           object may be shorter.
        """
        buf = self.read_buf('RX', self._role, self.REGS['READ_DATA'],
                            dwcount+DOEHeader.DWSIZE)
        pcie = DOEHeader.decode(buf[:DOEHeader.SIZE])
        self._log.debug('len %d', len(buf)-DOEHeader.SIZE)
        return pcie, buf[DOEHeader.SIZE:]

    def get_interrupt_info(self) -> tuple[int, int]:
        """Return the system-side interrupt information.

           :return: address and data values
        """
        data = self.read_buf('RS', self._role, self.REGS['INTR_MSG_ADDR'], 2)
        vals = sunpack('<II', data)
        self._log.debug('0x%08x 0x%08x', vals[0], vals[1])
        return vals

    def set_interrupt_addr(self, addr: int) -> None:
        """Return the system-side interrupt information.

           :param addr: set interrupt address
        """
        self._log.debug('0x%08x', addr)
        self.write_word(self._role, self.REGS['INTR_MSG_ADDR'], addr)

    def set_interrupt_data(self, data: int) -> None:
        """Return the system-side interrupt information.

           :param addr: set interrupt data
        """
        self._log.debug('0x%08x', data)
        self.write_word(self._role, self.REGS['INTR_MSG_DATA'], data)


class SoCProxy(DeviceProxy):
    """SoC proxy.

       Specialized DeviceProxy that helps managing an SOC PROXY device.

       :param args: forwarded as is to the parent, see #DevicePRoxy
       :param role: optional access role
    """

    DEVICE_ID = 'soc'

    REGS = {
        'INTR_STATE':  0x00,
        'INTR_ENABLE':  0x04,
        'INTR_TEST':  0x08,
        'ALERT_TEST':  0x0c,
    }
    """Supported registers."""

    def __init__(self, *args, role: Optional[int] = None):
        super().__init__(*args)
        self._role = 0xff  # ensure it should be defined through set_role
        if role is not None:
            self.set_role(role)

    def set_role(self, role: int):
        """Set the control access role to read/write remote registers."""
        if not isinstance(role, int) or role > 0xf:
            raise ValueError('Invalid role')
        self._role = role
        self._log.debug('%d', role)

    @property
    def interrupt_state(self) -> int:
        """Report which interrupts are active.

           :return: interrupt state bitfield
        """
        return self.read_word(self._role, self.REGS['INTR_STATE'])

    def enable_interrupts(self, intrs: int) -> None:
        """Enable interrupts.

           :param intrs: the bitfield of interrupts to enable
        """
        self._log.debug('0x%08x', intrs)
        self.write_word(self._role, self.REGS['INTR_ENABLE'], intrs)

    @property
    def enabled_interrupts(self) -> int:
        """Get enabled interrupt channels.
        """
        return self.read_word(self._role, self.REGS['INTR_ENABLE'])

    def clear_interrupts(self, intrs: int) -> None:
        """Clear interrupts.

           :param intrs: the bitfield of interrupts to clear
        """
        self._log.debug('0x%08x', intrs)
        self.write_word(self._role, self.REGS['INTR_STATE'], intrs)

    def test_interrupts(self, intrs: int) -> None:
        """Test interrupts.

           :param intrs: the bitfield of interrupts to test
        """
        self._log.debug('0x%08x', intrs)
        self.write_word(self._role, self.REGS['INTR_TEST'], intrs)

    def test_alerts(self, alerts: int) -> None:
        """Test alerts

           :param alerts: the bitfield of interrupts to test
        """
        self._log.debug('0x%08x', alerts)
        self.write_word(self._role, self.REGS['ALERT_TEST'], alerts)


class MemProxy(DeviceProxy):
    """Memory device proxy.

       Specialized DeviceProxy that helps managing random access memory.

       :param args: forwarded as is to the parent, see #DevicePRoxy
       :param role: optional access role
    """

    DEVICE_ID = 'm'

    def __init__(self, *args, role: Optional[int] = None):
        super().__init__(*args)
        self._role = 0xff  # ensure it should be defined through set_role
        if role is not None:
            self.set_role(role)

    def set_role(self, role: int):
        """Set the control access role to read/write remote registers."""
        if not isinstance(role, int) or role > 0xf:
            raise ValueError(f'Invalid role {role}')
        self._role = role

    @property
    def base_address(self) -> int:
        """Return the base address of the memory device, as seen from the
           local CPU if any, otherwise in the CTN address space.
        """
        return self._addr

    @property
    def size(self) -> int:
        """Return the size in bytes of the memory device.
        """
        return (self._end - self._offset) * 4

    def read(self, address: int, size: int, rel: bool = False) -> bytes:
        """Read a buffer from memory.

           :param address: the address of the first word to read
           :param size: the count of word to read
           :param rel: whether the address is relative or absolute
        """
        if self._role > 0xf:
            raise ValueError(f'Invalid role {self._role}')
        # if specified address is an absolute address, compute the relative
        # address within the device
        addr = (address - self._addr) if not rel else address
        if not 0 <= addr < self.size:
            raise ValueError(f'Invalid address 0x{addr:08x}')
        if size * 4 > self.size:
            raise ValueError(f'Invalid size {size}')
        request = spack('<HHII', 0, self._make_sel(self._devid), addr, size)
        response = self._proxy.exchange('RM', request)
        return response

    def write(self, address: int, buffer: bytes, rel: bool = False) -> None:
        """Write a buffer into memory.

           :param address: the address of the first byte to read
           :param size: the count of bytes to read
           :param rel: whether the address is relative or absolute
        """
        if self._role > 0xf:
            raise ValueError(f'Invalid role {self._role}')
        # if specified address is an absolute address, compute the relative
        # address within the device
        addr = (address - self._addr) if not rel else address
        if not 0 <= addr < self.size:
            raise ValueError(f'Invalid address 0x{address:08x}')
        trailing = len(buffer) & 0x3
        if trailing:
            buffer = b''.join((buffer, bytes(4-trailing)))
        size = len(buffer)
        wsize = size // 4
        if wsize > self.size:
            raise ValueError('Invalid buffer size {size}')
        request = b''.join((spack('<HHI', 0, self._make_sel(self._devid),
                                  addr), buffer))
        response = self._proxy.exchange('WM', request)
        respsize = scalc('<I')
        if len(response) != respsize:
            raise ProxyCommandError(0x403, f'expected {respsize}, '
                                           f'received {len(response)}')
        value, = sunpack('<I', response)
        if value != wsize:
            raise ProxyCommandError(0x404, f'expected {wsize}, wrote {value}')


class SramMemProxy(MemProxy):
    """SRAM memory device proxy.

       Specialized MemProxy that helps managing SRAM.

       :param args: forwarded as is to the parent, see #DevicePRoxy
       :param role: optional access role
    """

    DEVICE_ID = 'srm'


class SramCtrlProxy(DeviceProxy):
    """SRAM controller device proxy.

       Specialized DeviceProxy that help managing SRAM controllers.

       :param args: forwarded as is to the parent, see #DevicePRoxy
       :param role: optional access role
    """

    DEVICE_ID = 'src'

    REGS = {
        'ALERT_TEST': 0x0,
        'STATUS': 0x4,
        'EXEC_REGWEN': 0x8,
        'EXEC': 0xc,
        'CTRL_REGWEN': 0x10,
        'CTRL': 0x14,
        'SCR_KEY_ROTATED': 0x1c,
    }
    """Supported registers."""

    def __init__(self, *args, role: Optional[int] = None):
        super().__init__(*args)
        self._role = 0xff  # ensure it should be defined through set_role
        if role is not None:
            self.set_role(role)

    def set_role(self, role: int):
        """Set the control access role to read/write remote registers."""
        if not isinstance(role, int) or role > 0xf:
            raise ValueError('Invalid role')
        self._role = role
        self._log.debug('%d', role)

    def is_init_done(self) -> bool:
        """Report whether initialization is complete."""
        return bool(self.read_word(self._role, self.REGS['STATUS']) & (1 << 5))

    def is_scr_key_seed_valid(self) -> bool:
        """Report whether scrambing key seed is valid."""
        return bool(self.read_word(self._role, self.REGS['STATUS']) & (1 << 4))

    def is_scr_key_valid(self) -> bool:
        """Report whether scrambing key is valid."""
        return bool(self.read_word(self._role, self.REGS['STATUS']) & (1 << 3))

    def initialize(self, renew_src_key: bool) -> None:
        """Intialize SRAM content."""
        ctrl = 0b10
        if renew_src_key:
            ctrl |= 0b01
        self.write_word(self._role, self.REGS['CTRL'], ctrl)


class ProxyEngine:
    """Tool to access and remotely drive devices and memories.
    """

    VERSION = (0, 15)
    """Protocol version."""

    TIMEOUT = 2.0
    """Default allowed timeout to complete an exchange with the remote target.
    """

    POLL_TIMEOUT = 0.05
    """Maximum time to wait on a blocking operation.
    """

    POLL_RECOVER_DELAY = 0.1
    """Time to wait on a recovery operation.
    """

    EXIT_DELAY = 0.1
    """Time to wait before resuming once the termination command is sent.
    """

    BAUDRATE = 115200
    """Default baudrate for serial line communication."""

    HEADER = '<2sHI'
    """Proxy header."""

    HEADER_SIZE = scalc(HEADER)
    """Proxy header size."""

    NOTIFICATIONS = {
        'W': 'wired_irq',
        'R': 'mr_hit',
    }
    """Notification dispatch map."""

    LOG_CHANNELS = (
        'tb_out_asm',
        'tb_in_asm',
        'tb_op',
        'tb_op_opt',
        'int',
        'exec',
        'pcall',
        'tb_cpu',
        'reset',
        'unimp',
        'guest_error',
        'mmu',
        'tb_nochain',
        'page',
        'trace',
        'tb_op_ind',
        'tb_fpu',
        'plugin',
        'strace',
        'per_thread',
        'tb_vpu',
        'tb_op_plugin',
    )
    """Supported QEMU log channels."""

    class LogOp(IntEnum):
        """Log operations."""
        READ = 0
        ADD = 1
        REMOVE = 2
        SET = 3

    def __init__(self):
        self._log = getLogger('proxy.proxy')
        self._socket: Optional[socket] = None
        self._port: Optional[Serial] = None
        self._resume = False
        self._receiver: Optional[Thread] = None
        self._notifier: Optional[Thread] = None
        self._requ_q = deque()
        self._resp_q = deque()
        self._requ_event = Event()
        self._resp_event = Event()
        self._rx_uid = 0
        self._tx_uid = 0
        self._devices: dict[str, int] = {}
        self._mroots: dict[int, MemoryRoot] = {}
        self._request_handler: Optional[RequestHandler] = None
        self._request_args: list[Any] = []
        self._watchers: dict[int, tuple[MemoryWatcherHandler, Any]] = {}
        self._proxies = self._discover_proxies()

    def connect(self, host: str, port: int) -> None:
        """Open a TCP connection to the remote host.

           @param host the host name
           @paran port the TCP port
        """
        if self._socket or self._port:
            raise RuntimeError('Cannot open multiple comm port at once')
        try:
            self._socket = create_connection((host, port), timeout=self.TIMEOUT)
        except OSError:
            self._log.fatal('Cannot connect to %s:%d', host, port)
            raise
        self._socket.settimeout(self.POLL_TIMEOUT)
        self._kick_off()

    def open(self, url: str) -> None:
        """Open a PySerial communication port with the remote device.

           :param url: a pyserial-supported port URL
        """
        if self._socket or self._port:
            raise RuntimeError('Cannot open multiple comm port at once')
        self._port = serial_for_url(url, timeout=self.POLL_TIMEOUT,
                                    baudrate=self.BAUDRATE)
        self._kick_off()

    def close(self) -> None:
        """Close the connection with the remote device."""
        self._resume = False
        self._log.debug('Closing connection')
        if self._socket:
            self._socket.shutdown(SHUT_RDWR)
            self._socket.close()
            self._socket = None
        if self._port:
            self._port.close()
            self._port = None

    @property
    def socket(self) -> Optional[socket]:
        """Get the current socket to connect to the VM, if any."""
        return self._socket

    def quit(self, code: int) -> None:
        """Tell the remote target to exit the application.
        """
        payload = spack('<i', code)
        self.exchange('QT', payload)
        sleep(self.EXIT_DELAY)
        self.close()

    def resume(self) -> None:
        """Resume execution.
        """
        self.exchange('CX')

    def discover_devices(self) -> None:
        """Enumerate and store supported remote devices.
        """
        try:
            devices = self.exchange('ED')
        except ProxyCommandError as exc:
            self._log.fatal('%s', exc)
            raise
        devfmt = '<HHII16s'
        devlen = scalc(devfmt)
        if len(devices) % devlen:
            raise ValueError('Unexpected response length')
        devcount = len(devices) // devlen
        self._log.info('Found %d remote devices', devcount)
        self._devices.clear()
        while devices:
            devhdr = devices[0:devlen]
            devices = devices[devlen:]
            doff, dnum, daddr, dcount, dname = sunpack(devfmt, devhdr)
            try:
                devname = dname.rstrip(b'\x00').decode().lower()
            except UnicodeDecodeError as exc:
                self._log.critical('Malformed device name (%s): %s',
                                   str(exc), hexlify(dname).decode())
                continue
            if devname in self._devices:
                self._log.error("Multiple devices w/ identical identifier: "
                                "'%s'", devname)
                continue
            dnum &= 0xfff
            kind = devname.split('/', 1)[0]
            class_ = self._proxies.get(kind, DeviceProxy)
            self._devices[devname] = \
                class_(self, devname, dnum, daddr, dcount, doff)

    def discover_memory_spaces(self) -> None:
        """Enumerate and store memory spaces.
        """
        try:
            mregions = self.exchange('ES')
        except ProxyCommandError as exc:
            self._log.fatal('%s', exc)
            raise
        mrfmt = '<xxxBII32s'
        mrlen = scalc(mrfmt)
        if len(mregions) % mrlen:
            raise ValueError('Unexpected response length')
        mrcount = len(mregions) // mrlen
        self._log.info('Found %d remote memory root regions', mrcount)
        self._mroots.clear()
        while mregions:
            mrhdr = mregions[0:mrlen]
            mregions = mregions[mrlen:]
            mspc, address, size, mname = sunpack(mrfmt, mrhdr)
            mrname = mname.rstrip(b'\x00').decode().lower()
            if mspc in self._mroots:
                self._log.error("Multiple memory regions w/ identical "
                                "identifier: '%s' @ %d", mrname, mspc)
                continue
            self._mroots[mspc] = MemoryRoot(mrname, address, size)

    def enumerate_devices(self) -> Iterator[str]:
        """Provide an iterator on discovered devices.
        """
        yield from self._devices

    def enumerate_memory_spaces(self) -> Iterator[int]:
        """Provide an iterator on discovered memory spaces.
        """
        yield from self._mroots

    def get_device_by_name(self, name: str) -> Optional[DeviceProxy]:
        """Retrieve a device proxy from its name."""
        return self._devices.get(name.lower())

    def get_device_by_uid(self, uid: int) -> Optional[DeviceProxy]:
        """Retrieve a device proxy from its uid."""
        for dev in self._devices.values():
            if dev.uid == uid:
                return dev
        return None

    def get_region_by_name(self, name: str, approx: bool = False) \
            -> Optional[MemoryRoot]:
        """Retrieve a memory root region from its name.

           :param name: the name og the region (or a part of its name)
           :param approx: whether to look for a partial name
        """
        candidates = []
        name = name.lower()
        for region in self._mroots.values():
            lrname = region.name.lower()
            if lrname == name:
                return region
            if approx and lrname.find(name) >= 0:
                candidates.append(region)
        if len(candidates) == 1:
            return candidates[0]
        return None

    def get_region_by_uid(self, uid: int) -> Optional[DeviceProxy]:
        """Retrieve a memory root region from its uid."""
        return self._mroots.get(uid)

    def intercept_mmio_access(self, root: Optional[MemoryRoot], address: int,
                              size: int, read: bool, write: bool,
                              handler: MemoryWatcherHandler, *args, **kwargs) \
            -> None:
        """Request the remote proxy to intercept memory/IO access to a specified
           region.

           :param root: the root of the memory region to intercept, maybe None
                        if a single memory root region exist
           :param address: the adress of the first byte to intercept
           :param size: the size of the region to intercept
           :param read: whether to intercept read access
           :param write: whether to intercept write access
           :param kwargs:
              * 'stop': whether to stop auto-discard intercepter after the
                        specified count of access. Use 0 to disable auto-discard
              * 'prority': priority of the intercepter
        """
        if not read and not write:
            raise ValueError('Read or/and Write should be specified')
        priority = kwargs.pop('priority', 1)
        stop = kwargs.pop('stop', 0)
        if kwargs:
            raise ValueError(f'Unknown arguements: {", ".join(kwargs)}')
        if not isinstance(priority, int) or not priority or priority > 0x3f:
            raise ValueError('Invalid priority')
        if not isinstance(stop, int) or stop > 0x3f:
            raise ValueError('Invalid stop value')
        if root is None:
            if len(self._mroots) != 1:
                raise ValueError('Root region should be specified')
            rid = 0
        else:
            for rid, mroot in self._mroots.items():
                if mroot == root:
                    break
            else:
                raise ValueError('Unkown memory region')
        header = 0
        if read:
            header |= 0b01
        if write:
            header |= 0b10
        header |= priority << 2
        header |= stop << 10
        header |= rid << 24
        request = spack('<III', header, address, size)
        response = self.exchange('MI', request)
        if len(response) != scalc('<xxH'):
            raise ProxyCommandError(0x403)
        region, = sunpack('<xxH', response)
        region &= 0x3f
        if region in self._watchers:
            raise ProxyCommandError(0x802)
        self._watchers[region] = (handler, *args)

    def register_request_handler(self, handler: RequestHandler, *args) -> None:
        """Register a handler to receive requests from the remote peer.

           :param handler: the handler function
           :param args: any argument to forward to the handler
        """
        self._request_handler = handler
        self._request_args = args

    def change_log_mask(self, logop: 'ProxyEngine.LogOp', mask: int) -> int:
        """Change the QEMU log mask.

           :param op: the log modification operation
           :param mask: the log mask to apply
           :return: the previous log mask
        """
        if not 0 <= logop <= 3:
            raise ValueError('Invalid log operation')
        if not 0 <= mask < (1 << 30):
            raise ValueError('Invalid log mask')
        req = spack('<I', (logop << 30) | mask)
        try:
            resp = self.exchange('HL', req)
        except ProxyCommandError as exc:
            self._log.fatal('%s', exc)
            raise
        logfmt = '<I'
        loglen = scalc(logfmt)
        if len(resp) != loglen:
            raise ValueError('Unexpected response length')
        return sunpack(logfmt, resp)[0]

    def add_log_sources(self, *sources) -> tuple[int, list[str]]:
        """Add new log sources.

           :param sources: should be one or more from LOG_CHANNELS
           :return the previous sources
        """
        mask = self._convert_log_to_mask(*sources)
        oldmask = self.change_log_mask(self.LogOp.ADD, mask)
        return oldmask, self._convert_mask_to_log(oldmask)

    def remove_log_sources(self, *sources) -> tuple[int, list[str]]:
        """Remove new log sources.

           :param sources: should be one or more from LOG_CHANNELS
           :return the previous sources
        """
        mask = self._convert_log_to_mask(*sources)
        oldmask = self.change_log_mask(self.LogOp.REMOVE, mask)
        return oldmask, self._convert_mask_to_log(oldmask)

    def set_log_sources(self, *sources) -> tuple[int, list[str]]:
        """Set log sources.

           :param sources: should be one or more from LOG_CHANNELS
           :return the previous sources
        """
        mask = self._convert_log_to_mask(*sources)
        oldmask = self.change_log_mask(self.LogOp.SET, mask)
        return oldmask, self._convert_mask_to_log(oldmask)

    def exchange(self, command: str, payload: Optional[bytes] = None) -> bytes:
        """Execute a communication trip with the remote target.

           @param command the command to execute, as a single char
           @param payload optional payload to the command
           @return the target's response
        """
        if get_ident() in (self._receiver.ident, self._notifier.ident):
            raise RuntimeError('Cannot exchange using internal threads')
        try:
            self._resp_event.clear()
            self._send(command, payload or b'')
            timeout = self.TIMEOUT + now()
            while not self._resp_q:
                if not self._resume:
                    raise RuntimeError('Aborted')
                if now() > timeout:
                    self._log.error('Timeout on command completion')
                    self._resume = False
                    raise TimeoutError('No reply from peer')
                if self._resp_event.wait(self.POLL_TIMEOUT):
                    break
            rcmd, payload = self._resp_q.popleft()
            if rcmd != command.lower():
                if rcmd != 'xx':
                    raise ValueError(f"Unexpected command response '{rcmd}'")
                errfmt = '<I'
                errsize = scalc(errfmt)
                if len(payload) < errsize:
                    raise ProxyCommandError(1,
                                            'Rejected command w/ unknown error')
                errcode, = sunpack(errfmt, payload[:errsize])
                errdesc = payload[errsize:].rstrip(b'\x00').decode()
                raise ProxyCommandError(errcode, errdesc)
            return payload
        except Exception:
            self._resume = False
            raise

    def _send(self, command: str, data: bytes):
        """Send a command to the remote target.

           @param command the command to execute, as a dual char string
           @param data the command payload
        """
        if len(command) != 2 or not isinstance(command, str):
            raise ValueError('Invalid command')
        self._tx_uid += 1
        uid = (0 << 31) | (self._tx_uid & ~(1 << 31))
        self._log.debug('TX cmd:%s, len:%d, uid:%d', command, len(data), uid)
        request = spack(self.HEADER, bytes(reversed(command.encode())),
                        len(data), uid)
        request = b''.join((request, data))
        try:
            if self._port:
                self._port.write(request)
            elif self._socket:
                self._socket.send(request)
        except OSError:
            self._log.error("Cannot send command '%s'", command)
            self._resume = False

    def _receive(self):
        """Worker thread that handle data reception.
        """
        buffer = bytearray()
        cmd = ''
        length = 0
        uid = 0
        resp = False
        while self._resume:
            try:
                if self._port:
                    data = self._port.read(128)
                    if data:
                        buffer.extend(data)
                elif self._socket:
                    try:
                        data = self._socket.recv(128)
                        buffer.extend(data)
                    except TimeoutError:
                        pass
                else:
                    raise RuntimeError('No communication channel')
                if not buffer:
                    continue
                if not length:
                    if len(buffer) < self.HEADER_SIZE:
                        continue
                    bcmd, length, uid = sunpack(self.HEADER,
                                                buffer[:self.HEADER_SIZE])
                    resp = not bool(uid >> 31)
                    uid = uid & ~(1 << 31)
                    cmd = bytes(reversed(bcmd)).decode()
                    self._log.debug('RX cmd:%s, len:%d, uid:%d, resp:%d',
                                    cmd, length, uid, resp)
                    buffer = buffer[self.HEADER_SIZE:]
                if len(buffer) < length:
                    continue
                packet = bytes(buffer[:length])
                self._log.debug('RX payload:%s', self.to_str(packet))
                buffer = buffer[length:]
                if resp:
                    if self._tx_uid != uid:
                        raise ValueError('Unexpected TX tracking')
                    queue = self._resp_q
                else:
                    if self._rx_uid != uid:
                        raise ValueError('Unexpected RX tracking')
                    self._rx_uid += 1
                    queue = self._requ_q
                queue.append((cmd, packet))
                cmd = ''
                length = 0
                uid = 0
                resp = False
            # pylint: disable=broad-except
            except Exception as exc:
                # connection shutdown may have been requested
                if self._resume:
                    self._log.fatal('Exception: %s', exc)
                    self._resume = False
                break

    def _notify(self) -> None:
        """Worker thread that handles notifications and remote requests.
        """
        while self._resume:
            try:
                if not self._requ_q:
                    if not self._requ_event.wait(self.POLL_TIMEOUT):
                        continue
                    self._requ_event.clear()
                rcmd, payload = self._requ_q.popleft()
                notify = rcmd.startswith('^')
                if notify:
                    try:
                        handler = self.NOTIFICATIONS[rcmd[1:]]
                    except KeyError:
                        self._log.error('Unknown notification type: %s',
                                        rcmd[1:])
                        continue
                    dispatcher = getattr(self, f'_dispatch_{handler}', None)
                    if not dispatcher:
                        self._log.error('Unsupported notification: %s', handler)
                        continue
                    # pylint: disable=not-callable
                    dispatcher(payload)
                else:
                    if not self._request_handler:
                        self._log.warning('Missed request %s', rcmd)
                        continue
                    self._request_handler(rcmd, payload, *self._request_args)

            # pylint: disable=broad-except
            except Exception as exc:
                self._log.fatal('Exception: %s', exc)
                break

    def _dispatch_wired_irq(self, payload: bytes) -> None:
        wifmt = '<xxHHBxI'
        wisize = scalc(wifmt)
        if len(payload) != wisize:
            self._log.error('Unexpected W notification length')
            return
        devid, channel, group, value = sunpack(wifmt, payload[:wisize])
        devid &= 0xfff
        device = self.get_device_by_uid(devid)
        try:
            device.notify_interrupt(group, channel, value)
        except Exception as exc:
            self._log.critical('Exception in notify_interrupt: %s', exc)
            raise

    def _dispatch_mr_hit(self, payload: bytes) -> None:
        rifmt = '<BxHII'
        risize = scalc(rifmt)
        if len(payload) != risize:
            self._log.error('Unexpected R notification length')
            return
        info, reg, addr, val = sunpack(rifmt, payload[:risize])
        write = bool(info & 0b10)
        width = info >> 4
        wid = reg & 0x3f
        role = reg >> 12
        mhevent = MemoryHitEvent(addr, val, write, width, role)
        try:
            handler, args = self._watchers[wid]
        except KeyError:
            self._log.error('Memory Hit on unknown watcher: %d', wid)
            return
        try:
            handler(mhevent, *args)
        except Exception as exc:
            self._log.critical('Exception in Memory Hit hit handler: %s', exc)
            raise

    def _kick_off(self) -> None:
        """Start engine.
        """
        self._receiver = Thread(target=self._receive, name='receiver',
                                daemon=True)
        self._notifier = Thread(target=self._notify, name='notifier',
                                daemon=True)
        self._resume = True
        self._receiver.start()
        self._notifier.start()
        self._handshake()

    def _handshake(self):
        """Execute a handshake with the remote target, in order to check the
           communication link.
        """
        try:
            payload = self.exchange('HS')
        except ProxyCommandError as exc:
            self._log.fatal('%s', exc)
            raise
        hshdrfmt = '<BBH'
        hshdrlen = scalc(hshdrfmt)
        if len(payload) < hshdrlen:
            raise ValueError('Unexpected response length')
        vmin, vmaj, _ = sunpack(hshdrfmt, payload[:hshdrlen])
        self._log.info('Local version %d.%d, remote version %d.%d',
                       self.VERSION[0], self.VERSION[1], vmaj, vmin)
        if vmaj != self.VERSION[0]:
            raise ValueError('Unsuppported version: {vmaj}.{vmin}')
        if vmin < self.VERSION[1]:
            raise ValueError('Unsuppported version: {vmaj}.{vmin}')

    @classmethod
    def _discover_proxies(cls) -> dict[str, DeviceProxy]:
        """Create a map of device proxies.
        """
        proxymap = {}
        for cname in dir(modules[__name__]):
            if not cname.endswith('Proxy'):
                continue
            class_ = getattr(modules[__name__], cname)
            if isinstance(class_, DeviceProxy):
                continue
            devid = getattr(class_, 'DEVICE_ID', None)
            if not devid or not isinstance(devid, str):
                continue
            proxymap[devid] = class_
        return proxymap

    @classmethod
    def _convert_log_to_mask(cls, *srcs) -> int:
        channels = {l: i for i, l in enumerate(cls.LOG_CHANNELS)}
        try:
            mask = sum(1 << channels[s.lower()] for s in srcs)
        except KeyError as exc:
            raise ValueError("Unknown log channel {exc}") from exc
        return mask

    @classmethod
    def _convert_mask_to_log(cls, logmask: int) -> list[str]:
        srcs = []
        pos = 0
        while logmask:
            mask = 1 << pos
            if logmask & mask:
                srcs.append(cls.LOG_CHANNELS[pos])
                logmask &= ~mask
            pos += 1
        return srcs

    @classmethod
    def to_str(cls, data: Union[bytes, bytearray]) -> str:
        """Convert a byte sequence into an hexadecimal string.

           @param data the bytes to convert
           @return the bytes encoded as a hexadecimal string
        """
        return hexlify(data).decode()

    @classmethod
    def to_bytes(cls, data: str) -> bytes:
        """Convert an hexadecimal string into a byte sequence

           @param data the hexadecimal string to convert
           @return the decoded bytes
        """
        return unhexlify(data)


def _main():
    # pylint: disable=unknown-option-value
    # pylint: disable=possibly-used-before-assignment
    debug = False
    try:
        desc = modules[__name__].__doc__.split('.', 1)[0].strip()
        argparser = ArgumentParser(description=f'{desc} simple test.')
        argparser.add_argument('-d', '--debug', action='store_true',
                               help='enable debug mode')
        args = argparser.parse_args()
        debug = args.debug

        proxy = ProxyEngine()
        proxy.connect('localhost', 8003)
        proxy.discover_devices()
        proxy.discover_memory_spaces()
    # pylint: disable=broad-except
    except Exception as exc:
        print(f'\nError: {exc}', file=stderr)
        if debug:
            print(format_exc(chain=False), file=stderr)
        sysexit(1)
    except KeyboardInterrupt:
        sysexit(2)


if __name__ == '__main__':
    # pylint: disable=ungrouped-imports
    from argparse import ArgumentParser
    from sys import exit as sysexit, stderr
    from traceback import format_exc
    _main()
