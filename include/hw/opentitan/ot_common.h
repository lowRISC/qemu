/*
 * QEMU RISC-V Helpers for OpenTitan EarlGrey
 *
 * Copyright (c) 2023 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
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

#ifndef HW_OPENTITAN_OT_COMMON_H
#define HW_OPENTITAN_OT_COMMON_H

#include "chardev/char-fe.h"
#include "exec/memory.h"
#include "hw/core/cpu.h"

/* ------------------------------------------------------------------------ */
/* Multi-bit boolean values */
/* ------------------------------------------------------------------------ */

#define OT_MULTIBITBOOL4_TRUE  0x6u
#define OT_MULTIBITBOOL4_FALSE 0x9u

#define OT_MULTIBITBOOL8_TRUE  0x96u
#define OT_MULTIBITBOOL8_FALSE 0x69u

#define OT_MULTIBITBOOL12_TRUE  0x696u
#define OT_MULTIBITBOOL12_FALSE 0x969u

#define OT_MULTIBITBOOL16_TRUE  0x9696u
#define OT_MULTIBITBOOL16_FALSE 0x6969u

/* ------------------------------------------------------------------------ */
/* Shadow Registers */
/* ------------------------------------------------------------------------ */

/*
 * Shadow register, concept documented at:
 * https://docs.opentitan.org/doc/rm/register_tool/#shadow-registers
 */
typedef struct OtShadowReg {
    /* committed register value */
    uint32_t committed;
    /* staged register value */
    uint32_t staged;
    /* true if 'staged' holds a value */
    bool staged_p;
} OtShadowReg;

enum {
    OT_SHADOW_REG_ERROR = -1,
    OT_SHADOW_REG_COMMITTED = 0,
    OT_SHADOW_REG_STAGED = 1,
};

/**
 * Initialize a shadow register with a committed value and no staged value
 */
static inline void ot_shadow_reg_init(OtShadowReg *sreg, uint32_t value)
{
    sreg->committed = value;
    sreg->staged_p = false;
}

/**
 * Write a new value to a shadow register.
 * If no value was previously staged, the new value is only staged for next
 * write and the function returns OT_SHADOW_REG_STAGED.
 * If a value was previously staged and the new value is different, the function
 * returns OT_SHADOW_REG_ERROR and the new value is ignored. Otherwise the value
 * is committed, the staged value is discarded and the function returns
 * OT_SHADOW_REG_COMMITTED.
 */
static inline int ot_shadow_reg_write(OtShadowReg *sreg, uint32_t value)
{
    if (sreg->staged_p) {
        if (value != sreg->staged) {
            /* second write is different, return error status */
            return OT_SHADOW_REG_ERROR;
        }
        sreg->committed = value;
        sreg->staged_p = false;
        return OT_SHADOW_REG_COMMITTED;
    } else {
        sreg->staged = value;
        sreg->staged_p = true;
        return OT_SHADOW_REG_STAGED;
    }
}

/**
 * Return the current committed register value
 */
static inline uint32_t ot_shadow_reg_peek(const OtShadowReg *sreg)
{
    return sreg->committed;
}

/**
 * Discard the staged value and return the current committed register value
 */
static inline uint32_t ot_shadow_reg_read(OtShadowReg *sreg)
{
    sreg->staged_p = false;
    return sreg->committed;
}

/* ------------------------------------------------------------------------ */
/* Memory and Devices */
/* ------------------------------------------------------------------------ */

/**
 * Get the closest CPU for a device, if any.
 * @return the CPU if found or NULL
 */
CPUState *ot_common_get_local_cpu(DeviceState *s);

/**
 * Get the local address space for a device, if any.
 * The local address space if the address space the OT CPU uses to access this
 * device on its local bus.
 *
 * @s the device for each to find the local address space
 * @return the AddressSpace if found or NULL
 */
AddressSpace *ot_common_get_local_address_space(DeviceState *s);

/* ------------------------------------------------------------------------ */
/* CharDev utilities */
/* ------------------------------------------------------------------------ */

void ot_common_ignore_chr_status_lines(CharBackend *chr);

#endif /* HW_OPENTITAN_OT_COMMON_H */
