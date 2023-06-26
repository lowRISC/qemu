/*
 * QEMU LowRisc Ibex core features
 *
 * Copyright (c) 2023 Rivos, Inc.
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

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"

/* Custom CSRs */
#define CSR_CPUCTRLSTS 0x7c0
#define CSR_SECURESEED 0x7c1

#define CPUCTRLSTS_ICACHE_ENABLE     0x000
#define CPUCTRLSTS_DATA_IND_TIMING   0x001
#define CPUCTRLSTS_DUMMY_INSTR_EN    0x002
#define CPUCTRLSTS_DUMMY_INSTR_MASK  0x038
#define CPUCTRLSTS_SYNC_EXC_SEEN     0x040
#define CPUCTRLSTS_DOUBLE_FAULT_SEEN 0x080
#define CPUCTRLSTS_IC_SCR_KEY_VALID  0x100

#if !defined(CONFIG_USER_ONLY)

static RISCVException read_cpuctrlsts(CPURISCVState *env, int csrno,
                                      target_ulong *val)
{
    *val = CPUCTRLSTS_IC_SCR_KEY_VALID | env->cpuctrlsts;
    return RISCV_EXCP_NONE;
}

static RISCVException write_cpuctrlsts(CPURISCVState *env, int csrno,
                                       target_ulong val)
{
    /* b7 can only be cleared */
    env->cpuctrlsts &= ~0xbf;
    /* b6 should be cleared on mret */
    env->cpuctrlsts |= val & 0x3f;
    return RISCV_EXCP_NONE;
}

static RISCVException read_secureseed(CPURISCVState *env, int csrno,
                                      target_ulong *val)
{
    /*
     * "Seed values are not actually stored in a register and so reads to this
     * register will always return zero."
     */
    *val = 0;
    return RISCV_EXCP_NONE;
}

static RISCVException write_secureseed(CPURISCVState *env, int csrno,
                                       target_ulong val)
{
    (void)val;
    return RISCV_EXCP_NONE;
}

static RISCVException any(CPURISCVState *env, int csrno)
{
    /*
     *  unfortunately, this predicate is not public, so duplicate the standard
     *  implementation
     */
    return RISCV_EXCP_NONE;
}

typedef struct {
    unsigned csrno;
    riscv_csr_operations ops;
} riscv_custom_csr_operations;

static riscv_custom_csr_operations csr_ibex_ops[] = {
    {
        .csrno = CSR_CPUCTRLSTS,
        .ops = { "cpuctrlsts", any, &read_cpuctrlsts, &write_cpuctrlsts },
    },
    {
        .csrno = CSR_SECURESEED,
        .ops = { "secureseed", any, &read_secureseed, &write_secureseed },
    },
};

#endif /* !defined(CONFIG_USER_ONLY) */

static bool ibex_csr_ops_added;

void riscv_add_ibex_csr_ops(RISCVCPU *cpu)
{
    (void)cpu;

    /*
     * Since the CSR operations table is global, we only need to do
     * this once, regardless of where it's called from. Currently, the
     * call is coming from the Ibex/Earlgrey CPU instance init function,
     * which happens before all CPU properties are set. Therefore all
     * Ibex extension CSRs are added unconditionally, and the
     * predicate functions will filter out illegal requests based on
     * CPU config properties.
     */
    if (ibex_csr_ops_added) {
        return;
    }

#if !defined(CONFIG_USER_ONLY)
    for (unsigned ix = 0; ix < ARRAY_SIZE(csr_ibex_ops); ix++) {
        riscv_set_csr_ops(csr_ibex_ops[ix].csrno, &csr_ibex_ops[ix].ops);
    }
#endif /* !defined(CONFIG_USER_ONLY) */

    ibex_csr_ops_added = true;
}
