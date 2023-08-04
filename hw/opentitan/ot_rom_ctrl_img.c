/*
 * QEMU OpenTitan ROM image
 *
 * Copyright (c) 2023 Rivos, Inc.
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
 *
 * ROM Images can be instanciated from the command line:
 *   "-object ot-rom-img,id=rom1,file=rom1.raw,digest=0123456789abcdef"
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qom/object_interfaces.h"
#include "hw/opentitan/ot_rom_ctrl_img.h"

/* Current ROMs digests are 256 bits (32 bytes) */
#define ROM_DIGEST_BYTES (32u)

static const char HEX[] = "0123456789abcdef";

static void ot_rom_img_reset(OtRomImg *ri)
{
    ri->filename = NULL;
    ri->digest = NULL;
    ri->digest_len = 0;
    ri->fake_digest = false;
}

static void ot_rom_img_prop_set_file(Object *obj, const char *value,
                                     Error **errp)
{
    OtRomImg *ri = OT_ROM_IMG(obj);

    g_free(ri->filename);
    ri->filename = g_strdup(value);
}

static char *ot_rom_img_prop_get_file(Object *obj, Error **errp)
{
    OtRomImg *ri = OT_ROM_IMG(obj);

    return g_strdup(ri->filename);
}

static void ot_rom_img_prop_set_digest(Object *obj, const char *value,
                                       Error **errp)
{
    OtRomImg *ri = OT_ROM_IMG(obj);
    unsigned len = strlen(value);

    if (!g_strcmp0(value, "fake")) {
        ri->digest = NULL;
        ri->digest_len = 0;
        ri->fake_digest = true;
        return;
    }

    if (len != (2 * ROM_DIGEST_BYTES)) {
        error_setg(errp, "Invalid digest '%s': must be %u bytes long", value,
                   ROM_DIGEST_BYTES);
        return;
    }

    g_free(ri->digest);
    ri->digest_len = ROM_DIGEST_BYTES;
    ri->digest = g_new0(uint8_t, ri->digest_len);

    for (unsigned idx = 0; idx < len; idx++) {
        if (!g_ascii_isxdigit(value[idx])) {
            error_setg(errp,
                       "Invalid digest '%s': must only contain hex digits",
                       value);
            g_free(ri->digest);
            ri->digest_len = 0;
            ri->digest = NULL;
            return;
        }
        uint8_t digit = g_ascii_xdigit_value(value[idx]);
        digit = (idx & 1) ? (digit & 0xf) : (digit << 4);
        ri->digest[ri->digest_len - 1 - (idx / 2)] |= digit;
    }
}

static char *ot_rom_img_prop_get_digest(Object *obj, Error **errp)
{
    OtRomImg *ri = OT_ROM_IMG(obj);
    char *digest;

    if (ri->fake_digest) {
        return g_strdup("fake");
    }

    digest = g_new0(char, (ri->digest_len * 2) + 1);
    for (unsigned idx = 0; idx < ri->digest_len; idx++) {
        uint8_t val = ri->digest[ri->digest_len - 1 - idx];
        digest[(idx * 2)] = HEX[(val >> 4) & 0xf];
        digest[(idx * 2) + 1] = HEX[val & 0xf];
    }
    digest[ri->digest_len * 2] = '\0';

    return digest;
}

static void ot_rom_img_instance_init(Object *obj)
{
    OtRomImg *ri = OT_ROM_IMG(obj);
    ot_rom_img_reset(ri);
}

static void ot_rom_img_finalize(Object *obj)
{
    OtRomImg *ri = OT_ROM_IMG(obj);

    g_free(ri->filename);
    g_free(ri->digest);

    ot_rom_img_reset(ri);
}

static void ot_rom_img_complete(UserCreatable *uc, Error **errp)
{
    OtRomImg *ri = OT_ROM_IMG(uc);

    if (!ri->filename) {
        error_setg(errp, "Invalid ROM filename: cannot read file");
        return;
    }

    if (!g_file_test(ri->filename, G_FILE_TEST_EXISTS)) {
        error_setg(errp, "ROM file %s does not exist", ri->filename);
        return;
    }
}

static void ot_rom_img_class_init(ObjectClass *oc, void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);

    ucc->complete = &ot_rom_img_complete;

    object_class_property_add_str(oc, "file", ot_rom_img_prop_get_file,
                                  ot_rom_img_prop_set_file);
    object_class_property_add_str(oc, "digest", ot_rom_img_prop_get_digest,
                                  ot_rom_img_prop_set_digest);
}

static const TypeInfo ot_rom_img_info = {
    .parent = TYPE_OBJECT,
    .name = TYPE_OT_ROM_IMG,
    .instance_size = sizeof(OtRomImg),
    .instance_init = &ot_rom_img_instance_init,
    .instance_finalize = &ot_rom_img_finalize,
    .class_init = &ot_rom_img_class_init,
    .interfaces = (InterfaceInfo[]){ { TYPE_USER_CREATABLE }, {} }
};

static void ot_rom_img_register_types(void)
{
    type_register_static(&ot_rom_img_info);
}

type_init(ot_rom_img_register_types);
