import usb_cdc
import usb_midi

usb_midi.disable()
usb_cdc.enable(console=True, data=True)
