/*
 * QEMU OpenTitan EarlGrey GPIO device
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
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

#ifndef HW_OPENTITAN_OT_GPIO_EG_H
#define HW_OPENTITAN_OT_GPIO_EG_H

#include "hw/opentitan/ot_gpio.h"

#define TYPE_OT_GPIO_EG "ot-gpio-eg"
OBJECT_DECLARE_TYPE(OtGpioEgState, OtGpioEgClass, OT_GPIO_EG)

void ot_gpio_eg_notify_sysrst_change(void);
int ot_gpio_eg_get_mio_pad_in(unsigned mio_pad);
uint64_t ot_gpio_eg_get_total_us(OtGpioEgState *s);
void ot_gpio_eg_set_pattgen_batch(OtGpioEgState *s, bool active);
void ot_gpio_eg_notify_pattgen_change(OtGpioEgState *s, uint64_t step_us);

#endif /* HW_OPENTITAN_OT_GPIO_EG_H */
