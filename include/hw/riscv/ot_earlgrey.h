/*
 * QEMU RISC-V Board Compatible with OpenTitan EarlGrey FPGA platform
 *
 * Copyright (c) 2022-2024 Rivos, Inc.
 * Copyright (c) 2024-2025 lowRISC contributors.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_RISCV_OT_EARLGREY_H
#define HW_RISCV_OT_EARLGREY_H

#include "qom/object.h"

#define OT_EARLGREY "ot-earlgrey"

#define TYPE_RISCV_OT_EG_MACHINE MACHINE_TYPE_NAME(OT_EARLGREY)
OBJECT_DECLARE_SIMPLE_TYPE(OtEGMachineState, RISCV_OT_EG_MACHINE)

#define TYPE_RISCV_OT_EG_BOARD OT_EARLGREY "-board"
OBJECT_DECLARE_SIMPLE_TYPE(OtEGBoardState, RISCV_OT_EG_BOARD)

#define TYPE_RISCV_OT_EG_SOC OT_EARLGREY "-soc"
OBJECT_DECLARE_TYPE(OtEGSoCState, OtEGSoCClass, RISCV_OT_EG_SOC)

#endif /* HW_RISCV_OT_EARLGREY_H */
