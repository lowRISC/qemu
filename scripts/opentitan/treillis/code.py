# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Simple CircuitPython script that maps QEMU GPIO chardev backend device onto
   a NeoTreillis M4 Express device.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

# pylint: disable=import-error
# pylint: disable=invalid-name
# pylint: disable=missing-function-docstring
# pylint: disable=consider-using-f-string
# pylint: disable=missing-class-docstring
# pylint: disable=protected-access
# pylint: disable=attribute-defined-outside-init

try:
    from time import monotonic_ns as now
    import usb_cdc
    from adafruit_trellism4 import TrellisM4Express
except ImportError:
    print('This code should run on Adafruit NeoTrellis M4 Express with '
          'CircuitPython')
    raise


class OtGPIO:
    """OpenTitan GPIO interface with an Adafruit NeoTrellis M4 Express.

       Demonstrate the GPIO protocol over a QEMU CharDev.
    """

    GPO_ON = (20, 0, 0)  # red
    GPO_OFF = (0, 20, 0)  # green
    GPI_ON = (20, 20, 0)  # bright blue
    GPI_OFF = (0, 0, 80)  # yellow
    GP_HIZ = (1, 1, 1)  # greyish
    GP_PU = (15, 5, 0)  # orange
    GP_PD = (0, 8, 3)  # greenish-blue

    LOCK_TIME_MS = 300   # key depressed time to lock/unlock an input key

    def __init__(self, serial):
        self._serial = serial
        self._trellis = TrellisM4Express()
        # be sure to disable auto-refresh, as this causes a full matrix
        # dump on each pixel change
        self._trellis.pixels.auto_write = False
        pixels = self._trellis.pixels
        self._cache = [(0, 0, 0)] * pixels.width * pixels.height
        # bypass the Adafruit API which is far too slow
        self._neopixels = pixels._neopixel
        self._wipe()

    def _wipe(self):
        # 32-bit bitmaps
        self._oe = 0  # output enable (1: out, 0: in)
        self._out = 0  # output value (from peer)
        self._hiz = 0xfffffff  # high-Z
        self._wpud = 0  # weak pull (1: up 0: down)
        self._inact = 0  # input activated
        self._in = 0  # input value (to peer)
        self._kin = 0  # keyboard input
        pixels = self._trellis.pixels
        self._cache = [(0, 0, 0)] * pixels.width * pixels.height
        self._lock_in = 0  # locked keys
        self._lock_time = {}  # when key has been first pressed (ns timestamp)

    def _update_input(self, newval, force=False):
        # pylint: disable=unused-argument
        ts = now()
        change = self._kin ^ newval
        self._inact |= change
        self._kin = newval
        # handle flip-flop: if a key is pressed long enough, its new value
        # is stored
        for pos in range(32):
            bit = 1 << pos
            # only consider keys that change
            if not bit & change:
                continue
            # is the key down?
            down = bool(bit & newval)
            if down:
                self._lock_time[pos] = ts
                continue
            # key is released
            dtime = self._lock_time.get(pos)
            if dtime is None:
                continue
            delay_ms = (ts - dtime) // 1_000_000
            if delay_ms > self.LOCK_TIME_MS:
                # Lock action
                on = bool(self._lock_in & bit)
                if on:
                    self._lock_in &= ~bit
                else:
                    self._lock_in |= bit
                self._lock_time.pop(pos)
        self._in = self._lock_in ^ newval
        return change

    def _update_output(self, newval):
        change = (self._out ^ newval) & self._oe
        self._out = newval
        return change

    def _refresh_input(self, update):
        lastpix = len(self._cache) - 1
        for pos in range(32):
            bit = 1 << pos
            if not bit & update:
                continue
            if self._inact & bit:
                if self._in & bit:
                    color = self.GPI_ON
                else:
                    color = self.GPI_OFF
            else:
                if self._hiz & bit:
                    color = self.GP_HIZ
                else:
                    if self._wpud & bit:
                        color = self.GP_PU
                    else:
                        color = self.GP_PD
            pixel = lastpix - pos
            if self._cache[pixel] != color:
                self._neopixels[pixel] = color
                self._cache[pixel] = color

    def _refresh_output(self, update):
        lastpix = len(self._cache) - 1
        for pos in range(32):
            bit = 1 << pos
            if not update & bit:
                continue
            if self._out & bit:
                color = self.GPO_ON
            else:
                color = self.GPO_OFF
            pixel = lastpix - pos
            if self._cache[pixel] != color:
                self._neopixels[pixel] = color
                self._cache[pixel] = color

    def run(self):
        self._serial.timeout = 0.005
        self._serial.write_timeout = 0.5
        # query QEMU to repeat I/O config on startup.
        self._serial.write(b'R:00000000\r\n')
        buf = bytearray()
        last_kin = 0
        last_kact = 0
        force = False
        change = False
        while True:
            if change:
                self._trellis.pixels.show()
                change = False
            kin = 0
            for x, y in self._trellis.pressed_keys:
                kin |= 1 << (31 - (8 * y + x))
            if last_kin != kin:
                change = self._update_input(kin)
                if not force:
                    change &= ~self._oe
                self._refresh_input(change)
                if last_kact != self._inact:
                    self._serial.write(b'M:%08x\r\n' % ~self._inact)
                    last_kact = self._inact
                self._serial.write(b'I:%08x\r\n' % self._in)
            last_kin = kin
            data = self._serial.read()
            if not data:
                self._serial.timeout = 0.005
                continue
            self._serial.timeout = 0
            for b in data:
                if b != 0x0d:
                    buf.append(b)
            pos = buf.find(b'\n')
            if pos < 0:
                continue
            line = bytes(buf[:pos])
            buf = buf[pos+1:]
            if not line:
                continue
            if len(line) < 2 or line[1] != ord(':'):
                continue
            cmd = line[0]
            try:
                val = int(line[2:], 16)
            except ValueError:
                continue
            change = True
            if cmd == ord('D'):
                # update I/O direction
                self._oe = val
                self._refresh_output(self._oe)
                self._refresh_input(~self._oe)
            elif cmd == ord('O'):
                # update output
                change = self._update_output(val)
                self._refresh_output(change)
            elif cmd == ord('Z'):
                # update high-z
                self._hiz = val
                self._refresh_input(~self._oe)
            elif cmd == ord('P'):
                # update pull-up/pull-down
                self._wpud = val
                self._refresh_input(~self._oe)
            elif cmd == ord('C'):
                self._wipe()
                self._refresh_output(self._oe)
                self._refresh_input(~self._oe)
            elif cmd == ord('Q'):
                # QEMU query for current Input state
                self._serial.write(b'M:%08x\r\n' % ~self._inact)
                self._serial.write(b'I:%08x\r\n' % self._in)
            else:
                print('Unknown command %s' % cmd)


if __name__ == '__main__':
    if not usb_cdc.data:
        # boot.py should be used to enable the secondary, data CDC
        # serial-over-USB device. The first port is reserved for the Console.
        raise RuntimeError('No serial port available')
    gpio = OtGPIO(usb_cdc.data)
    gpio.run()
