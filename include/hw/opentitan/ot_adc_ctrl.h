/*
 * QEMU OpenTitan ADC Controller device
 *
 * Copyright lowRISC contributors.
 *
 * Licensed under the Apache License, Version 2.0, see LICENSE for details.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HW_OPENTITAN_OT_ADC_CTRL_H
#define HW_OPENTITAN_OT_ADC_CTRL_H

#include "qom/object.h"

#define TYPE_OT_ADC_CTRL "ot-adc-ctrl"
OBJECT_DECLARE_SIMPLE_TYPE(OtAdcCtrlState, OT_ADC_CTRL)

#define OT_ADC_CTRL_WKUP TYPE_OT_ADC_CTRL "-wkup"

#endif /* HW_OPENTITAN_OT_ADC_CTRL_H */
