/*
 * QEMU RISC-V Helpers for OpenTitan EarlGrey
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
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
/* Timer */
/* ------------------------------------------------------------------------ */

/* QEMU virtual timer to use for OpenTitan devices */
#define OT_VIRTUAL_CLOCK QEMU_CLOCK_VIRTUAL

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

#define OT_MULTIBITBOOL_LC4_TRUE  0xau
#define OT_MULTIBITBOOL_LC4_FALSE 0x5u

/*
 * Performs a logical OR operation between two multibit values.
 * This treats "act" as logical 1, and all other values are treated as 0.
 * Truth table:
 *
 *  A    | B    | OUT
 * ------+------+-----
 *  !act | !act | !act
 *  act  | !act | act
 *  !act | act  | act
 *  act  | act  | act
 */
static inline uint32_t
ot_multibitbool_or(uint32_t a, uint32_t b, uint32_t act, uint32_t size)
{
    uint32_t mask = (1u << size) - 1u;
    return (((a | b) & act) | ((a & b) & ~act)) & mask;
}

/*
 * Performs a logical AND operation between two multibit values.
 * This treats "act" as logical 1, and all other values are treated as 0.
 * Truth table:
 *
 *  A    | B    | OUT
 * ------+------+-----
 *  !act | !act | !act
 *  act  | !act | !act
 *  !act | act  | !act
 *  act  | act  | act
 */
static inline uint32_t
ot_multibitbool_and(uint32_t a, uint32_t b, uint32_t act, uint32_t size)
{
    uint32_t mask = (1u << size) - 1u;
    return (((a & b) & act) | ((a | b) & ~act)) & mask;
}

/*
 * Performs a logical OR operation between two multibit values.
 * This treats "True" as logical 1, and all other values are
 * treated as 0.
 */
static inline uint32_t
ot_multibitbool_or_hi(uint32_t a, uint32_t b, uint32_t size)
{
    return ot_multibitbool_or(a, b, OT_MULTIBITBOOL16_TRUE, size);
}

/*
 * Performs a logical AND operation between two multibit values.
 * This treats "True" as logical 1, and all other values are
 * treated as 0.
 */
static inline uint32_t
ot_multibitbool_and_hi(uint32_t a, uint32_t b, uint32_t size)
{
    return ot_multibitbool_and(a, b, OT_MULTIBITBOOL16_TRUE, size);
}

/*
 * Performs a logical OR operation between two multibit values.
 * This treats "False" as logical 1, and all other values are
 * treated as 0.
 */
static inline uint32_t
ot_multibitbool_or_lo(uint32_t a, uint32_t b, uint32_t size)
{
    return ot_multibitbool_or(a, b, OT_MULTIBITBOOL16_FALSE, size);
}

/*
 * Performs a logical AND operation between two multibit values.
 * This treats "False" as logical 1, and all other values are
 * treated as 0.
 */
static inline uint32_t
ot_multibitbool_and_lo(uint32_t a, uint32_t b, uint32_t size)
{
    return ot_multibitbool_and(a, b, OT_MULTIBITBOOL16_FALSE, size);
}

/*
 * Computes the new multibit register value when writing to a W1S register field.
 */
static inline uint32_t
ot_multibitbool_w1s_write(uint32_t old, uint32_t val, uint32_t size)
{
    return ot_multibitbool_or_hi(old, val, size);
}

/*
 * Computes the new multibit register value when writing to a W1C register field.
 */
static inline uint32_t
ot_multibitbool_w1c_write(uint32_t old, uint32_t val, uint32_t size)
{
    return ot_multibitbool_and_hi(old, ~val, size);
}

/*
 * Computes the new multibit register value when writing to a W0C register field.
 */
static inline uint32_t
ot_multibitbool_w0c_write(uint32_t old, uint32_t val, uint32_t size)
{
    return ot_multibitbool_and_hi(old, val, size);
}

/* ------------------------------------------------------------------------ */
/* Extended memory transactions (MemTxAttrs can be tained with .role attr)  */
/* ------------------------------------------------------------------------ */

#if defined(MEMTXATTRS_HAS_ROLE) && (MEMTXATTRS_HAS_ROLE != 0)
#define MEMTXATTRS_WITH_ROLE(_r_) \
    (MemTxAttrs) \
    { \
        .role = (_r_) \
    }
#define MEMTXATTRS_GET_ROLE(_a_) ((_a_).unspecified ? 0xfu : (_a_).role);
#else
#define MEMTXATTRS_WITH_ROLE(_r_) MEMTXATTRS_UNSPECIFIED
#define MEMTXATTRS_GET_ROLE(_a_)  ((_a_).unspecified ? 0xfu : 0x0)
#endif

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
 * Verify that command-line ROM image definitions are compatible with the
 * current machine; emit warning message if they are not.
 *
 * @return the count of ROM controllers with no assigned ROM image
 */
unsigned ot_common_check_rom_configuration(void);

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

/**
 * Configure a (PTY) char backend to ignore status lines.
 *
 * @chr the character backend to configure.
 */
void ot_common_ignore_chr_status_lines(CharBackend *chr);

/* ------------------------------------------------------------------------ */
/* String utilities */
/* ------------------------------------------------------------------------ */

int ot_common_string_ends_with(const char *str, const char *suffix);

#endif /* HW_OPENTITAN_OT_COMMON_H */
