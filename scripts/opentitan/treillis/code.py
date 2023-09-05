"""Simple CircuitPython script (which is stuck with Python 3.4) that maps
   QEMU GPIO chardev backend device onto a NeoTreillis M4 Express device.
"""

#pylint: disable=import-error
#pylint: disable=invalid-name
#pylint: disable=missing-function-docstring
#pylint: disable=consider-using-f-string
#pylint: disable=missing-class-docstring
#pylint: disable=too-few-public-methods
#pylint: disable=too-many-branches
#pylint: disable=too-many-instance-attributes


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
    GPO_OFF = (0, 20, 0) # green
    GPI_ON = (0, 0, 80)  # bright blue
    GPI_OFF = (2, 2, 2)  # greyish

    LOCK_TIME_MS = 300   # key depressed time to lock/unlock an input key

    def __init__(self, serial):
        self._serial = serial
        self._trellis = TrellisM4Express()
        # 32-bit bitmaps
        self._oe = 0  # output enable (1: out, 0: in)
        self._out = 0  # output value (from peer)
        self._in = 0  # input value (to peer)
        self._kin = 0  # keyboard input
        self._lock_in = 0  # locked keys
        self._lock_time = {}  # when key has been first pressed (ns timestamp)

    def _update_input(self, newval, force=False):
        ts = now()
        change = self._kin ^ newval
        self._kin = newval
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

    def _refresh_input(self, change):
        for pos in range(32):
            bit = 1 << pos
            if not bit & change:
                continue
            y = pos >> 3
            x = pos & 0x7
            if self._in & bit:
                color = self.GPI_ON
            else:
                color = self.GPI_OFF
            self._trellis.pixels[7-x, 3-y] = color

    def _refresh_output(self, change):
        for pos in range(32):
            bit = 1 << pos
            if not change & bit:
                continue
            y = pos >> 3
            x = pos & 0x7
            if self._out & bit:
                color = self.GPO_ON
            else:
                color = self.GPO_OFF
            self._trellis.pixels[7-x, 3-y] = color

    def run(self):
        self._serial.timeout = 0.1
        self._serial.write_timeout = 0.5
        # query QEMU to repeat I/O config on startup.
        self._serial.write(b'R:00000000\r\n')
        buf = bytearray()
        last_kin = 0
        force = False
        while True:
            kin = 0
            for x, y in self._trellis.pressed_keys:
                kin |= 1 << (31 - (8 * y + x))
            if last_kin != kin:
                change = self._update_input(kin)
                if not force:
                    change &= ~self._oe
                self._refresh_input(change)
                self._serial.write(b'I:%08x\r\n' % self._in)
            last_kin = kin
            data = self._serial.read()
            if not data:
                continue
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
            if cmd == ord('D'):
                # update I/O direction
                self._oe = val
                self._refresh_output(self._oe)
                self._refresh_input(~self._oe)
            elif cmd == ord('O'):
                # update output
                change = self._update_output(val)
                self._refresh_output(change)
            elif cmd == ord('Q'):
                # QEMU query for current Input state
                self._serial.write(b'I:%08x\r\n' % self._in)
            else:
                print('Unknown command %s' % cmd)


if __name__ == '__main__':
    if not usb_cdc.data:
        # boot.py should be used to enable the secondary, data CDC serial-over-USB
        # device. The first port is reserved for the Console.
        raise RuntimeError('No serial port available')
    gpio = OtGPIO(usb_cdc.data)
    gpio.run()
