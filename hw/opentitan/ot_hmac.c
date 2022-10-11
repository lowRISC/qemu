/*
 * QEMU OpenTitan HMAC device
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
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
#include "qemu/bswap.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_clkmgr.h"
#include "hw/opentitan/ot_hmac.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "tomcrypt.h"
#include "trace.h"

/* Input FIFO length is 64 bytes (16 x 32 bits) */
#define OT_HMAC_FIFO_LENGTH 64u

/* Digest length is 32 bytes (256 bits) */
#define OT_HMAC_DIGEST_LENGTH 32u

/* HMAC key length is 32 bytes (256 bits) */
#define OT_HMAC_KEY_LENGTH 32u

/* Delay FIFO ingestion and compute by 100ns */
#define FIFO_TRIGGER_DELAY_NS 100u

/* clang-format off */
REG32(INTR_STATE, 0x00u)
    SHARED_FIELD(INTR_HMAC_DONE, 0u, 1u)
    SHARED_FIELD(INTR_FIFO_EMPTY, 1u, 1u)
    SHARED_FIELD(INTR_HMAC_ERR, 2u, 1u)
REG32(INTR_ENABLE, 0x04u)
REG32(INTR_TEST, 0x08u)
REG32(ALERT_TEST, 0x0cu)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(CFG, 0x10u)
    FIELD(CFG, HMAC_EN, 0u, 1u)
    FIELD(CFG, SHA_EN, 1u, 1u)
    FIELD(CFG, ENDIAN_SWAP, 2u, 1u)
    FIELD(CFG, DIGEST_SWAP, 3u, 1u)
REG32(CMD, 0x14u)
    FIELD(CMD, HASH_START, 0u, 1u)
    FIELD(CMD, HASH_PROCESS, 1u, 1u)
REG32(STATUS, 0x18u)
    FIELD(STATUS, FIFO_EMPTY, 0u, 1u)
    FIELD(STATUS, FIFO_FULL, 1u, 1u)
    FIELD(STATUS, FIFO_DEPTH, 4u, 5u)
REG32(ERR_CODE, 0x1cu)
#define R_ERR_CODE_PUSH_MSG_WHEN_SHA_DISABLED   0x00000001u
#define R_ERR_CODE_HASH_START_WHEN_SHA_DISABLED 0x00000002u
#define R_ERR_CODE_UPDATE_SECRET_KEY_INPROCESS  0x00000003u
#define R_ERR_CODE_HASH_START_WHEN_ACTIVE       0x00000004u
#define R_ERR_CODE_PUSH_MSG_WHEN_DISALLOWED     0x00000005u
REG32(WIPE_SECRET, 0x20u)
REG32(KEY_0, 0x24u)
REG32(KEY_1, 0x28u)
REG32(KEY_2, 0x2cu)
REG32(KEY_3, 0x30u)
REG32(KEY_4, 0x34u)
REG32(KEY_5, 0x38u)
REG32(KEY_6, 0x3cu)
REG32(KEY_7, 0x40u)
REG32(DIGEST_0, 0x44u)
REG32(DIGEST_1, 0x48u)
REG32(DIGEST_2, 0x4cu)
REG32(DIGEST_3, 0x50u)
REG32(DIGEST_4, 0x54u)
REG32(DIGEST_5, 0x58u)
REG32(DIGEST_6, 0x5cu)
REG32(DIGEST_7, 0x60u)
REG32(MSG_LENGTH_LOWER, 0x64u)
REG32(MSG_LENGTH_UPPER, 0x68u)
/* clang-format on */

#define INTR_MASK \
    (INTR_HMAC_ERR_MASK | INTR_FIFO_EMPTY_MASK | INTR_HMAC_DONE_MASK)

/* base offset for MMIO registers */
#define OT_HMAC_REGS_BASE 0x00000000u
/* base offset for MMIO FIFO */
#define OT_HMAC_FIFO_BASE 0x00000800u
/* length of MMIO FIFO */
#define OT_HMAC_FIFO_SIZE 0x00000800u
/* length of the whole device MMIO region */
#define OT_HMAC_WHOLE_SIZE (OT_HMAC_FIFO_BASE + OT_HMAC_FIFO_SIZE)

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_MSG_LENGTH_UPPER)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(CFG),
    REG_NAME_ENTRY(CMD),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(ERR_CODE),
    REG_NAME_ENTRY(WIPE_SECRET),
    REG_NAME_ENTRY(KEY_0),
    REG_NAME_ENTRY(KEY_1),
    REG_NAME_ENTRY(KEY_2),
    REG_NAME_ENTRY(KEY_3),
    REG_NAME_ENTRY(KEY_4),
    REG_NAME_ENTRY(KEY_5),
    REG_NAME_ENTRY(KEY_6),
    REG_NAME_ENTRY(KEY_7),
    REG_NAME_ENTRY(DIGEST_0),
    REG_NAME_ENTRY(DIGEST_1),
    REG_NAME_ENTRY(DIGEST_2),
    REG_NAME_ENTRY(DIGEST_3),
    REG_NAME_ENTRY(DIGEST_4),
    REG_NAME_ENTRY(DIGEST_5),
    REG_NAME_ENTRY(DIGEST_6),
    REG_NAME_ENTRY(DIGEST_7),
    REG_NAME_ENTRY(MSG_LENGTH_LOWER),
    REG_NAME_ENTRY(MSG_LENGTH_UPPER),
};
#undef REG_NAME_ENTRY

struct OtHMACRegisters {
    uint32_t intr_state;
    uint32_t intr_enable;
    uint32_t alert_test;
    uint32_t cfg;
    uint32_t cmd;
    uint32_t err_code;
    uint32_t wipe_secret;
    uint32_t key[OT_HMAC_KEY_LENGTH / sizeof(uint32_t)];
    uint32_t digest[OT_HMAC_DIGEST_LENGTH / sizeof(uint32_t)];
    uint64_t msg_length;
};
typedef struct OtHMACRegisters OtHMACRegisters;

struct OtHMACContext {
    hash_state state;
};
typedef struct OtHMACContext OtHMACContext;

struct OtHMACState {
    /* <private> */
    SysBusDevice parent_obj;

    /* <public> */
    MemoryRegion mmio;
    MemoryRegion regs_mmio;
    MemoryRegion fifo_mmio;

    IbexIRQ irq_done;
    IbexIRQ irq_fifo_empty;
    IbexIRQ irq_hmac_err;
    IbexIRQ alert;
    IbexIRQ clkmgr;

    OtHMACRegisters *regs;
    OtHMACContext *ctx;

    Fifo8 input_fifo;
    QEMUTimer *fifo_trigger_handle;
};

static void ot_hmac_update_irqs(OtHMACState *s)
{
    uint32_t irq_masked = s->regs->intr_state & s->regs->intr_enable;
    bool level;

    level = irq_masked & INTR_HMAC_DONE_MASK;
    ibex_irq_set(&s->irq_done, level);

    level = irq_masked & INTR_FIFO_EMPTY_MASK;
    ibex_irq_set(&s->irq_fifo_empty, level);

    level = irq_masked & INTR_HMAC_ERR_MASK;
    ibex_irq_set(&s->irq_hmac_err, level);
}

static void ot_hmac_update_alert(OtHMACState *s)
{
    bool level = s->regs->alert_test & R_ALERT_TEST_FATAL_FAULT_MASK;
    ibex_irq_set(&s->alert, level);
}

static void ot_hmac_report_error(OtHMACState *s, uint32_t error)
{
    s->regs->err_code = error;
    s->regs->intr_state |= INTR_HMAC_ERR_MASK;
    ot_hmac_update_irqs(s);
}

static void ot_hmac_compute_digest(OtHMACState *s)
{
    trace_ot_hmac_debug("ot_hmac_compute_digest");

    /* HMAC mode, perform outer hash */
    if (s->regs->cfg & R_CFG_HMAC_EN_MASK) {
        sha256_done(&s->ctx->state, (uint8_t *)s->regs->digest);

        uint64_t opad[8u];
        memset(opad, 0, sizeof(opad));
        memcpy(opad, s->regs->key, sizeof(s->regs->key));
        for (unsigned i = 0; i < ARRAY_SIZE(opad); i++) {
            opad[i] ^= 0x5c5c5c5c5c5c5c5cu;
        }
        sha256_init(&s->ctx->state);
        sha256_process(&s->ctx->state, (const uint8_t *)opad, sizeof(opad));
        sha256_process(&s->ctx->state, (const uint8_t *)s->regs->digest,
                       sizeof(s->regs->digest));
    }

    sha256_done(&s->ctx->state, (uint8_t *)s->regs->digest);
}

static void ot_hmac_fifo_trigger_update(void *opaque)
{
    OtHMACState *s = opaque;

    trace_ot_hmac_debug("ot_hmac_fifo_trigger_update");

    if (!fifo8_is_empty(&s->input_fifo)) {
        while (!fifo8_is_empty(&s->input_fifo)) {
            uint8_t value = fifo8_pop(&s->input_fifo);
            sha256_process(&s->ctx->state, &value, 1);
        }

        /* assert FIFO Empty IRQ */
        s->regs->intr_state |= INTR_FIFO_EMPTY_MASK;
    }

    if (s->regs->cmd & R_CMD_HASH_PROCESS_MASK) {
        ot_hmac_compute_digest(s);
        s->regs->intr_state |= INTR_HMAC_DONE_MASK;
        s->regs->cmd = 0;
    }

    ot_hmac_update_irqs(s);

    ibex_irq_set(&s->clkmgr,
                 !fifo8_is_empty(&s->input_fifo) || (bool)s->regs->cmd);
}

static inline void ot_hmac_wipe_buffer(OtHMACState *s, uint32_t *buffer,
                                       size_t size)
{
    for (unsigned index = 0; index < size; index++) {
        buffer[index] = s->regs->wipe_secret;
    }
}

static uint64_t ot_hmac_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtHMACState *s = OT_HMAC(opaque);
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_INTR_STATE:
        val32 = s->regs->intr_state;
        break;
    case R_INTR_ENABLE:
        val32 = s->regs->intr_enable;
        break;
    case R_CFG:
        val32 = s->regs->cfg;
        break;
    case R_CMD:
        val32 = 0;
        /* always read 0: CMD is r0w1c */
        break;
    case R_STATUS: {
        uint32_t num_used = fifo8_num_used(&s->input_fifo);
        if (num_used == 0) {
            val32 = R_STATUS_FIFO_EMPTY_MASK;
        } else {
            val32 = ((num_used / 4u) << R_STATUS_FIFO_DEPTH_SHIFT) &
                    R_STATUS_FIFO_DEPTH_MASK;
            if (num_used == OT_HMAC_FIFO_LENGTH) {
                val32 |= R_STATUS_FIFO_FULL_MASK;
            }
        }
    } break;
    case R_ERR_CODE:
        val32 = s->regs->err_code;
        break;
    case R_DIGEST_0:
    case R_DIGEST_1:
    case R_DIGEST_2:
    case R_DIGEST_3:
    case R_DIGEST_4:
    case R_DIGEST_5:
    case R_DIGEST_6:
    case R_DIGEST_7:
        if (s->regs->cfg & R_CFG_DIGEST_SWAP_MASK) {
            val32 = s->regs->digest[reg - R_DIGEST_0];
        } else {
            val32 = bswap32(s->regs->digest[reg - R_DIGEST_0]);
        }
        break;
    case R_MSG_LENGTH_LOWER:
        val32 = s->regs->msg_length;
        break;
    case R_MSG_LENGTH_UPPER:
        val32 = s->regs->msg_length >> 32u;
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_WIPE_SECRET:
    case R_KEY_0:
    case R_KEY_1:
    case R_KEY_2:
    case R_KEY_3:
    case R_KEY_4:
    case R_KEY_5:
    case R_KEY_6:
    case R_KEY_7:
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
    trace_ot_hmac_io_read_out((unsigned)addr, REG_NAME(reg), val32, pc);

    return (uint64_t)val32;
}

static void ot_hmac_regs_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    OtHMACState *s = OT_HMAC(opaque);
    uint32_t val32 = (uint32_t)value;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_hmac_io_write((unsigned)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_INTR_STATE:
        s->regs->intr_state &= ~(val32 & INTR_MASK);
        ot_hmac_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        s->regs->intr_enable = val32 & INTR_MASK;
        ot_hmac_update_irqs(s);
        break;
    case R_INTR_TEST:
        s->regs->intr_state |= val32 & INTR_MASK;
        ot_hmac_update_irqs(s);
        break;
    case R_ALERT_TEST:
        s->regs->alert_test |= val32 & R_ALERT_TEST_FATAL_FAULT_MASK;
        ot_hmac_update_alert(s);
        break;
    case R_CFG:
        /* ignore write if engine is not idle */
        if (s->regs->cmd) {
            break;
        }

        s->regs->cfg =
            val32 & (R_CFG_HMAC_EN_MASK | R_CFG_SHA_EN_MASK |
                     R_CFG_ENDIAN_SWAP_MASK | R_CFG_DIGEST_SWAP_MASK);

        /* clear digest when SHA is disabled */
        if (!(s->regs->cfg & R_CFG_SHA_EN_MASK)) {
            ot_hmac_wipe_buffer(s, s->regs->digest,
                                ARRAY_SIZE(s->regs->digest));
        }
        break;
    case R_CMD:
        if (val32 & R_CMD_HASH_START_MASK) {
            if (!(s->regs->cfg & R_CFG_SHA_EN_MASK)) {
                ot_hmac_report_error(s,
                                     R_ERR_CODE_HASH_START_WHEN_SHA_DISABLED);
                break;
            }
            if (s->regs->cmd) {
                ot_hmac_report_error(s, R_ERR_CODE_HASH_START_WHEN_ACTIVE);
                break;
            }
            s->regs->cmd = R_CMD_HASH_START_MASK;
            s->regs->msg_length = 0;

            ibex_irq_set(&s->clkmgr, true);

            sha256_init(&s->ctx->state);

            /* HMAC mode, process input padding */
            if (s->regs->cfg & R_CFG_HMAC_EN_MASK) {
                uint64_t ipad[8u];
                memset(ipad, 0, sizeof(ipad));
                memcpy(ipad, s->regs->key, sizeof(s->regs->key));
                for (unsigned i = 0; i < ARRAY_SIZE(ipad); i++) {
                    ipad[i] ^= 0x3636363636363636u;
                }
                sha256_process(&s->ctx->state, (const uint8_t *)ipad,
                               sizeof(ipad));
            }
        }

        if (val32 & R_CMD_HASH_PROCESS_MASK) {
            if (!(s->regs->cmd & R_CMD_HASH_START_MASK)) {
                qemu_log_mask(
                    LOG_GUEST_ERROR,
                    "%s: CMD.PROCESS requested but hash not started yet\n",
                    __func__);
                break;
            }
            if (s->regs->cmd & R_CMD_HASH_PROCESS_MASK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: CMD.PROCESS requested but hash is currently "
                              "processing\n",
                              __func__);
                break;
            }
            s->regs->cmd |= R_CMD_HASH_PROCESS_MASK;

            /* trigger delayed processing of FIFO */
            timer_del(s->fifo_trigger_handle);
            ibex_irq_set(&s->clkmgr, true);
            timer_mod(s->fifo_trigger_handle,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          FIFO_TRIGGER_DELAY_NS);
        }
        break;
    case R_WIPE_SECRET:
        /* TODO ignore write if engine is not idle? */
        s->regs->wipe_secret = val32;
        ot_hmac_wipe_buffer(s, s->regs->key, ARRAY_SIZE(s->regs->key));
        ot_hmac_wipe_buffer(s, s->regs->digest, ARRAY_SIZE(s->regs->digest));
        break;
    case R_KEY_0:
    case R_KEY_1:
    case R_KEY_2:
    case R_KEY_3:
    case R_KEY_4:
    case R_KEY_5:
    case R_KEY_6:
    case R_KEY_7:
        /* ignore write and report error if engine is not idle */
        if (s->regs->cmd) {
            ot_hmac_report_error(s, R_ERR_CODE_UPDATE_SECRET_KEY_INPROCESS);
            break;
        }
        s->regs->key[reg - R_KEY_0] = bswap32(val32);
        break;
    case R_STATUS:
    case R_ERR_CODE:
    case R_DIGEST_0:
    case R_DIGEST_1:
    case R_DIGEST_2:
    case R_DIGEST_3:
    case R_DIGEST_4:
    case R_DIGEST_5:
    case R_DIGEST_6:
    case R_DIGEST_7:
    case R_MSG_LENGTH_LOWER:
    case R_MSG_LENGTH_UPPER:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "R/O register 0x02%" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static uint64_t ot_hmac_fifo_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: MSG_FIFO is write only\n", __func__);

    return 0;
}

static void ot_hmac_fifo_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    OtHMACState *s = OT_HMAC(opaque);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_hmac_fifo_write((unsigned int)addr, (uint32_t)value, size, pc);

    if (!s->regs->cmd) {
        ot_hmac_report_error(s, R_ERR_CODE_PUSH_MSG_WHEN_DISALLOWED);
        return;
    }

    if (!(s->regs->cfg & R_CFG_SHA_EN_MASK)) {
        ot_hmac_report_error(s, R_ERR_CODE_PUSH_MSG_WHEN_SHA_DISABLED);
        return;
    }

    if (s->regs->cfg & R_CFG_ENDIAN_SWAP_MASK) {
        if (size == 4u) {
            value = bswap32((uint32_t)value);
        } else if (size == 2u) {
            value = bswap16((uint16_t)value);
        }
    }

    ibex_irq_set(&s->clkmgr, true);

    for (unsigned i = 0; i < size; i++) {
        uint8_t b = value;
        if (fifo8_is_full(&s->input_fifo)) {
            /* FIFO full. Should stall but cannot be done in QEMU? */
            ot_hmac_fifo_trigger_update(s);
        }
        fifo8_push(&s->input_fifo, b);
        value >>= 8u;
    }

    s->regs->msg_length += 8u * size;

    /* trigger delayed processing of FIFO */
    timer_del(s->fifo_trigger_handle);
    timer_mod(s->fifo_trigger_handle,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FIFO_TRIGGER_DELAY_NS);
}

static Property ot_hmac_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_hmac_regs_ops = {
    .read = &ot_hmac_regs_read,
    .write = &ot_hmac_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4u,
        .max_access_size = 4u,
    },
};

static const MemoryRegionOps ot_hmac_fifo_ops = {
    .read = &ot_hmac_fifo_read,
    .write = &ot_hmac_fifo_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1u,
        .max_access_size = 4u,
    },
};

static void ot_hmac_init(Object *obj)
{
    OtHMACState *s = OT_HMAC(obj);

    s->regs = g_new0(OtHMACRegisters, 1u);
    s->ctx = g_new(OtHMACContext, 1u);

    ibex_sysbus_init_irq(obj, &s->irq_done);
    ibex_sysbus_init_irq(obj, &s->irq_fifo_empty);
    ibex_sysbus_init_irq(obj, &s->irq_hmac_err);
    ibex_qdev_init_irq(obj, &s->alert, OPENTITAN_DEVICE_ALERT);
    ibex_qdev_init_irq(obj, &s->clkmgr, OPENTITAN_CLOCK_ACTIVE);

    memory_region_init(&s->mmio, OBJECT(s), TYPE_OT_HMAC, OT_HMAC_WHOLE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    memory_region_init_io(&s->regs_mmio, obj, &ot_hmac_regs_ops, s,
                          TYPE_OT_HMAC "-regs", REGS_SIZE);
    memory_region_add_subregion(&s->mmio, OT_HMAC_REGS_BASE, &s->regs_mmio);

    memory_region_init_io(&s->fifo_mmio, obj, &ot_hmac_fifo_ops, s,
                          TYPE_OT_HMAC "-fifo", OT_HMAC_FIFO_SIZE);
    memory_region_add_subregion(&s->mmio, OT_HMAC_FIFO_BASE, &s->fifo_mmio);

    /* setup FIFO Interrupt Timer */
    s->fifo_trigger_handle =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, &ot_hmac_fifo_trigger_update, s);

    /* FIFO sizes as per OT Spec */
    fifo8_create(&s->input_fifo, OT_HMAC_FIFO_LENGTH);
}

static void ot_hmac_reset(DeviceState *dev)
{
    OtHMACState *s = OT_HMAC(dev);

    timer_del(s->fifo_trigger_handle);
    ibex_irq_set(&s->clkmgr, false);

    memset(s->ctx, 0, sizeof(*(s->ctx)));
    memset(s->regs, 0, sizeof(*(s->regs)));

    ot_hmac_update_irqs(s);
    ot_hmac_update_alert(s);

    fifo8_reset(&s->input_fifo);
}

static void ot_hmac_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ot_hmac_reset;
    device_class_set_props(dc, ot_hmac_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_hmac_info = {
    .name = TYPE_OT_HMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtHMACState),
    .instance_init = &ot_hmac_init,
    .class_init = &ot_hmac_class_init,
};

static void ot_hmac_register_types(void)
{
    type_register_static(&ot_hmac_info);
}

type_init(ot_hmac_register_types)
