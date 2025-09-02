
#include "qemu/osdep.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_usbdev.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

/* clang-format off */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(PKT_RECEIVED, 0u, 1u)
    SHARED_FIELD(PKT_SENT, 1u, 1u)
    SHARED_FIELD(DISCONNECTED, 2u, 1u)
    SHARED_FIELD(HOST_LOST, 3u, 1u)
    SHARED_FIELD(LINK_RESET, 4u, 1u)
    SHARED_FIELD(LINK_SUSPEND, 5u, 1u)
    SHARED_FIELD(LINK_RESUME, 6u, 1u)
    SHARED_FIELD(AV_OUT_EMPTY, 7u, 1u)
    SHARED_FIELD(RX_FULL, 8u, 1u)
    SHARED_FIELD(AV_OVERFLOW, 9u, 1u)
    SHARED_FIELD(LINK_IN_ERR, 10u, 1u)
    SHARED_FIELD(RX_CRC_ERR, 11u, 1u)
    SHARED_FIELD(RX_PID_ERR, 12u, 1u)
    SHARED_FIELD(RX_BITSTUFF_ERR, 13u, 1u)
    SHARED_FIELD(FRAME, 14u, 1u)
    SHARED_FIELD(POWERED, 15u, 1u)
    SHARED_FIELD(LINK_OUT_ERR, 16u, 1u)
    SHARED_FIELD(AV_SETUP_EMPTY, 17u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(USBCTRL, 0x10u)
    FIELD(USBCTRL, ENABLE, 0u, 1u)
    FIELD(USBCTRL, RESUME_LINK_ACTIVE, 1u, 1u)
    FIELD(USBCTRL, DEVICE_ADDRESS, 16u, 7u)
REG32(EP_OUT_ENABLE, 0x14u)
    FIELD(EP_OUT_ENABLE, ENABLE_0, 0u, 1u)
    FIELD(EP_OUT_ENABLE, ENABLE_1, 1u, 1u)
    FIELD(EP_OUT_ENABLE, ENABLE_2, 2u, 1u)
    FIELD(EP_OUT_ENABLE, ENABLE_3, 3u, 1u)
    FIELD(EP_OUT_ENABLE, ENABLE_4, 4u, 1u)
    FIELD(EP_OUT_ENABLE, ENABLE_5, 5u, 1u)
    FIELD(EP_OUT_ENABLE, ENABLE_6, 6u, 1u)
    FIELD(EP_OUT_ENABLE, ENABLE_7, 7u, 1u)
    FIELD(EP_OUT_ENABLE, ENABLE_8, 8u, 1u)
    FIELD(EP_OUT_ENABLE, ENABLE_9, 9u, 1u)
    FIELD(EP_OUT_ENABLE, ENABLE_10, 10u, 1u)
    FIELD(EP_OUT_ENABLE, ENABLE_11, 11u, 1u)
REG32(EP_IN_ENABLE, 0x18u)
    FIELD(EP_IN_ENABLE, ENABLE_0, 0u, 1u)
    FIELD(EP_IN_ENABLE, ENABLE_1, 1u, 1u)
    FIELD(EP_IN_ENABLE, ENABLE_2, 2u, 1u)
    FIELD(EP_IN_ENABLE, ENABLE_3, 3u, 1u)
    FIELD(EP_IN_ENABLE, ENABLE_4, 4u, 1u)
    FIELD(EP_IN_ENABLE, ENABLE_5, 5u, 1u)
    FIELD(EP_IN_ENABLE, ENABLE_6, 6u, 1u)
    FIELD(EP_IN_ENABLE, ENABLE_7, 7u, 1u)
    FIELD(EP_IN_ENABLE, ENABLE_8, 8u, 1u)
    FIELD(EP_IN_ENABLE, ENABLE_9, 9u, 1u)
    FIELD(EP_IN_ENABLE, ENABLE_10, 10u, 1u)
    FIELD(EP_IN_ENABLE, ENABLE_11, 11u, 1u)
REG32(USBSTAT, 0x1cu)
    FIELD(USBSTAT, FRAME, 0u, 11u)
    FIELD(USBSTAT, HOST_LOST, 11u, 1u)
    FIELD(USBSTAT, LINK_STATE, 12u, 3u)
    FIELD(USBSTAT, SENSE, 15u, 1u)
    FIELD(USBSTAT, AV_OUT_DEPTH, 16u, 4u)
    FIELD(USBSTAT, AV_SETUP_DEPTH, 20u, 3u)
    FIELD(USBSTAT, AV_OUT_FULL, 23u, 1u)
    FIELD(USBSTAT, RX_DEPTH, 24u, 4u)
    FIELD(USBSTAT, AV_SETUP_FULL, 30u, 1u)
    FIELD(USBSTAT, RX_EMPTY, 31u, 1u)
REG32(AVOUTBUFFER, 0x20u)
    FIELD(AVOUTBUFFER, BUFFER, 0u, 5u)
REG32(AVSETUPBUFFER, 0x24u)
    FIELD(AVSETUPBUFFER, BUFFER, 0u, 5u)
REG32(RXFIFO, 0x28u)
    FIELD(RXFIFO, BUFFER, 0u, 5u)
    FIELD(RXFIFO, SIZE, 8u, 7u)
    FIELD(RXFIFO, SETUP, 19u, 1u)
    FIELD(RXFIFO, EP, 20u, 4u)
REG32(RXENABLE_SETUP, 0x2cu)
    FIELD(RXENABLE_SETUP, SETUP_0, 0u, 1u)
    FIELD(RXENABLE_SETUP, SETUP_1, 1u, 1u)
    FIELD(RXENABLE_SETUP, SETUP_2, 2u, 1u)
    FIELD(RXENABLE_SETUP, SETUP_3, 3u, 1u)
    FIELD(RXENABLE_SETUP, SETUP_4, 4u, 1u)
    FIELD(RXENABLE_SETUP, SETUP_5, 5u, 1u)
    FIELD(RXENABLE_SETUP, SETUP_6, 6u, 1u)
    FIELD(RXENABLE_SETUP, SETUP_7, 7u, 1u)
    FIELD(RXENABLE_SETUP, SETUP_8, 8u, 1u)
    FIELD(RXENABLE_SETUP, SETUP_9, 9u, 1u)
    FIELD(RXENABLE_SETUP, SETUP_10, 10u, 1u)
    FIELD(RXENABLE_SETUP, SETUP_11, 11u, 1u)
REG32(RXENABLE_OUT, 0x30u)
    FIELD(RXENABLE_OUT, OUT_0, 0u, 1u)
    FIELD(RXENABLE_OUT, OUT_1, 1u, 1u)
    FIELD(RXENABLE_OUT, OUT_2, 2u, 1u)
    FIELD(RXENABLE_OUT, OUT_3, 3u, 1u)
    FIELD(RXENABLE_OUT, OUT_4, 4u, 1u)
    FIELD(RXENABLE_OUT, OUT_5, 5u, 1u)
    FIELD(RXENABLE_OUT, OUT_6, 6u, 1u)
    FIELD(RXENABLE_OUT, OUT_7, 7u, 1u)
    FIELD(RXENABLE_OUT, OUT_8, 8u, 1u)
    FIELD(RXENABLE_OUT, OUT_9, 9u, 1u)
    FIELD(RXENABLE_OUT, OUT_10, 10u, 1u)
    FIELD(RXENABLE_OUT, OUT_11, 11u, 1u)
REG32(SET_NAK_OUT, 0x34u)
    FIELD(SET_NAK_OUT, ENABLE_0, 0u, 1u)
    FIELD(SET_NAK_OUT, ENABLE_1, 1u, 1u)
    FIELD(SET_NAK_OUT, ENABLE_2, 2u, 1u)
    FIELD(SET_NAK_OUT, ENABLE_3, 3u, 1u)
    FIELD(SET_NAK_OUT, ENABLE_4, 4u, 1u)
    FIELD(SET_NAK_OUT, ENABLE_5, 5u, 1u)
    FIELD(SET_NAK_OUT, ENABLE_6, 6u, 1u)
    FIELD(SET_NAK_OUT, ENABLE_7, 7u, 1u)
    FIELD(SET_NAK_OUT, ENABLE_8, 8u, 1u)
    FIELD(SET_NAK_OUT, ENABLE_9, 9u, 1u)
    FIELD(SET_NAK_OUT, ENABLE_10, 10u, 1u)
    FIELD(SET_NAK_OUT, ENABLE_11, 11u, 1u)
REG32(IN_SENT, 0x38u)
    FIELD(IN_SENT, SENT_0, 0u, 1u)
    FIELD(IN_SENT, SENT_1, 1u, 1u)
    FIELD(IN_SENT, SENT_2, 2u, 1u)
    FIELD(IN_SENT, SENT_3, 3u, 1u)
    FIELD(IN_SENT, SENT_4, 4u, 1u)
    FIELD(IN_SENT, SENT_5, 5u, 1u)
    FIELD(IN_SENT, SENT_6, 6u, 1u)
    FIELD(IN_SENT, SENT_7, 7u, 1u)
    FIELD(IN_SENT, SENT_8, 8u, 1u)
    FIELD(IN_SENT, SENT_9, 9u, 1u)
    FIELD(IN_SENT, SENT_10, 10u, 1u)
    FIELD(IN_SENT, SENT_11, 11u, 1u)
REG32(OUT_STALL, 0x3cu)
    FIELD(OUT_STALL, ENDPOINT_0, 0u, 1u)
    FIELD(OUT_STALL, ENDPOINT_1, 1u, 1u)
    FIELD(OUT_STALL, ENDPOINT_2, 2u, 1u)
    FIELD(OUT_STALL, ENDPOINT_3, 3u, 1u)
    FIELD(OUT_STALL, ENDPOINT_4, 4u, 1u)
    FIELD(OUT_STALL, ENDPOINT_5, 5u, 1u)
    FIELD(OUT_STALL, ENDPOINT_6, 6u, 1u)
    FIELD(OUT_STALL, ENDPOINT_7, 7u, 1u)
    FIELD(OUT_STALL, ENDPOINT_8, 8u, 1u)
    FIELD(OUT_STALL, ENDPOINT_9, 9u, 1u)
    FIELD(OUT_STALL, ENDPOINT_10, 10u, 1u)
    FIELD(OUT_STALL, ENDPOINT_11, 11u, 1u)
REG32(IN_STALL, 0x40u)
    FIELD(IN_STALL, ENDPOINT_0, 0u, 1u)
    FIELD(IN_STALL, ENDPOINT_1, 1u, 1u)
    FIELD(IN_STALL, ENDPOINT_2, 2u, 1u)
    FIELD(IN_STALL, ENDPOINT_3, 3u, 1u)
    FIELD(IN_STALL, ENDPOINT_4, 4u, 1u)
    FIELD(IN_STALL, ENDPOINT_5, 5u, 1u)
    FIELD(IN_STALL, ENDPOINT_6, 6u, 1u)
    FIELD(IN_STALL, ENDPOINT_7, 7u, 1u)
    FIELD(IN_STALL, ENDPOINT_8, 8u, 1u)
    FIELD(IN_STALL, ENDPOINT_9, 9u, 1u)
    FIELD(IN_STALL, ENDPOINT_10, 10u, 1u)
    FIELD(IN_STALL, ENDPOINT_11, 11u, 1u)
REG32(CONFIGIN_0, 0x44u)
    FIELD(CONFIGIN_0, BUFFER, 0u, 5u)
    FIELD(CONFIGIN_0, SIZE, 8u, 7u)
    FIELD(CONFIGIN_0, SENDING, 29u, 1u)
    FIELD(CONFIGIN_0, PEND, 30u, 1u)
    FIELD(CONFIGIN_0, RDY, 31u, 1u)
REG32(CONFIGIN_1, 0x48u)
    FIELD(CONFIGIN_1, BUFFER, 0u, 5u)
    FIELD(CONFIGIN_1, SIZE, 8u, 7u)
    FIELD(CONFIGIN_1, SENDING, 29u, 1u)
    FIELD(CONFIGIN_1, PEND, 30u, 1u)
    FIELD(CONFIGIN_1, RDY, 31u, 1u)
REG32(CONFIGIN_2, 0x4cu)
    FIELD(CONFIGIN_2, BUFFER, 0u, 5u)
    FIELD(CONFIGIN_2, SIZE, 8u, 7u)
    FIELD(CONFIGIN_2, SENDING, 29u, 1u)
    FIELD(CONFIGIN_2, PEND, 30u, 1u)
    FIELD(CONFIGIN_2, RDY, 31u, 1u)
REG32(CONFIGIN_3, 0x50u)
    FIELD(CONFIGIN_3, BUFFER, 0u, 5u)
    FIELD(CONFIGIN_3, SIZE, 8u, 7u)
    FIELD(CONFIGIN_3, SENDING, 29u, 1u)
    FIELD(CONFIGIN_3, PEND, 30u, 1u)
    FIELD(CONFIGIN_3, RDY, 31u, 1u)
REG32(CONFIGIN_4, 0x54u)
    FIELD(CONFIGIN_4, BUFFER, 0u, 5u)
    FIELD(CONFIGIN_4, SIZE, 8u, 7u)
    FIELD(CONFIGIN_4, SENDING, 29u, 1u)
    FIELD(CONFIGIN_4, PEND, 30u, 1u)
    FIELD(CONFIGIN_4, RDY, 31u, 1u)
REG32(CONFIGIN_5, 0x58u)
    FIELD(CONFIGIN_5, BUFFER, 0u, 5u)
    FIELD(CONFIGIN_5, SIZE, 8u, 7u)
    FIELD(CONFIGIN_5, SENDING, 29u, 1u)
    FIELD(CONFIGIN_5, PEND, 30u, 1u)
    FIELD(CONFIGIN_5, RDY, 31u, 1u)
REG32(CONFIGIN_6, 0x5cu)
    FIELD(CONFIGIN_6, BUFFER, 0u, 5u)
    FIELD(CONFIGIN_6, SIZE, 8u, 7u)
    FIELD(CONFIGIN_6, SENDING, 29u, 1u)
    FIELD(CONFIGIN_6, PEND, 30u, 1u)
    FIELD(CONFIGIN_6, RDY, 31u, 1u)
REG32(CONFIGIN_7, 0x60u)
    FIELD(CONFIGIN_7, BUFFER, 0u, 5u)
    FIELD(CONFIGIN_7, SIZE, 8u, 7u)
    FIELD(CONFIGIN_7, SENDING, 29u, 1u)
    FIELD(CONFIGIN_7, PEND, 30u, 1u)
    FIELD(CONFIGIN_7, RDY, 31u, 1u)
REG32(CONFIGIN_8, 0x64u)
    FIELD(CONFIGIN_8, BUFFER, 0u, 5u)
    FIELD(CONFIGIN_8, SIZE, 8u, 7u)
    FIELD(CONFIGIN_8, SENDING, 29u, 1u)
    FIELD(CONFIGIN_8, PEND, 30u, 1u)
    FIELD(CONFIGIN_8, RDY, 31u, 1u)
REG32(CONFIGIN_9, 0x68u)
    FIELD(CONFIGIN_9, BUFFER, 0u, 5u)
    FIELD(CONFIGIN_9, SIZE, 8u, 7u)
    FIELD(CONFIGIN_9, SENDING, 29u, 1u)
    FIELD(CONFIGIN_9, PEND, 30u, 1u)
    FIELD(CONFIGIN_9, RDY, 31u, 1u)
REG32(CONFIGIN_10, 0x6cu)
    FIELD(CONFIGIN_10, BUFFER, 0u, 5u)
    FIELD(CONFIGIN_10, SIZE, 8u, 7u)
    FIELD(CONFIGIN_10, SENDING, 29u, 1u)
    FIELD(CONFIGIN_10, PEND, 30u, 1u)
    FIELD(CONFIGIN_10, RDY, 31u, 1u)
REG32(CONFIGIN_11, 0x70u)
    FIELD(CONFIGIN_11, BUFFER, 0u, 5u)
    FIELD(CONFIGIN_11, SIZE, 8u, 7u)
    FIELD(CONFIGIN_11, SENDING, 29u, 1u)
    FIELD(CONFIGIN_11, PEND, 30u, 1u)
    FIELD(CONFIGIN_11, RDY, 31u, 1u)
REG32(OUT_ISO, 0x74u)
    FIELD(OUT_ISO, ISO_0, 0u, 1u)
    FIELD(OUT_ISO, ISO_1, 1u, 1u)
    FIELD(OUT_ISO, ISO_2, 2u, 1u)
    FIELD(OUT_ISO, ISO_3, 3u, 1u)
    FIELD(OUT_ISO, ISO_4, 4u, 1u)
    FIELD(OUT_ISO, ISO_5, 5u, 1u)
    FIELD(OUT_ISO, ISO_6, 6u, 1u)
    FIELD(OUT_ISO, ISO_7, 7u, 1u)
    FIELD(OUT_ISO, ISO_8, 8u, 1u)
    FIELD(OUT_ISO, ISO_9, 9u, 1u)
    FIELD(OUT_ISO, ISO_10, 10u, 1u)
    FIELD(OUT_ISO, ISO_11, 11u, 1u)
REG32(IN_ISO, 0x78u)
    FIELD(IN_ISO, ISO_0, 0u, 1u)
    FIELD(IN_ISO, ISO_1, 1u, 1u)
    FIELD(IN_ISO, ISO_2, 2u, 1u)
    FIELD(IN_ISO, ISO_3, 3u, 1u)
    FIELD(IN_ISO, ISO_4, 4u, 1u)
    FIELD(IN_ISO, ISO_5, 5u, 1u)
    FIELD(IN_ISO, ISO_6, 6u, 1u)
    FIELD(IN_ISO, ISO_7, 7u, 1u)
    FIELD(IN_ISO, ISO_8, 8u, 1u)
    FIELD(IN_ISO, ISO_9, 9u, 1u)
    FIELD(IN_ISO, ISO_10, 10u, 1u)
    FIELD(IN_ISO, ISO_11, 11u, 1u)
REG32(OUT_DATA_TOGGLE, 0x7cu)
    FIELD(OUT_DATA_TOGGLE, STATUS, 0u, 12u)
    FIELD(OUT_DATA_TOGGLE, MASK, 16u, 12u)
REG32(IN_DATA_TOGGLE, 0x80u)
    FIELD(IN_DATA_TOGGLE, STATUS, 0u, 12u)
    FIELD(IN_DATA_TOGGLE, MASK, 16u, 12u)
REG32(PHY_PINS_SENSE, 0x84u)
    FIELD(PHY_PINS_SENSE, RX_DP_I, 0u, 1u)
    FIELD(PHY_PINS_SENSE, RX_DN_I, 1u, 1u)
    FIELD(PHY_PINS_SENSE, RX_D_I, 2u, 1u)
    FIELD(PHY_PINS_SENSE, TX_DP_O, 8u, 1u)
    FIELD(PHY_PINS_SENSE, TX_DN_O, 9u, 1u)
    FIELD(PHY_PINS_SENSE, TX_D_O, 10u, 1u)
    FIELD(PHY_PINS_SENSE, TX_SE0_O, 11u, 1u)
    FIELD(PHY_PINS_SENSE, TX_OE_O, 12u, 1u)
    FIELD(PHY_PINS_SENSE, PWR_SENSE, 16u, 1u)
REG32(PHY_PINS_DRIVE, 0x88u)
    FIELD(PHY_PINS_DRIVE, DP_O, 0u, 1u)
    FIELD(PHY_PINS_DRIVE, DN_O, 1u, 1u)
    FIELD(PHY_PINS_DRIVE, D_O, 2u, 1u)
    FIELD(PHY_PINS_DRIVE, SE0_O, 3u, 1u)
    FIELD(PHY_PINS_DRIVE, OE_O, 4u, 1u)
    FIELD(PHY_PINS_DRIVE, RX_ENABLE_O, 5u, 1u)
    FIELD(PHY_PINS_DRIVE, DP_PULLUP_EN_O, 6u, 1u)
    FIELD(PHY_PINS_DRIVE, DN_PULLUP_EN_O, 7u, 1u)
    FIELD(PHY_PINS_DRIVE, EN, 16u, 1u)
REG32(PHY_CONFIG, 0x8cu)
    FIELD(PHY_CONFIG, USE_DIFF_RCVR, 0u, 1u)
    FIELD(PHY_CONFIG, TX_USE_D_SE0, 1u, 1u)
    FIELD(PHY_CONFIG, EOP_SINGLE_BIT, 2u, 1u)
    FIELD(PHY_CONFIG, PINFLIP, 5u, 1u)
    FIELD(PHY_CONFIG, USB_REF_DISABLE, 6u, 1u)
    FIELD(PHY_CONFIG, TX_OSC_TEST_MODE, 7u, 1u)
REG32(WAKE_CONTROL, 0x90u)
    FIELD(WAKE_CONTROL, SUSPEND_REQ, 0u, 1u)
    FIELD(WAKE_CONTROL, WAKE_ACK, 1u, 1u)
REG32(WAKE_EVENTS, 0x94u)
    FIELD(WAKE_EVENTS, MODULE_ACTIVE, 0u, 1u)
    FIELD(WAKE_EVENTS, DISCONNECTED, 8u, 1u)
    FIELD(WAKE_EVENTS, BUS_RESET, 9u, 1u)
    FIELD(WAKE_EVENTS, BUS_NOT_IDLE, 10u, 1u)
REG32(FIFO_CTRL, 0x98u)
    FIELD(FIFO_CTRL, AVOUT_RST, 0u, 1u)
    FIELD(FIFO_CTRL, AVSETUP_RST, 1u, 1u)
    FIELD(FIFO_CTRL, RX_RST, 2u, 1u)
REG32(COUNT_OUT, 0x9cu)
    FIELD(COUNT_OUT, COUNT, 0u, 8u)
    FIELD(COUNT_OUT, DATATOG_OUT, 12u, 1u)
    FIELD(COUNT_OUT, DROP_RX, 13u, 1u)
    FIELD(COUNT_OUT, DROP_AVOUT, 14u, 1u)
    FIELD(COUNT_OUT, IGN_AVSETUP, 15u, 1u)
    FIELD(COUNT_OUT, ENDPOINTS, 16u, 12u)
    FIELD(COUNT_OUT, RST, 31u, 1u)
REG32(COUNT_IN, 0xa0u)
    FIELD(COUNT_IN, COUNT, 0u, 8u)
    FIELD(COUNT_IN, NODATA, 13u, 1u)
    FIELD(COUNT_IN, NAK, 14u, 1u)
    FIELD(COUNT_IN, TIMEOUT, 15u, 1u)
    FIELD(COUNT_IN, ENDPOINTS, 16u, 12u)
    FIELD(COUNT_IN, RST, 31u, 1u)
REG32(COUNT_NODATA_IN, 0xa4u)
    FIELD(COUNT_NODATA_IN, COUNT, 0u, 8u)
    FIELD(COUNT_NODATA_IN, ENDPOINTS, 16u, 12u)
    FIELD(COUNT_NODATA_IN, RST, 31u, 1u)
REG32(COUNT_ERRORS, 0xa8u)
    FIELD(COUNT_ERRORS, COUNT, 0u, 8u)
    FIELD(COUNT_ERRORS, PID_INVALID, 27u, 1u)
    FIELD(COUNT_ERRORS, BITSTUFF, 28u, 1u)
    FIELD(COUNT_ERRORS, CRC16, 29u, 1u)
    FIELD(COUNT_ERRORS, CRC5, 30u, 1u)
    FIELD(COUNT_ERRORS, RST, 31u, 1u)
/* clang-format on */

#define USBDEV_IRQ_NUM      18u

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_COUNT_ERRORS)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(USBCTRL),
    REG_NAME_ENTRY(EP_OUT_ENABLE),
    REG_NAME_ENTRY(EP_IN_ENABLE),
    REG_NAME_ENTRY(USBSTAT),
    REG_NAME_ENTRY(AVOUTBUFFER),
    REG_NAME_ENTRY(AVSETUPBUFFER),
    REG_NAME_ENTRY(RXFIFO),
    REG_NAME_ENTRY(RXENABLE_SETUP),
    REG_NAME_ENTRY(RXENABLE_OUT),
    REG_NAME_ENTRY(SET_NAK_OUT),
    REG_NAME_ENTRY(IN_SENT),
    REG_NAME_ENTRY(OUT_STALL),
    REG_NAME_ENTRY(IN_STALL),
    REG_NAME_ENTRY(CONFIGIN_0),
    REG_NAME_ENTRY(CONFIGIN_1),
    REG_NAME_ENTRY(CONFIGIN_2),
    REG_NAME_ENTRY(CONFIGIN_3),
    REG_NAME_ENTRY(CONFIGIN_4),
    REG_NAME_ENTRY(CONFIGIN_5),
    REG_NAME_ENTRY(CONFIGIN_6),
    REG_NAME_ENTRY(CONFIGIN_7),
    REG_NAME_ENTRY(CONFIGIN_8),
    REG_NAME_ENTRY(CONFIGIN_9),
    REG_NAME_ENTRY(CONFIGIN_10),
    REG_NAME_ENTRY(CONFIGIN_11),
    REG_NAME_ENTRY(OUT_ISO),
    REG_NAME_ENTRY(IN_ISO),
    REG_NAME_ENTRY(OUT_DATA_TOGGLE),
    REG_NAME_ENTRY(IN_DATA_TOGGLE),
    REG_NAME_ENTRY(PHY_PINS_SENSE),
    REG_NAME_ENTRY(PHY_PINS_DRIVE),
    REG_NAME_ENTRY(PHY_CONFIG),
    REG_NAME_ENTRY(WAKE_CONTROL),
    REG_NAME_ENTRY(WAKE_EVENTS),
    REG_NAME_ENTRY(FIFO_CTRL),
    REG_NAME_ENTRY(COUNT_OUT),
    REG_NAME_ENTRY(COUNT_IN),
    REG_NAME_ENTRY(COUNT_NODATA_IN),
    REG_NAME_ENTRY(COUNT_ERRORS),
    /* clang-format on */
};
#undef REG_NAME_ENTRY

struct OtUSBDEVState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    IbexIRQ irqs[USBDEV_IRQ_NUM];

    uint32_t regs[REGS_COUNT];

    char *ot_id;
    uint32_t pclk;
    CharBackend chr;
};

static void usbdev_update_irqs(OtUSBDEVState *s)
{
    uint32_t state_masked = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    trace_ot_usbdev_irqs(s->ot_id, s->regs[R_INTR_STATE], s->regs[R_INTR_ENABLE],
                       state_masked);

    for (int index = 0; index < USBDEV_IRQ_NUM; index++) {
        bool level = (state_masked & (1U << index)) != 0;
        ibex_irq_set(&s->irqs[index], level);
    }
}

static int usbdev_can_receive(void *opaque)
{
   OtUSBDEVState *s = opaque;
   (void)s;

    return 0;
}

static uint64_t usbdev_read(void *opaque, hwaddr addr, unsigned size)
{
   OtUSBDEVState *s = opaque;
   (void)s;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_USBCTRL:
    case R_EP_OUT_ENABLE:
    case R_EP_IN_ENABLE:
    case R_USBSTAT:
    case R_AVOUTBUFFER:
    case R_AVSETUPBUFFER:
    case R_RXFIFO:
    case R_RXENABLE_SETUP:
    case R_RXENABLE_OUT:
    case R_SET_NAK_OUT:
    case R_IN_SENT:
    case R_OUT_STALL:
    case R_IN_STALL:
    case R_CONFIGIN_0:
    case R_CONFIGIN_1:
    case R_CONFIGIN_2:
    case R_CONFIGIN_3:
    case R_CONFIGIN_4:
    case R_CONFIGIN_5:
    case R_CONFIGIN_6:
    case R_CONFIGIN_7:
    case R_CONFIGIN_8:
    case R_CONFIGIN_9:
    case R_CONFIGIN_10:
    case R_CONFIGIN_11:
    case R_OUT_ISO:
    case R_IN_ISO:
    case R_OUT_DATA_TOGGLE:
    case R_IN_DATA_TOGGLE:
    case R_PHY_PINS_SENSE:
    case R_PHY_PINS_DRIVE:
    case R_PHY_CONFIG:
    case R_WAKE_CONTROL:
    case R_WAKE_EVENTS:
    case R_FIFO_CTRL:
    case R_COUNT_OUT:
    case R_COUNT_IN:
    case R_COUNT_NODATA_IN:
    case R_COUNT_ERRORS:
        qemu_log_mask(LOG_UNIMP, "%s: %s is not supported\n", __func__,
                      REG_NAME(reg));
        // fall through
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    return (uint64_t)val32;
}

static void usbdev_write(void *opaque, hwaddr addr, uint64_t val64,
                          unsigned size)
{
    OtUSBDEVState *s = opaque;
   (void)s;
    (void)size;
    // uint32_t val32 = val64;

    hwaddr reg = R32_OFF(addr);

    // uint32_t pc = ibex_get_current_pc();

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_USBCTRL:
    case R_EP_OUT_ENABLE:
    case R_EP_IN_ENABLE:
    case R_USBSTAT:
    case R_AVOUTBUFFER:
    case R_AVSETUPBUFFER:
    case R_RXFIFO:
    case R_RXENABLE_SETUP:
    case R_RXENABLE_OUT:
    case R_SET_NAK_OUT:
    case R_IN_SENT:
    case R_OUT_STALL:
    case R_IN_STALL:
    case R_CONFIGIN_0:
    case R_CONFIGIN_1:
    case R_CONFIGIN_2:
    case R_CONFIGIN_3:
    case R_CONFIGIN_4:
    case R_CONFIGIN_5:
    case R_CONFIGIN_6:
    case R_CONFIGIN_7:
    case R_CONFIGIN_8:
    case R_CONFIGIN_9:
    case R_CONFIGIN_10:
    case R_CONFIGIN_11:
    case R_OUT_ISO:
    case R_IN_ISO:
    case R_OUT_DATA_TOGGLE:
    case R_IN_DATA_TOGGLE:
    case R_PHY_PINS_SENSE:
    case R_PHY_PINS_DRIVE:
    case R_PHY_CONFIG:
    case R_WAKE_CONTROL:
    case R_WAKE_EVENTS:
    case R_FIFO_CTRL:
    case R_COUNT_OUT:
    case R_COUNT_IN:
    case R_COUNT_NODATA_IN:
    case R_COUNT_ERRORS:
        qemu_log_mask(LOG_UNIMP, "%s: %s is not supported\n", __func__,
                      REG_NAME(reg));
        // fall through
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps usbdev_ops = {
    .read = usbdev_read,
    .write = usbdev_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};


static Property usbdev_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtUSBDEVState, ot_id),
    DEFINE_PROP_UINT32("pclk", OtUSBDEVState, pclk, 0u),
    DEFINE_PROP_END_OF_LIST(),
};

static void usbdev_receive(void *opaque, const uint8_t *buf, int size)
{
    OtUSBDEVState *s = opaque;
    (void) s;

    // ot_usbdev_update_irqs(s);
}

static int usbdev_be_change(void *opaque)
{
    OtUSBDEVState *s = opaque;

    qemu_chr_fe_set_handlers(&s->chr, usbdev_can_receive, usbdev_receive,
                             NULL, usbdev_be_change, s, NULL, true);

    return 0;
}

static void usbdev_reset(DeviceState *dev)
{
    OtUSBDEVState *s = OT_USBDEV(dev);

    memset(&s->regs[0], 0, sizeof(s->regs));

    usbdev_update_irqs(s);
}

static void usbdev_realize(DeviceState *dev, Error **errp)
{
    OtUSBDEVState *s = OT_USBDEV(dev);
    (void)errp;

    g_assert(s->ot_id);

    qemu_chr_fe_set_handlers(&s->chr, usbdev_can_receive, usbdev_receive,
                             NULL, usbdev_be_change, s, NULL, true);
}

static void usbdev_init(Object *obj)
{
    OtUSBDEVState *s = OT_USBDEV(obj);


    memory_region_init_io(&s->mmio, obj, &usbdev_ops, s, TYPE_OT_USBDEV,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void usbdev_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = usbdev_realize;
    device_class_set_legacy_reset(dc, usbdev_reset);
    device_class_set_props(dc, usbdev_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usbdev_info = {
    .name = TYPE_OT_USBDEV,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtUSBDEVState),
    .instance_init = usbdev_init,
    .class_init = usbdev_class_init,
};

static void usbdev_register_types(void)
{
    type_register_static(&usbdev_info);
}

type_init(usbdev_register_types);
