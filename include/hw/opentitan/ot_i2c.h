/*
 * QEMU OpenTitan I2C device
 *
 * Copyright (c) 2024-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Duncan Laurie <duncan@rivosinc.com>
 *  Alice Ziuziakowska <a.ziuziakowska@lowrisc.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_OPENTITAN_OT_I2C_H
#define HW_OPENTITAN_OT_I2C_H

#include "qom/object.h"
#include "hw/i2c/i2c.h"
#include "hw/sysbus.h"

#define TYPE_OT_I2C "ot-i2c"
OBJECT_DECLARE_TYPE(OtI2CState, OtI2CClass, OT_I2C)

#define TYPE_OT_I2C_TARGET TYPE_OT_I2C "-target"
OBJECT_DECLARE_SIMPLE_TYPE(OtI2CTarget, OT_I2C_TARGET)

bool ot_i2c_bus_target_is_stretching(I2CBus *bus);
int ot_i2c_bus_target_get_last_ack(I2CBus *bus);
bool ot_i2c_bus_target_check_tx_stretch(I2CBus *bus);
void ot_i2c_bus_target_repeated_start(I2CBus *bus);

bool ot_i2c_is_override_waveform_ready(OtI2CState *s);
bool ot_i2c_is_override_enabled(OtI2CState *s);
void ot_i2c_set_waveform_start_ns(OtI2CState *s, int64_t val_ns, bool relative);
bool ot_i2c_get_override_pin_level(OtI2CState *s, bool is_sda,
                                   int64_t sample_ns);
void ot_i2c_consume_override_waveform(OtI2CState *s);

I2CBus *ot_i2c_get_active_target_bus(I2CBus *default_bus, uint8_t address);
bool ot_i2c_is_target_enabled(OtI2CState *s);
bool ot_i2c_is_target_polling_acq(OtI2CState *s);
void ot_i2c_reset_bitbang_target(OtI2CState *s);
bool ot_i2c_bitbang_target_step(OtI2CState *s, bool scl, bool sda_in);

#endif /* HW_OPENTITAN_OT_I2C_H */
