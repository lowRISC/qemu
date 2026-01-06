/*
 * QEMU OpenTitan Prince implementation
 *
 * Copyright (c) lowRISC contributors.
 * Licensed under the Apache License, Version 2.0, see LICENSE for details.
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#ifndef HW_OPENTITAN_OT_PRINCE_H
#define HW_OPENTITAN_OT_PRINCE_H

uint64_t ot_prince_run(uint64_t data, uint64_t khi, uint64_t klo,
                       unsigned num_rounds_half);

#endif /* HW_OPENTITAN_OT_PRINCE_H */
