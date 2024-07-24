/*
 * QEMU OpenTitan OTP backend
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
 *
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
 * Constants are based on what can be extracted from
 *    https://github.com/lowRISC/opentitan-integrated/commit/eaf699f001
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/opentitan/ot_otp.h"
#include "hw/opentitan/ot_otp_be_if.h"
#include "hw/opentitan/ot_otp_ot_be.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/sysbus.h"
#include "trace.h"

/* clang-format off */
REG32(CSR0, 0x0u)
    FIELD(CSR0, FIELD0, 0u, 1u)
    FIELD(CSR0, FIELD1, 1u, 1u)
    FIELD(CSR0, FIELD2, 2u, 1u)
    FIELD(CSR0, FIELD3, 4u, 10u)
    FIELD(CSR0, FIELD4, 16u, 11u)
REG32(CSR1, 0x4u)
    FIELD(CSR1, FIELD0, 0u, 7u)
    FIELD(CSR1, FIELD1, 7u, 1u)
    FIELD(CSR1, FIELD2, 8u, 7u)
    FIELD(CSR1, FIELD3, 15u, 1u)
    FIELD(CSR1, FIELD4, 16u, 16u)
REG32(CSR2, 0x8u)
    FIELD(CSR2, FIELD0, 0u, 1u)
REG32(CSR3, 0xcu)
    FIELD(CSR3, FIELD0, 0u, 3u)
    FIELD(CSR3, FIELD1, 4u, 10u)
    FIELD(CSR3, FIELD2, 16u, 1u)
    FIELD(CSR3, FIELD3, 17u, 1u)
    FIELD(CSR3, FIELD4, 18u, 1u)
    FIELD(CSR3, FIELD5, 19u, 1u)
    FIELD(CSR3, FIELD6, 20u, 1u)
    FIELD(CSR3, FIELD7, 21u, 1u)
    FIELD(CSR3, FIELD8, 22u, 1u)
REG32(CSR4, 0x10u)
    FIELD(CSR4, FIELD0, 0u, 10u)
    FIELD(CSR4, FIELD1, 12u, 1u)
    FIELD(CSR4, FIELD2, 13u, 1u)
    FIELD(CSR4, FIELD3, 14u, 1u)
REG32(CSR5, 0x14u)
    FIELD(CSR5, FIELD0, 0u, 6u)
    FIELD(CSR5, FIELD1, 6u, 2u)
    FIELD(CSR5, FIELD2, 8u, 1u)
    FIELD(CSR5, FIELD3, 9u, 3u)
    FIELD(CSR5, FIELD4, 12u, 1u)
    FIELD(CSR5, FIELD5, 13u, 1u)
    FIELD(CSR5, FIELD6, 16u, 16u)
REG32(CSR6, 0x18u)
    FIELD(CSR6, FIELD0, 0u, 10u)
    FIELD(CSR6, FIELD1, 11u, 1u)
    FIELD(CSR6, FIELD2, 12u, 1u)
    FIELD(CSR6, FIELD3, 16u, 16u)
REG32(CSR7, 0x1cu)
    FIELD(CSR7, FIELD0, 0u, 6u)
    FIELD(CSR7, FIELD1, 8u, 3u)
    FIELD(CSR7, FIELD2, 14u, 1u)
    FIELD(CSR7, FIELD3, 15u, 1u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_CSR7)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")


/* clang-format off */
#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char REG_NAMES[REGS_COUNT][6u] = {
    REG_NAME_ENTRY(CSR0),
    REG_NAME_ENTRY(CSR1),
    REG_NAME_ENTRY(CSR2),
    REG_NAME_ENTRY(CSR3),
    REG_NAME_ENTRY(CSR4),
    REG_NAME_ENTRY(CSR5),
    REG_NAME_ENTRY(CSR6),
    REG_NAME_ENTRY(CSR7),
};
#undef REG_NAME_ENTRY

struct OtOtpOtBeState {
    SysBusDevice parent_obj;

    MemoryRegion prim_mr;

    uint32_t regs[REGS_COUNT];

    char *ot_id;
    DeviceState *parent;
};

static uint64_t ot_otp_ot_be_read(void *opaque, hwaddr addr, unsigned size)
{
    OtOtpOtBeState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_CSR0:
    case R_CSR1:
    case R_CSR2:
    case R_CSR3:
    case R_CSR4:
    case R_CSR5:
    case R_CSR6:
    case R_CSR7:
        /* TODO: not yet implemented */
        val32 = s->regs[reg];
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_otp_ot_be_read_out((uint32_t)addr, REG_NAME(reg), val32, pc);

    return (uint64_t)val32;
}

static void ot_otp_ot_be_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    OtOtpOtBeState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)value;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_otp_ot_be_write((uint32_t)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_CSR0:
    case R_CSR1:
    case R_CSR2:
    case R_CSR3:
    case R_CSR4:
    case R_CSR5:
    case R_CSR6:
        /* TODO: not yet implemented */
        s->regs[reg] = val32;
        break;
    case R_CSR7:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: R/O register 0x02%" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static Property ot_otp_ot_be_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtOtpOtBeState, ot_id),
    DEFINE_PROP_LINK("parent", OtOtpOtBeState, parent, TYPE_DEVICE,
                    DeviceState*),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_otp_ot_be_ops = {
    .read = &ot_otp_ot_be_read,
    .write = &ot_otp_ot_be_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static bool ot_otp_ot_be_is_ecc_enabled(OtOtpBeIf *beif)
{
    (void)beif;

    return true;
}

static void ot_otp_ot_be_init(Object *obj)
{
    OtOtpOtBeState *s = OT_OTP_OT_BE(obj);

    memory_region_init_io(&s->prim_mr, obj, &ot_otp_ot_be_ops, s,
                          TYPE_OT_OTP_OT_BE, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->prim_mr);
}

static void ot_otp_ot_be_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    device_class_set_props(dc, ot_otp_ot_be_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    OtOtpBeIfClass *bec = OT_OTP_BE_IF_CLASS(klass);
    bec->is_ecc_enabled = &ot_otp_ot_be_is_ecc_enabled;
}

static const TypeInfo ot_otp_ot_be_init_info = {
    .name = TYPE_OT_OTP_OT_BE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtOtpOtBeState),
    .instance_init = &ot_otp_ot_be_init,
    .class_init = &ot_otp_ot_be_class_init,
    .interfaces =
        (InterfaceInfo[]){
            { TYPE_OT_OTP_BE_IF },
            {},
        },
};

static void ot_otp_ot_be_init_register_types(void)
{
    type_register_static(&ot_otp_ot_be_init_info);
}

type_init(ot_otp_ot_be_init_register_types);
