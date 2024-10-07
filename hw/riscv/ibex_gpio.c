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

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qmp/qerror.h"
#include "qapi/visitor.h"
#include "hw/riscv/ibex_gpio.h"
#include "trace.h"

ibex_gpio ibex_gpio_combine(const ibex_gpio *levels, unsigned count)
{
    bool oweak = true;
    bool odefined = false;
    bool olevel = false;

    while (count--) {
        ibex_gpio ilevel = *levels++;
        ibex_gpio_assert(ilevel);
        if (ibex_gpio_is_hiz(ilevel)) {
            continue;
        }

        bool weak = ibex_gpio_is_weak(ilevel);
        bool level = ibex_gpio_level(ilevel);

        if (!odefined) {
            olevel = level;
            oweak = weak;
            odefined = true;
            continue;
        }

        if (oweak) {
            if (!weak) {
                /* strong signal always overrides weak */
                olevel = level;
                oweak = weak;
            } else if (level != olevel) {
                qemu_log("%s: level conflict between weak signals\n", __func__);
            }
        } else if (!weak && (level != olevel)) {
            qemu_log("%s: level conflict between strong signals\n", __func__);
        }
    }

    if (!odefined) {
        return IBEX_GPIO_INIT;
    }

    return oweak ? IBEX_GPIO_FROM_WEAK_SIG(olevel) :
                   IBEX_GPIO_FROM_ACTIVE_SIG(olevel);
}

bool ibex_gpio_parse_level(const char *name, const char *value, ibex_gpio *obj,
                           Error **errp)
{
    if (g_str_equal(value, "on") || g_str_equal(value, "hi") ||
        g_str_equal(value, "1") || g_str_equal(value, "high")) {
        *obj = IBEX_GPIO_HIGH;
        return true;
    }

    if (g_str_equal(value, "off") || g_str_equal(value, "lo") ||
        g_str_equal(value, "0") || g_str_equal(value, "low")) {
        *obj = IBEX_GPIO_LOW;
        return true;
    }

    if (g_str_equal(value, "pu") || g_str_equal(value, "pullup")) {
        *obj = IBEX_GPIO_PULL_UP;
        return true;
    }

    if (g_str_equal(value, "pd") || g_str_equal(value, "pulldown")) {
        *obj = IBEX_GPIO_PULL_DOWN;
        return true;
    }

    if (g_str_equal(value, "hiz") || g_str_equal(value, "z")) {
        *obj = IBEX_GPIO_HIZ;
        return true;
    }

    error_setg(errp, QERR_INVALID_PARAMETER_VALUE, name,
               "'high' or 'low' or 'pu' or 'pd' or 'hiz'");
    return false;
}

typedef struct {
    ibex_gpio *value;
} GpioLevelProperty;

static void ibex_gpio_property_get(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    (void)obj;
    GpioLevelProperty *prop = opaque;
    char *value;

    g_assert(prop->value);
    ibex_gpio val = *prop->value;
    if (!ibex_gpio_check(val)) {
        error_setg(errp, "Invalid IbexGPIO");
        return;
    }
    if (ibex_gpio_is_hiz(val)) {
        value = g_strdup("hiz");
    } else if (ibex_gpio_is_weak(val)) {
        value = g_strdup(ibex_gpio_level(val) ? "pu" : "pd");
    } else {
        value = g_strdup(ibex_gpio_level(val) ? "high" : "low");
    }

    visit_type_str(v, name, &value, errp);
    g_free(value);
}

static void ibex_gpio_property_set(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    (void)obj;
    GpioLevelProperty *prop = opaque;
    char *value;

    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }

    ibex_gpio_parse_level(name, value, prop->value, errp);
    g_free(value);
}

static void
ibex_gpio_property_release_data(Object *obj, const char *name, void *opaque)
{
    (void)obj;
    (void)name;
    g_free(opaque);
}

ObjectProperty *
object_property_add_ibex_gpio(Object *obj, const char *name, ibex_gpio *value)
{
    GpioLevelProperty *prop = g_new0(GpioLevelProperty, 1u);
    prop->value = value;

    return object_property_add(obj, name, "string", &ibex_gpio_property_get,
                               &ibex_gpio_property_set,
                               &ibex_gpio_property_release_data, prop);
}
