/*
 * QEMU monitor for RISC-V
 *
 * Copyright (c) 2019 Bin Meng <bmeng.cn@gmail.com>
 *
 * RISC-V specific monitor commands implementation
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

#include "qemu/osdep.h"
#include "cpu.h"
#include "cpu_bits.h"
#include "monitor/monitor.h"
#include "monitor/hmp-target.h"

#ifdef TARGET_RISCV64
#define PTE_HEADER_FIELDS       "vaddr            paddr            "\
                                "size             attr\n"
#define PTE_HEADER_DELIMITER    "---------------- ---------------- "\
                                "---------------- -------\n"
#else
#define PTE_HEADER_FIELDS       "vaddr    paddr            size     attr\n"
#define PTE_HEADER_DELIMITER    "-------- ---------------- -------- -------\n"
#endif

/* Perform linear address sign extension */
static target_ulong addr_canonical(int va_bits, target_ulong addr)
{
#ifdef TARGET_RISCV64
    if (addr & (1UL << (va_bits - 1))) {
        addr |= (hwaddr)-(1L << va_bits);
    }
#endif

    return addr;
}

static void print_pte_header(Monitor *mon)
{
    monitor_printf(mon, PTE_HEADER_FIELDS);
    monitor_printf(mon, PTE_HEADER_DELIMITER);
}

static void print_pte(Monitor *mon, int va_bits, target_ulong vaddr,
                      hwaddr paddr, target_ulong size, int attr)
{
    /* santity check on vaddr */
    if (vaddr >= (1UL << va_bits)) {
        return;
    }

    if (!size) {
        return;
    }

    monitor_printf(mon, TARGET_FMT_lx " " HWADDR_FMT_plx " " TARGET_FMT_lx
                   " %c%c%c%c%c%c%c\n",
                   addr_canonical(va_bits, vaddr),
                   paddr, size,
                   attr & PTE_R ? 'r' : '-',
                   attr & PTE_W ? 'w' : '-',
                   attr & PTE_X ? 'x' : '-',
                   attr & PTE_U ? 'u' : '-',
                   attr & PTE_G ? 'g' : '-',
                   attr & PTE_A ? 'a' : '-',
                   attr & PTE_D ? 'd' : '-');
}

static void walk_pte(Monitor *mon, hwaddr base, target_ulong start,
                     int level, int ptidxbits, int ptesize, int va_bits,
                     target_ulong *vbase, hwaddr *pbase, hwaddr *last_paddr,
                     target_ulong *last_size, int *last_attr)
{
    hwaddr pte_addr;
    hwaddr paddr;
    target_ulong last_start = -1;
    target_ulong pgsize;
    target_ulong pte;
    int ptshift;
    int attr;
    int idx;

    if (level < 0) {
        return;
    }

    ptshift = level * ptidxbits;
    pgsize = 1UL << (PGSHIFT + ptshift);

    for (idx = 0; idx < (1UL << ptidxbits); idx++) {
        pte_addr = base + idx * ptesize;
        cpu_physical_memory_read(pte_addr, &pte, ptesize);

        paddr = (hwaddr)(pte >> PTE_PPN_SHIFT) << PGSHIFT;
        attr = pte & 0xff;

        /* PTE has to be valid */
        if (attr & PTE_V) {
            if (attr & (PTE_R | PTE_W | PTE_X)) {
                /*
                 * A leaf PTE has been found
                 *
                 * If current PTE's permission bits differ from the last one,
                 * or the current PTE breaks up a contiguous virtual or
                 * physical mapping, address block together with the last one,
                 * print out the last contiguous mapped block details.
                 */
                if ((*last_attr != attr) ||
                    (*last_paddr + *last_size != paddr) ||
                    (last_start + *last_size != start)) {
                    print_pte(mon, va_bits, *vbase, *pbase,
                              *last_paddr + *last_size - *pbase, *last_attr);

                    *vbase = start;
                    *pbase = paddr;
                    *last_attr = attr;
                }

                last_start = start;
                *last_paddr = paddr;
                *last_size = pgsize;
            } else {
                /* pointer to the next level of the page table */
                walk_pte(mon, paddr, start, level - 1, ptidxbits, ptesize,
                         va_bits, vbase, pbase, last_paddr,
                         last_size, last_attr);
            }
        }

        start += pgsize;
    }

}

static void mem_info_svxx(Monitor *mon, CPUArchState *env)
{
    int levels, ptidxbits, ptesize, vm, va_bits;
    hwaddr base;
    target_ulong vbase;
    hwaddr pbase;
    hwaddr last_paddr;
    target_ulong last_size;
    int last_attr;

    if (riscv_cpu_mxl(env) == MXL_RV32) {
        base = (hwaddr)get_field(env->satp, SATP32_PPN) << PGSHIFT;
        vm = get_field(env->satp, SATP32_MODE);
    } else {
        base = (hwaddr)get_field(env->satp, SATP64_PPN) << PGSHIFT;
        vm = get_field(env->satp, SATP64_MODE);
    }

    switch (vm) {
    case VM_1_10_SV32:
        levels = 2;
        ptidxbits = 10;
        ptesize = 4;
        break;
    case VM_1_10_SV39:
        levels = 3;
        ptidxbits = 9;
        ptesize = 8;
        break;
    case VM_1_10_SV48:
        levels = 4;
        ptidxbits = 9;
        ptesize = 8;
        break;
    case VM_1_10_SV57:
        levels = 5;
        ptidxbits = 9;
        ptesize = 8;
        break;
    default:
        g_assert_not_reached();
        break;
    }

    /* calculate virtual address bits */
    va_bits = PGSHIFT + levels * ptidxbits;

    /* print header */
    print_pte_header(mon);

    vbase = -1;
    pbase = -1;
    last_paddr = -1;
    last_size = 0;
    last_attr = 0;

    /* walk page tables, starting from address 0 */
    walk_pte(mon, base, 0, levels - 1, ptidxbits, ptesize, va_bits,
             &vbase, &pbase, &last_paddr, &last_size, &last_attr);

    /* don't forget the last one */
    print_pte(mon, va_bits, vbase, pbase,
              last_paddr + last_size - pbase, last_attr);
}

void hmp_info_mem(Monitor *mon, const QDict *qdict)
{
    CPUArchState *env;

    env = mon_get_cpu_env(mon);
    if (!env) {
        monitor_printf(mon, "No CPU available\n");
        return;
    }

    if (!riscv_cpu_cfg(env)->mmu) {
        monitor_printf(mon, "S-mode MMU unavailable\n");
        return;
    }

    if (riscv_cpu_mxl(env) == MXL_RV32) {
        if (!(env->satp & SATP32_MODE)) {
            monitor_printf(mon, "No translation or protection\n");
            return;
        }
    } else {
        if (!(env->satp & SATP64_MODE)) {
            monitor_printf(mon, "No translation or protection\n");
            return;
        }
    }

    mem_info_svxx(mon, env);
}

static const MonitorDef monitor_defs[] = {
    { "pc",  offsetof(CPURISCVState, pc) },
    { "x0",  offsetof(CPURISCVState, gpr[0]) },
    { "x1",  offsetof(CPURISCVState, gpr[1]) },
    { "x2",  offsetof(CPURISCVState, gpr[2]) },
    { "x3",  offsetof(CPURISCVState, gpr[3]) },
    { "x4",  offsetof(CPURISCVState, gpr[4]) },
    { "x5",  offsetof(CPURISCVState, gpr[5]) },
    { "x6",  offsetof(CPURISCVState, gpr[6]) },
    { "x7",  offsetof(CPURISCVState, gpr[7]) },
    { "x8",  offsetof(CPURISCVState, gpr[8]) },
    { "x9",  offsetof(CPURISCVState, gpr[9]) },
    { "x10", offsetof(CPURISCVState, gpr[10]) },
    { "x11", offsetof(CPURISCVState, gpr[11]) },
    { "x12", offsetof(CPURISCVState, gpr[12]) },
    { "x13", offsetof(CPURISCVState, gpr[13]) },
    { "x14", offsetof(CPURISCVState, gpr[14]) },
    { "x15", offsetof(CPURISCVState, gpr[15]) },
    { "x16", offsetof(CPURISCVState, gpr[16]) },
    { "x17", offsetof(CPURISCVState, gpr[17]) },
    { "x18", offsetof(CPURISCVState, gpr[18]) },
    { "x19", offsetof(CPURISCVState, gpr[19]) },
    { "x20", offsetof(CPURISCVState, gpr[20]) },
    { "x21", offsetof(CPURISCVState, gpr[21]) },
    { "x22", offsetof(CPURISCVState, gpr[22]) },
    { "x23", offsetof(CPURISCVState, gpr[23]) },
    { "x24", offsetof(CPURISCVState, gpr[24]) },
    { "x25", offsetof(CPURISCVState, gpr[25]) },
    { "x26", offsetof(CPURISCVState, gpr[26]) },
    { "x27", offsetof(CPURISCVState, gpr[27]) },
    { "x28", offsetof(CPURISCVState, gpr[28]) },
    { "x29", offsetof(CPURISCVState, gpr[29]) },
    { "x30", offsetof(CPURISCVState, gpr[30]) },
    { "x31", offsetof(CPURISCVState, gpr[31]) },
    { "f0",  offsetof(CPURISCVState, fpr[0]) },
    { "f1",  offsetof(CPURISCVState, fpr[1]) },
    { "f2",  offsetof(CPURISCVState, fpr[2]) },
    { "f3",  offsetof(CPURISCVState, fpr[3]) },
    { "f4",  offsetof(CPURISCVState, fpr[4]) },
    { "f5",  offsetof(CPURISCVState, fpr[5]) },
    { "f6",  offsetof(CPURISCVState, fpr[6]) },
    { "f7",  offsetof(CPURISCVState, fpr[7]) },
    { "f8",  offsetof(CPURISCVState, fpr[8]) },
    { "f9",  offsetof(CPURISCVState, fpr[9]) },
    { "f10", offsetof(CPURISCVState, fpr[10]) },
    { "f11", offsetof(CPURISCVState, fpr[11]) },
    { "f12", offsetof(CPURISCVState, fpr[12]) },
    { "f13", offsetof(CPURISCVState, fpr[13]) },
    { "f14", offsetof(CPURISCVState, fpr[14]) },
    { "f15", offsetof(CPURISCVState, fpr[15]) },
    { "f16", offsetof(CPURISCVState, fpr[16]) },
    { "f17", offsetof(CPURISCVState, fpr[17]) },
    { "f18", offsetof(CPURISCVState, fpr[18]) },
    { "f19", offsetof(CPURISCVState, fpr[19]) },
    { "f20", offsetof(CPURISCVState, fpr[20]) },
    { "f21", offsetof(CPURISCVState, fpr[21]) },
    { "f22", offsetof(CPURISCVState, fpr[22]) },
    { "f23", offsetof(CPURISCVState, fpr[23]) },
    { "f24", offsetof(CPURISCVState, fpr[24]) },
    { "f25", offsetof(CPURISCVState, fpr[25]) },
    { "f26", offsetof(CPURISCVState, fpr[26]) },
    { "f27", offsetof(CPURISCVState, fpr[27]) },
    { "f28", offsetof(CPURISCVState, fpr[28]) },
    { "f29", offsetof(CPURISCVState, fpr[29]) },
    { "f30", offsetof(CPURISCVState, fpr[30]) },
    { "f31", offsetof(CPURISCVState, fpr[31]) },
    { "satp", offsetof(CPURISCVState, satp) },
    { "stval", offsetof(CPURISCVState, stval) },
    { "medeleg", offsetof(CPURISCVState, medeleg) },
    { "stvec", offsetof(CPURISCVState, stvec) },
    { "sepc", offsetof(CPURISCVState, sepc) },
    { "scause", offsetof(CPURISCVState, scause) },
    { "mtvec", offsetof(CPURISCVState, mtvec) },
    { "mepc", offsetof(CPURISCVState, mepc) },
    { "mcause", offsetof(CPURISCVState, mcause) },
    { "mtval", offsetof(CPURISCVState, mtval) },
    { "sscratch", offsetof(CPURISCVState, sscratch) },
    { "mscratch", offsetof(CPURISCVState, mscratch) },
    { NULL },
};

const MonitorDef *target_monitor_defs(void)
{
    return monitor_defs;
}
