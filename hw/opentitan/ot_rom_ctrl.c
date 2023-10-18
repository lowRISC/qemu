/*
 * QEMU OpenTitan ROM controller
 *
 * Copyright (c) 2023 Rivos, Inc.
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
 *
 * Note: This implementation is missing some features:
 *   - Scrambling (including loading of scrambled VMEM files)
 *   - KeyMgr interface (to send digest to Key Manager)
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "elf.h"
#include "hw/loader.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_kmac.h"
#include "hw/opentitan/ot_rom_ctrl.h"
#include "hw/opentitan/ot_rom_ctrl_img.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"


#define PARAM_NUM_ALERTS 1u

#define ROM_DIGEST_WORDS 8u
#define ROM_DIGEST_BYTES (ROM_DIGEST_WORDS * sizeof(uint32_t))

/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, FATAL_ERROR, 0u, 1u)
REG32(FATAL_ALERT_CAUSE, 0x4u)
    FIELD(FATAL_ALERT_CAUSE, CHECKER_ERROR, 0u, 1u)
    FIELD(FATAL_ALERT_CAUSE, INTEGRITY_ERROR, 1u, 1u)
REG32(DIGEST_0, 0x8u)
REG32(DIGEST_1, 0xcu)
REG32(DIGEST_2, 0x10u)
REG32(DIGEST_3, 0x14u)
REG32(DIGEST_4, 0x18u)
REG32(DIGEST_5, 0x1cu)
REG32(DIGEST_6, 0x20u)
REG32(DIGEST_7, 0x24u)
REG32(EXP_DIGEST_0, 0x28u)
REG32(EXP_DIGEST_1, 0x2cu)
REG32(EXP_DIGEST_2, 0x30u)
REG32(EXP_DIGEST_3, 0x34u)
REG32(EXP_DIGEST_4, 0x38u)
REG32(EXP_DIGEST_5, 0x3cu)
REG32(EXP_DIGEST_6, 0x40u)
REG32(EXP_DIGEST_7, 0x44u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_EXP_DIGEST_7)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(ALERT_TEST),   REG_NAME_ENTRY(FATAL_ALERT_CAUSE),
    REG_NAME_ENTRY(DIGEST_0),     REG_NAME_ENTRY(DIGEST_1),
    REG_NAME_ENTRY(DIGEST_2),     REG_NAME_ENTRY(DIGEST_3),
    REG_NAME_ENTRY(DIGEST_4),     REG_NAME_ENTRY(DIGEST_5),
    REG_NAME_ENTRY(DIGEST_6),     REG_NAME_ENTRY(DIGEST_7),
    REG_NAME_ENTRY(EXP_DIGEST_0), REG_NAME_ENTRY(EXP_DIGEST_1),
    REG_NAME_ENTRY(EXP_DIGEST_2), REG_NAME_ENTRY(EXP_DIGEST_3),
    REG_NAME_ENTRY(EXP_DIGEST_4), REG_NAME_ENTRY(EXP_DIGEST_5),
    REG_NAME_ENTRY(EXP_DIGEST_6), REG_NAME_ENTRY(EXP_DIGEST_7),
};
#undef REG_NAME_ENTRY

static const OtKMACAppCfg kmac_app_cfg = {
    .mode = OT_KMAC_MODE_CSHAKE,
    .strength = 256u,
    .prefix = {
        .funcname = { "" },
        .funcname_len = 0,
        .customstr = { "ROM_CTRL" },
        .customstr_len = 8u,
    },
};

struct OtRomCtrlClass {
    DeviceClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

struct OtRomCtrlState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    MemoryRegion mmio;
    IbexIRQ pwrmgr_good;
    IbexIRQ pwrmgr_done;
    IbexIRQ alert;

    uint32_t regs[REGS_COUNT];

    hwaddr digest_offset;

    uint32_t size;
    OtKMACState *kmac;
    char *rom_id;
    uint8_t kmac_app;

    bool first_reset;
    bool fake_digest;
};

static void ot_rom_ctrl_get_mem_bounds(OtRomCtrlState *s, hwaddr *minaddr,
                                       hwaddr *maxaddr)
{
    *minaddr = s->mem.addr;
    *maxaddr = s->mem.addr + (hwaddr)memory_region_size(&s->mem);
}

static void ot_rom_ctrl_load_elf(OtRomCtrlState *s, const OtRomImg *ri)
{
    AddressSpace *as = ot_common_get_local_address_space(DEVICE(s));
    hwaddr minaddr;
    hwaddr maxaddr;
    ot_rom_ctrl_get_mem_bounds(s, &minaddr, &maxaddr);
    uint64_t loaddr;
    if (load_elf_ram_sym(ri->filename, NULL, NULL, NULL, NULL, &loaddr, NULL,
                         NULL, 0, EM_RISCV, 1, 0, as, false, NULL) <= 0) {
        error_setg(&error_fatal,
                   "ot_rom_ctrl: %s: ROM image '%s', ELF loading failed",
                   s->rom_id, ri->filename);
        return;
    }
    if ((loaddr < minaddr) || (loaddr > maxaddr)) {
        /* cannot test upper load address as QEMU loader returns VMA, not LMA */
        error_setg(&error_fatal, "ot_rom_ctrl: %s: ELF cannot fit into ROM\n",
                   s->rom_id);
    }
}

static void ot_rom_ctrl_load_binary(OtRomCtrlState *s, const OtRomImg *ri)
{
    hwaddr minaddr;
    hwaddr maxaddr;
    ot_rom_ctrl_get_mem_bounds(s, &minaddr, &maxaddr);

    hwaddr binaddr = (hwaddr)ri->address;

    if (binaddr < minaddr) {
        error_setg(&error_fatal, "ot_rom_ctrl: %s: address 0x%x: not in ROM:\n",
                   s->rom_id, ri->address);
    }

    int fd = open(ri->filename, O_RDONLY | O_BINARY | O_CLOEXEC);
    if (fd == -1) {
        error_setg(&error_fatal,
                   "ot_rom_ctrl: %s: could not open ROM '%s': %s\n",
                   ri->filename, s->rom_id, strerror(errno));
    }

    ssize_t binsize = (ssize_t)lseek(fd, 0, SEEK_END);
    if (binsize == -1) {
        close(fd);
        error_setg(&error_fatal,
                   "ot_rom_ctrl: %s: file %s: get size error: %s\n", s->rom_id,
                   ri->filename, strerror(errno));
    }

    if (binaddr + binsize > maxaddr) {
        close(fd);
        error_setg(&error_fatal, "ot_rom_ctrl: cannot fit into ROM\n");
    }

    uint8_t *data = g_malloc0(binsize);
    lseek(fd, 0, SEEK_SET);

    ssize_t rc = read(fd, data, binsize);
    close(fd);
    if (rc != binsize) {
        g_free(data);
        error_setg(
            &error_fatal,
            "ot_rom_ctrl: %s: file %s: read error: rc=%zd (expected %zd)\n",
            s->rom_id, ri->filename, rc, binsize);
        return; /* static analyzer does not know error_fatal never returns */
    }

    hwaddr offset = binaddr - minaddr;

    uintptr_t hostptr = (uintptr_t)memory_region_get_ram_ptr(&s->mem);
    hostptr += offset;
    memcpy((void *)hostptr, data, binsize);
    g_free(data);

    memory_region_set_dirty(&s->mem, offset, binsize);
}


static void ot_rom_ctrl_load_rom(OtRomCtrlState *s)
{
    Object *obj = NULL;
    OtRomImg *rom_img = NULL;

    /* let assume we'll use fake digest */
    s->fake_digest = true;

    /* try to find our ROM image object */
    obj = object_resolve_path_component(object_get_objects_root(), s->rom_id);
    if (!obj) {
        return;
    }
    rom_img = (OtRomImg *)object_dynamic_cast(obj, TYPE_OT_ROM_IMG);
    if (!rom_img) {
        error_setg(&error_fatal, "ot_rom_ctrl: %s: Object is not a ROM Image",
                   s->rom_id);
        return;
    }

    if (rom_img->address == UINT32_MAX) {
        ot_rom_ctrl_load_elf(s, rom_img);
    } else {
        ot_rom_ctrl_load_binary(s, rom_img);
    }

    /* check if fake digest is requested */
    if (rom_img->fake_digest) {
        return;
    }

    /* copy digest to registers */
    g_assert(rom_img->digest);
    g_assert(rom_img->digest_len == ROM_DIGEST_BYTES);
    s->fake_digest = false;
    for (unsigned ix = 0; ix < ROM_DIGEST_WORDS; ix++) {
        memcpy(&s->regs[R_EXP_DIGEST_0 + ix],
               &rom_img->digest[ix * sizeof(uint32_t)], sizeof(uint32_t));
    }
}

static void ot_rom_ctrl_compare_and_notify(OtRomCtrlState *s)
{
    /* compare digests */
    bool rom_good = true;
    for (unsigned ix = 0; ix < ROM_DIGEST_WORDS; ix++) {
        if (s->regs[R_EXP_DIGEST_0 + ix] != s->regs[R_DIGEST_0 + ix]) {
            rom_good = false;
            error_setg(
                &error_fatal,
                "ot_rom_ctrl: %s: DIGEST_%u mismatch (expected 0x%08x got "
                "0x%08x)",
                s->rom_id, ix, s->regs[R_EXP_DIGEST_0 + ix],
                s->regs[R_DIGEST_0 + ix]);
        }
    }

    /* notify end of check */
    ibex_irq_set(&s->pwrmgr_good, rom_good);
    ibex_irq_set(&s->pwrmgr_done, true);
}

static uint64_t ot_rom_ctrl_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtRomCtrlState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_FATAL_ALERT_CAUSE:
    case R_DIGEST_0:
    case R_DIGEST_1:
    case R_DIGEST_2:
    case R_DIGEST_3:
    case R_DIGEST_4:
    case R_DIGEST_5:
    case R_DIGEST_6:
    case R_DIGEST_7:
    case R_EXP_DIGEST_0:
    case R_EXP_DIGEST_1:
    case R_EXP_DIGEST_2:
    case R_EXP_DIGEST_3:
    case R_EXP_DIGEST_4:
    case R_EXP_DIGEST_5:
    case R_EXP_DIGEST_6:
    case R_EXP_DIGEST_7:
        val32 = s->regs[reg];
        break;
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "W/O register 0x%02" HWADDR_PRIx " (%s)\n", addr,
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
    trace_ot_rom_ctrl_io_read_out((unsigned)addr, REG_NAME(reg),
                                  (uint64_t)val32, pc);

    return (uint64_t)val32;
};

static void ot_rom_ctrl_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                   unsigned size)
{
    OtRomCtrlState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_rom_ctrl_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

    switch (reg) {
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_ERROR_MASK;
        if (val32) {
            ibex_irq_set(&s->alert, (int)val32);
        }
        break;
    case R_FATAL_ALERT_CAUSE:
    case R_DIGEST_0:
    case R_DIGEST_1:
    case R_DIGEST_2:
    case R_DIGEST_3:
    case R_DIGEST_4:
    case R_DIGEST_5:
    case R_DIGEST_6:
    case R_DIGEST_7:
    case R_EXP_DIGEST_0:
    case R_EXP_DIGEST_1:
    case R_EXP_DIGEST_2:
    case R_EXP_DIGEST_3:
    case R_EXP_DIGEST_4:
    case R_EXP_DIGEST_5:
    case R_EXP_DIGEST_6:
    case R_EXP_DIGEST_7:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: R/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static Property ot_rom_ctrl_properties[] = {
    DEFINE_PROP_STRING("rom_id", OtRomCtrlState, rom_id),
    DEFINE_PROP_UINT32("size", OtRomCtrlState, size, 0u),
    DEFINE_PROP_LINK("kmac", OtRomCtrlState, kmac, TYPE_OT_KMAC, OtKMACState *),
    DEFINE_PROP_UINT8("kmac-app", OtRomCtrlState, kmac_app, UINT8_MAX),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_rom_ctrl_regs_ops = {
    .read = &ot_rom_ctrl_regs_read,
    .write = &ot_rom_ctrl_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_rom_ctrl_send_kmac_req(OtRomCtrlState *s)
{
    OtKMACAppReq req;
    uint8_t *rom_ptr = (uint8_t *)memory_region_get_ram_ptr(&s->mem);
    size_t size = MIN(s->size - ROM_DIGEST_BYTES - s->digest_offset,
                      OT_KMAC_APP_MSG_BYTES);

    memcpy(req.msg_data, rom_ptr + s->digest_offset, size);
    req.msg_len = size;
    req.last = s->digest_offset == (s->size - ROM_DIGEST_BYTES);
    s->digest_offset += size;

    ot_kmac_app_request(s->kmac, s->kmac_app, &req);
}

static void
ot_rom_ctrl_handle_kmac_response(void *opaque, const OtKMACAppRsp *rsp)
{
    OtRomCtrlState *s = OT_ROM_CTRL(opaque);

    if (rsp->done) {
        g_assert(s->digest_offset == (s->size - ROM_DIGEST_BYTES));

        /* switch to ROMD mode */
        memory_region_rom_device_set_romd(&s->mem, true);

        /* retrieve digest */
        for (unsigned ix = 0; ix < 8; ix++) {
            uint32_t share0;
            uint32_t share1;
            memcpy(&share0, &rsp->digest_share0[ix * sizeof(uint32_t)],
                   sizeof(uint32_t));
            memcpy(&share1, &rsp->digest_share1[ix * sizeof(uint32_t)],
                   sizeof(uint32_t));
            s->regs[R_DIGEST_0 + ix] = share0 ^ share1;
            /* "fake digest" mode enabled, copy computed digest to expected */
            if (s->fake_digest) {
                s->regs[R_EXP_DIGEST_0 + ix] = s->regs[R_DIGEST_0 + ix];
            }
        }

        /* compare digests and send notification */
        ot_rom_ctrl_compare_and_notify(s);
    } else {
        ot_rom_ctrl_send_kmac_req(s);
    }
}

static void ot_rom_ctrl_reset_hold(Object *obj)
{
    OtRomCtrlClass *c = OT_ROM_CTRL_GET_CLASS(obj);
    OtRomCtrlState *s = OT_ROM_CTRL(obj);

    if (c->parent_phases.hold) {
        c->parent_phases.hold(obj);
    }

    /* reset all registers on first reset, otherwise keep digests */
    if (s->first_reset) {
        memset(s->regs, 0, REGS_SIZE);
    } else {
        s->regs[R_ALERT_TEST] = 0;
        s->regs[R_FATAL_ALERT_CAUSE] = 0;
    }

    ibex_irq_set(&s->pwrmgr_good, false);
    ibex_irq_set(&s->pwrmgr_done, false);

    /* connect to KMAC */
    ot_kmac_connect_app(s->kmac, s->kmac_app, &kmac_app_cfg,
                        ot_rom_ctrl_handle_kmac_response, s);
}

static void ot_rom_ctrl_reset_exit(Object *obj)
{
    OtRomCtrlClass *c = OT_ROM_CTRL_GET_CLASS(obj);
    OtRomCtrlState *s = OT_ROM_CTRL(obj);
    uint8_t *rom_ptr = (uint8_t *)memory_region_get_ram_ptr(&s->mem);

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj);
    }

    /* on initial reset, load ROM then set it read-only */
    if (s->first_reset) {
        /* pre-fill ROM region with zeros */
        memset(rom_ptr, 0, s->size);

        /* load ROM from file */
        ot_rom_ctrl_load_rom(s);

        /* ensure ROM can no longer be written */
        s->first_reset = false;

        /* start computing ROM digest */
        s->digest_offset = 0;
        ot_rom_ctrl_send_kmac_req(s);
    } else {
        /* only compare existing digests and send notification to pwrmgr */
        ot_rom_ctrl_compare_and_notify(s);
    }
}

static void ot_rom_ctrl_mem_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    OtRomCtrlState *s = opaque;
    uint64_t pc = ibex_get_current_pc();

    trace_ot_rom_ctrl_mem_write((unsigned int)addr, (uint32_t)value, pc);

    uint8_t *rom_ptr = (uint8_t *)memory_region_get_ram_ptr(&s->mem);

    if ((addr + size) <= s->size) {
        stn_le_p(&rom_ptr[addr], (int)size, value);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx ", pc=0x%x\n",
                      __func__, s->rom_id, addr, (unsigned)pc);
    }
}

static bool ot_rom_ctrl_mem_accepts(void *opaque, hwaddr addr, unsigned size,
                                    bool is_write, MemTxAttrs attrs)
{
    OtRomCtrlState *s = opaque;
    (void)attrs;
    uint64_t pc = ibex_get_current_pc();

    trace_ot_rom_ctrl_mem_accepts((unsigned int)addr, is_write, pc);

    if (!is_write) {
        /*
         * only allow reads during first reset (after complete check, MR gets
         * turned to ROMD mode where mem_ops->valid.accepts is no longer called.
         */
        return s->first_reset;
    }

    return ((addr + size) <= s->size && s->first_reset);
}

static const MemoryRegionOps ot_rom_ctrl_mem_ops = {
    .write = &ot_rom_ctrl_mem_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 1u,
    .impl.max_access_size = 4u,
    .valid.accepts = &ot_rom_ctrl_mem_accepts,
};

static void ot_rom_ctrl_realize(DeviceState *dev, Error **errp)
{
    OtRomCtrlState *s = OT_ROM_CTRL(dev);

    g_assert(s->rom_id);
    g_assert(s->size);
    g_assert(s->kmac);
    g_assert(s->kmac_app != UINT8_MAX);

    memory_region_init_rom_device_nomigrate(&s->mem, OBJECT(dev),
                                            &ot_rom_ctrl_mem_ops, s,
                                            TYPE_OT_ROM_CTRL "-mem", s->size,
                                            errp);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mem);

    /*
     * at creation, set to read-write and disable ROMD mode:
     * - read-write required for initial loading of ROM content
     * - ROMD mode disabled effectively disables all reads until ROMD is enabled
     * again after a successful digest check (mem_ops.valid.accepts rejects
     * reads).
     */
    s->first_reset = true;
    memory_region_rom_device_set_romd(&s->mem, false);
}

static void ot_rom_ctrl_init(Object *obj)
{
    OtRomCtrlState *s = OT_ROM_CTRL(obj);

    ibex_qdev_init_irq(obj, &s->pwrmgr_good, OPENTITAN_ROM_CTRL_GOOD);
    ibex_qdev_init_irq(obj, &s->pwrmgr_done, OPENTITAN_ROM_CTRL_DONE);

    memory_region_init_io(&s->mmio, obj, &ot_rom_ctrl_regs_ops, s,
                          TYPE_OT_ROM_CTRL "-regs", REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    ibex_qdev_init_irq(obj, &s->alert, OPENTITAN_DEVICE_ALERT);
}

static void ot_rom_ctrl_class_init(ObjectClass *klass, void *data)
{
    OtRomCtrlClass *rcc = OT_ROM_CTRL_CLASS(klass);
    (void)data;

    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(dc);

    resettable_class_set_parent_phases(rc, NULL, &ot_rom_ctrl_reset_hold,
                                       &ot_rom_ctrl_reset_exit,
                                       &rcc->parent_phases);
    dc->realize = &ot_rom_ctrl_realize;
    device_class_set_props(dc, ot_rom_ctrl_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_rom_ctrl_info = {
    .name = TYPE_OT_ROM_CTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtRomCtrlState),
    .instance_init = &ot_rom_ctrl_init,
    .class_size = sizeof(OtRomCtrlClass),
    .class_init = &ot_rom_ctrl_class_init,
};

static void ot_rom_ctrl_register_types(void)
{
    type_register_static(&ot_rom_ctrl_info);
}

type_init(ot_rom_ctrl_register_types)
