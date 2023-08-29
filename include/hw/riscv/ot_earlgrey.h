/*
 * QEMU RISC-V Board Compatible with OpenTitan EarlGrey FPGA platform
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
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

#define TYPE_RISCV_OT_EARLGREY_MACHINE MACHINE_TYPE_NAME("ot-earlgrey")
OBJECT_DECLARE_SIMPLE_TYPE(OtEarlGreyMachineState, RISCV_OT_EARLGREY_MACHINE)

#define TYPE_RISCV_OT_EARLGREY_BOARD "riscv.ot_earlgrey.board"
OBJECT_DECLARE_SIMPLE_TYPE(OtEarlGreyBoardState, RISCV_OT_EARLGREY_BOARD)

#define TYPE_RISCV_OT_EARLGREY_SOC "riscv.ot_earlgrey.soc"
OBJECT_DECLARE_TYPE(OtEarlGreySoCState, OtEarlGreySoCClass,
                    RISCV_OT_EARLGREY_SOC)

#endif /* HW_RISCV_OT_EARLGREY_H */
