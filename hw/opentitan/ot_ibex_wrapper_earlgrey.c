/*
 * QEMU OpenTitan EarlGrey Ibex wrapper device
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
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
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_ibex_wrapper_earlgrey.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/sysbus.h"
#include "trace.h"


/* DEBUG: define to print the full memory view on remap */
#undef PRINT_MTREE

#define PARAM_NUM_SW_ALERTS     2u
#define PARAM_NUM_REGIONS       2u
#define PARAM_NUM_SCRATCH_WORDS 8u
#define PARAM_NUM_ALERTS        4u

/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, FATAL_SW, 0u, 1u)
    FIELD(ALERT_TEST, RECOV_SW, 1u, 1u)
    FIELD(ALERT_TEST, FATAL_HW, 2u, 1u)
    FIELD(ALERT_TEST, RECOV_HW, 3u, 1u)
REG32(SW_RECOV_ERR, 0x4u)
    FIELD(SW_RECOV_ERR, VAL, 0u, 4u)
REG32(SW_FATAL_ERR, 0x8u)
    FIELD(SW_FATAL_ERR, VAL, 0u, 4u)
REG32(IBUS_REGWEN_0, 0xcu)
    SHARED_FIELD(REGWEN_EN, 0u, 1u)
REG32(IBUS_REGWEN_1, 0x10u)
REG32(IBUS_ADDR_EN_0, 0x14u)
    SHARED_FIELD(ADDR_EN, 0u, 1u)
REG32(IBUS_ADDR_EN_1, 0x18u)
REG32(IBUS_ADDR_MATCHING_0, 0x1cu)
REG32(IBUS_ADDR_MATCHING_1, 0x20u)
REG32(IBUS_REMAP_ADDR_0, 0x24u)
REG32(IBUS_REMAP_ADDR_1, 0x28u)
REG32(DBUS_REGWEN_0, 0x2cu)
REG32(DBUS_REGWEN_1, 0x30u)
REG32(DBUS_ADDR_EN_0, 0x34u)
REG32(DBUS_ADDR_EN_1, 0x38u)
REG32(DBUS_ADDR_MATCHING_0, 0x3cu)
REG32(DBUS_ADDR_MATCHING_1, 0x40u)
REG32(DBUS_REMAP_ADDR_0, 0x44u)
REG32(DBUS_REMAP_ADDR_1, 0x48u)
REG32(NMI_ENABLE, 0x4cu)
    SHARED_FIELD(NMI_ALERT_EN_BIT, 0u, 1u)
    SHARED_FIELD(NMI_WDOG_EN_BIT, 1u, 1u)
REG32(NMI_STATE, 0x50u)
REG32(ERR_STATUS, 0x54u)
    FIELD(ERR_STATUS, REG_INTG, 0u, 1u)
    FIELD(ERR_STATUS, FATAL_INTG, 8u, 1u)
    FIELD(ERR_STATUS, FATAL_CORE, 9u, 1u)
    FIELD(ERR_STATUS, RECOV_CORE, 10u, 1u)
REG32(RND_DATA, 0x58u)
REG32(RND_STATUS, 0x5cu)
    FIELD(RND_STATUS, RND_DATA_VALID, 0u, 1u)
    FIELD(RND_STATUS, RND_DATA_FIPS, 1u, 1u)
REG32(FPGA_INFO, 0x60u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_FPGA_INFO)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(SW_RECOV_ERR),
    REG_NAME_ENTRY(SW_FATAL_ERR),
    REG_NAME_ENTRY(IBUS_REGWEN_0),
    REG_NAME_ENTRY(IBUS_REGWEN_1),
    REG_NAME_ENTRY(IBUS_ADDR_EN_0),
    REG_NAME_ENTRY(IBUS_ADDR_EN_1),
    REG_NAME_ENTRY(IBUS_ADDR_MATCHING_0),
    REG_NAME_ENTRY(IBUS_ADDR_MATCHING_1),
    REG_NAME_ENTRY(IBUS_REMAP_ADDR_0),
    REG_NAME_ENTRY(IBUS_REMAP_ADDR_1),
    REG_NAME_ENTRY(DBUS_REGWEN_0),
    REG_NAME_ENTRY(DBUS_REGWEN_1),
    REG_NAME_ENTRY(DBUS_ADDR_EN_0),
    REG_NAME_ENTRY(DBUS_ADDR_EN_1),
    REG_NAME_ENTRY(DBUS_ADDR_MATCHING_0),
    REG_NAME_ENTRY(DBUS_ADDR_MATCHING_1),
    REG_NAME_ENTRY(DBUS_REMAP_ADDR_0),
    REG_NAME_ENTRY(DBUS_REMAP_ADDR_1),
    REG_NAME_ENTRY(NMI_ENABLE),
    REG_NAME_ENTRY(NMI_STATE),
    REG_NAME_ENTRY(ERR_STATUS),
    REG_NAME_ENTRY(RND_DATA),
    REG_NAME_ENTRY(RND_STATUS),
    REG_NAME_ENTRY(FPGA_INFO),
};

#define xtrace_ot_ibex_wrapper_info(_msg_) \
    trace_ot_ibex_wrapper_info(__func__, __LINE__, _msg_)

#define OtIbexWrapperEgState OtIbexWrapperEarlGreyState

struct OtIbexWrapperEgState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    MemoryRegion remappers[PARAM_NUM_REGIONS];

    uint32_t *regs;
    bool entropy_requested;
    bool edn_connected;

    OtEDNState *edn;
    uint8_t edn_ep;
};

static void
ot_ibex_wrapper_eg_remapper_destroy(OtIbexWrapperEgState *s, unsigned slot)
{
    g_assert(slot < PARAM_NUM_REGIONS);
    MemoryRegion *mr = &s->remappers[slot];
    if (memory_region_is_mapped(mr)) {
        trace_ot_ibex_wrapper_unmap(slot);
        memory_region_transaction_begin();
        memory_region_set_enabled(mr, false);
        /* QEMU memory model enables unparenting alias regions */
        MemoryRegion *sys_mem = get_system_memory();
        memory_region_del_subregion(sys_mem, mr);
        memory_region_transaction_commit();
    }
}

static void ot_ibex_wrapper_eg_remapper_create(
    OtIbexWrapperEgState *s, unsigned slot, hwaddr dst, hwaddr src, size_t size)
{
    g_assert(slot < PARAM_NUM_REGIONS);
    MemoryRegion *mr = &s->remappers[slot];
    trace_ot_ibex_wrapper_map(slot, src, dst, size);
    g_assert(!memory_region_is_mapped(mr));

    int priority = (int)(PARAM_NUM_REGIONS - slot);

    MemoryRegion *sys_mem = get_system_memory();
    MemoryRegion *mr_dst;

    char *name =
        g_strdup_printf(TYPE_OT_IBEX_WRAPPER_EARLGREY "-remap[%u]", slot);

    memory_region_transaction_begin();
    /*
     * try to map onto the actual device if there's a single one, otherwise
     * map on the whole address space.
     */
    MemoryRegionSection mrs;
    mrs = memory_region_find(sys_mem, dst, (uint64_t)size);
    size_t mrs_lsize = int128_getlo(mrs.size);
    mr_dst = (mrs.mr && mrs_lsize >= size) ? mrs.mr : sys_mem;
    hwaddr offset = dst - mr_dst->addr;
    memory_region_init_alias(mr, OBJECT(s), name, mr_dst, offset,
                             (uint64_t)size);
    memory_region_add_subregion_overlap(sys_mem, src, mr, priority);
    memory_region_set_enabled(mr, true);
    memory_region_transaction_commit();
    g_free(name);

#ifdef PRINT_MTREE
    mtree_info(false, false, false, true);
#endif
}

static void
ot_ibex_wrapper_eg_fill_entropy(void *opaque, uint32_t bits, bool fips)
{
    OtIbexWrapperEgState *s = opaque;

    trace_ot_ibex_wrapper_fill_entropy(bits, fips);

    s->regs[R_RND_DATA] = bits;
    s->regs[R_RND_STATUS] = R_RND_STATUS_RND_DATA_VALID_MASK;
    if (fips) {
        s->regs[R_RND_STATUS] |= R_RND_STATUS_RND_DATA_FIPS_MASK;
    }

    s->entropy_requested = false;
}

static void ot_ibex_wrapper_eg_request_entropy(OtIbexWrapperEgState *s)
{
    if (!s->entropy_requested) {
        if (unlikely(!s->edn_connected)) {
            ot_edn_connect_endpoint(s->edn, s->edn_ep,
                                    &ot_ibex_wrapper_eg_fill_entropy, s);
            s->edn_connected = true;
        }
        s->entropy_requested = true;
        trace_ot_ibex_wrapper_request_entropy(s->entropy_requested);
        if (ot_edn_request_entropy(s->edn, s->edn_ep)) {
            s->entropy_requested = false;
            trace_ot_ibex_wrapper_error("failed to request entropy");
        }
    }
}

static void ot_ibex_wrapper_eg_update_remap(OtIbexWrapperEgState *s, bool doi,
                                            unsigned slot)
{
    g_assert(slot < PARAM_NUM_REGIONS);
    /*
     * Warning:
     * for now, QEMU is unable to distinguish instruction or data access.
     * in this implementation, we chose to enable remap whenever either D or I
     * remapping is selected, and both D & I configuration match; we disable
     * translation when both D & I are remapping are disabled
     */

    bool en_remap_i = s->regs[R_IBUS_ADDR_EN_0 + slot];
    bool en_remap_d = s->regs[R_DBUS_ADDR_EN_0 + slot];
    if (!en_remap_i && !en_remap_d) {
        /* disable */
        ot_ibex_wrapper_eg_remapper_destroy(s, slot);
    } else {
        uint32_t src_match_i = s->regs[R_IBUS_ADDR_MATCHING_0 + slot];
        uint32_t src_match_d = s->regs[R_DBUS_ADDR_MATCHING_0 + slot];
        if (src_match_i != src_match_d) {
            /* I and D do not match, do nothing */
            xtrace_ot_ibex_wrapper_info("src remapping do not match");
            return;
        }
        uint32_t remap_addr_i = s->regs[R_IBUS_REMAP_ADDR_0 + slot];
        uint32_t remap_addr_d = s->regs[R_DBUS_REMAP_ADDR_0 + slot];
        if (remap_addr_i != remap_addr_d) {
            /* I and D do not match, do nothing */
            xtrace_ot_ibex_wrapper_info("dst remapping do not match");
            return;
        }
        /* enable */
        uint32_t map_size = (-src_match_i & (src_match_i + 1u)) << 1u;
        uint32_t src_base = src_match_i & ~(map_size - 1u);
        uint32_t dst_base = remap_addr_i;

        ot_ibex_wrapper_eg_remapper_destroy(s, slot);
        ot_ibex_wrapper_eg_remapper_create(s, slot, (hwaddr)dst_base,
                                           (hwaddr)src_base, (size_t)map_size);
    }
}

static uint64_t
ot_ibex_wrapper_eg_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtIbexWrapperEgState *s = opaque;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_RND_DATA:
        val32 = s->regs[reg];
        if (!(s->regs[R_RND_STATUS] & R_RND_STATUS_RND_DATA_VALID_MASK)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Read invalid entropy data 0x%08x\n", __func__,
                          val32);
        }
        s->regs[reg] = 0;
        s->regs[R_RND_STATUS] = 0;
        ot_ibex_wrapper_eg_request_entropy(s);
        break;
    case R_RND_STATUS:
        val32 = s->regs[reg];
        if (!(val32 & R_RND_STATUS_RND_DATA_VALID_MASK)) {
            ot_ibex_wrapper_eg_request_entropy(s);
        }
        break;
    default:
        val32 = s->regs[reg];
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_ibex_wrapper_io_read_out((unsigned)addr, REG_NAME(reg), val32, pc);

    return (uint64_t)val32;
};

static void ot_ibex_wrapper_eg_regs_write(void *opaque, hwaddr addr,
                                          uint64_t val64, unsigned size)
{
    OtIbexWrapperEgState *s = opaque;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_ibex_wrapper_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

    switch (reg) {
    case R_SW_FATAL_ERR:
        if ((val32 >> 16u) == 0xC0DEu) {
            /* discard MSB magic */
            val32 &= UINT16_MAX;
            /* discard multibool4false mark */
            val32 >>= 4u;
            /* std exit code should be in [0..127] range */
            if (val32 > 127u) {
                val32 = 127u;
            }
            exit((int)val32);
        }
        break;
    case R_IBUS_REGWEN_0:
    case R_IBUS_REGWEN_1:
    case R_DBUS_REGWEN_0:
    case R_DBUS_REGWEN_1:
        val32 &= REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_IBUS_ADDR_EN_0:
    case R_IBUS_ADDR_EN_1:
        if (s->regs[reg - R_IBUS_ADDR_EN_0 + R_IBUS_REGWEN_0]) {
            s->regs[reg] = val32;
        }
        ot_ibex_wrapper_eg_update_remap(s, false, reg - R_IBUS_ADDR_EN_0);
        break;
    case R_IBUS_ADDR_MATCHING_0:
    case R_IBUS_ADDR_MATCHING_1:
        if (s->regs[reg - R_IBUS_ADDR_MATCHING_0 + R_IBUS_REGWEN_0]) {
            s->regs[reg] = val32;
        }
        break;
    case R_IBUS_REMAP_ADDR_0:
    case R_IBUS_REMAP_ADDR_1:
        if (s->regs[reg - R_IBUS_REMAP_ADDR_0 + R_IBUS_REGWEN_0]) {
            s->regs[reg] = val32;
        }
        ot_ibex_wrapper_eg_update_remap(s, false, reg - R_IBUS_REMAP_ADDR_0);
        break;
    case R_DBUS_ADDR_EN_0:
    case R_DBUS_ADDR_EN_1:
        if (s->regs[reg - R_DBUS_ADDR_EN_0 + R_DBUS_REGWEN_0]) {
            s->regs[reg] = val32;
        }
        ot_ibex_wrapper_eg_update_remap(s, true, reg - R_DBUS_ADDR_EN_0);
        break;
    case R_DBUS_ADDR_MATCHING_0:
    case R_DBUS_ADDR_MATCHING_1:
        if (s->regs[reg - R_DBUS_ADDR_MATCHING_0 + R_DBUS_REGWEN_0]) {
            s->regs[reg] = val32;
        }
        break;
    case R_DBUS_REMAP_ADDR_0:
    case R_DBUS_REMAP_ADDR_1:
        if (s->regs[reg - R_DBUS_REMAP_ADDR_0 + R_DBUS_REGWEN_0]) {
            s->regs[reg] = val32;
        }
        ot_ibex_wrapper_eg_update_remap(s, true, reg - R_DBUS_REMAP_ADDR_0);
        break;
    default:
        s->regs[reg] = val32;
        break;
    }
};

static Property ot_ibex_wrapper_eg_properties[] = {
    DEFINE_PROP_LINK("edn", OtIbexWrapperEgState, edn, TYPE_OT_EDN,
                     OtEDNState *),
    DEFINE_PROP_UINT8("edn-ep", OtIbexWrapperEgState, edn_ep, UINT8_MAX),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_ibex_wrapper_eg_regs_ops = {
    .read = &ot_ibex_wrapper_eg_regs_read,
    .write = &ot_ibex_wrapper_eg_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_ibex_wrapper_eg_reset(DeviceState *dev)
{
    OtIbexWrapperEgState *s = OT_IBEX_WRAPPER_EARLGREY(dev);

    g_assert(s->edn);
    g_assert(s->edn_ep != UINT8_MAX);

    for (unsigned slot = 0; slot < PARAM_NUM_REGIONS; slot++) {
        ot_ibex_wrapper_eg_remapper_destroy(s, slot);
    }

    memset(s->regs, 0, REGS_SIZE);
    s->regs[R_SW_RECOV_ERR] = 0x9u;
    s->regs[R_SW_FATAL_ERR] = 0x9u;
    s->regs[R_IBUS_REGWEN_0] = 0x1u;
    s->regs[R_IBUS_REGWEN_1] = 0x1u;
    s->regs[R_DBUS_REGWEN_0] = 0x1u;
    s->regs[R_DBUS_REGWEN_1] = 0x1u;
    s->regs[R_FPGA_INFO] = 0x554d4551u; /* 'QEMU' in LE */
    s->entropy_requested = false;
}

static void ot_ibex_wrapper_eg_init(Object *obj)
{
    OtIbexWrapperEgState *s = OT_IBEX_WRAPPER_EARLGREY(obj);

    memory_region_init_io(&s->mmio, obj, &ot_ibex_wrapper_eg_regs_ops, s,
                          TYPE_OT_IBEX_WRAPPER_EARLGREY, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(uint32_t, REGS_COUNT);
}

static void ot_ibex_wrapper_eg_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ot_ibex_wrapper_eg_reset;
    device_class_set_props(dc, ot_ibex_wrapper_eg_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_ibex_wrapper_eg_info = {
    .name = TYPE_OT_IBEX_WRAPPER_EARLGREY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtIbexWrapperEgState),
    .instance_init = &ot_ibex_wrapper_eg_init,
    .class_init = &ot_ibex_wrapper_eg_class_init,
    .class_size = sizeof(OtIbexWrapperStateClass),
};

static void ot_ibex_wrapper_eg_register_types(void)
{
    type_register_static(&ot_ibex_wrapper_eg_info);
}

type_init(ot_ibex_wrapper_eg_register_types)
