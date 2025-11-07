/*
 * QEMU RISC-V Helpers for OpenTitan EarlGrey
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
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

#include "chardev/char.h"
#include "exec/memory.h"
#include "hw/core/cpu.h"
#include "hw/riscv/ibex_common.h"

/* ------------------------------------------------------------------------ */
/* Timer */
/* ------------------------------------------------------------------------ */

/* QEMU virtual timer to use for OpenTitan devices */
#define OT_VIRTUAL_CLOCK QEMU_CLOCK_VIRTUAL

/* ------------------------------------------------------------------------ */
/* TL-UL Bus Characteristics */
/* ------------------------------------------------------------------------ */

#define OT_TL_UL_D_WIDTH_BITS  32u
#define OT_TL_UL_D_WIDTH_BYTES ((OT_TL_UL_D_WIDTH_BITS) / 8u)

/* ------------------------------------------------------------------------ */
/* Register Span Definitions */
/* ------------------------------------------------------------------------ */

#define OT_REG_NAME_IDX(_n_, _i_) (R_##_n_##_##_i_)
#define OT_REG_COUNT(_n_, _l_) \
    (OT_REG_NAME_IDX(_n_, _l_) - OT_REG_NAME_IDX(_n_, 0) + 1u)
#define OT_REG_SPAN(_n_, _l_) (OT_REG_COUNT(_n_, _l_) * sizeof(uint32_t))

/* ------------------------------------------------------------------------ */
/* Multi-bit boolean values */
/* ------------------------------------------------------------------------ */

#define OT_MULTIBITBOOL4_TRUE  0x6u
#define OT_MULTIBITBOOL4_FALSE 0x9u
#define OT_MULTIBITBOOL4_WIDTH 4u
#define OT_MULTIBITBOOL4_MASK  ((1u << OT_MULTIBITBOOL4_WIDTH) - 1u)

#define OT_MULTIBITBOOL8_TRUE  0x96u
#define OT_MULTIBITBOOL8_FALSE 0x69u
#define OT_MULTIBITBOOL8_WIDTH 8u
#define OT_MULTIBITBOOL8_MASK  ((1u << OT_MULTIBITBOOL8_WIDTH) - 1u)

#define OT_MULTIBITBOOL12_TRUE  0x696u
#define OT_MULTIBITBOOL12_FALSE 0x969u
#define OT_MULTIBITBOOL12_WIDTH 12u
#define OT_MULTIBITBOOL12_MASK  ((1u << OT_MULTIBITBOOL12_WIDTH) - 1u)

#define OT_MULTIBITBOOL16_TRUE  0x9696u
#define OT_MULTIBITBOOL16_FALSE 0x6969u
#define OT_MULTIBITBOOL16_WIDTH 16u
#define OT_MULTIBITBOOL16_MASK  ((1u << OT_MULTIBITBOOL16_WIDTH) - 1u)

#define OT_MULTIBITBOOL_LC4_TRUE  0xau
#define OT_MULTIBITBOOL_LC4_FALSE 0x5u
#define OT_MULTIBITBOOL_LC4_WIDTH 4u
#define OT_MULTIBITBOOL_LC4_MASK  ((1u << OT_MULTIBITBOOL_LC4_WIDTH) - 1u)

typedef uint8_t ot_mb4_t;
typedef uint8_t ot_mb8_t;
typedef uint16_t ot_mb12_t;
typedef uint16_t ot_mb16_t;
typedef uint8_t ot_mb_lc4_t;

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
 * Computes the new multibit register value when writing to a W1S register
 * field.
 */
static inline uint32_t
ot_multibitbool_w1s_write(uint32_t old, uint32_t val, uint32_t size)
{
    return ot_multibitbool_or_hi(old, val, size);
}

/*
 * Computes the new multibit register value when writing to a W1C register
 * field.
 */
static inline uint32_t
ot_multibitbool_w1c_write(uint32_t old, uint32_t val, uint32_t size)
{
    return ot_multibitbool_and_hi(old, ~val, size);
}

/*
 * Computes the new multibit register value when writing to a W0C register
 * field.
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

int ot_common_parse_hexa_str(uint8_t *out, const char *xstr, size_t olen,
                             bool reverse, bool exact);

/*
 * Generate a uppercase hex-string representation of a buffer.
 *
 * @buf input byte buffer
 * @size size of the buffer
 * @order true to print the buffer in reverse order
 * @hexstr output hex string buffer
 * @hexstr_size max size of the output hex string buffer
 *
 * @return the hex string representation of the buffer
 */
const char *ot_common_uhexdump(const uint8_t *buf, size_t size, bool order,
                               char *hexstr, size_t hexstr_size);

/*
 * Generate a lowercase hex-string representation of a buffer.
 *
 * @buf input byte buffer
 * @size size of the buffer
 * @order true to print the buffer in reverse order
 * @hexstr output hex string buffer
 * @hexstr_size max size of the output hex string buffer
 *
 * @return the hex string representation of the buffer
 */
const char *ot_common_lhexdump(const uint8_t *buf, size_t size, bool order,
                               char *hexstr, size_t hexstr_size);

/* ------------------------------------------------------------------------ */
/* Configuration utilities */
/* ------------------------------------------------------------------------ */

#define OT_COMMON_DEV_ID "ot_id"

void ot_common_configure_devices_with_id(
    DeviceState **devices, BusState *bus, const char *id_value, bool id_prepend,
    const IbexDeviceDef *defs, size_t count);

void ot_common_configure_device_opts(DeviceState **devices, unsigned count);

/* ------------------------------------------------------------------------ */
/* OBJECT macros */
/* ------------------------------------------------------------------------ */

/*
 * OBJECT_DEFINE_TYPE_EXTENDED with explicit class name
 */
#define OT_OBJECT_DEFINE_TYPE_EXTENDED(ModuleObjName, ModuleClassName, \
                                       module_obj_name, MODULE_OBJ_NAME, \
                                       PARENT_MODULE_OBJ_NAME, ABSTRACT, ...) \
    DO_OBJECT_DEFINE_TYPE_EXTENDED(ModuleObjName, module_obj_name, \
                                   MODULE_OBJ_NAME, PARENT_MODULE_OBJ_NAME, \
                                   ABSTRACT, sizeof(ModuleClassName), \
                                   __VA_ARGS__)

/*
 * OBJECT_DEFINE_ABSTRACT_TYPE with explicit class name
 */
#define OT_OBJECT_DEFINE_ABSTRACT_TYPE(ModuleObjName, ModuleClassName, \
                                       module_obj_name, MODULE_OBJ_NAME, \
                                       PARENT_MODULE_OBJ_NAME) \
    OT_OBJECT_DEFINE_TYPE_EXTENDED(ModuleObjName, ModuleClassName, \
                                   module_obj_name, MODULE_OBJ_NAME, \
                                   PARENT_MODULE_OBJ_NAME, true, { NULL })

#endif /* HW_OPENTITAN_OT_COMMON_H */
