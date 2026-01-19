/*
 * QEMU Ibex GPIOs
 *
 * Copyright (c) 2024 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
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

#ifndef HW_RISCV_IBEX_GPIO_H
#define HW_RISCV_IBEX_GPIO_H

#include "hw/registerfields.h"
#include "hw/riscv/ibex_irq.h"

/* GPIO multi-level storage */
typedef int ibex_gpio;

/* set if GPIO is not Hi-Z */
SHARED_FIELD(IBEX_GPIO_ACTIVE, 0u, 1u)
/* GPIO signal level */
SHARED_FIELD(IBEX_GPIO_LEVEL, 1u, 1u)
/* GPIO is applying a strong signed (vs. weak pull-up or pull-down) */
SHARED_FIELD(IBEX_GPIO_STRENGTH, 2u, 1u)
/* always 0 (reserved for strength extension) */
SHARED_FIELD(IBEX_GPIO_SBZ, 3u, 5u)
/* Ibex GPIO 'G' marker (sanity check) */
SHARED_FIELD(IBEX_GPIO_FLAG, 8u, 8u)

/* default initialization: Hi-Z */
#define IBEX_GPIO_INIT ('G' << IBEX_GPIO_FLAG_SHIFT)
#define IBEX_GPIO_HIZ  IBEX_GPIO_INIT
#define IBEX_GPIO_FROM_ACTIVE_SIG(_lvl_) \
    (IBEX_GPIO_INIT | IBEX_GPIO_ACTIVE_MASK | IBEX_GPIO_STRENGTH_MASK | \
     (((int)(bool)(_lvl_)) << IBEX_GPIO_LEVEL_SHIFT))
#define IBEX_GPIO_FROM_WEAK_SIG(_lvl_) \
    (IBEX_GPIO_INIT | IBEX_GPIO_ACTIVE_MASK | \
     (((int)(bool)(_lvl_)) << IBEX_GPIO_LEVEL_SHIFT))
#define IBEX_GPIO_LOW       IBEX_GPIO_FROM_ACTIVE_SIG(false)
#define IBEX_GPIO_HIGH      IBEX_GPIO_FROM_ACTIVE_SIG(true)
#define IBEX_GPIO_PULL_DOWN IBEX_GPIO_FROM_WEAK_SIG(false)
#define IBEX_GPIO_PULL_UP   IBEX_GPIO_FROM_WEAK_SIG(true)


static inline bool ibex_gpio_level(ibex_gpio level)
{
    return (bool)(level & IBEX_GPIO_LEVEL_MASK);
}

static inline bool ibex_gpio_is_hiz(ibex_gpio level)
{
    return !(level & IBEX_GPIO_ACTIVE_MASK);
}

static inline bool ibex_gpio_is_weak(ibex_gpio level)
{
    return !(level & IBEX_GPIO_STRENGTH_MASK);
}

static inline bool ibex_gpio_check(ibex_gpio level)
{
    return (level & (IBEX_GPIO_FLAG_MASK | IBEX_GPIO_SBZ_MASK)) ==
           ('G' << IBEX_GPIO_FLAG_SHIFT);
}

/*
 * Debug representation of an Ibex GPIO signal.
 *
 * 'X': invalid signal (not a valid Ibex IO)
 * 'z': Hi-Z
 * 'H': active high
 * 'L': active low
 * 'h': weak high / pull up
 * 'l': weak low / pull down
 */
static inline char ibex_gpio_repr(ibex_gpio level)
{
    return ibex_gpio_check(level) ?
               (!ibex_gpio_is_hiz(level) ?
                    (ibex_gpio_level(level) ? 'H' : 'L') +
                        (ibex_gpio_is_weak(level) ? 0x20 : 0) :
                    'z') :
               'X';
}

static inline void ibex_gpio_assert(ibex_gpio level)
{
    g_assert(ibex_gpio_check(level));
}

ibex_gpio ibex_gpio_combine(const ibex_gpio *levels, unsigned count);

bool ibex_gpio_parse_level(const char *name, const char *value, ibex_gpio *obj,
                           Error **errp);

ObjectProperty *
object_property_add_ibex_gpio(Object *obj, const char *name, ibex_gpio *value);

#endif /* HW_RISCV_IBEX_GPIO_H */
