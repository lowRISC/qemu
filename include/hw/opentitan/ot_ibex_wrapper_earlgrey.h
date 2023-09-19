/*
 * QEMU OpenTitan EarlGrey Ibex Wrapper device
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Loïc Lefort <loic@rivosinc.com>
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

#ifndef HW_OPENTITAN_OT_IBEX_WRAPPER_EARLGREY_H
#define HW_OPENTITAN_OT_IBEX_WRAPPER_EARLGREY_H

#include "qom/object.h"
#include "hw/opentitan/ot_ibex_wrapper.h"

#define TYPE_OT_IBEX_WRAPPER_EARLGREY "ot-ibex_wrapper-earlgrey"
OBJECT_DECLARE_TYPE(OtIbexWrapperEarlGreyState, OtIbexWrapperStateClass,
                    OT_IBEX_WRAPPER_EARLGREY)

#endif /* HW_OPENTITAN_OT_IBEX_WRAPPER_EARLGREY_H */
