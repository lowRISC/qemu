/*
 * QEMU OpenTitan Debug Module to TileLink bridge
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
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "exec/memory.h"
#include "hw/opentitan/ot_address_space.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_dm_tl.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/debug.h"
#include "hw/riscv/dtm.h"
#include "hw/sysbus.h"
#include "trace.h"

/** Debug Module */
struct OtDMTLState {
    RISCVDebugDeviceState parent;
    AddressSpace *as; /* Hart address space */
    hwaddr tl_offset;
    uint32_t value;
    MemTxAttrs attrs;
    char *dev_name;
    bool dtm_ok; /* DTM is available */

    RISCVDTMState *dtm;
    SysBusDevice *tl_dev;
    char *tl_as_name;
    uint64_t tl_base;
    uint32_t dmi_addr;
    unsigned dmi_size;
    bool enable;
    uint8_t role;
};

/* -------------------------------------------------------------------------- */
/* DTM interface implementation */
/* -------------------------------------------------------------------------- */

static RISCVDebugResult
ot_dm_tl_write_rq(RISCVDebugDeviceState *dev, uint32_t addr, uint32_t value)
{
    OtDMTLState *dmtl = OT_DM_TL(dev);

    if (!dmtl->dtm_ok) {
        trace_ot_dm_tl_dtm_not_available(dmtl->dev_name);
        return RISCV_DEBUG_FAILED;
    }

    if (addr >= dmtl->dmi_size) {
        trace_ot_dm_tl_invalid_addr(dmtl->dev_name, addr);
        return RISCV_DEBUG_FAILED;
    }

    /* store address for next read back */
    dmtl->tl_offset = addr * sizeof(uint32_t);

    MemTxResult res;

    res = address_space_rw(dmtl->as, dmtl->tl_base + dmtl->tl_offset,
                           dmtl->attrs, &value, sizeof(value), true);

    trace_ot_dm_tl_update(dmtl->dev_name, addr, value, "write", res);

    return (res == MEMTX_OK) ? RISCV_DEBUG_NOERR : RISCV_DEBUG_FAILED;
}

static RISCVDebugResult
ot_dm_tl_read_rq(RISCVDebugDeviceState *dev, uint32_t addr)
{
    OtDMTLState *dmtl = OT_DM_TL(dev);

    if (!dmtl->dtm_ok) {
        trace_ot_dm_tl_dtm_not_available(dmtl->dev_name);
        return RISCV_DEBUG_FAILED;
    }

    if (addr >= dmtl->dmi_size) {
        trace_ot_dm_tl_invalid_addr(dmtl->dev_name, addr);
        return RISCV_DEBUG_FAILED;
    }

    /* store address for next read back */
    dmtl->tl_offset = addr * sizeof(uint32_t);

    MemTxResult res;

    res =
        address_space_rw(dmtl->as, dmtl->tl_base + dmtl->tl_offset, dmtl->attrs,
                         &dmtl->value, sizeof(dmtl->value), false);

    trace_ot_dm_tl_update(dmtl->dev_name, addr, 0, "read", res);

    return (res == MEMTX_OK) ? RISCV_DEBUG_NOERR : RISCV_DEBUG_FAILED;
}

static uint32_t ot_dm_tl_read_value(RISCVDebugDeviceState *dev)
{
    OtDMTLState *dmtl = OT_DM_TL(dev);

    uint32_t value = dmtl->value;

    trace_ot_dm_tl_capture(dmtl->dev_name, dmtl->tl_offset, value);

    return value;
}

static Property ot_dm_tl_properties[] = {
    DEFINE_PROP_LINK("dtm", OtDMTLState, dtm, TYPE_RISCV_DTM, RISCVDTMState *),
    DEFINE_PROP_UINT32("dmi_addr", OtDMTLState, dmi_addr, 0),
    DEFINE_PROP_UINT32("dmi_size", OtDMTLState, dmi_size, 0),
    DEFINE_PROP_STRING("tl_as_name", OtDMTLState, tl_as_name),
    DEFINE_PROP_UINT64("tl_addr", OtDMTLState, tl_base, 0),
    DEFINE_PROP_LINK("tl_dev", OtDMTLState, tl_dev, TYPE_SYS_BUS_DEVICE,
                     SysBusDevice *),
    DEFINE_PROP_BOOL("enable", OtDMTLState, enable, true),
    DEFINE_PROP_UINT8("role", OtDMTLState, role, UINT8_MAX),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_dm_tl_reset(DeviceState *dev)
{
    OtDMTLState *dmtl = OT_DM_TL(dev);

    g_assert(dmtl->dtm != NULL);
    g_assert(dmtl->dmi_size);

    if (!dmtl->dtm_ok) {
        RISCVDTMClass *dtmc = RISCV_DTM_GET_CLASS(OBJECT(dmtl->dtm));
        dmtl->dtm_ok =
            (*dtmc->register_dm)(DEVICE(dmtl->dtm), RISCV_DEBUG_DEVICE(dev),
                                 dmtl->dmi_addr, dmtl->dmi_size, dmtl->enable);
    }

    if (dmtl->dtm_ok) {
        Object *soc = OBJECT(dev)->parent;
        Object *obj;
        OtAddressSpaceState *oas;

        g_assert(dmtl->tl_as_name);
        obj = object_property_get_link(soc, dmtl->tl_as_name, &error_fatal);
        oas = OBJECT_CHECK(OtAddressSpaceState, obj, TYPE_OT_ADDRESS_SPACE);
        dmtl->as = ot_address_space_get(oas);
        g_assert(dmtl->as);
    }
}

static void ot_dm_tl_realize(DeviceState *dev, Error **errp)
{
    OtDMTLState *dmtl = OT_DM_TL(dev);
    (void)errp;

    /* NOLINTNEXTLINE */
    if (dmtl->role == UINT8_MAX) {
        dmtl->attrs = MEMTXATTRS_UNSPECIFIED;
    } else {
        dmtl->attrs = MEMTXATTRS_WITH_ROLE(dmtl->role);
    }

    if (dmtl->tl_dev) {
        dmtl->dev_name = g_strdup(object_get_typename(OBJECT(dmtl->tl_dev)));
    } else {
        dmtl->dev_name = g_strdup("");
    }
}

static void ot_dm_tl_init(Object *obj)
{
    OtDMTLState *s = OT_DM_TL(obj);
    (void)s;
}

static void ot_dm_tl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_dm_tl_reset;
    dc->realize = &ot_dm_tl_realize;
    device_class_set_props(dc, ot_dm_tl_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    RISCVDebugDeviceClass *dmc = RISCV_DEBUG_DEVICE_CLASS(klass);
    dmc->write_rq = &ot_dm_tl_write_rq;
    dmc->read_rq = &ot_dm_tl_read_rq;
    dmc->read_value = &ot_dm_tl_read_value;
}

static const TypeInfo ot_dm_tl_info = {
    .name = TYPE_OT_DM_TL,
    .parent = TYPE_RISCV_DEBUG_DEVICE,
    .instance_init = &ot_dm_tl_init,
    .instance_size = sizeof(OtDMTLState),
    .class_init = &ot_dm_tl_class_init,
};

static void ot_dm_tl_register_types(void)
{
    type_register_static(&ot_dm_tl_info);
}

type_init(ot_dm_tl_register_types);
