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
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qom/object_interfaces.h"
#include "hw/opentitan/ot_rom_ctrl_img.h"

/* Current ROMs digests are 256 bits (32 bytes) */
#define ROM_DIGEST_BYTES 32u

static const uint8_t ELF_HEADER[] = {
    0x7fu, 0x45u, 0x4cu, 0x46u, 0x01u, 0x01u, 0x01u, 0x00u,
};

static OtRomImgFormat ot_rom_img_guess_image_format(const char *filename)
{
    /* NOLINTNEXTLINE(misc-redundant-expression) */
    int fd = open(filename, O_RDONLY | O_BINARY | O_CLOEXEC);
    if (fd == -1) {
        return OT_ROM_IMG_FORMAT_NONE;
    }

    uint8_t data[128u];
    ssize_t len = read(fd, data, sizeof(data));
    data[sizeof(data) - 1u] = '\0';
    close(fd);

    if (len < sizeof(data)) {
        return OT_ROM_IMG_FORMAT_NONE;
    }

    if (!memcmp(data, ELF_HEADER, sizeof(ELF_HEADER))) {
        return OT_ROM_IMG_FORMAT_ELF;
    }

    /*
     * Discard comments; only cope with single-line comments:
     * we do not need to handle more, since OT-generated files do not contain
     * multi-line comments in VMEM files; keep it as simple as possible.
     */
    if (data[0u] == '/' && (data[1u] == '*' || data[1u] == '/')) {
        for (unsigned ix = 2u; ix < sizeof(data) - 1u; ix++) {
            if (data[ix] == '\n') {
                unsigned rem = sizeof(data) - 1u - ix;
                memmove(&data[0], &data[ix + 1u], rem);
                len = read(fd, &data[rem], sizeof(data) - rem);
                (void)len; /* GCC unused result warning */
                data[sizeof(data) - 1u] = '\0';
                break;
            }
        }
    }

    if (data[0u] == '@') { /* likely a VMEM file */
        bool addr = true;
        unsigned dlen = 0u;
        for (unsigned ix = 1u; ix < sizeof(data); ix++) {
            if (data[ix] == ' ') { /* separator */
                if (addr) {
                    addr = false;
                    continue;
                }
                break;
            }
            if (addr) {
                if (data[ix] != '0') {
                    /* first address is always expected to be 0 */
                    break;
                }
                continue;
            }
            dlen += 1u;
        }
        if (dlen == 8u) { /* 32 bits */
            return OT_ROM_IMG_FORMAT_VMEM_PLAIN;
        }
        if (dlen == 10u) { /* 40 bits */
            return OT_ROM_IMG_FORMAT_VMEM_SCRAMBLED_ECC;
        }
    }

    bool hexa_only = true;
    unsigned cr = 0u;
    unsigned ix;
    for (ix = 0u; ix < sizeof(data); ix++) {
        if (data[ix] == '\r') {
            cr = ix;
            continue;
        }
        if (data[ix] == '\n') {
            if (cr) {
                if (cr != ix - 1) {
                    /* the only valid pos for useless CR is right before LF */
                    hexa_only = false;
                } else {
                    /* ignore CR in line length */
                    ix -= 1u;
                }
            }
            break;
        }
        if (!g_ascii_isxdigit(data[ix])) {
            hexa_only = false;
            break;
        }
    }
    if (hexa_only && ix == 10u) {
        return OT_ROM_IMG_FORMAT_HEX_SCRAMBLED_ECC;
    }

    return OT_ROM_IMG_FORMAT_BINARY;
}

static void ot_rom_img_reset(OtRomImg *ri)
{
    ri->filename = NULL;
    ri->format = OT_ROM_IMG_FORMAT_NONE;
}

static void ot_rom_img_prop_set_file(Object *obj, const char *value,
                                     Error **errp)
{
    OtRomImg *ri = OT_ROM_IMG(obj);
    (void)errp;

    g_free(ri->filename);
    ri->filename = NULL;

    struct stat rom_stat;
    int res = stat(value, &rom_stat);
    if (res) {
        error_setg(errp, "ROM image '%s' not found", value);
        return;
    }
    if ((rom_stat.st_mode & S_IFMT) != S_IFREG) {
        error_setg(errp, "ROM image '%s' is not a file", value);
        return;
    }
    ri->raw_size = (unsigned)rom_stat.st_size;
    ri->format = ot_rom_img_guess_image_format(value);
    ri->filename = g_strdup(value);
}

static char *ot_rom_img_prop_get_file(Object *obj, Error **errp)
{
    OtRomImg *ri = OT_ROM_IMG(obj);
    (void)errp;

    return g_strdup(ri->filename);
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

static void ot_rom_img_class_init(ObjectClass *oc, const void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);
    (void)data;

    ucc->complete = &ot_rom_img_complete;

    object_class_property_add_str(oc, "file", &ot_rom_img_prop_get_file,
                                  &ot_rom_img_prop_set_file);
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
