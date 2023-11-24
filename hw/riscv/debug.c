/*
 * QEMU RISC-V Debug
 *
 * Copyright (c) 2023 Rivos, Inc.
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
 *
 * Generated code for absract commands has been extracted from the PULP Debug
 * module whose file (dm_mem.sv) contains the following copyright. This piece of
 * code is self contained within the `riscv_dmi_dm_access_register` function:
 *
 *   Copyright and related rights are licensed under the Solderpad Hardware
 *   License, Version 0.51 (the “License”); you may not use this file except in
 *   compliance with the License. You may obtain a copy of the License at
 *   http://solderpad.org/licenses/SHL-0.51. Unless required by applicable law
 *   or agreed to in writing, software, hardware and materials distributed under
 *   this License is distributed on an “AS IS” BASIS, WITHOUT WARRANTIES OR
 *   CONDITIONS OF ANY KIND, either express or implied. See the License for the
 *   specific language governing permissions and limitations under the License.
 */

#include "qemu/osdep.h"
#include "hw/riscv/debug.h"

static const TypeInfo riscv_debug_device_info = {
    .name = TYPE_RISCV_DEBUG_DEVICE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(RISCVDebugDeviceState),
    .class_size = sizeof(RISCVDebugDeviceClass),
    .abstract = true,
};

static void riscv_debug_register_types(void)
{
    type_register_static(&riscv_debug_device_info);
}

type_init(riscv_debug_register_types);
