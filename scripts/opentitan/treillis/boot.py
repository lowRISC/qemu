# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Configuration file for CircuitPython."""

# pylint: disable=import-error

import usb_cdc
import usb_midi

usb_midi.disable()
usb_cdc.enable(console=True, data=True)
