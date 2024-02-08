/*
 * QEMU RISC-V Board Compatible with OpenTitan "integrated" Darjeeling platform
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
 *  Emmanuel Blot <eblot@rivosinc.com>
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

#ifndef HW_RISCV_OT_DARJEELING_H
#define HW_RISCV_OT_DARJEELING_H

#include "qom/object.h"

#define TYPE_RISCV_OT_DJ_MACHINE MACHINE_TYPE_NAME("ot-darjeeling")
OBJECT_DECLARE_SIMPLE_TYPE(OtDjMachineState, RISCV_OT_DJ_MACHINE)

#define TYPE_RISCV_OT_DJ_BOARD "riscv.ot_darjeeling.board"
OBJECT_DECLARE_SIMPLE_TYPE(OtDjBoardState, RISCV_OT_DJ_BOARD)

#define TYPE_RISCV_OT_DJ_SOC "riscv.ot_darjeeling.soc"
OBJECT_DECLARE_TYPE(OtDjSoCState, OtDjSoCClass, RISCV_OT_DJ_SOC)

#endif /* HW_RISCV_OT_DARJEELING_H */
