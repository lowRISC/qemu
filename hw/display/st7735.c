/*
 * QEMU Sitronix ST7735 controller device
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
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
 * Note: only a small subset of the ST7735 are supported.
 *       - inversion/rotation/... are ignored
 *       - gamma settings are ignored
 *       - read back functions are not supported
 *       - ...
 *       Moreover, only host consoles with 24bpp/32bpp are supported.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/display/st7735.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"
#include "trace.h"
#include "ui/console.h"

/* clang-format off */
REG8(NOP, 0x00u)
REG8(SWRESET, 0x01u)
REG8(RDDID, 0x04u)
REG8(RDDST, 0x09u)
REG8(RDDPM, 0x0au)
REG8(RDDMADCTL, 0x0bu)
REG8(RDDCOLMOD, 0x0cu)
REG8(RDDIM, 0x0du)
REG8(RDDSM, 0x0eu)
REG8(SLPIN, 0x10u)
REG8(SLPOUT, 0x11u)
REG8(PTLON, 0x12u)
REG8(NORON, 0x13u)
REG8(INVOFF, 0x20u)
REG8(INVON, 0x21u)
REG8(GAMSET, 0x26u)
REG8(DISPOFF, 0x28u)
REG8(DISPON, 0x29u)
REG8(CASET, 0x2au)
REG8(RASET, 0x2bu)
REG8(RAMWR, 0x2cu)
REG8(RAMRD, 0x2eu)
REG8(PTLAR, 0x30u)
REG8(TEOFF, 0x34u)
REG8(TEON, 0x35u)
REG8(MADCTL, 0x36u)
    FIELD(MADCTL, MH, 2u, 1u)
    FIELD(MADCTL, RGB, 2u, 1u)
    FIELD(MADCTL, ML, 4u, 1u)
    FIELD(MADCTL, MV, 5u, 1u)
    FIELD(MADCTL, MX, 6u, 1u)
    FIELD(MADCTL, MY, 7u, 1u)
REG8(IDMOFF, 0x38u)
REG8(IDMON, 0x39u)
REG8(COLMOD, 0x3au)
REG8(FRMCTR1, 0xb1u)
REG8(FRMCTR2, 0xb2u)
REG8(FRMCTR3, 0xb3u)
REG8(INVCTR , 0xb4u)
    FIELD(INVCTR, NLC, 0u, 1u)
    FIELD(INVCTR, NLB, 1u, 1u)
    FIELD(INVCTR, NLA, 2u, 1u)
REG8(DISSET5, 0xb6u)
REG8(PWCTR1, 0xc0u)
REG8(PWCTR2, 0xc1u)
REG8(PWCTR3, 0xc2u)
REG8(PWCTR4, 0xc3u)
REG8(PWCTR5, 0xc4u)
REG8(VMCTR1, 0xc5u)
REG8(VMOFCTR, 0xc7u)
REG8(WRID2, 0xd1u)
REG8(NVFCTR1, 0xd9u)
REG8(NVFCTR2, 0xdeu)
REG8(NVFCTR3, 0xdfu)
REG8(RDID1, 0xdau)
REG8(RDID2, 0xdbu)
REG8(RDID3, 0xdcu)
REG8(GMCTRP1, 0xe0u)
REG8(GMCTRN1, 0xe1u)
REG8(EXTCTRL, 0xf0u)
REG8(VCOM4L, 0xffu)
REG8(PWCTR6, 0xfcu)
/* clang-format on */

#define ST7735_DEFAULT_WIDTH  162u
#define ST7735_DEFAULT_HEIGHT 132u

#define ST7735_BUFFER_LEN 16u /* max SSI payload (except. gfx data) */

#define ST7735_FB_PIXEL       uint32_t /* 32 bpp (24 bpp real)*/
#define ST7735_FB_BPP         sizeof(ST7735_FB_PIXEL)
#define ST7735_FB_PIXELS(_s_) ((_s_)->width * (_s_)->height)

enum St7735SsiState {
    STATE_IDLE,
    STATE_COLLECTING_DATA,
    STATE_WRITING_MEMORY,
};

typedef enum St7735PixelMode {
    PX_INV = 0u, /* invalid mode */
    PX_444 = 0x3u, /* 12-bit pixel */
    PX_565 = 0x5u, /* 16-bit pixel */
    PX_666 = 0x6u, /* 18-bit pixel */
} St7735PixelMode;

struct St7735State {
    SSIPeripheral ssidev;
    QemuConsole *con;

    uint8_t state; /* SM state */
    uint8_t command; /* command in progress */
    uint32_t length; /* payload length for the current command */
    uint32_t pos; /* count of received payload bytes */
    uint8_t buffer[ST7735_BUFFER_LEN];
    bool dc; /* false: handling command, true: handling data/payload */
    bool nreset; /* reset is active low */

    bool redraw;
    ST7735_FB_PIXEL *fb; /* framebuffer (may be NULL) */
    uint8_t col; /* current column in FB */
    uint8_t row; /* current row in FB */

    uint16_t width;
    uint16_t height;

    struct {
        St7735PixelMode pixmode;
        uint8_t madctl;
        uint8_t invctr;
        uint16_t xs;
        uint16_t xe;
        uint16_t ys;
        uint16_t ye;
    } disp;
};

static void st7735_sw_reset(St7735State *s)
{
    trace_st7735_reset("");

    s->command = R_NOP;
    s->pos = 0;
    s->length = 0;
    s->col = 0;
    s->row = 0;
}

static void st7735_reset(DeviceState *dev)
{
    St7735State *s = ST7735(dev);

    trace_st7735_reset("hw");

    memset(&s->disp, 0, sizeof(s->disp));

    st7735_sw_reset(s);

    /* note: FB is not cleared even on HW reset as per the datasheet. */
}

static void st7735_sleep(St7735State *s, bool on)
{
    trace_st7735_set("sleep", on);
}

static void st7735_partial_mode(St7735State *s, bool on)
{
    trace_st7735_set("partial mode", on);
}

static void st7735_invert(St7735State *s, bool on)
{
    trace_st7735_set("invert", on);
}

static void st7735_idle(St7735State *s, bool on)
{
    trace_st7735_set("idle", on);
}

static void st7735_display(St7735State *s, bool on)
{
    trace_st7735_set("display", on);
}

static void st7735_gpio_event(void *opaque, int irq, int level)
{
    St7735State *s = opaque;
    bool value = (bool)level;


    switch (irq) {
    case ST7735_IO_RESET:
        trace_st7735_gpio("reset", value);
        if (!value && s->nreset) {
            st7735_reset(DEVICE(s));
        }
        s->nreset = value;
        break;
    case ST7735_IO_D_C:
        trace_st7735_gpio("d/c", value);
        s->dc = value;
        break;
    default:
        break;
    }
}

static void st7735_decode_command(St7735State *s, uint8_t cmd)
{
    s->pos = 0;

    s->command = cmd;

    switch (cmd) {
    case R_NOP:
        break;
    case R_SWRESET:
        st7735_sw_reset(s);
        break;
    case R_SLPIN:
    case R_SLPOUT:
        st7735_sleep(s, !(cmd - R_SLPIN));
        break;
    case R_PTLON:
    case R_NORON:
        st7735_partial_mode(s, !(cmd - R_PTLON));
        break;
    case R_INVOFF:
    case R_INVON:
        st7735_invert(s, (bool)(cmd - R_INVOFF));
        break;
    case R_DISPOFF:
    case R_DISPON:
        st7735_display(s, (bool)(cmd - R_DISPOFF));
        break;
    case R_CASET:
    case R_RASET:
        s->length = 4u;
        break;
    case R_RAMWR:
        s->state = STATE_WRITING_MEMORY;
        s->col = s->disp.xs;
        s->row = s->disp.ys;
        break;
    case R_MADCTL:
        s->length = 1u;
        break;
    case R_IDMOFF:
    case R_IDMON:
        st7735_idle(s, !(cmd - R_IDMOFF));
        break;
    case R_COLMOD:
        s->length = 1u;
        break;
    case R_FRMCTR1:
    case R_FRMCTR2:
        s->length = 3u;
        break;
    case R_FRMCTR3:
        s->length = 6u;
        break;
    case R_DISSET5:
        s->length = 2u;
        break;
    case R_INVCTR:
        s->length = 1u;
        break;
    case R_PWCTR1:
    case R_PWCTR3:
    case R_PWCTR4:
    case R_PWCTR5:
    case R_PWCTR6:
        s->length = 2u;
        break;
    case R_PWCTR2:
        s->length = 1u;
        break;
    case R_VMCTR1:
        s->length = 2u;
        break;
    case R_GMCTRP1:
    case R_GMCTRN1:
        s->length = 16u;
        break;
    case R_GAMSET:
    case R_RAMRD:
    case R_PTLAR:
    case R_TEOFF:
    case R_TEON:
    case R_RDDID:
    case R_RDDST:
    case R_RDDPM:
    case R_RDDMADCTL:
    case R_RDDCOLMOD:
    case R_RDDIM:
    case R_RDDSM:
    case R_VMOFCTR:
    case R_WRID2:
    case R_NVFCTR1:
    case R_NVFCTR2:
    case R_NVFCTR3:
    case R_RDID1:
    case R_RDID2:
    case R_RDID3:
    case R_EXTCTRL:
    case R_VCOM4L:
    default:
        qemu_log_mask(LOG_UNIMP, "%s: Command not supported 0x%02x\n", __func__,
                      cmd);
        break;
    }
}

static void st7735_execute_command(St7735State *s)
{
    switch (s->command) {
    case R_COLMOD:
        switch (s->buffer[0]) {
        case PX_444:
        case PX_565:
        case PX_666:
            s->disp.pixmode = (St7735PixelMode)s->buffer[0];
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Unknown pixel mode %u\n",
                          __func__, s->buffer[0]);
            break;
        }
        break;
    case R_MADCTL:
        s->disp.madctl = s->buffer[0];
        break;
    case R_INVCTR:
        s->disp.invctr = s->buffer[0];
        break;
    case R_CASET: /* See .10 Memory Data Write/ Read Direction */
        s->disp.xs = MIN(lduw_be_p(&s->buffer[0u]), s->width);
        s->disp.xe = MIN(lduw_be_p(&s->buffer[2u]), s->width);
        s->row = s->disp.ys;
        trace_st7735_cursor("X", s->disp.xs, s->disp.xe);
        break;
    case R_RASET:
        s->disp.ys = MIN(lduw_be_p(&s->buffer[0u]), s->height);
        s->disp.ye = MIN(lduw_be_p(&s->buffer[2u]), s->height);
        s->col = s->disp.xs;
        trace_st7735_cursor("Y", s->disp.xs, s->disp.xe);
        break;
    case R_FRMCTR1: /* don't care */
    case R_FRMCTR2:
    case R_FRMCTR3:
    case R_DISSET5:
    case R_PWCTR1:
    case R_PWCTR2:
    case R_PWCTR3:
    case R_PWCTR4:
    case R_PWCTR5:
    case R_PWCTR6:
    case R_VMCTR1:
    case R_GMCTRP1: /* gamma control */
    case R_GMCTRN1:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: Command not supported 0x%02x\n", __func__,
                      s->command);
        break;
    }
}

static inline uint32_t st7735_make_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    /* PIXMAN_x8r8g8b8 encoding */
    return (((uint32_t)r) << 16u) | (((uint32_t)g) << 8u) | ((uint32_t)b << 0u);
}

static ST7735_FB_PIXEL *st7735_get_pixel(St7735State *s)
{
    if (s->col > s->disp.xe) {
        s->col = s->disp.xs;
        s->row++;
    }
    if (s->row > s->disp.ye) {
        s->row = s->disp.ys;
    }

    return &s->fb[s->col + s->row * s->width];
}

static void st7735_write_memory(St7735State *s, uint8_t byte)
{
    s->buffer[s->pos++] = byte;
    ST7735_FB_PIXEL *pixel;
    uint8_t r, g, b;
    switch (s->disp.pixmode) {
    case PX_565:
        if (s->pos == 2u) {
            s->pos = 0u;
            pixel = st7735_get_pixel(s);
            uint16_t px16 = lduw_be_p(&s->buffer[0u]);
            r = (uint8_t)((px16 >> 8u) & 0xf8u);
            g = (uint8_t)((px16 >> 3u) & 0xfcu);
            b = (uint8_t)((px16 << 3u) & 0xf8u);
            pixel[0u] = st7735_make_pixel(r, g, b);
            s->col += 1u;
            break;
        }
        return;
    case PX_666:
        if (s->pos == 3u) {
            s->pos = 0u;
            pixel = st7735_get_pixel(s);
            r = (uint8_t)(s->buffer[0u] & 0xfcu);
            g = (uint8_t)(s->buffer[1u] & 0xfcu);
            b = (uint8_t)(s->buffer[2u] & 0xfcu);
            pixel[0u] = st7735_make_pixel(r, g, b);
            s->col += 1u;
            break;
        };
        return;
    case PX_444:
        if (s->pos == 3u) {
            s->pos = 0u;
            pixel = st7735_get_pixel(s);
            r = (uint8_t)(s->buffer[0u] & 0xf0u);
            g = (uint8_t)(s->buffer[0u] << 4u);
            b = (uint8_t)(s->buffer[1u] & 0xf0u);
            pixel[0u] = st7735_make_pixel(r, g, b);
            r = (uint8_t)(s->buffer[1u] << 4u);
            g = (uint8_t)(s->buffer[2u] & 0xf0u);
            b = (uint8_t)(s->buffer[2u] << 4u);
            pixel[1u] = st7735_make_pixel(r, g, b);
            s->col += 2u;
            break;
        }
        return;
    default:
        /* be sure to reset the pixel buffer if not handled */
        s->pos = 0;
        return;
    }
}

static uint32_t st7735_transfer(SSIPeripheral *dev, uint32_t rx)
{
    St7735State *s = ST7735(dev);

    if (!s->nreset) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Ignoring SSI in HW reset\n",
                      __func__);
        return 0u;
    }

    switch (s->state) {
    case STATE_WRITING_MEMORY:
        if (s->dc && s->fb) {
            st7735_write_memory(s, rx);
            return 0;
        }
        break;
    case STATE_IDLE:
        if (!s->dc) {
            s->length = 0;
            st7735_decode_command(s, (uint8_t)rx);
            if (s->length) {
                s->state = STATE_COLLECTING_DATA;
            }
            return 0;
        }
        break;
    case STATE_COLLECTING_DATA:
        if (s->dc) {
            if (s->pos < ST7735_BUFFER_LEN) {
                s->buffer[s->pos++] = (uint8_t)rx;
                if (s->pos == s->length) {
                    st7735_execute_command(s);
                }
            } else {
                s->pos++;
            }
            return 0;
        }
        break;
    }

    qemu_log_mask(LOG_GUEST_ERROR, "%s: Ignoring SSI data, D/C: %u, state %d\n",
                  __func__, s->dc, s->state);

    return 0u;
}

static int st7735_set_cs(SSIPeripheral *dev, bool select)
{
    St7735State *s = ST7735(dev);

    if (select) {
        if (s->state == STATE_WRITING_MEMORY) {
            s->redraw = true;
        }
        s->pos = 0;
        s->length = 0;
        s->state = STATE_IDLE;
    }

    return 0;
}

static void st7735_invalidate_display(void *opaque)
{
    St7735State *s = opaque;

    s->redraw = true;
}

static void st7735_update_display(void *opaque)
{
    St7735State *s = opaque;

    if (!s->redraw || !s->fb) {
        return;
    }

    DisplaySurface *surface = qemu_console_surface(s->con);
    void *dest = surface_data(surface);
    memcpy(dest, s->fb, ST7735_FB_PIXELS(s) * ST7735_FB_BPP);
    dpy_gfx_update(s->con, 0, 0, s->width, s->height);
    s->redraw = false;
}

static Property st7735_properties[] = {
    DEFINE_PROP_UINT16("width", St7735State, width, ST7735_DEFAULT_WIDTH),
    DEFINE_PROP_UINT16("height", St7735State, height, ST7735_DEFAULT_HEIGHT),
    DEFINE_PROP_END_OF_LIST(),
};

static const GraphicHwOps st7735_ui_ops = {
    .invalidate = &st7735_invalidate_display,
    .gfx_update = &st7735_update_display,
};

static void st7735_realize(SSIPeripheral *dev, Error **errp)
{
    St7735State *s = ST7735(dev);

    s->con = graphic_console_init(DEVICE(dev), 0, &st7735_ui_ops, s);
    qemu_console_resize(s->con, s->width, s->height);

    qdev_init_gpio_in_named(DEVICE(dev), &st7735_gpio_event, ST7735_IO_LINES,
                            ST7735_IO_COUNT);

    DisplaySurface *surface = qemu_console_surface(s->con);
    int bpp = surface_bits_per_pixel(surface);
    if (bpp >= 24) {
        s->fb = g_new0(ST7735_FB_PIXEL, ST7735_FB_PIXELS(s));
    } else {
        qemu_log_mask(LOG_UNIMP, "%s: No support for display w/ %d bpp\n",
                      __func__, bpp);
        s->fb = NULL;
    }
}

static void st7735_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    dc->reset = &st7735_reset;
    k->realize = &st7735_realize;
    k->transfer = &st7735_transfer;
    k->set_cs = &st7735_set_cs;
    k->cs_polarity = SSI_CS_LOW;

    device_class_set_props(dc, st7735_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo st7735_info = {
    .name = TYPE_ST7735,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(St7735State),
    .class_init = &st7735_class_init,
};

static void st7735_register_types(void)
{
    type_register_static(&st7735_info);
}

type_init(st7735_register_types);

void st7735_configure(DeviceState *dev, hwaddr addr)
{
    SysBusDevice *busdev = SYS_BUS_DEVICE(dev);

    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, addr);
}
