/*
 * QEMU OpenTitan System Reset Controller (sysrst_ctrl) device
 *
 * Copyright lowRISC contributors.
 *
 * Licensed under the Apache License, Version 2.0, see LICENSE for details.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HW_OPENTITAN_OT_SYSRST_CTRL_H
#define HW_OPENTITAN_OT_SYSRST_CTRL_H

#include "qom/object.h"
#include "hw/sysbus.h"

#define TYPE_OT_SYSRST_CTRL "ot-sysrst_ctrl"
OBJECT_DECLARE_SIMPLE_TYPE(OtSysrstCtrlState, OT_SYSRST_CTRL)

#define OT_SYSRST_CTRL_RST_REQ  TYPE_OT_SYSRST_CTRL "-rst-req"
#define OT_SYSRST_CTRL_WKUP_REQ TYPE_OT_SYSRST_CTRL "-wkup-req"
#define OT_SYSRST_CTRL_INPUT    TYPE_OT_SYSRST_CTRL "-input"

enum OtSysrstCtrlInput {
    OT_SYSRST_CTRL_IN_AC_PRESENT = 0,
    OT_SYSRST_CTRL_IN_KEY0 = 1,
    OT_SYSRST_CTRL_IN_KEY1 = 2,
    OT_SYSRST_CTRL_IN_KEY2 = 3,
    OT_SYSRST_CTRL_IN_PWRB = 4,
    OT_SYSRST_CTRL_IN_LID_OPEN = 5,
    OT_SYSRST_CTRL_IN_EC_RST_L = 6,
    OT_SYSRST_CTRL_IN_FLASH_WP_L = 7,
    OT_SYSRST_CTRL_IN_COUNT = 8,
};

int ot_sysrst_ctrl_get_outsel_level(const OtSysrstCtrlState *s,
                                    uint32_t outsel);

#endif /* HW_OPENTITAN_OT_SYSRST_CTRL_H */
