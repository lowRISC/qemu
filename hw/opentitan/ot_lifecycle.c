/*
 * QEMU OpenTitan Life Cycle controller device
 *
 * Copyright (c) 2023 Rivos, Inc.
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
 * Note: for now, only a minimalist subset of Life Cycle controller device is
 *       implemented in order to enable OpenTitan's ROM boot to progress
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_lifecycle.h"
#include "hw/opentitan/ot_otp.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "ot_lcstate.h"
#include "trace.h"

#define PARAM_PRODUCT_ID_WIDTH         16u
#define PARAM_SILICON_CREATOR_ID_WIDTH 16u
#define PARAM_REVISION_ID_WIDTH        8u

/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, FATAL_PROG_ERROR, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_STATE_ERROR, 1u, 1u)
    FIELD(ALERT_TEST, FATAL_BUS_INTEG_ERROR, 2u, 1u)
REG32(STATUS, 0x4u)
    FIELD(STATUS, INITIALIZED, 0u, 1u)
    FIELD(STATUS, READY, 1u, 1u)
    FIELD(STATUS, EXT_CLOCK_SWITCHED, 2u, 1u)
    FIELD(STATUS, TRANSITION_SUCCESSFUL, 3u, 1u)
    FIELD(STATUS, TRANSITION_COUNT_ERROR, 4u, 1u)
    FIELD(STATUS, TRANSITION_ERROR, 5u, 1u)
    FIELD(STATUS, TOKEN_ERROR, 6u, 1u)
    FIELD(STATUS, FLASH_RMA_ERROR, 7u, 1u)
    FIELD(STATUS, OTP_ERROR, 8u, 1u)
    FIELD(STATUS, STATE_ERROR, 9u, 1u)
    FIELD(STATUS, BUS_INTEG_ERROR, 10u, 1u)
    FIELD(STATUS, OTP_PARTITION_ERROR, 11u, 1u)
REG32(CLAIM_TRANSITION_IF_REGWEN, 0x8u)
    FIELD(CLAIM_TRANSITION_IF_REGWEN, EN, 0u, 1u)
REG32(CLAIM_TRANSITION_IF, 0xcu)
    FIELD(CLAIM_TRANSITION, IF_MUTEX, 0u, 8u)
REG32(TRANSITION_REGWEN, 0x10u)
    FIELD(TRANSITION_REGWEN, TRANSITION_REGWEN, 0u, 1u)
REG32(TRANSITION_CMD, 0x14u)
    FIELD(TRANSITION_CMD, START, 0u, 1u)
REG32(TRANSITION_CTRL, 0x18u)
    FIELD(TRANSITION_CTRL, EXT_CLOCK_EN, 0u, 1u)
    FIELD(TRANSITION_CTRL, VOLATILE_RAW_UNLOCK, 1u, 1u)
REG32(TRANSITION_TOKEN_0, 0x1cu)
REG32(TRANSITION_TOKEN_1, 0x20u)
REG32(TRANSITION_TOKEN_2, 0x24u)
REG32(TRANSITION_TOKEN_3, 0x28u)
REG32(TRANSITION_TARGET, 0x2cu)
    FIELD(TRANSITION_TARGET, STATE, 0u, 30u)
REG32(OTP_VENDOR_TEST_CTRL, 0x30u)
REG32(OTP_VENDOR_TEST_STATUS, 0x34u)
REG32(LC_STATE, 0x38u)
    FIELD(LC_STATE, STATE, 0u, 30u)
REG32(LC_TRANSITION_CNT, 0x3cu)
    FIELD(LC_TRANSITION_CNT, CNT, 0u, 5u)
REG32(LC_ID_STATE, 0x40u)
REG32(HW_REVISION0, 0x44u)
    FIELD(HW_REVISION0, PRODUCT_ID, 0u, PARAM_PRODUCT_ID_WIDTH)
    FIELD(HW_REVISION0, SILICON_CREATOR_ID, PARAM_PRODUCT_ID_WIDTH,
          PARAM_SILICON_CREATOR_ID_WIDTH)
REG32(HW_REVISION1, 0x48u)
    FIELD(HW_REVISION1, REVISION_ID, 0u, PARAM_REVISION_ID_WIDTH)
    FIELD(HW_REVISION1, RESERVED,
          PARAM_REVISION_ID_WIDTH, (32u - PARAM_REVISION_ID_WIDTH))
REG32(DEVICE_ID_0, 0x4cu)
REG32(DEVICE_ID_1, 0x50u)
REG32(DEVICE_ID_2, 0x54u)
REG32(DEVICE_ID_3, 0x58u)
REG32(DEVICE_ID_4, 0x5cu)
REG32(DEVICE_ID_5, 0x60u)
REG32(DEVICE_ID_6, 0x64u)
REG32(DEVICE_ID_7, 0x68u)
REG32(MANUF_STATE_0, 0x6cu)
REG32(MANUF_STATE_1, 0x70u)
REG32(MANUF_STATE_2, 0x74u)
REG32(MANUF_STATE_3, 0x78u)
REG32(MANUF_STATE_4, 0x7cu)
REG32(MANUF_STATE_5, 0x80u)
REG32(MANUF_STATE_6, 0x84u)
REG32(MANUF_STATE_7, 0x88u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_MANUF_STATE_7)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define ALERT_TEST_MASK \
    (R_ALERT_TEST_FATAL_PROG_ERROR_MASK | \
     R_ALERT_TEST_FATAL_STATE_ERROR_MASK | \
     R_ALERT_TEST_FATAL_BUS_INTEG_ERROR_MASK)

#define ID_STATE_STATE_VALUE_BLANK        0x00000000u
#define ID_STATE_STATE_VALUE_PERSONALIZED 0x11111111u
#define ID_STATE_STATE_VALUE_INVALID      0x22222222u

#define LC_TRANSITION_COUNT_MAX 24u

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(CLAIM_TRANSITION_IF_REGWEN),
    REG_NAME_ENTRY(CLAIM_TRANSITION_IF),
    REG_NAME_ENTRY(TRANSITION_REGWEN),
    REG_NAME_ENTRY(TRANSITION_CMD),
    REG_NAME_ENTRY(TRANSITION_CTRL),
    REG_NAME_ENTRY(TRANSITION_TOKEN_0),
    REG_NAME_ENTRY(TRANSITION_TOKEN_1),
    REG_NAME_ENTRY(TRANSITION_TOKEN_2),
    REG_NAME_ENTRY(TRANSITION_TOKEN_3),
    REG_NAME_ENTRY(TRANSITION_TARGET),
    REG_NAME_ENTRY(OTP_VENDOR_TEST_CTRL),
    REG_NAME_ENTRY(OTP_VENDOR_TEST_STATUS),
    REG_NAME_ENTRY(LC_STATE),
    REG_NAME_ENTRY(LC_TRANSITION_CNT),
    REG_NAME_ENTRY(LC_ID_STATE),
    REG_NAME_ENTRY(HW_REVISION0),
    REG_NAME_ENTRY(HW_REVISION1),
    REG_NAME_ENTRY(DEVICE_ID_0),
    REG_NAME_ENTRY(DEVICE_ID_1),
    REG_NAME_ENTRY(DEVICE_ID_2),
    REG_NAME_ENTRY(DEVICE_ID_3),
    REG_NAME_ENTRY(DEVICE_ID_4),
    REG_NAME_ENTRY(DEVICE_ID_5),
    REG_NAME_ENTRY(DEVICE_ID_6),
    REG_NAME_ENTRY(DEVICE_ID_7),
    REG_NAME_ENTRY(MANUF_STATE_0),
    REG_NAME_ENTRY(MANUF_STATE_1),
    REG_NAME_ENTRY(MANUF_STATE_2),
    REG_NAME_ENTRY(MANUF_STATE_3),
    REG_NAME_ENTRY(MANUF_STATE_4),
    REG_NAME_ENTRY(MANUF_STATE_5),
    REG_NAME_ENTRY(MANUF_STATE_6),
    REG_NAME_ENTRY(MANUF_STATE_7),
};
#undef REG_NAME_ENTRY

#define LC_ENCODE_STATE(_x_) \
    (((_x_) << 0u) | ((_x_) << 5u) | ((_x_) << 10u) | ((_x_) << 15u) | \
     ((_x_) << 20u) | ((_x_) << 25u))

enum lc_enc_state {
    LC_ENC_STATE_RAW = LC_ENCODE_STATE(LC_STATE_RAW),
    LC_ENC_STATE_TESTUNLOCKED0 = LC_ENCODE_STATE(LC_STATE_TESTUNLOCKED0),
    LC_ENC_STATE_TESTLOCKED0 = LC_ENCODE_STATE(LC_STATE_TESTLOCKED0),
    LC_ENC_STATE_TESTUNLOCKED1 = LC_ENCODE_STATE(LC_STATE_TESTUNLOCKED1),
    LC_ENC_STATE_TESTLOCKED1 = LC_ENCODE_STATE(LC_STATE_TESTLOCKED1),
    LC_ENC_STATE_TESTUNLOCKED2 = LC_ENCODE_STATE(LC_STATE_TESTUNLOCKED2),
    LC_ENC_STATE_TESTLOCKED2 = LC_ENCODE_STATE(LC_STATE_TESTLOCKED2),
    LC_ENC_STATE_TESTUNLOCKED3 = LC_ENCODE_STATE(LC_STATE_TESTUNLOCKED3),
    LC_ENC_STATE_TESTLOCKED3 = LC_ENCODE_STATE(LC_STATE_TESTLOCKED3),
    LC_ENC_STATE_TESTUNLOCKED4 = LC_ENCODE_STATE(LC_STATE_TESTUNLOCKED4),
    LC_ENC_STATE_TESTLOCKED4 = LC_ENCODE_STATE(LC_STATE_TESTLOCKED4),
    LC_ENC_STATE_TESTUNLOCKED5 = LC_ENCODE_STATE(LC_STATE_TESTUNLOCKED5),
    LC_ENC_STATE_TESTLOCKED5 = LC_ENCODE_STATE(LC_STATE_TESTLOCKED5),
    LC_ENC_STATE_TESTUNLOCKED6 = LC_ENCODE_STATE(LC_STATE_TESTUNLOCKED6),
    LC_ENC_STATE_TESTLOCKED6 = LC_ENCODE_STATE(LC_STATE_TESTLOCKED6),
    LC_ENC_STATE_TESTUNLOCKED7 = LC_ENCODE_STATE(LC_STATE_TESTUNLOCKED7),
    LC_ENC_STATE_DEV = LC_ENCODE_STATE(LC_STATE_DEV),
    LC_ENC_STATE_PROD = LC_ENCODE_STATE(LC_STATE_PROD),
    LC_ENC_STATE_PRODEND = LC_ENCODE_STATE(LC_STATE_PRODEND),
    LC_ENC_STATE_RMA = LC_ENCODE_STATE(LC_STATE_RMA),
    LC_ENC_STATE_SCRAP = LC_ENCODE_STATE(LC_STATE_SCRAP),
    LC_ENC_STATE_POST_TRANSITION = LC_ENCODE_STATE(LC_STATE_POST_TRANSITION),
    LC_ENC_STATE_ESCALATE = LC_ENCODE_STATE(LC_STATE_ESCALATE),
    LC_ENC_STATE_INVALID = LC_ENCODE_STATE(LC_STATE_INVALID),
};

struct OtLifeCycleState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ alert;

    uint32_t *regs;

    OtOTPState *otp_ctrl;
};

static void ot_lifecycle_lock_hw_mutex(OtLifeCycleState *s)
{
    FIELD_DP32(s->regs[R_TRANSITION_REGWEN], TRANSITION_REGWEN,
               TRANSITION_REGWEN, 1u);
}

static void ot_lifecycle_release_hw_mutex(OtLifeCycleState *s)
{
    FIELD_DP32(s->regs[R_TRANSITION_REGWEN], TRANSITION_REGWEN,
               TRANSITION_REGWEN, 0u);
}

static bool ot_lifecycle_own_hw_mutex(OtLifeCycleState *s)
{
    return (bool)FIELD_EX32(s->regs[R_TRANSITION_REGWEN], TRANSITION_REGWEN,
                            TRANSITION_REGWEN);
}

static void ot_lifecycle_start_transition(OtLifeCycleState *s)
{
    qemu_log_mask(LOG_UNIMP, "%s: Transition commands not implemented\n",
                  __func__);
}

static uint32_t ot_lifecycle_get_lc_state(OtLifeCycleState *s)
{
    uint32_t lc_state;

    if (s->otp_ctrl) {
        OtOTPStateClass *oc =
            OBJECT_GET_CLASS(OtOTPStateClass, s->otp_ctrl, TYPE_OT_OTP);
        oc->get_lc_info(s->otp_ctrl, &lc_state, NULL);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "OTP controller not connected\n");
        lc_state = LC_STATE_INVALID;
    }

    return lc_state;
}

static uint32_t ot_lifecycle_get_lc_transition_count(OtLifeCycleState *s)
{
    uint32_t lc_tcount;

    if (s->otp_ctrl) {
        OtOTPStateClass *oc =
            OBJECT_GET_CLASS(OtOTPStateClass, s->otp_ctrl, TYPE_OT_OTP);
        oc->get_lc_info(s->otp_ctrl, NULL, &lc_tcount);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "OTP controller not connected\n");
        lc_tcount = LC_TRANSITION_COUNT_MAX + 1u;
    }

    return (uint32_t)lc_tcount;
}

static bool ot_lifecycle_is_known_state(uint32_t state)
{
    switch (state) {
    case LC_ENC_STATE_RAW:
    case LC_ENC_STATE_TESTUNLOCKED0:
    case LC_ENC_STATE_TESTLOCKED0:
    case LC_ENC_STATE_TESTUNLOCKED1:
    case LC_ENC_STATE_TESTLOCKED1:
    case LC_ENC_STATE_TESTUNLOCKED2:
    case LC_ENC_STATE_TESTLOCKED2:
    case LC_ENC_STATE_TESTUNLOCKED3:
    case LC_ENC_STATE_TESTLOCKED3:
    case LC_ENC_STATE_TESTUNLOCKED4:
    case LC_ENC_STATE_TESTLOCKED4:
    case LC_ENC_STATE_TESTUNLOCKED5:
    case LC_ENC_STATE_TESTLOCKED5:
    case LC_ENC_STATE_TESTUNLOCKED6:
    case LC_ENC_STATE_TESTLOCKED6:
    case LC_ENC_STATE_TESTUNLOCKED7:
    case LC_ENC_STATE_DEV:
    case LC_ENC_STATE_PROD:
    case LC_ENC_STATE_PRODEND:
    case LC_ENC_STATE_RMA:
    case LC_ENC_STATE_SCRAP:
        return true;
    default:
        return false;
    }
}

static bool ot_lifecycle_is_vendor_test_state(uint32_t state)
{
    switch (state) {
    case LC_ENC_STATE_RAW:
    case LC_ENC_STATE_TESTUNLOCKED0:
    case LC_ENC_STATE_TESTLOCKED0:
    case LC_ENC_STATE_TESTUNLOCKED1:
    case LC_ENC_STATE_TESTLOCKED1:
    case LC_ENC_STATE_TESTUNLOCKED2:
    case LC_ENC_STATE_TESTLOCKED2:
    case LC_ENC_STATE_TESTUNLOCKED3:
    case LC_ENC_STATE_TESTLOCKED3:
    case LC_ENC_STATE_TESTUNLOCKED4:
    case LC_ENC_STATE_TESTLOCKED4:
    case LC_ENC_STATE_TESTUNLOCKED5:
    case LC_ENC_STATE_TESTLOCKED5:
    case LC_ENC_STATE_TESTUNLOCKED6:
    case LC_ENC_STATE_TESTLOCKED6:
    case LC_ENC_STATE_TESTUNLOCKED7:
    case LC_ENC_STATE_RMA:
        return true;
    default:
        return false;
    }
}

static uint64_t ot_lifecycle_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtLifeCycleState *s = opaque;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_LC_TRANSITION_CNT:
        val32 = ot_lifecycle_get_lc_transition_count(s);
        break;
    case R_LC_STATE:
        val32 = ot_lifecycle_get_lc_state(s);
        break;
    case R_OTP_VENDOR_TEST_STATUS:
        val32 =
            ot_lifecycle_is_vendor_test_state(ot_lifecycle_get_lc_state(s)) ?
                s->regs[reg] :
                0u;
        break;
    case R_STATUS:
    case R_TRANSITION_CMD: /* r0w1c */
    case R_CLAIM_TRANSITION_IF_REGWEN:
    case R_CLAIM_TRANSITION_IF:
    case R_TRANSITION_REGWEN:
    case R_TRANSITION_CTRL:
    case R_TRANSITION_TOKEN_0:
    case R_TRANSITION_TOKEN_1:
    case R_TRANSITION_TOKEN_2:
    case R_TRANSITION_TOKEN_3:
    case R_TRANSITION_TARGET:
    case R_OTP_VENDOR_TEST_CTRL:
    case R_LC_ID_STATE:
    case R_HW_REVISION0:
    case R_HW_REVISION1:
    case R_DEVICE_ID_0:
    case R_DEVICE_ID_1:
    case R_DEVICE_ID_2:
    case R_DEVICE_ID_3:
    case R_DEVICE_ID_4:
    case R_DEVICE_ID_5:
    case R_DEVICE_ID_6:
    case R_DEVICE_ID_7:
    case R_MANUF_STATE_0:
    case R_MANUF_STATE_1:
    case R_MANUF_STATE_2:
    case R_MANUF_STATE_3:
    case R_MANUF_STATE_4:
    case R_MANUF_STATE_5:
    case R_MANUF_STATE_6:
    case R_MANUF_STATE_7:
        val32 = s->regs[reg];
        break;
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "W/O register 0x02%" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_lifecycle_io_read_out((unsigned)addr, REG_NAME(reg),
                                   (uint64_t)val32, pc);

    return (uint64_t)val32;
};

static void ot_lifecycle_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                    unsigned size)
{
    OtLifeCycleState *s = opaque;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_lifecycle_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

    switch (reg) {
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        if (val32) {
            ibex_irq_set(&s->alert, (int)val32);
        }
        break;
    case R_CLAIM_TRANSITION_IF_REGWEN:
        val32 &= R_CLAIM_TRANSITION_IF_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* rw0c */
        break;
    case R_CLAIM_TRANSITION_IF:
        if (s->regs[R_CLAIM_TRANSITION_IF_REGWEN] &
            R_CLAIM_TRANSITION_IF_REGWEN_EN_MASK) {
            val32 &= R_CLAIM_TRANSITION_IF_MUTEX_MASK;
            if (val32 == OT_MULTIBITBOOL8_TRUE) {
                if (!ot_lifecycle_own_hw_mutex(s)) {
                    ot_lifecycle_lock_hw_mutex(s);
                    s->regs[reg] = OT_MULTIBITBOOL8_TRUE;
                }
            } else if (val32 == 0u) {
                ot_lifecycle_release_hw_mutex(s);
                s->regs[reg] = OT_MULTIBITBOOL8_FALSE;
            }
        }
        break;
    case R_TRANSITION_CMD:
        val32 &= R_TRANSITION_CMD_START_MASK;
        if (ot_lifecycle_own_hw_mutex(s)) {
            ot_lifecycle_start_transition(s);
        }
        break;
    case R_TRANSITION_CTRL:
        val32 &= R_TRANSITION_CTRL_EXT_CLOCK_EN_MASK;
        /* VOLATILE_RAW_UNLOCK_BIT is not supported for now */
        if (val32) { /* rw1s */
            s->regs[reg] = val32;
        }
        break;
    case R_TRANSITION_TOKEN_0:
    case R_TRANSITION_TOKEN_1:
    case R_TRANSITION_TOKEN_2:
    case R_TRANSITION_TOKEN_3:
        if (ot_lifecycle_own_hw_mutex(s)) {
            s->regs[reg] = val32;
        }
        break;
    case R_TRANSITION_TARGET:
        if (ot_lifecycle_own_hw_mutex(s)) {
            val32 &= R_TRANSITION_TARGET_STATE_MASK;
            if (ot_lifecycle_is_known_state(val32)) {
                s->regs[reg] = val32;
            }
        }
        break;
    case R_OTP_VENDOR_TEST_CTRL:
        if (ot_lifecycle_own_hw_mutex(s)) {
            s->regs[reg] = val32;
        }
        break;
    case R_STATUS:
    case R_TRANSITION_REGWEN:
    case R_OTP_VENDOR_TEST_STATUS:
    case R_LC_STATE:
    case R_LC_TRANSITION_CNT:
    case R_LC_ID_STATE:
    case R_HW_REVISION0:
    case R_HW_REVISION1:
    case R_DEVICE_ID_0:
    case R_DEVICE_ID_1:
    case R_DEVICE_ID_2:
    case R_DEVICE_ID_3:
    case R_DEVICE_ID_4:
    case R_DEVICE_ID_5:
    case R_DEVICE_ID_6:
    case R_DEVICE_ID_7:
    case R_MANUF_STATE_0:
    case R_MANUF_STATE_1:
    case R_MANUF_STATE_2:
    case R_MANUF_STATE_3:
    case R_MANUF_STATE_4:
    case R_MANUF_STATE_5:
    case R_MANUF_STATE_6:
    case R_MANUF_STATE_7:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "R/O register 0x02%" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static Property ot_lifecycle_properties[] = {
    DEFINE_PROP_LINK("otp_ctrl", OtLifeCycleState, otp_ctrl, TYPE_OT_OTP,
                     OtOTPState *),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_lifecycle_regs_ops = {
    .read = &ot_lifecycle_regs_read,
    .write = &ot_lifecycle_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_lifecycle_reset(DeviceState *dev)
{
    OtLifeCycleState *s = OT_LIFECYCLE(dev);

    memset(s->regs, 0, REGS_SIZE);

    s->regs[R_CLAIM_TRANSITION_IF] = OT_MULTIBITBOOL8_FALSE;
    ibex_irq_set(&s->alert, 0);

    s->regs[R_CLAIM_TRANSITION_IF_REGWEN] = 1u;
    /* temporary, till lifecycle state management is implemented */
    s->regs[R_STATUS] = FIELD_DP32(s->regs[R_STATUS], STATUS, INITIALIZED, 1u);
    s->regs[R_STATUS] = FIELD_DP32(s->regs[R_STATUS], STATUS, READY, 1u);
}

static void ot_lifecycle_init(Object *obj)
{
    OtLifeCycleState *s = OT_LIFECYCLE(obj);

    memory_region_init_io(&s->mmio, obj, &ot_lifecycle_regs_ops, s,
                          TYPE_OT_LIFECYCLE, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(uint32_t, REGS_COUNT);
    ibex_qdev_init_irq(obj, &s->alert, OPENTITAN_DEVICE_ALERT);
}

static void ot_lifecycle_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ot_lifecycle_reset;
    device_class_set_props(dc, ot_lifecycle_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_lifecycle_info = {
    .name = TYPE_OT_LIFECYCLE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtLifeCycleState),
    .instance_init = &ot_lifecycle_init,
    .class_init = &ot_lifecycle_class_init,
};

static void ot_lifecycle_register_types(void)
{
    type_register_static(&ot_lifecycle_info);
}

type_init(ot_lifecycle_register_types)
