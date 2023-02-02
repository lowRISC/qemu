/*
 * QEMU RISC-V Board Compatible with Ibex Demo FPGA platform
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
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

#ifndef HW_RISCV_IBEXDEMO_H
#define HW_RISCV_IBEXDEMO_H

#include "qom/object.h"

#define TYPE_RISCV_IBEXDEMO_MACHINE MACHINE_TYPE_NAME("ibexdemo")
OBJECT_DECLARE_SIMPLE_TYPE(IbexDemoMachineState, RISCV_IBEXDEMO_MACHINE)

#define TYPE_RISCV_IBEXDEMO_BOARD "riscv.ibexdemo.board"
OBJECT_DECLARE_SIMPLE_TYPE(IbexDemoBoardState, RISCV_IBEXDEMO_BOARD)

#define TYPE_RISCV_IBEXDEMO_SOC "riscv.ibexdemo.soc"
OBJECT_DECLARE_SIMPLE_TYPE(IbexDemoSoCState, RISCV_IBEXDEMO_SOC)

#endif /* HW_RISCV_IBEXDEMO_H */
