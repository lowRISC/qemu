/*
 * QEMU OpenTitan USBDEV device
 *
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Amaury Pouly <amaury.pouly@lowrisc.org>
 *  Douglas Reis <doreis@lowrisc.org>
 *
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

/*
 * OpenTitan has a "zero-time" simulation model in
 * hw/ip/usbdev/dv/env/usbdev_bfm.sv. This driver makes references to this
 * behavioural model as "BFM".
 */

#include "qemu/osdep.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "chardev/char-fe.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/opentitan/ot_usbdev.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"


#define USBDEV_PARAM_N_ENDPOINTS 12u
#define USBDEV_PARAM_NUM_ALERTS  1u
#define USBDEV_PARAM_REG_WIDTH   32u

/*
 * The following are not parameters of the IP but hardcoded
 * constants in the RTL.
 */
#define OT_USBDEV_RX_FIFO_DEPTH       8u
#define OT_USBDEV_AV_SETUP_FIFO_DEPTH 4u
#define OT_USBDEV_AV_OUT_FIFO_DEPTH   8u
#define OT_USBDEV_MAX_PACKET_SIZE     64u
#define OT_USBDEV_BUFFER_COUNT        32u

/* Standard constants */
#define OT_USBDEV_SETUP_PACKET_SIZE 8u

/* NOLINTNEXTLINE(misc-redundant-expression) */
static_assert(USBDEV_PARAM_N_ENDPOINTS == 12u,
              "This driver only supports 12 endpoints");

/* clang-format off */
REG32(USBDEV_INTR_STATE, 0x0u)
    SHARED_FIELD(USBDEV_INTR_PKT_RECEIVED, 0u, 1u)
    SHARED_FIELD(USBDEV_INTR_PKT_SENT, 1u, 1u)
    SHARED_FIELD(USBDEV_INTR_DISCONNECTED, 2u, 1u)
    SHARED_FIELD(USBDEV_INTR_HOST_LOST, 3u, 1u)
    SHARED_FIELD(USBDEV_INTR_LINK_RESET, 4u, 1u)
    SHARED_FIELD(USBDEV_INTR_LINK_SUSPEND, 5u, 1u)
    SHARED_FIELD(USBDEV_INTR_LINK_RESUME, 6u, 1u)
    SHARED_FIELD(USBDEV_INTR_AV_OUT_EMPTY, 7u, 1u)
    SHARED_FIELD(USBDEV_INTR_RX_FULL, 8u, 1u)
    SHARED_FIELD(USBDEV_INTR_AV_OVERFLOW, 9u, 1u)
    SHARED_FIELD(USBDEV_INTR_LINK_IN_ERR, 10u, 1u)
    SHARED_FIELD(USBDEV_INTR_RX_CRC_ERR, 11u, 1u)
    SHARED_FIELD(USBDEV_INTR_RX_PID_ERR, 12u, 1u)
    SHARED_FIELD(USBDEV_INTR_RX_BITSTUFF_ERR, 13u, 1u)
    SHARED_FIELD(USBDEV_INTR_FRAME, 14u, 1u)
    SHARED_FIELD(USBDEV_INTR_POWERED, 15u, 1u)
    SHARED_FIELD(USBDEV_INTR_LINK_OUT_ERR, 16u, 1u)
    SHARED_FIELD(USBDEV_INTR_AV_SETUP_EMPTY, 17u, 1u)
REG32(USBDEV_INTR_ENABLE, 0x4u)
REG32(USBDEV_INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(USBCTRL, 0x10u)
    FIELD(USBCTRL, ENABLE, 0u, 1u)
    FIELD(USBCTRL, RESUME_LINK_ACTIVE, 1u, 1u)
    FIELD(USBCTRL, DEVICE_ADDRESS, 16u, 7u)
/*
 * The following registers use the same layout:
 * the i-th bit corresponds to endpoint i.
 */
REG32(EP_OUT_ENABLE, 0x14u)
REG32(EP_IN_ENABLE, 0x18u)
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
/*
 * The following registers use the same layout:
 * the i-th bit corresponds to endpoint i.
 */
REG32(RXENABLE_SETUP, 0x2cu)
REG32(RXENABLE_OUT, 0x30u)
REG32(SET_NAK_OUT, 0x34u)
REG32(IN_SENT, 0x38u)
REG32(OUT_STALL, 0x3cu)
REG32(IN_STALL, 0x40u)
/*
 * The following is a multi register with
 * USBDEV_PARAM_N_ENDPOINTS instances.
 */
REG32(CONFIGIN_0, 0x44u)
    SHARED_FIELD(CONFIGIN_BUFFER, 0u, 5u)
    SHARED_FIELD(CONFIGIN_SIZE, 8u, 7u)
    SHARED_FIELD(CONFIGIN_SENDING, 29u, 1u)
    SHARED_FIELD(CONFIGIN_PEND, 30u, 1u)
    SHARED_FIELD(CONFIGIN_RDY, 31u, 1u)
REG32(CONFIGIN_1, 0x48u)
REG32(CONFIGIN_2, 0x4cu)
REG32(CONFIGIN_3, 0x50u)
REG32(CONFIGIN_4, 0x54u)
REG32(CONFIGIN_5, 0x58u)
REG32(CONFIGIN_6, 0x5cu)
REG32(CONFIGIN_7, 0x60u)
REG32(CONFIGIN_8, 0x64u)
REG32(CONFIGIN_9, 0x68u)
REG32(CONFIGIN_10, 0x6cu)
REG32(CONFIGIN_11, 0x70u)
/*
 * The following registers use the same layout:
 * the i-th bit corresponds to endpoint i.
 */
REG32(OUT_ISO, 0x74u)
REG32(IN_ISO, 0x78u)
REG32(OUT_DATA_TOGGLE, 0x7cu)
    FIELD(OUT_DATA_TOGGLE, STATUS, 0u, USBDEV_PARAM_N_ENDPOINTS)
    FIELD(OUT_DATA_TOGGLE, MASK, 16u, USBDEV_PARAM_N_ENDPOINTS)
REG32(IN_DATA_TOGGLE, 0x80u)
    FIELD(IN_DATA_TOGGLE, STATUS, 0u, USBDEV_PARAM_N_ENDPOINTS)
    FIELD(IN_DATA_TOGGLE, MASK, 16u, USBDEV_PARAM_N_ENDPOINTS)
REG32(PHY_PINS_SENSE, 0x84u)
    FIELD(PHY_PINS_SENSE, RX_DP_I, 0u, 1u)
    FIELD(PHY_PINS_SENSE, RX_DN_I, 1u, 1u)
    FIELD(PHY_PINS_SENSE, RX_D_I, 2u, 1u)
    FIELD(PHY_PINS_SENSE, TX_DP_O, 8u, 1u)
    FIELD(PHY_PINS_SENSE, TX_DN_O, 9u, 1u)
    FIELD(PHY_PINS_SENSE, TX_D_O, 10u, 1u)
    FIELD(PHY_PINS_SENSE, TX_SE0_O, 11u, 1u)
    FIELD(PHY_PINS_SENSE, TX_OE_O, USBDEV_PARAM_N_ENDPOINTS, 1u)
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
    FIELD(COUNT_OUT, DATATOG_OUT, USBDEV_PARAM_N_ENDPOINTS, 1u)
    FIELD(COUNT_OUT, DROP_RX, 13u, 1u)
    FIELD(COUNT_OUT, DROP_AVOUT, 14u, 1u)
    FIELD(COUNT_OUT, IGN_AVSETUP, 15u, 1u)
    FIELD(COUNT_OUT, ENDPOINTS, 16u, USBDEV_PARAM_N_ENDPOINTS)
    FIELD(COUNT_OUT, RST, 31u, 1u)
REG32(COUNT_IN, 0xa0u)
    FIELD(COUNT_IN, COUNT, 0u, 8u)
    FIELD(COUNT_IN, NODATA, 13u, 1u)
    FIELD(COUNT_IN, NAK, 14u, 1u)
    FIELD(COUNT_IN, TIMEOUT, 15u, 1u)
    FIELD(COUNT_IN, ENDPOINTS, 16u, USBDEV_PARAM_N_ENDPOINTS)
    FIELD(COUNT_IN, RST, 31u, 1u)
REG32(COUNT_NODATA_IN, 0xa4u)
    FIELD(COUNT_NODATA_IN, COUNT, 0u, 8u)
    FIELD(COUNT_NODATA_IN, ENDPOINTS, 16u, USBDEV_PARAM_N_ENDPOINTS)
    FIELD(COUNT_NODATA_IN, RST, 31u, 1u)
REG32(COUNT_ERRORS, 0xa8u)
    FIELD(COUNT_ERRORS, COUNT, 0u, 8u)
    FIELD(COUNT_ERRORS, PID_INVALID, 27u, 1u)
    FIELD(COUNT_ERRORS, BITSTUFF, 28u, 1u)
    FIELD(COUNT_ERRORS, CRC16, 29u, 1u)
    FIELD(COUNT_ERRORS, CRC5, 30u, 1u)
    FIELD(COUNT_ERRORS, RST, 31u, 1u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG       (R_COUNT_ERRORS)
#define REGS_COUNT       (R_LAST_REG + 1u)
#define USBDEV_REGS_SIZE (REGS_COUNT * sizeof(uint32_t))

#define USBDEV_INTR_NUM 18u

#define USBDEV_INTR_RW1C_MASK \
    (USBDEV_INTR_DISCONNECTED_MASK | USBDEV_INTR_HOST_LOST_MASK | \
     USBDEV_INTR_LINK_RESET_MASK | USBDEV_INTR_LINK_SUSPEND_MASK | \
     USBDEV_INTR_LINK_RESUME_MASK | USBDEV_INTR_AV_OVERFLOW_MASK | \
     USBDEV_INTR_LINK_IN_ERR_MASK | USBDEV_INTR_RX_CRC_ERR_MASK | \
     USBDEV_INTR_RX_PID_ERR_MASK | USBDEV_INTR_RX_BITSTUFF_ERR_MASK | \
     USBDEV_INTR_FRAME_MASK | USBDEV_INTR_POWERED_MASK | \
     USBDEV_INTR_LINK_OUT_ERR_MASK)

#define USBDEV_INTR_MASK \
    (USBDEV_INTR_RW1C_MASK | USBDEV_INTR_PKT_RECEIVED_MASK | \
     USBDEV_INTR_PKT_SENT_MASK | USBDEV_INTR_AV_OUT_EMPTY_MASK | \
     USBDEV_INTR_RX_FULL_MASK | USBDEV_INTR_AV_SETUP_EMPTY_MASK)

#define USBDEV_USBCTRL_R_MASK \
    (R_USBCTRL_ENABLE_MASK | R_USBCTRL_DEVICE_ADDRESS_MASK)

#define USBDEV_CONFIGIN_RW1C_MASK (CONFIGIN_PEND_MASK | CONFIGIN_SENDING_MASK)

static_assert(USBDEV_INTR_MASK == (1u << USBDEV_INTR_NUM) - 1u,
              "Interrupt mask mismatch");

#define USBDEV_DEVICE_SIZE   0x1000u
#define USBDEV_REGS_OFFSET   0u
#define USBDEV_BUFFER_OFFSET 0x800u
#define USBDEV_BUFFER_SIZE   0x800u

static_assert(OT_USBDEV_MAX_PACKET_SIZE * OT_USBDEV_BUFFER_COUNT ==
                  USBDEV_BUFFER_SIZE,
              "USBDEV buffer size mismatch");

/*
 * Link state: this is the register field value, which we also use to
 * track the state of the link in general.
 */
typedef enum {
    /* Link disconnected (no VBUS or no pull-up connected). */
    OT_USBDEV_LINK_STATE_DISCONNECTED = 0,
    /* Link powered and connected, but not reset yet. */
    OT_USBDEV_LINK_STATE_POWERED = 1,
    /* Link suspended (constant idle/J for > 3 ms), but not reset yet. */
    OT_USBDEV_LINK_STATE_POWERED_SUSP = 2,
    /* Link active. */
    OT_USBDEV_LINK_STATE_ACTIVE = 3,
    /*
     * Link suspended (constant idle for > 3 ms), was active before
     * becoming suspended.
     */
    OT_USBDEV_LINK_STATE_SUSPENDED = 4,
    /* Link active but no SOF has been received since the last reset. */
    OT_USBDEV_LINK_STATE_ACTIVE_NOSOF = 5,
    /*
     * Link resuming to an active state,
     * pending the end of resume signaling.
     */
    OT_USBDEV_LINK_STATE_RESUMING = 6,
    OT_USBDEV_LINK_STATE_COUNT,
} OtUsbdevLinkState;

#define LINK_STATE_ENTRY(_name) \
    [OT_USBDEV_LINK_STATE_##_name] = stringify(_name)

static const char *LINK_STATE_NAME[] = {
    /* clang-format off */
    LINK_STATE_ENTRY(DISCONNECTED),
    LINK_STATE_ENTRY(POWERED),
    LINK_STATE_ENTRY(POWERED_SUSP),
    LINK_STATE_ENTRY(ACTIVE),
    LINK_STATE_ENTRY(SUSPENDED),
    LINK_STATE_ENTRY(ACTIVE_NOSOF),
    LINK_STATE_ENTRY(RESUMING),
    /* clang-format on */
};
#undef LINK_STATE_ENTRY

#define LINK_STATE_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(LINK_STATE_NAME) ? \
         LINK_STATE_NAME[(_st_)] : \
         "?")

struct OtUsbdevClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

/* Size of the communication buffer for the chardev. */
#define CMD_BUF_SIZE 32u

/* NOLINTNEXTLINE(misc-redundant-expression) */
static_assert(USBDEV_PARAM_NUM_ALERTS == 1u,
              "This only supports a single alert (fault)");

/*
 * USB server protocol
 *
 * The protocol is documented in docs/opentitan/ot_usbdev.md
 */

typedef enum {
    OT_USBDEV_SERVER_CMD_INVALID,
    OT_USBDEV_SERVER_CMD_HELLO,
    OT_USBDEV_SERVER_CMD_VBUS_ON,
    OT_USBDEV_SERVER_CMD_VBUS_OFF,
    OT_USBDEV_SERVER_CMD_CONNECT,
    OT_USBDEV_SERVER_CMD_DISCONNECT,
    OT_USBDEV_SERVER_CMD_RESET,
    OT_USBDEV_SERVER_CMD_RESUME,
    OT_USBDEV_SERVER_CMD_SUSPEND,
    OT_USBDEV_SERVER_CMD_SETUP,
    OT_USBDEV_SERVER_CMD_TRANSFER,
    OT_USBDEV_SERVER_CMD_COMPLETE,
} OtUsbdevServerCmd;

typedef struct {
    uint32_t cmd; /* Command, see OtUsbdevServerCmd */
    uint32_t size; /* Size, excluding header */
    uint32_t id; /* Unique ID */
} OtUsbdevServerPktHdr;

static_assert(sizeof(OtUsbdevServerPktHdr) == 12u,
              "Packet header has the wrong size");

#define OT_USBDEV_SERVER_HELLO_MAGIC "UDCX"
#define OT_USBDEV_SERVER_MAJOR_VER   1u
#define OT_USBDEV_SERVER_MINOR_VER   0u

typedef struct {
    char magic[4];
    uint16_t major_version;
    uint16_t minor_version;
} OtUsbdevServerHelloPkt;

static_assert(sizeof(OtUsbdevServerHelloPkt) == 8u,
              "Hello packet has the wrong size");

typedef struct {
    uint8_t address;
    uint8_t endpoint;
    uint8_t reserved[2];
    uint8_t setup[OT_USBDEV_SETUP_PACKET_SIZE];
} OtUsbdevServerSetupPkt;

static_assert(sizeof(OtUsbdevServerSetupPkt) == 12u,
              "Setup packet has the wrong size");

#define OT_USBDEV_EP_DIR_IN   0x80u
#define OT_USBDEV_EP_NUM_MASK 0x7fu

typedef enum {
    OT_USBDEV_SERVER_FLAG_ZLP = 1u << 0u,
} OtUsbdevServerTransferFlags;

typedef struct {
    uint8_t address;
    uint8_t endpoint;
    uint16_t packet_size;
    uint8_t flags;
    uint8_t reserved[3];
    uint32_t transfer_size;
} OtUsbdevServerTransferPkt;

static_assert(sizeof(OtUsbdevServerTransferPkt) == 12u,
              "Transfer packet has the wrong size");

typedef enum {
    OT_USBDEV_SERVER_STATUS_SUCCESS,
    OT_USBDEV_SERVER_STATUS_STALLED,
    OT_USBDEV_SERVER_STATUS_CANCELLED,
    OT_USBDEV_SERVER_STATUS_ERROR,
} OtUsbdevServerTransferStatus;

#define XFER_STATUS_ENTRY(_name) \
    [OT_USBDEV_SERVER_STATUS_##_name] = stringify(_name)

static const char *XFER_STATUS_NAME[] = {
    /* clang-format off */
    XFER_STATUS_ENTRY(SUCCESS),
    XFER_STATUS_ENTRY(STALLED),
    XFER_STATUS_ENTRY(CANCELLED),
    XFER_STATUS_ENTRY(ERROR),
    /* clang-format on */
};
#undef XFER_STATUS_ENTRY

#define XFER_STATUS_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(XFER_STATUS_NAME) ? \
         XFER_STATUS_NAME[(_st_)] : \
         "?")

typedef struct {
    uint8_t status;
    uint8_t reserved[3];
    uint32_t transfer_size;
} OtUsbdevServerCompletePkt;

/* State machine for the server receive path */
typedef enum {
    /* Wait for packet header */
    OT_USBDEV_SERVER_RECV_WAIT_HEADER,
    OT_USBDEV_SERVER_RECV_WAIT_DATA,
} OtUsbdevServerRecvState;

typedef struct {
    bool pending; /* Is there a transfer pending? */
    uint32_t id; /* ID of the transfer */
    uint32_t xfer_len; /* Maximum length of the transfer */
    uint8_t *xfer_buf; /* Buffer allocated to hold the data */
    uint16_t max_packet_size; /* Maximum packet size */
    uint32_t recv_rem; /* Remaining quantity to receive */
    uint8_t *recv_buf; /* Pointer to buffer where to receive */
} OtUsbdevServerEpInXfer;

typedef struct {
    bool pending; /* Is there a transfer pending? */
    bool send_zlp; /* Send a ZLP */
    uint32_t id; /* ID of the transfer */
    uint32_t xfer_len; /* Length of the transfer */
    uint8_t *xfer_buf; /* Buffer allocated to hold the data */
    uint16_t max_packet_size; /* Maximum packet size */
    uint32_t send_rem; /* Remaining quantity to send */
    uint8_t *send_buf; /* Pointer to buffer where to send */
} OtUsbdevServerEpOutXfer;

typedef struct {
    OtUsbdevServerEpInXfer in;
    OtUsbdevServerEpOutXfer out;
} OtUsbdevServerEpXfer;

typedef struct {
    /* Current state of the receiver */
    OtUsbdevServerRecvState recv_state;
    size_t recv_rem; /* Remaining quantity to receive */
    uint8_t *recv_buf; /* Pointer to buffer where to receive */
    /* Packet header under reception or processing */
    OtUsbdevServerPktHdr recv_pkt;
    /* Packet data under reception or processing */
    uint8_t *recv_data;

    /* endpoint transfers in progress */
    OtUsbdevServerEpXfer ep[USBDEV_PARAM_N_ENDPOINTS];

    /* Current state of server */
    bool client_connected; /* We have a client */
    bool vbus_connected; /* The host has turned on VBUS */
    bool hello_done; /* Have received a HELLO? */
} OtUsbdevServer;

struct OtUsbdevState {
    SysBusDevice parent_obj;
    struct {
        MemoryRegion main;
        MemoryRegion regs;
        MemoryRegion buffer;
    } mmio;
    IbexIRQ irqs[USBDEV_INTR_NUM];
    IbexIRQ alert;

    /* Register content */
    uint32_t regs[REGS_COUNT];
    /* VBUS gate: meaning depends on the vbus_override mode */
    bool vbus_gate;

    /*
     * Content of the RX FIFO: each entry is encoded like the
     * RXFIFO register.
     */
    OtFifo32 rx_fifo;
    /*
     * Content of the available SETUP and OUT buffer FIFOs:
     * each entry is a buffer ID.
     */
    Fifo8 av_setup_fifo;
    Fifo8 av_out_fifo;

    /* Buffer content */
    uint32_t buffer[USBDEV_BUFFER_SIZE / sizeof(uint32_t)];

    /* Communication device and buffer for management. */
    CharBackend cmd_chr;
    char cmd_buf[CMD_BUF_SIZE];
    unsigned cmd_buf_pos;
    /* Communication device for the USB protocol. */
    CharBackend usb_chr;
    OtUsbdevServer usb_server;

    char *ot_id;
    /* Link to the clkmgr. */
    DeviceState *clock_src;
    /* Name of the USB clock. */
    char *usbclk_name;
    /* Name of the AON clock. */
    char *aonclk_name;
    /* VBUS override mode. */
    bool vbus_override;
};

#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(USBDEV_INTR_STATE),
    REG_NAME_ENTRY(USBDEV_INTR_ENABLE),
    REG_NAME_ENTRY(USBDEV_INTR_TEST),
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

/*
 * Forward definitions
 */
static void ot_usbdev_server_report_connected(OtUsbdevState *s, bool connected);
static void ot_usbdev_server_complete_transfer(
    OtUsbdevState *s, uint32_t id, OtUsbdevServerTransferStatus status,
    uint32_t xfer_size, const uint8_t *data, uint32_t data_size,
    const char *msg);
static void ot_usbdev_complete_transfer_in(OtUsbdevState *s, uint8_t epnum,
                                           OtUsbdevServerTransferStatus status,
                                           const char *msg);
static void ot_usbdev_complete_transfer_out(OtUsbdevState *s, uint8_t epnum,
                                            OtUsbdevServerTransferStatus status,
                                            const char *msg);
static bool ot_usbdev_is_enabled(const OtUsbdevState *s);

/*
 * Update the INTR_STATE register for status interrupts.
 */
static void ot_usbdev_update_status_irqs(OtUsbdevState *s)
{
    uint32_t set_mask = 0u;
    if (ot_usbdev_is_enabled(s)) {
        if (s->regs[R_IN_SENT]) {
            set_mask |= USBDEV_INTR_PKT_SENT_MASK;
        }
        if (fifo8_is_empty(&s->av_setup_fifo)) {
            set_mask |= USBDEV_INTR_AV_SETUP_EMPTY_MASK;
        }
        if (fifo8_is_empty(&s->av_out_fifo)) {
            set_mask |= USBDEV_INTR_AV_OUT_EMPTY_MASK;
        }
        if (ot_fifo32_is_full(&s->rx_fifo)) {
            set_mask |= USBDEV_INTR_RX_FULL_MASK;
        }
        if (!ot_fifo32_is_empty(&s->rx_fifo)) {
            set_mask |= USBDEV_INTR_PKT_RECEIVED_MASK;
        }
    }
    s->regs[R_USBDEV_INTR_STATE] |= set_mask;
}

/*
 * Update the IRQs at the Ibex given the content of the INTR_*
 * register.
 */
static void ot_usbdev_update_irqs(OtUsbdevState *s)
{
    ot_usbdev_update_status_irqs(s);
    uint32_t state_masked =
        s->regs[R_USBDEV_INTR_STATE] & s->regs[R_USBDEV_INTR_ENABLE];

    trace_ot_usbdev_irqs(s->ot_id, s->regs[R_USBDEV_INTR_STATE],
                         s->regs[R_USBDEV_INTR_ENABLE], state_masked);

    for (unsigned irq_index = 0u; irq_index < USBDEV_INTR_NUM; irq_index++) {
        bool level = (state_masked & (1U << irq_index)) != 0u;
        ibex_irq_set(&s->irqs[irq_index], level);
    }
}

/*
 * Update the alerts at the Ibex given the content of the ALERT_TEST
 * register.
 */
static void ot_usbdev_update_alerts(OtUsbdevState *s)
{
    uint32_t level = s->regs[R_ALERT_TEST];
    s->regs[R_ALERT_TEST] = 0u;

    /* @todo add other sources of alerts here */

    if (level & R_ALERT_TEST_FATAL_FAULT_MASK) {
        ibex_irq_set(&s->alert, 1);
        ibex_irq_set(&s->alert, 0);
    }
}

/*
 * Determine whether the USBDEV is enabled (ie asserting the pullup).
 *
 * @return true if enabled (USBCTRL.ENABLE is set).
 */
bool ot_usbdev_is_enabled(const OtUsbdevState *s)
{
    /*
     * Note: this works even if the device is in reset because
     * we clear registers on reset entry.
     */
    return (bool)FIELD_EX32(s->regs[R_USBCTRL], USBCTRL, ENABLE);
}

/*
 * Determine whether VBUS is on.
 *
 * @return true if VBUS is present (USBSTAT.SENSE is set).
 */
static bool ot_usbdev_has_vbus(const OtUsbdevState *s)
{
    /*
     * Note: always returns 0 when device is in reset.
     */
    return FIELD_EX32(s->regs[R_USBSTAT], USBSTAT, SENSE);
}

/*
 * Return the current link state.
 *
 * @return value of USBSTAT.LINK_STATE.
 */
static OtUsbdevLinkState ot_usbdev_get_link_state(const OtUsbdevState *s)
{
    unsigned int v = FIELD_EX32(s->regs[R_USBSTAT], USBSTAT, LINK_STATE);
    /* The SW cannot modify the USBSTAT register but let's be paranoid */
    g_assert(v <= OT_USBDEV_LINK_STATE_COUNT);
    return (OtUsbdevLinkState)v;
}

/*
 * Return the current device address.
 *
 * @return value of USBCTRL.DEVICE_ADDRESS
 */
static uint8_t ot_usbdev_get_address(const OtUsbdevState *s)
{
    return (uint8_t)FIELD_EX32(s->regs[R_USBCTRL], USBCTRL, DEVICE_ADDRESS);
}

/*
 * Determine whether an OUT endpoint is enabled.
 *
 * @ep endpoint number
 * @return value of ep_out_enable.enable_<ep>
 */
static bool ot_usbdev_is_ep_out_enabled(const OtUsbdevState *s, uint8_t ep)
{
    g_assert(ep < USBDEV_PARAM_N_ENDPOINTS);
    return (bool)((s->regs[R_EP_OUT_ENABLE] >> ep) & 1u);
}

/*
 * Determine whether an OUT endpoint can receive a packet.
 *
 * @ep endpoint number
 * @return value of rxenable_out.out_<ep>
 */
static bool ot_usbdev_is_ep_out_rxenabled(const OtUsbdevState *s, uint8_t ep)
{
    g_assert(ep < USBDEV_PARAM_N_ENDPOINTS);
    return (bool)((s->regs[R_RXENABLE_OUT] >> ep) & 1u);
}

/*
 * Determine whether RX reception is enabled on an endpoint.
 *
 * @ep endpoint number
 * @return value of rxenable_setup.setup_<ep>
 */
static bool ot_usbdev_is_ep_setup_rx_enabled(const OtUsbdevState *s, uint8_t ep)
{
    g_assert(ep < USBDEV_PARAM_N_ENDPOINTS);
    return (bool)((s->regs[R_RXENABLE_SETUP] >> ep) & 1u);
}

/*
 * Determine whether an IN endpoint is enabled.
 *
 * @ep endpoint number
 * @return value of ep_in_enable.enable_<ep>
 */
static bool ot_usbdev_is_ep_in_enabled(const OtUsbdevState *s, uint8_t ep)
{
    g_assert(ep < USBDEV_PARAM_N_ENDPOINTS);
    return (bool)((s->regs[R_EP_IN_ENABLE] >> ep) & 1u);
}

/*
 * Determine whether an IN endpoint is stalled.
 *
 * @ep endpoint number
 * @return value of in_stall.endpoint_<ep>
 */
static bool ot_usbdev_is_ep_in_stalled(const OtUsbdevState *s, uint8_t ep)
{
    g_assert(ep < USBDEV_PARAM_N_ENDPOINTS);
    return (bool)((s->regs[R_IN_STALL] >> ep) & 1u);
}

/*
 * Determine whether an OUT endpoint is stalled.
 *
 * @ep endpoint number
 * @return value of out_stall.endpoint_<ep>
 */
static bool ot_usbdev_is_ep_out_stalled(const OtUsbdevState *s, uint8_t ep)
{
    g_assert(ep < USBDEV_PARAM_N_ENDPOINTS);
    return (bool)((s->regs[R_OUT_STALL] >> ep) & 1u);
}

/*
 * Determine whether an OUT endpoint has "auto NAK" enabled.
 *
 * @ep endpoint number
 * @return value of set_nak_out.endpoint_<ep>
 */
static bool ot_usbdev_is_set_nak_out_enabled(const OtUsbdevState *s, uint8_t ep)
{
    g_assert(ep < USBDEV_PARAM_N_ENDPOINTS);
    return (bool)((s->regs[R_SET_NAK_OUT] >> ep) & 1u);
}

/*
 * Change the link state in the USBSTAT register and trace change.
 *
 * Note: no events (IRQs, etc) are triggered, this is the caller's
 * responsibility.
 */
static void
ot_usbdev_set_raw_link_state(OtUsbdevState *s, OtUsbdevLinkState state)
{
    OtUsbdevLinkState old_state = ot_usbdev_get_link_state(s);

    if (old_state != state) {
        trace_ot_usbdev_link_state_changed(s->ot_id, LINK_STATE_NAME(state));
    }

    s->regs[R_USBSTAT] =
        FIELD_DP32(s->regs[R_USBSTAT], USBSTAT, LINK_STATE, (uint32_t)state);
}

/*
 * Write the content of a USB buffer.
 *
 * @buf_id ID of the buffer
 * @buf Buffer holding the data
 * @size How much data to copy
 */
static void ot_usbdev_write_buffer(OtUsbdevState *s, uint8_t buf_id,
                                   const uint8_t *data, size_t size)
{
    g_assert(size <= OT_USBDEV_MAX_PACKET_SIZE);
    g_assert(buf_id < OT_USBDEV_BUFFER_COUNT);

    /*
     * See BFM (write_pkt_bytes).
     *
     * The buffer is stored as words, which means that if the packet size
     * is not a multiple of 4, the RTL will zero out the bytes at the end
     * of the last word.
     *
     * This function does not fully emulate the complex behaviour w.r.t
     * the CRC being written to the buffer or data being remembered in
     * the flops (see BFM).
     */
    uint32_t *ptr =
        &s->buffer[buf_id * OT_USBDEV_MAX_PACKET_SIZE / sizeof(uint32_t)];
    while (size >= 4u) {
        *ptr++ = ldl_le_p(data);
        data += sizeof(uint32_t);
        size -= 4u;
    }
    if (size == 0u) {
        return;
    }
    /* Handle last incomplete word */
    uint32_t val32 = 0u;
    val32 |= *data++;
    if (size >= 2u) {
        val32 |= *data++ << 8u;
    }
    if (size >= 3u) {
        val32 |= *data++ << 16u;
    }
    *ptr++ = val32;
}

/*
 * Read the content of a USB buffer.
 *
 * @buf_id ID of the buffer
 * @buf Buffer where to copy the data
 * @size How much data to copy
 */
static void ot_usbdev_read_buffer(OtUsbdevState *s, uint8_t buf_id,
                                  uint8_t *buf, size_t size)
{
    g_assert(size <= OT_USBDEV_MAX_PACKET_SIZE);
    g_assert(buf_id < OT_USBDEV_BUFFER_COUNT);

    /*
     * The buffer is stored as words, we need to emulate that. Could be
     * optimized.
     */
    uint32_t *ptr =
        &s->buffer[buf_id * OT_USBDEV_MAX_PACKET_SIZE / sizeof(uint32_t)];
    while (size >= 4u) {
        stl_le_p(buf, *ptr++);
        buf += sizeof(uint32_t);
        size -= 4u;
    }
    /* Handle last incomplete word */
    if (size >= 1u) {
        uint32_t val32 = *ptr;
        *buf++ = (uint8_t)val32;
        val32 >>= 8u;
        if (size >= 2u) {
            *buf++ = (uint8_t)val32;
            val32 >>= 8u;
        }
        if (size >= 3u) {
            *buf++ = (uint8_t)val32;
        }
    }
}

/*
 * Update the content of the USBSTAT buffer to reflect the FIFO state
 * and update the corresponding IRQs.
 */
static void ot_usbdev_update_fifos_status(OtUsbdevState *s)
{
    uint32_t val = s->regs[R_USBSTAT];
    val = FIELD_DP32(val, USBSTAT, RX_EMPTY,
                     (uint32_t)ot_fifo32_is_empty(&s->rx_fifo));
    val = FIELD_DP32(val, USBSTAT, AV_SETUP_FULL,
                     (uint32_t)fifo8_is_full(&s->av_setup_fifo));
    val = FIELD_DP32(val, USBSTAT, RX_DEPTH,
                     (uint32_t)ot_fifo32_num_used(&s->rx_fifo));
    val = FIELD_DP32(val, USBSTAT, AV_OUT_FULL,
                     (uint32_t)fifo8_is_full(&s->av_out_fifo));
    val = FIELD_DP32(val, USBSTAT, AV_SETUP_DEPTH,
                     (uint32_t)fifo8_num_used(&s->av_setup_fifo));
    val = FIELD_DP32(val, USBSTAT, AV_OUT_DEPTH,
                     (uint32_t)fifo8_num_used(&s->av_out_fifo));
    s->regs[R_USBSTAT] = val;

    ot_usbdev_update_irqs(s);
}

/*
 * Update the device state after a potential VBUS change.
 *
 * This function will trigger all necessary state and IRQs changes necessary.
 * If the device becomes connected/disconnected as a result, the server will
 * be notified.
 */
static void ot_usbdev_update_vbus(OtUsbdevState *s)
{
    /* Do nothing if the device is in reset */
    if (resettable_is_in_reset(OBJECT(s))) {
        return;
    }

    /*
     * In VBUS override mode, VBUS sense is directly equal to
     * the VBUS gate. Otherwise, it is the AND between the gate
     * and the host VBUS control.
     */
    bool vbus_sense = s->vbus_gate;
    if (!s->vbus_override) {
        vbus_sense = vbus_sense && s->usb_server.vbus_connected;
    }

    bool old_vbus_sense = (bool)FIELD_EX32(s->regs[R_USBSTAT], USBSTAT, SENSE);
    if (old_vbus_sense == vbus_sense) {
        return;
    }

    trace_ot_usbdev_vbus_changed(s->ot_id, vbus_sense);

    s->regs[R_USBSTAT] =
        FIELD_DP32(s->regs[R_USBSTAT], USBSTAT, SENSE, vbus_sense);

    /* VBUS was turned on */
    if (vbus_sense) {
        /* See BFM (bus_connect) */

        /*
         * If we end up in a non-disconnected state here, something is very
         * wrong
         */
        g_assert(ot_usbdev_get_link_state(s) ==
                 OT_USBDEV_LINK_STATE_DISCONNECTED);
        s->regs[R_USBDEV_INTR_STATE] |= USBDEV_INTR_POWERED_MASK;

        if (ot_usbdev_is_enabled(s)) {
            ot_usbdev_set_raw_link_state(s, OT_USBDEV_LINK_STATE_POWERED);
            ot_usbdev_server_report_connected(s, true);
        }
    }
    /* VBUS was turned off */
    else {
        /* See BFM (bus_disconnect) */
        g_assert(ot_usbdev_get_link_state(s) !=
                 OT_USBDEV_LINK_STATE_DISCONNECTED);
        ot_usbdev_set_raw_link_state(s, OT_USBDEV_LINK_STATE_DISCONNECTED);
        s->regs[R_USBDEV_INTR_STATE] |= USBDEV_INTR_DISCONNECTED_MASK;
        s->regs[R_USBCTRL] =
            FIELD_DP32(s->regs[R_USBCTRL], USBCTRL, DEVICE_ADDRESS, 0u);
    }

    ot_usbdev_update_irqs(s);
}

/*
 * Update the VBUS gate.
 *
 * Changes will trigger events (see ot_usbdev_update_vbus).
 *
 * @gate New value of the gate.
 */
static void ot_usbdev_set_vbus_gate(OtUsbdevState *s, bool gate)
{
    s->vbus_gate = gate;

    ot_usbdev_update_vbus(s);
}

/*
 * Retire any IN packet on the given endpoint.
 *
 * Mark any ready packet as pending but not ready and not sending.
 */
static void ot_usbdev_retire_in_packets(OtUsbdevState *s, uint8_t ep)
{
    g_assert(ep < USBDEV_PARAM_N_ENDPOINTS);

    uint32_t configin = s->regs[R_CONFIGIN_0 + ep];
    if ((bool)SHARED_FIELD_EX32(configin, CONFIGIN_RDY)) {
        configin = SHARED_FIELD_DP32(configin, CONFIGIN_RDY, 0u);
        configin = SHARED_FIELD_DP32(configin, CONFIGIN_PEND, 1u);

        trace_ot_usbdev_retire_in_packet(s->ot_id, ep);
    }
    configin = SHARED_FIELD_DP32(configin, CONFIGIN_SENDING, 0u);
    s->regs[R_CONFIGIN_0 + ep] = configin;
}

/*
 * Simulate a link reset.
 *
 * This function will trigger all necessary state and IRQs changes necessary to
 * perform a link reset.
 */
static void ot_usbdev_simulate_link_reset(OtUsbdevState *s)
{
    g_assert(!resettable_is_in_reset(OBJECT(s)));

    trace_ot_usbdev_link_reset(s->ot_id);

    /* We cannot simulate a reset if the device is disconnected! */
    if (ot_usbdev_get_link_state(s) == OT_USBDEV_LINK_STATE_DISCONNECTED) {
        trace_ot_usbdev_server_protocol_error(s->ot_id,
                                              "link reset while disconnected");
        return;
    }

    /* See BFM (bus_reset) */
    s->regs[R_USBCTRL] =
        FIELD_DP32(s->regs[R_USBCTRL], USBCTRL, DEVICE_ADDRESS, 0u);
    s->regs[R_USBDEV_INTR_STATE] |= USBDEV_INTR_LINK_RESET_MASK;

    for (unsigned ep = 0; ep < USBDEV_PARAM_N_ENDPOINTS; ep++) {
        /* Cancel any pending IN packets */
        ot_usbdev_retire_in_packets(s, ep);
        /* Cancel all pending transfers on the server */
        ot_usbdev_complete_transfer_in(s, ep, OT_USBDEV_SERVER_STATUS_CANCELLED,
                                       "cancelled by link reset");
        ot_usbdev_complete_transfer_out(s, ep,
                                        OT_USBDEV_SERVER_STATUS_CANCELLED,
                                        "cancelled by link reset");
    }

    if (ot_usbdev_has_vbus(s) && ot_usbdev_is_enabled(s)) {
        /*
         * @todo BFM has some extra state processing but it seems incorrect
         * @todo If we start tracking frames, this should transition the link
         * state to ACTIVE_NOSOF and then simulate a transition to ACTIVE on
         * the next SOF.
         */
        ot_usbdev_set_raw_link_state(s, OT_USBDEV_LINK_STATE_ACTIVE);
    }
    ot_usbdev_update_irqs(s);
}

/*
 * Simulate the reception of a SETUP token followed by an 8-byte
 * DATA packet containing the SETUP packet.
 *
 * Note: when called, the trace_ot_usbdev_setup event has not been called yet.
 */
static void ot_usbdev_simulate_setup(OtUsbdevState *s, uint8_t ep,
                                     uint8_t setup[OT_USBDEV_SETUP_PACKET_SIZE])
{
    g_assert(!resettable_is_in_reset(OBJECT(s)));
    g_assert(ep < USBDEV_PARAM_N_ENDPOINTS);

    /* We cannot simulate a reset if the device is disconnected! */
    if (ot_usbdev_get_link_state(s) == OT_USBDEV_LINK_STATE_DISCONNECTED) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id, "SETUP packet while disconnected");
        trace_ot_usbdev_setup(s->ot_id, ep,
                              "ignored (device is not connected)");
        return;
    }

    /* See BFM (token_packet) */

    /* A SETUP packet resets the data toggles of enabled endpoints */
    if (ot_usbdev_is_ep_out_enabled(s, ep)) {
        s->regs[R_OUT_DATA_TOGGLE] &=
            ~FIELD_DP32(0u, OUT_DATA_TOGGLE, STATUS, 1u << ep);
    }
    if (ot_usbdev_is_ep_in_enabled(s, ep)) {
        s->regs[R_IN_DATA_TOGGLE] |=
            FIELD_DP32(0u, IN_DATA_TOGGLE, STATUS, 1u << ep);
    }

    /* Drop SETUP if EP is not enabled or RX is not enabled for SETUP. */
    if (!ot_usbdev_is_ep_out_enabled(s, ep)) {
        /* Packet is silently ignored without triggering an error. */
        trace_ot_usbdev_setup(s->ot_id, ep,
                              "dropped (device has not enabled OUT endpoint)");
        return;
    }

    /* See BFM (data_packet and out_packet) */
    if (!ot_usbdev_is_ep_setup_rx_enabled(s, ep)) {
        /* Packet is silently ignored without triggering an error. */
        trace_ot_usbdev_setup(
            s->ot_id, ep, "dropped (device has not enabled SETUP reception)");
        return;
    }
    if (fifo8_is_empty(&s->av_setup_fifo) || ot_fifo32_is_full(&s->rx_fifo)) {
        trace_ot_usbdev_setup(s->ot_id, ep,
                              "dropped (AV SETUP FIFO empty or RX FIFO full)");
        s->regs[R_USBDEV_INTR_STATE] |= USBDEV_INTR_LINK_OUT_ERR_MASK;
        ot_usbdev_update_irqs(s);
        return;
    }

    trace_ot_usbdev_setup(s->ot_id, ep, "accepted");

    /*
     * Obtain the next SETUP buffer ID and write the SETUP packet to the buffer
     */
    uint8_t buf_id = fifo8_pop(&s->av_setup_fifo);
    ot_usbdev_write_buffer(s, buf_id, setup, OT_USBDEV_SETUP_PACKET_SIZE);

    /* Create and push an entry in the RX FIFO */
    uint32_t rx_fifo_entry = 0u;
    rx_fifo_entry = FIELD_DP32(rx_fifo_entry, RXFIFO, BUFFER, buf_id);
    rx_fifo_entry = FIELD_DP32(rx_fifo_entry, RXFIFO, SETUP, 1u);
    rx_fifo_entry = FIELD_DP32(rx_fifo_entry, RXFIFO, EP, ep);
    rx_fifo_entry =
        FIELD_DP32(rx_fifo_entry, RXFIFO, SIZE, OT_USBDEV_SETUP_PACKET_SIZE);
    ot_fifo32_push(&s->rx_fifo, rx_fifo_entry);

    /* clear any STALL condition */
    s->regs[R_OUT_STALL] &= ~(1u << ep);
    s->regs[R_IN_STALL] &= ~(1u << ep);

    /* Cancel any pending IN packet on this endpoint */
    ot_usbdev_retire_in_packets(s, ep);

    /* Cancel any transfer on the server side on this endpoint */
    ot_usbdev_complete_transfer_in(s, ep, OT_USBDEV_SERVER_STATUS_CANCELLED,
                                   "cancelled by setup packet");
    ot_usbdev_complete_transfer_out(s, ep, OT_USBDEV_SERVER_STATUS_CANCELLED,
                                    "cancelled by setup packet");

    ot_usbdev_update_fifos_status(s);
    ot_usbdev_update_irqs(s);
}

/*
 * Complete an IN transfer and notify the host.
 *
 * If there is no transfer pending on this endpoint, this function does nothing.
 *
 * @epnum Endpoint number
 * @status Status of the transfer (sent to the host)
 * @msg Details on the transfer status (using for tracing only)
 */
void ot_usbdev_complete_transfer_in(OtUsbdevState *s, uint8_t epnum,
                                    OtUsbdevServerTransferStatus status,
                                    const char *msg)
{
    g_assert(epnum < USBDEV_PARAM_N_ENDPOINTS);
    OtUsbdevServer *server = &s->usb_server;
    OtUsbdevServerEpInXfer *xfer = &server->ep[epnum].in;

    /* Nothing to do if no transfer is pending */
    if (!xfer->pending) {
        return;
    }

    uint32_t xfered_len = xfer->xfer_len - xfer->recv_rem;
    ot_usbdev_server_complete_transfer(s, xfer->id, status, xfered_len,
                                       xfer->xfer_buf, xfered_len, msg);

    /* cleanup */
    g_free(xfer->xfer_buf);
    memset(xfer, 0, sizeof(OtUsbdevServerEpInXfer));
}

/*
 * Complete an OUT transfer and notify the host.
 *
 * If there is no transfer pending on this endpoint, this function does nothing.
 *
 * @epnum Endpoint number
 * @status Status of the transfer (sent to the host)
 * @msg Details on the transfer status (using for tracing only)
 */
void ot_usbdev_complete_transfer_out(OtUsbdevState *s, uint8_t epnum,
                                     OtUsbdevServerTransferStatus status,
                                     const char *msg)
{
    g_assert(epnum < USBDEV_PARAM_N_ENDPOINTS);
    OtUsbdevServer *server = &s->usb_server;
    OtUsbdevServerEpOutXfer *xfer = &server->ep[epnum].out;

    /* Nothing to do if no transfer is pending */
    if (!xfer->pending) {
        return;
    }

    uint32_t xfered_len = xfer->xfer_len - xfer->send_rem;
    ot_usbdev_server_complete_transfer(s, xfer->id, status, xfered_len, NULL,
                                       0u, msg);

    /* cleanup */
    g_free(xfer->xfer_buf);
    memset(xfer, 0, sizeof(OtUsbdevServerEpOutXfer));
}

/*
 * Try to make progress with an IN transfer on this endpoint.
 *
 * This function will check if the necessary conditions are present
 * to send a new packet to the host and trigger the necessary events.
 * If a STALL condition is detected, the transfer will automatically
 * be cancelled and the host will be notified.
 * If a NAK condition is detected (e.g. host has a pending transfer
 * but the device has not yet given a packet for transmission), this
 * function will do nothing.
 */
static void ot_usbdev_advance_transfer_in(OtUsbdevState *s, uint8_t epnum)
{
    g_assert(epnum < USBDEV_PARAM_N_ENDPOINTS);
    /* See BFM (in_packet and handshake_packet) */

    /* Nothing to do if there is no pending transfer */
    OtUsbdevServer *server = &s->usb_server;
    OtUsbdevServerEpInXfer *xfer = &server->ep[epnum].in;
    if (!xfer->pending) {
        return;
    }

    /*
     * If the endpoint is not enabled, the device will silently ignore the
     * packet, causing an error.
     */
    if (!ot_usbdev_is_ep_in_enabled(s, epnum)) {
        ot_usbdev_complete_transfer_in(s, epnum, OT_USBDEV_SERVER_STATUS_ERROR,
                                       "endpoint IN not enabled");
        return;
    }

    /* Check for STALL */
    if (ot_usbdev_is_ep_in_stalled(s, epnum)) {
        ot_usbdev_complete_transfer_in(s, epnum,
                                       OT_USBDEV_SERVER_STATUS_STALLED,
                                       "stalled by device");
        return;
    }

    hwaddr reg = R_CONFIGIN_0 + epnum;
    uint32_t configin = s->regs[reg];
    bool ready = (bool)SHARED_FIELD_EX32(configin, CONFIGIN_RDY);
    uint32_t pkt_size = SHARED_FIELD_EX32(configin, CONFIGIN_SIZE);
    uint8_t buf_id = (uint8_t)SHARED_FIELD_EX32(configin, CONFIGIN_BUFFER);

    /* Nothing to do if no packet is ready */
    if (!ready) {
        return;
    }

    /*
     * Make sure buffer ID is valid. With the current constants, this check is
     * always true but potentially could before false if the IP became more
     * parametrized.
     */
    if (buf_id >= OT_USBDEV_BUFFER_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: write to CONFIGIN%d with invalid BUFFER (%u)\n",
                      __func__, s->ot_id, epnum, buf_id);
        return;
    }

    trace_ot_usbdev_packet_sent(s->ot_id, epnum, buf_id, pkt_size);

    /* Mark the packet as sent and acknowledged by the host */
    configin = SHARED_FIELD_DP32(configin, CONFIGIN_RDY, 0u);
    s->regs[reg] = configin;
    s->regs[R_IN_DATA_TOGGLE] ^=
        FIELD_DP32(0u, IN_DATA_TOGGLE, STATUS, 1u << epnum);
    s->regs[R_IN_SENT] |= 1u << epnum;
    ot_usbdev_update_irqs(s);

    /* Check for overflow */
    if (pkt_size > xfer->max_packet_size) {
        ot_usbdev_complete_transfer_in(
            s, epnum, OT_USBDEV_SERVER_STATUS_ERROR,
            "device sent packet bigger than max packet size");
        return;
    }
    if (pkt_size > xfer->recv_rem) {
        ot_usbdev_complete_transfer_in(s, epnum, OT_USBDEV_SERVER_STATUS_ERROR,
                                       "device sent more data than expected");
        return;
    }

    /* Copy data and advance buffers */
    ot_usbdev_read_buffer(s, buf_id, xfer->recv_buf, pkt_size);

    xfer->recv_buf += pkt_size;
    xfer->recv_rem -= pkt_size;

    /* Handle completion */
    if (pkt_size < xfer->max_packet_size || xfer->recv_rem == 0u) {
        ot_usbdev_complete_transfer_in(s, epnum,
                                       OT_USBDEV_SERVER_STATUS_SUCCESS,
                                       "success");
        return;
    }
}

/*
 * Try to make progress with an OUT transfer on this endpoint.
 *
 * This function will check if the necessary conditions are present
 * to receive a new packet from the host and trigger the necessary events.
 * If a STALL condition is detected, the transfer will automatically
 * be cancelled and the host will be notified.
 * If a NAK condition is detected (e.g. the host has no transfer pending,
 * or the device has not enabled reception), this
 * function will do nothing.
 */
static void ot_usbdev_advance_transfer_out(OtUsbdevState *s, uint8_t epnum)
{
    g_assert(epnum < USBDEV_PARAM_N_ENDPOINTS);
    /* See BFM (out_packet) */

    /* Nothing to do if there is no pending transfer */
    OtUsbdevServer *server = &s->usb_server;
    OtUsbdevServerEpOutXfer *xfer = &server->ep[epnum].out;
    if (!xfer->pending) {
        return;
    }

    /*
     * If the endpoint OUT is not enabled, the device will silently ignore the
     * packet, causing an error.
     */
    if (!ot_usbdev_is_ep_out_enabled(s, epnum)) {
        ot_usbdev_complete_transfer_out(s, epnum, OT_USBDEV_SERVER_STATUS_ERROR,
                                        "endpoint OUT not enabled");
        return;
    }

    /* Check for STALL */
    if (ot_usbdev_is_ep_out_stalled(s, epnum)) {
        ot_usbdev_complete_transfer_out(s, epnum,
                                        OT_USBDEV_SERVER_STATUS_STALLED,
                                        "stalled by device");
        s->regs[R_USBDEV_INTR_STATE] |= USBDEV_INTR_LINK_OUT_ERR_MASK;
        ot_usbdev_update_irqs(s);
        return;
    }

    /*
     * Only accept the DATA packet if the endpoint is enabled, and there is
     * buffer available and there is space in the RX FIFO. The last entry of the
     * FIFO is reserved for SETUP packets.
     */
    if (!ot_usbdev_is_ep_out_rxenabled(s, epnum) ||
        fifo8_is_empty(&s->av_out_fifo) ||
        ot_fifo32_num_free(&s->rx_fifo) <= 1u) {
        /* Notify the software about the error */
        s->regs[R_USBDEV_INTR_STATE] |= USBDEV_INTR_LINK_OUT_ERR_MASK;
        ot_usbdev_update_irqs(s);
        /* "NAK" packet, OUT transfer will be retried */
        return;
    }

    /* Write data to buffer and update transfer status */
    uint8_t buf_id = fifo8_pop(&s->av_out_fifo);
    uint32_t size = MIN((uint32_t)xfer->max_packet_size, xfer->send_rem);
    ot_usbdev_write_buffer(s, buf_id, xfer->send_buf, (size_t)size);
    xfer->send_buf += size;
    xfer->send_rem -= size;

    /* Create and push an entry in the RX FIFO */
    uint32_t rx_fifo_entry = 0u;
    rx_fifo_entry = FIELD_DP32(rx_fifo_entry, RXFIFO, BUFFER, buf_id);
    rx_fifo_entry = FIELD_DP32(rx_fifo_entry, RXFIFO, SETUP, 0u);
    rx_fifo_entry = FIELD_DP32(rx_fifo_entry, RXFIFO, EP, epnum);
    rx_fifo_entry = FIELD_DP32(rx_fifo_entry, RXFIFO, SIZE, size);
    ot_fifo32_push(&s->rx_fifo, rx_fifo_entry);

    s->regs[R_OUT_DATA_TOGGLE] ^=
        FIELD_DP32(0u, IN_DATA_TOGGLE, STATUS, 1u << epnum);

    trace_ot_usbdev_packet_received(s->ot_id, epnum, buf_id, size);

    /* Disable RX if auto-NAK on OUT is set. */
    if (ot_usbdev_is_set_nak_out_enabled(s, epnum)) {
        s->regs[R_RXENABLE_OUT] &= ~(1u << epnum);
    }

    ot_usbdev_update_fifos_status(s);
    ot_usbdev_update_irqs(s);

    /* Handle completion */
    if (xfer->send_rem == 0 && !xfer->send_zlp) {
        ot_usbdev_complete_transfer_out(s, epnum,
                                        OT_USBDEV_SERVER_STATUS_SUCCESS,
                                        "success");
        return;
    }
    if (xfer->send_rem == 0 && xfer->send_zlp) {
        /*
         * If we have sent everything and a ZLP is requested, go for a last
         * round
         */
        xfer->send_zlp = false;
    }

    /*
     * @todo If the transfer is not complete and RX is still active, we need to
     * trigger one or more packet reception. However we do not want to do that
     * in a loop here because it could lead to starvation on other endpoints.
     * Need to have a timer to do some scheduling.
     */
    qemu_log_mask(LOG_UNIMP, "%s: %s: unfinished transfer on EP%u OUT\n",
                  __func__, s->ot_id, epnum);
}

/*
 * Update the value of the USBCTRL register.
 *
 * This function will update the USBCTRL register after a write and
 * trigger the necessary changes if the ENABLE bit is changed. If
 * as a result of this change the device becomes (dis)connected,
 * the server will be notified.
 *
 * @val32 New value
 */
static void ot_usbdev_write_usbctrl(OtUsbdevState *s, uint32_t val32)
{
    bool old_enable = ot_usbdev_is_enabled(s);

    /* @todo Handle resume_link_active eventually, this is a W/O field */
    s->regs[R_USBCTRL] = val32 & USBDEV_USBCTRL_R_MASK;

    bool enable = ot_usbdev_is_enabled(s);
    if (enable == old_enable) {
        return;
    }

    trace_ot_usbdev_enable_changed(s->ot_id, enable);

    /* Device has been enabled */
    if (enable) {
        /* See BFM (set_enable) */
        g_assert(ot_usbdev_get_link_state(s) ==
                 OT_USBDEV_LINK_STATE_DISCONNECTED);
        if (ot_usbdev_has_vbus(s)) {
            ot_usbdev_set_raw_link_state(s, OT_USBDEV_LINK_STATE_POWERED);
            ot_usbdev_server_report_connected(s, true);
        }
    } else {
        /* See BFM (set_enable) */
        ot_usbdev_set_raw_link_state(s, OT_USBDEV_LINK_STATE_DISCONNECTED);
        s->regs[R_USBDEV_INTR_STATE] |= USBDEV_INTR_DISCONNECTED_MASK;
        s->regs[R_USBCTRL] =
            FIELD_DP32(s->regs[R_USBCTRL], USBCTRL, DEVICE_ADDRESS, 0u);

        if (ot_usbdev_has_vbus(s)) {
            ot_usbdev_server_report_connected(s, false);
        }
    }

    /*
     * Status interrupts are only active when enabled, we may need to update
     * them
     */
    ot_usbdev_update_status_irqs(s);
    ot_usbdev_update_irqs(s);
}

/*
 * Update the value of a CONFIGINx register.
 *
 * This function will update the CONFIGINx register after a write and
 * trigger the necessary changes. If as a result of this change a transfer
 * completes, the server will be notified.
 *
 * @reg Register index
 * @val32 New value
 */
static void ot_usbdev_write_configin(OtUsbdevState *s, hwaddr reg,
                                     uint32_t val32)
{
    uint8_t ep = (uint8_t)(reg - R_CONFIGIN_0);
    g_assert(ep < USBDEV_PARAM_N_ENDPOINTS);

    /*
     * @todo clarify what writing 1 to SENDING is supposed to do since this
     * is a hardware signal and not a status bit, doc is unclear.
     * In this emulated driver, the SENDING bit is never set by the code.
     */

    /* clear RW1C fields */
    uint32_t rw1c = val32 & USBDEV_CONFIGIN_RW1C_MASK;
    s->regs[reg] &= ~rw1c;
    /* set RW fields */
    s->regs[reg] &= USBDEV_CONFIGIN_RW1C_MASK;
    s->regs[reg] |= val32 & ~USBDEV_CONFIGIN_RW1C_MASK;

    ot_usbdev_advance_transfer_in(s, ep);
}

/*
 * Update the transfers pending on one or more endpoints whose status
 * has changed.
 *
 * After changing the status of an endpoint (e.g. (un)stall, nak,
 * RX/TX enable), this function must be called to re-evaluate whether
 * the pending transfers can make progress. Only endpoints set in the
 * mask will be considered.
 *
 * @dir_in True for IN transfers, false for OUT.
 * @ep_mask The n-th bit represents the n-th endpoint.
 *
 */
static void ot_usbdev_update_ep_xfers(OtUsbdevState *s, bool dir_in,
                                      uint32_t ep_mask)
{
    /* For each disabled ep, update the transfer to trigger an error */
    for (uint8_t ep = 0u; ep < USBDEV_PARAM_N_ENDPOINTS; ep++) {
        if (((ep_mask >> ep) & 1u) == 0u) {
            continue;
        }
        if (dir_in) {
            ot_usbdev_advance_transfer_in(s, ep);
        } else {
            ot_usbdev_advance_transfer_out(s, ep);
        }
    }
}

/*
 * Handle write to register which changes whether an endpoint is
 * enabled or not. Trigger the necessary changes to transfers if any.
 *
 * This function will update the content of the register with the given value.
 *
 * @reg One of R_EP_IN_ENABLE, R_EP_OUT_ENABLE, R_RXENABLE_SETUP or
 * R_RXENABLE_OUT
 * @val32 New value of the register.
 */
static void
ot_usbdev_update_ep_enabled(OtUsbdevState *s, hwaddr reg, uint32_t val32)
{
    /* Find which endpoints have been disabled */
    uint32_t disabled_ep = s->regs[reg] & ~val32;
    s->regs[reg] = val32 & ((1u << USBDEV_PARAM_N_ENDPOINTS) - 1u);

    bool dir_in = reg == R_EP_IN_ENABLE;
    ot_usbdev_update_ep_xfers(s, dir_in, disabled_ep);
}

/*
 * Handle write to register which changes whether an endpoint is
 * stalled or not. Trigger the necessary changes to transfers if any.
 *
 * This function will update the content of the register with the given value.
 *
 * @reg One of  R_OUT_STALL or R_IN_STALL:
 * @val32 New value of the register.
 */
static void
ot_usbdev_update_ep_stalled(OtUsbdevState *s, hwaddr reg, uint32_t val32)
{
    /* Find which endpoints have been stalled */
    uint32_t stalled_ep =
        (~s->regs[reg] & val32) & ((1u << USBDEV_PARAM_N_ENDPOINTS) - 1u);
    s->regs[reg] = val32 & ((1u << USBDEV_PARAM_N_ENDPOINTS) - 1u);

    bool dir_in = reg == R_IN_STALL;
    ot_usbdev_update_ep_xfers(s, dir_in, stalled_ep);
}

/*
 * Register read/write handling
 */

static uint64_t ot_usbdev_read(void *opaque, hwaddr addr, unsigned size)
{
    OtUsbdevState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    /* Reads with no side-effects */
    case R_PHY_CONFIG:
    case R_USBSTAT:
    case R_USBDEV_INTR_STATE:
    case R_USBDEV_INTR_ENABLE:
    case R_USBCTRL:
    case R_EP_OUT_ENABLE:
    case R_EP_IN_ENABLE:
    case R_AVOUTBUFFER:
    case R_AVSETUPBUFFER:
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
    case R_WAKE_CONTROL:
    case R_WAKE_EVENTS:
    case R_FIFO_CTRL:
    case R_COUNT_OUT:
    case R_COUNT_IN:
    case R_COUNT_NODATA_IN:
    case R_COUNT_ERRORS:
        val32 = s->regs[reg];
        break;
    case R_RXFIFO:
        if (ot_fifo32_is_empty(&s->rx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: read to RXFIFO but FIFO is empty\n",
                          __func__, s->ot_id);
            val32 = 0u;
        } else {
            val32 = ot_fifo32_pop(&s->rx_fifo);
            trace_ot_usbdev_pop_rx_fifo(s->ot_id, val32);
            ot_usbdev_update_fifos_status(s);
            /* @todo trigger out transfers potentially */
        }
        break;
    case R_USBDEV_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s Read to W/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        val32 = 0u;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, s->ot_id, addr);
        val32 = 0u;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_usbdev_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                                pc);

    return (uint64_t)val32;
}

static void ot_usbdev_write(void *opaque, hwaddr addr, uint64_t val64,
                            unsigned size)
{
    OtUsbdevState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_usbdev_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                             pc);

    switch (reg) {
    case R_USBDEV_INTR_STATE:
        val32 &= USBDEV_INTR_RW1C_MASK;
        s->regs[reg] &= ~val32;
        ot_usbdev_update_irqs(s);
        break;
    case R_USBDEV_INTR_ENABLE:
        val32 &= USBDEV_INTR_MASK;
        s->regs[reg] = val32;
        ot_usbdev_update_irqs(s);
        break;
    case R_USBDEV_INTR_TEST:
        val32 &= USBDEV_INTR_MASK;
        s->regs[R_USBDEV_INTR_STATE] |= val32;
        ot_usbdev_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        /* Use the register to record alerts sets */
        s->regs[R_ALERT_TEST] = val32;
        ot_usbdev_update_alerts(s);
        break;
    case R_RXFIFO:
    case R_USBSTAT:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: write to R/O register 0x%02" HWADDR_PRIx
                      " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        break;
    case R_PHY_CONFIG:
        /* @todo mask against actual fields? */
        s->regs[R_PHY_CONFIG] = val32;
        break;
    case R_USBCTRL:
        ot_usbdev_write_usbctrl(s, val32);
        break;
    case R_EP_IN_ENABLE:
    case R_EP_OUT_ENABLE:
    case R_RXENABLE_SETUP:
    case R_RXENABLE_OUT:
        ot_usbdev_update_ep_enabled(s, reg, val32);
        break;
    case R_OUT_STALL:
    case R_IN_STALL:
        ot_usbdev_update_ep_stalled(s, reg, val32);
        break;
    case R_AVOUTBUFFER:
        if (fifo8_is_full(&s->av_out_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: write to AVOUTBUFFER but FIFO is full\n",
                          __func__, s->ot_id);
        } else {
            uint8_t buf_id = (uint8_t)FIELD_EX32(val32, AVOUTBUFFER, BUFFER);
            fifo8_push(&s->av_out_fifo, buf_id);
            trace_ot_usbdev_push_av_out_buffer(s->ot_id, buf_id);
            ot_usbdev_update_fifos_status(s);
            /* @todo trigger out transfers potentially */
        }
        break;
    case R_AVSETUPBUFFER:
        if (fifo8_is_full(&s->av_setup_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: write to AVSETUPBUFFER but FIFO is full\n",
                          __func__, s->ot_id);
        } else {
            uint8_t buf_id = (uint8_t)FIELD_EX32(val32, AVSETUPBUFFER, BUFFER);
            fifo8_push(&s->av_setup_fifo, buf_id);
            trace_ot_usbdev_push_av_setup_buffer(s->ot_id, buf_id);
            ot_usbdev_update_fifos_status(s);
        }
        break;
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
        ot_usbdev_write_configin(s, reg, val32);
        break;
    case R_IN_SENT:
        /* register is rw1c */
        s->regs[reg] &= ~val32;
        break;
    case R_SET_NAK_OUT:
        s->regs[reg] = val32;
        break;
    case R_OUT_ISO:
    case R_IN_ISO:
    case R_OUT_DATA_TOGGLE:
    case R_IN_DATA_TOGGLE:
    case R_PHY_PINS_SENSE:
    case R_PHY_PINS_DRIVE:
    case R_WAKE_CONTROL:
    case R_WAKE_EVENTS:
    case R_FIFO_CTRL:
    case R_COUNT_OUT:
    case R_COUNT_IN:
    case R_COUNT_NODATA_IN:
    case R_COUNT_ERRORS:
        s->regs[reg] = val32;
        qemu_log_mask(LOG_UNIMP, "%s: %s: %s is not supported\n", __func__,
                      s->ot_id, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, s->ot_id, addr);
        break;
    }
}

static uint64_t ot_usbdev_buffer_read(void *opaque, hwaddr addr, unsigned size)
{
    OtUsbdevState *s = opaque;
    (void)size;

    hwaddr word = R32_OFF(addr);
    uint32_t val32 = s->buffer[word];

    uint32_t pc = ibex_get_current_pc();
    trace_ot_usbdev_io_buffer_read_out(s->ot_id, (uint32_t)addr, val32, pc);

    return (uint64_t)val32;
}

static void ot_usbdev_buffer_write(void *opaque, hwaddr addr, uint64_t val64,
                                   unsigned size)
{
    OtUsbdevState *s = opaque;
    (void)size;
    uint32_t val32 = val64;

    hwaddr word = R32_OFF(addr);
    s->buffer[word] = val32;

    uint32_t pc = ibex_get_current_pc();
    trace_ot_usbdev_io_buffer_write(s->ot_id, (uint32_t)addr, val32, pc);
}

/*
 * USB server
 */
static void ot_usbdev_server_reset_recv_state(OtUsbdevState *s)
{
    OtUsbdevServer *server = &s->usb_server;

    /* cleanup previous state */
    g_free(server->recv_data);
    server->recv_data = NULL;

    server->recv_state = OT_USBDEV_SERVER_RECV_WAIT_HEADER;
    server->recv_rem = sizeof(server->recv_pkt);
    server->recv_buf = (uint8_t *)&server->recv_pkt;
}

/*
 * Open a session with the server.
 *
 * This function will reset the server state to start a new session.
 */
static void ot_usbdev_server_open(OtUsbdevState *s)
{
    OtUsbdevServer *server = &s->usb_server;
    server->client_connected = true;
    server->vbus_connected = false;
    server->hello_done = false;
    ot_usbdev_server_reset_recv_state(s);

    ot_usbdev_update_vbus(s);
}

/*
 * Close a session with the server.
 *
 * In particular, if VBUS is still on, it will be turned off by this call,
 * which can trigger events on the device side.
 */
static void ot_usbdev_server_close(OtUsbdevState *s)
{
    OtUsbdevServer *server = &s->usb_server;
    server->client_connected = false;
    server->vbus_connected = false;

    ot_usbdev_update_vbus(s);
}

/*
 * Send a message to the client.
 *
 * To avoid an allocation, this function supports sending the payload
 * data in two segments, represented by two buffers (data0 and data1).
 * Any buffer with size zero will be ignored.
 *
 * @todo This function must be non-blocking, clarify if that's really
 * the case.
 *
 * @s Device state
 * @cmd Command
 * @id Packet ID
 * @data0 First part of the payload data (can be NULL if size0 is set)
 * @data0 Size of the first part of the payload data
 * @data1 Second part of the payload data (can be NULL if size1 is set)
 * @data1 Size of the second part of the payload data
 */
static void ot_usbdev_server_write_packet(
    OtUsbdevState *s, OtUsbdevServerCmd cmd, uint32_t id, const uint8_t *data0,
    size_t size0, const uint8_t *data1, size_t size1)
{
    /*
     * @todo Update this to handle partial writes, by adding them to a queue and
     * using a watch callback
     */
    const OtUsbdevServerPktHdr hdr = {
        .cmd = cmd,
        .size = size0 + size1,
        .id = id,
    };
    int res =
        qemu_chr_fe_write(&s->usb_chr, (const uint8_t *)&hdr, sizeof(hdr));
    if (res < sizeof(hdr)) {
        qemu_log_mask(LOG_UNIMP, "%s: %s server: unhandled partial write\n",
                      __func__, s->ot_id);
        return;
    }
    if (size0 > 0u) {
        res = qemu_chr_fe_write(&s->usb_chr, data0, (int)size0);
        if (res < size0) {
            qemu_log_mask(LOG_UNIMP, "%s: %s server: unhandled partial write\n",
                          __func__, s->ot_id);
            return;
        }
    }
    if (size1 > 0u) {
        res = qemu_chr_fe_write(&s->usb_chr, data1, (int)size1);
        if (res < size1) {
            qemu_log_mask(LOG_UNIMP, "%s: %s server: unhandled partial write\n",
                          __func__, s->ot_id);
            return;
        }
    }
}

/*
 * Report a (dis)connection event to the client.
 *
 * Send a message to the client to notify the connection status change.
 * Note that this function does not keep track of the current status and will
 * send a message if the new status is the same as the old one.
 *
 * @connected New connection status
 */
void ot_usbdev_server_report_connected(OtUsbdevState *s, bool connected)
{
    trace_ot_usbdev_server_report_connected(s->ot_id, connected);

    /* Nothing to do if no client is connected */
    if (!s->usb_server.client_connected) {
        return;
    }

    /* @todo clarify ID to use */
    ot_usbdev_server_write_packet(s,
                                  connected ? OT_USBDEV_SERVER_CMD_CONNECT :
                                              OT_USBDEV_SERVER_CMD_DISCONNECT,
                                  0u, NULL, 0u, NULL, 0u);
}

/*
 * Process a HELLO packet from the client and send one back.
 *
 * After this function, the server will be ready to receive messages from
 * the client, unless the packet was invalid in which case the server will
 * keep expecting a valid HELLO packet.
 */
static void ot_usbdev_server_process_hello(OtUsbdevState *s)
{
    OtUsbdevServer *server = &s->usb_server;

    if (server->hello_done) {
        trace_ot_usbdev_server_protocol_error(s->ot_id, "unexpected HELLO");
        /* Not a fatal error */
    }
    if (server->recv_pkt.size != sizeof(OtUsbdevServerHelloPkt)) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id,
            "HELLO packet with unexpected payload size, ignoring packet");
        return;
    }
    /*
     * @todo maybe recover packet ID and drop any packet with ID smaller than
     * the last hello?
     */
    OtUsbdevServerHelloPkt hello;
    memcpy(&hello, server->recv_data, sizeof(OtUsbdevServerHelloPkt));
    if (memcmp(hello.magic, OT_USBDEV_SERVER_HELLO_MAGIC,
               sizeof(hello.magic)) != 0) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id,
            "HELLO packet with unexpected magic value, ignoring packet");
        return;
    }
    if (hello.major_version != OT_USBDEV_SERVER_MAJOR_VER ||
        hello.minor_version != OT_USBDEV_SERVER_MINOR_VER) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id, "HELLO packet with unexpected version, ignoring packet");
        return;
    }
    server->hello_done = true;

    trace_ot_usbdev_server_hello(s->ot_id, hello.major_version,
                                 hello.minor_version);

    /* Answer back */
    OtUsbdevServerHelloPkt resp_hello;
    memcpy(&resp_hello.magic, OT_USBDEV_SERVER_HELLO_MAGIC,
           sizeof(resp_hello.magic));
    resp_hello.major_version = OT_USBDEV_SERVER_MAJOR_VER;
    resp_hello.minor_version = OT_USBDEV_SERVER_MINOR_VER;
    ot_usbdev_server_write_packet(s, OT_USBDEV_SERVER_CMD_HELLO,
                                  server->recv_pkt.id,
                                  (const void *)&resp_hello, sizeof(resp_hello),
                                  NULL, 0u);
}

static void ot_usbdev_server_process_reset(OtUsbdevState *s)
{
    OtUsbdevServer *server = &s->usb_server;

    if (server->recv_pkt.size != 0u) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id, "RESET packet with unexpected payload, ignoring packet");
        return;
    }
    /* Ignore if device is not enabled */
    if (resettable_is_in_reset(OBJECT(s))) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id, "RESET when device is in reset, ignoring packet");
        return;
    }

    ot_usbdev_simulate_link_reset(s);
}

static void ot_usbdev_server_process_vbus_changed(OtUsbdevState *s, bool vbus)
{
    OtUsbdevServer *server = &s->usb_server;

    if (server->recv_pkt.size != 0u) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id,
            "VBUS_ON/OFF packet with unexpected payload, ignoring packet");
        return;
    }

    server->vbus_connected = vbus;
    ot_usbdev_update_vbus(s);
}

static void ot_usbdev_server_process_setup(OtUsbdevState *s)
{
    OtUsbdevServer *server = &s->usb_server;

    if (server->recv_pkt.size != sizeof(OtUsbdevServerSetupPkt)) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id,
            "SETUP packet with unexpected payload size, ignoring packet");
        return;
    }
    /* Ignore if device is not enabled */
    if (resettable_is_in_reset(OBJECT(s))) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id, "SETUP when device is in reset, ignoring packet");
        return;
    }

    OtUsbdevServerSetupPkt setup;
    memcpy(&setup, server->recv_data, sizeof(OtUsbdevServerSetupPkt));

    /* @todo enforce that reserved is 0 */

    /* Ignore SETUP if address or endpoint is not ours */
    if (setup.address != ot_usbdev_get_address(s) ||
        setup.endpoint >= USBDEV_PARAM_N_ENDPOINTS) {
        trace_ot_usbdev_setup_addr(
            s->ot_id, setup.address, setup.endpoint,
            "ignore (mismatching address or invalid endpoint)");
        return;
    }

    ot_usbdev_simulate_setup(s, setup.endpoint, setup.setup);
}

void ot_usbdev_server_complete_transfer(OtUsbdevState *s, uint32_t id,
                                        OtUsbdevServerTransferStatus status,
                                        uint32_t xfer_size, const uint8_t *data,
                                        uint32_t data_size, const char *msg)
{
    trace_ot_usbdev_complete_transfer(s->ot_id, id, xfer_size,
                                      XFER_STATUS_NAME(status), msg);

    OtUsbdevServerCompletePkt pkt;
    memset(&pkt, 0u, sizeof(pkt));
    pkt.status = (uint8_t)status;
    pkt.transfer_size = xfer_size;
    ot_usbdev_server_write_packet(s, OT_USBDEV_SERVER_CMD_COMPLETE, id,
                                  (const uint8_t *)&pkt, sizeof(pkt), data,
                                  data_size);
}

static void ot_usbdev_server_process_transfer_in(
    OtUsbdevState *s, uint8_t ep, uint8_t flags, uint16_t max_packet_size,
    uint32_t transfer_size)
{
    OtUsbdevServer *server = &s->usb_server;
    OtUsbdevServerEpInXfer *xfer = &server->ep[ep].in;

    /* It is an error to send a transfer if one is pending */
    if (xfer->pending) {
        trace_ot_usbdev_transfer(s->ot_id, server->recv_pkt.id, ep, "IN", flags,
                                 max_packet_size, transfer_size,
                                 "error (transfer already pending)");
        ot_usbdev_server_complete_transfer(s, server->recv_pkt.id,
                                           OT_USBDEV_SERVER_STATUS_ERROR, 0u,
                                           NULL, 0u,
                                           "transfer already pending");
    }

    /* @todo check for unknown flags */

    trace_ot_usbdev_transfer(s->ot_id, server->recv_pkt.id, ep, "IN", flags,
                             max_packet_size, transfer_size, "accepted");

    /* Sanity check */
    g_assert(xfer->xfer_buf == NULL);

    /* record transfer */
    xfer->id = server->recv_pkt.id;
    xfer->xfer_len = transfer_size;
    /* @todo do some basic checks on reasonable length? */
    xfer->xfer_buf = g_malloc(transfer_size);
    xfer->max_packet_size = max_packet_size;
    xfer->recv_buf = xfer->xfer_buf;
    xfer->recv_rem = transfer_size;
    xfer->pending = true;

    ot_usbdev_advance_transfer_in(s, ep);
}

static void ot_usbdev_server_process_transfer_out(
    OtUsbdevState *s, uint8_t ep, uint8_t flags, uint16_t max_packet_size,
    uint32_t transfer_size, const uint8_t *data)
{
    OtUsbdevServer *server = &s->usb_server;
    OtUsbdevServerEpOutXfer *xfer = &server->ep[ep].out;

    /* It is an error to send a transfer if one is pending */
    if (xfer->pending) {
        trace_ot_usbdev_transfer(s->ot_id, server->recv_pkt.id, ep, "OUT",
                                 flags, max_packet_size, transfer_size,
                                 "error (transfer already pending)");
        ot_usbdev_server_complete_transfer(s, server->recv_pkt.id,
                                           OT_USBDEV_SERVER_STATUS_ERROR, 0u,
                                           NULL, 0u,
                                           "transfer already pending");
    }

    /* @todo check for unknown flags */
    bool zlp = (flags & OT_USBDEV_SERVER_FLAG_ZLP) != 0u;

    /* @todo check max packet size is smaller than buffer size! */

    trace_ot_usbdev_transfer(s->ot_id, server->recv_pkt.id, ep, "OUT", flags,
                             max_packet_size, transfer_size, "accepted");

    /* sanity check */
    g_assert(xfer->xfer_buf == NULL);

    /* record transfer */
    xfer->id = server->recv_pkt.id;
    /* @todo do some basic checks on reasonable length? */
    xfer->xfer_buf = g_malloc(transfer_size);
    memcpy(xfer->xfer_buf, data, transfer_size);
    xfer->xfer_len = transfer_size;
    xfer->max_packet_size = max_packet_size;
    xfer->send_buf = xfer->xfer_buf;
    xfer->send_rem = transfer_size;
    /*
     * Due to the logic in ot_usbdev_advance_transfer_out, the ZLP flag
     * must only be set for non-zero length transfers, otherwise we will
     * send two ZLPs.
     */
    xfer->send_zlp = zlp && transfer_size != 0u;
    xfer->pending = true;

    ot_usbdev_advance_transfer_out(s, ep);
}

static void ot_usbdev_server_process_transfer(OtUsbdevState *s)
{
    OtUsbdevServer *server = &s->usb_server;

    if (server->recv_pkt.size < sizeof(OtUsbdevServerTransferPkt)) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id, "TRANSFER packet with unexpectedly small payload size, "
                      "ignoring packet");
        return;
    }

    OtUsbdevServerTransferPkt xfer;
    memcpy(&xfer, server->recv_data, sizeof(OtUsbdevServerTransferPkt));

    uint8_t epnum = xfer.endpoint & OT_USBDEV_EP_NUM_MASK;
    bool dir_in = (xfer.endpoint & OT_USBDEV_EP_DIR_IN) == OT_USBDEV_EP_DIR_IN;

    /* Ignore if device is not enabled */
    if (resettable_is_in_reset(OBJECT(s))) {
        trace_ot_usbdev_transfer(s->ot_id, server->recv_pkt.id, epnum,
                                 dir_in ? "IN" : "OUT", xfer.flags,
                                 xfer.packet_size, xfer.transfer_size,
                                 "ignored (device is in reset)");
        ot_usbdev_server_complete_transfer(s, server->recv_pkt.id,
                                           OT_USBDEV_SERVER_STATUS_ERROR, 0u,
                                           NULL, 0u, "device is in reset");
        return;
    }

    if (epnum >= USBDEV_PARAM_N_ENDPOINTS) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id, "TRANSFER to non-existent endpoint, ignoring packet");
        return;
    }

    uint32_t expected_data_len = dir_in ? 0u : xfer.transfer_size;
    if (server->recv_pkt.size !=
        sizeof(OtUsbdevServerTransferPkt) + expected_data_len) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id,
            "TRANSFER packet with unexpected payload size, ignoring packet");
        return;
    }

    if (dir_in) {
        ot_usbdev_server_process_transfer_in(s, epnum, xfer.flags,
                                             xfer.packet_size,
                                             xfer.transfer_size);
    } else {
        const uint8_t *data =
            server->recv_data + sizeof(OtUsbdevServerTransferPkt);
        ot_usbdev_server_process_transfer_out(s, epnum, xfer.flags,
                                              xfer.packet_size,
                                              xfer.transfer_size, data);
    }
}

static void ot_usbdev_server_process_packet(OtUsbdevState *s)
{
    OtUsbdevServer *server = &s->usb_server;

    /*
     * Important note: the ->recv_data pointer is freed in
     * ot_usbdev_server_reset_recv_state() if left non-NULL. The code below must
     * either make a copy of the data or take ownership of the pointer and set
     * ->recv_data to NULL.
     */

    /* If HELLO hasn't been received yet, ignore everything else */
    if (server->recv_pkt.cmd != OT_USBDEV_SERVER_CMD_HELLO &&
        !server->hello_done) {
        trace_ot_usbdev_server_protocol_error(
            s->ot_id, "unexpected command before HELLO, ignoring packet");
        /* Do not process packet */
        return;
    }

    switch (server->recv_pkt.cmd) {
    case OT_USBDEV_SERVER_CMD_HELLO:
        ot_usbdev_server_process_hello(s);
        break;
    case OT_USBDEV_SERVER_CMD_RESET:
        ot_usbdev_server_process_reset(s);
        break;
    case OT_USBDEV_SERVER_CMD_VBUS_ON:
        ot_usbdev_server_process_vbus_changed(s, true);
        break;
    case OT_USBDEV_SERVER_CMD_VBUS_OFF:
        ot_usbdev_server_process_vbus_changed(s, false);
        break;
    case OT_USBDEV_SERVER_CMD_SETUP:
        ot_usbdev_server_process_setup(s);
        break;
    case OT_USBDEV_SERVER_CMD_TRANSFER:
        ot_usbdev_server_process_transfer(s);
        break;
    default:
        trace_ot_usbdev_server_protocol_error(
            s->ot_id, "unknown command, ignoring packet");
        break;
    }

    ot_usbdev_server_reset_recv_state(s);
}

static void ot_usbdev_server_advance_recv_state(OtUsbdevState *s)
{
    OtUsbdevServer *server = &s->usb_server;

    if (server->recv_state == OT_USBDEV_SERVER_RECV_WAIT_HEADER) {
        /* We received a header, now receive data if necessary */
        if (server->recv_pkt.size > 0u) {
            /* sanity check */
            g_assert(server->recv_data == NULL);
            /* @todo have some bound on allocation size here? */
            server->recv_data = g_malloc(server->recv_pkt.size);
            server->recv_rem = server->recv_pkt.size;
            server->recv_buf = (uint8_t *)server->recv_data;
            server->recv_state = OT_USBDEV_SERVER_RECV_WAIT_DATA;
        } else {
            /* Process packet */
            ot_usbdev_server_process_packet(s);
        }
    } else if (server->recv_state == OT_USBDEV_SERVER_RECV_WAIT_DATA) {
        /* Process packet */
        ot_usbdev_server_process_packet(s);
    } else {
        g_assert_not_reached();
    }
}

static int ot_usbdev_chr_usb_can_receive(void *opaque)
{
    OtUsbdevState *s = opaque;

    /* Return remaining size in the buffer. */
    return (int)s->usb_server.recv_rem;
}

static void ot_usbdev_chr_usb_receive(void *opaque, const uint8_t *buf,
                                      int size)
{
    OtUsbdevState *s = opaque;
    OtUsbdevServer *server = &s->usb_server;

    if (size > server->recv_rem) {
        error_report("%s: %s: Received too much data on the usb chardev",
                     __func__, s->ot_id);
        return;
    }
    /* Copy data at the end of the buffer. */
    memcpy(server->recv_buf, buf, (size_t)size);
    server->recv_buf += size;
    server->recv_rem -= size;

    /* Advance state if done */
    if (server->recv_rem == 0u) {
        ot_usbdev_server_advance_recv_state(s);
    }
}

static void ot_usbdev_chr_usb_event_handler(void *opaque, QEMUChrEvent event)
{
    OtUsbdevState *s = opaque;

    trace_ot_usbdev_chr_usb_event_handler(s->ot_id, event);

    if (event == CHR_EVENT_OPENED) {
        ot_usbdev_server_open(s);
    }

    if (event == CHR_EVENT_CLOSED) {
        ot_usbdev_server_close(s);
    }
}

/*
 * Communication device handling
 */

static int ot_usbdev_chr_cmd_can_receive(void *opaque)
{
    OtUsbdevState *s = opaque;

    /* Return remaining size in the buffer. */
    return (int)sizeof(s->cmd_buf) - (int)s->cmd_buf_pos;
}

/*
 * Process a nul-terminated command. A size (which does not count NUL) is
 * provided if necessary. The following commands are supported:
 * - "vbus_on": set the VBUS sensing signal to 1.
 * - "vbus_off": set the VBUS sensing signal to 0.
 */
static void ot_usbdev_chr_process_cmd(OtUsbdevState *s, char *cmd,
                                      size_t cmd_size)
{
    trace_ot_usbdev_chr_process_cmd(s->ot_id, cmd);

    if (strncmp(cmd, "vbus_on", cmd_size) == 0) {
        ot_usbdev_set_vbus_gate(s, true);
    } else if (strncmp(cmd, "vbus_off", cmd_size) == 0) {
        ot_usbdev_set_vbus_gate(s, false);
    } else {
        qemu_log_mask(LOG_UNIMP, "%s: %s: unsupported command %s\n", __func__,
                      s->ot_id, cmd);
    }
}

static void ot_usbdev_chr_cmd_receive(void *opaque, const uint8_t *buf,
                                      int size)
{
    OtUsbdevState *s = opaque;

    if (s->cmd_buf_pos + (unsigned)size > sizeof(s->cmd_buf)) {
        error_report("%s: %s: Received too much data on the cmd chardev",
                     __func__, s->ot_id);
        return;
    }
    /* Copy data at the end of the buffer. */
    memcpy(&s->cmd_buf[s->cmd_buf_pos], buf, (size_t)size);
    s->cmd_buf_pos += (unsigned)size;
    /* Process any line. */
    while (true) {
        /* Search end of line, stop if none is found. */
        char *eol = memchr(s->cmd_buf, (int)'\n', s->cmd_buf_pos);
        if (!eol) {
            break;
        }
        *eol = 0;
        /*
         * Process command and then remove it from the buffer
         * by shifting everything left.
         */
        size_t cmd_size = eol - s->cmd_buf;
        ot_usbdev_chr_process_cmd(s, s->cmd_buf, cmd_size);
        memmove(s->cmd_buf, eol + 1u, s->cmd_buf_pos - cmd_size - 1u);
        s->cmd_buf_pos -= cmd_size - 1u;
    }
}

/*
 * QEMU Initialization
 */

static const MemoryRegionOps ot_usbdev_ops = {
    .read = &ot_usbdev_read,
    .write = &ot_usbdev_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = sizeof(uint32_t),
    .impl.max_access_size = sizeof(uint32_t),
};

static const MemoryRegionOps ot_usbdev_buffer_ops = {
    .read = &ot_usbdev_buffer_read,
    .write = &ot_usbdev_buffer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    /* @todo The RTL probably supports sub-word reads, implement this */
    .impl.min_access_size = sizeof(uint32_t),
    .impl.max_access_size = sizeof(uint32_t),
};

static Property ot_usbdev_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtUsbdevState, ot_id),
    DEFINE_PROP_STRING("clock-name", OtUsbdevState, usbclk_name),
    DEFINE_PROP_STRING("clock-name-aon", OtUsbdevState, aonclk_name),
    DEFINE_PROP_LINK("clock-src", OtUsbdevState, clock_src, TYPE_DEVICE,
                     DeviceState *),
    /*
     * Communication device used to control the USBDEV block. Every command
     * must be terminated by a newline. See ot_usbdev_chr_process_cmd for
     * the list of supported devices.
     */
    DEFINE_PROP_CHR("chardev-cmd", OtUsbdevState, cmd_chr),
    /*
     * Communication device used to emulate a USB host.
     */
    DEFINE_PROP_CHR("chardev-usb", OtUsbdevState, usb_chr),
    /* VBUS control mode. */
    DEFINE_PROP_BOOL("vbus-override", OtUsbdevState, vbus_override, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_usbdev_realize(DeviceState *dev, Error **errp)
{
    OtUsbdevState *s = OT_USBDEV(dev);
    (void)errp;

    /* @todo: clarify if we need to track events/backend changes. */
    qemu_chr_fe_set_handlers(&s->cmd_chr, &ot_usbdev_chr_cmd_can_receive,
                             &ot_usbdev_chr_cmd_receive, NULL, NULL, s, NULL,
                             true);

    qemu_chr_fe_set_handlers(&s->usb_chr, &ot_usbdev_chr_usb_can_receive,
                             &ot_usbdev_chr_usb_receive,
                             &ot_usbdev_chr_usb_event_handler, NULL, s, NULL,
                             true);

    g_assert(s->ot_id);
    g_assert(s->usbclk_name);
    g_assert(s->aonclk_name);

    /* If not in VBUS override mode, the VBUS gate starts on by default. */
    if (!s->vbus_override) {
        s->vbus_gate = true;
    }
}

/*
 * QEMU reset handling
 */

static void ot_usbdev_reset_enter(Object *obj, ResetType type)
{
    OtUsbdevClass *c = OT_USBDEV_GET_CLASS(obj);
    OtUsbdevState *s = OT_USBDEV(obj);

    trace_ot_usbdev_reset(s->ot_id, "enter");

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    /* @todo cancel everything here. */

    /* See BFM (dut_reset) */
    memset(s->regs, 0u, sizeof(s->regs));

    ot_fifo32_reset(&s->rx_fifo);
    fifo8_reset(&s->av_setup_fifo);
    fifo8_reset(&s->av_out_fifo);

    /*
     * @todo The BFM says that 'Disconnected' is held at 1 during IP
     * reset, need to investigate this detail.
     */
    ot_usbdev_update_irqs(s);
}

static void ot_usbdev_reset_exit(Object *obj, ResetType type)
{
    OtUsbdevClass *c = OT_USBDEV_GET_CLASS(obj);
    OtUsbdevState *s = OT_USBDEV(obj);

    trace_ot_usbdev_reset(s->ot_id, "exit");

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    ot_usbdev_update_vbus(s);
    ot_usbdev_update_fifos_status(s);
}

static void ot_usbdev_init(Object *obj)
{
    OtUsbdevState *s = OT_USBDEV(obj);

    for (unsigned idx = 0u; idx < ARRAY_SIZE(s->irqs); idx++) {
        ibex_sysbus_init_irq(obj, &s->irqs[idx]);
    }
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);

    memory_region_init(&s->mmio.main, obj, TYPE_OT_USBDEV ".mmio",
                       USBDEV_DEVICE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio.main);
    memory_region_init_io(&s->mmio.regs, obj, &ot_usbdev_ops, s,
                          TYPE_OT_USBDEV ".reg", USBDEV_REGS_SIZE);
    memory_region_add_subregion(&s->mmio.main, USBDEV_REGS_OFFSET,
                                &s->mmio.regs);
    memory_region_init_io(&s->mmio.buffer, obj, &ot_usbdev_buffer_ops, s,
                          TYPE_OT_USBDEV ".buffer", USBDEV_BUFFER_SIZE);
    memory_region_add_subregion(&s->mmio.main, USBDEV_BUFFER_OFFSET,
                                &s->mmio.buffer);

    ot_fifo32_create(&s->rx_fifo, OT_USBDEV_RX_FIFO_DEPTH);
    fifo8_create(&s->av_out_fifo, OT_USBDEV_AV_OUT_FIFO_DEPTH);
    fifo8_create(&s->av_setup_fifo, OT_USBDEV_AV_SETUP_FIFO_DEPTH);
}

static void ot_usbdev_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_usbdev_realize;
    device_class_set_props(dc, ot_usbdev_properties);
    /* @todo: check what does DEVICE_CATEGORY_USB entail? */
    set_bit(DEVICE_CATEGORY_USB, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtUsbdevClass *uc = OT_USBDEV_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_usbdev_reset_enter, NULL,
                                       &ot_usbdev_reset_exit,
                                       &uc->parent_phases);
}

static const TypeInfo ot_usbdev_info = {
    .name = TYPE_OT_USBDEV,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtUsbdevState),
    .instance_init = &ot_usbdev_init,
    .class_size = sizeof(OtUsbdevClass),
    .class_init = &ot_usbdev_class_init,
};

static void ot_usbdev_register_types(void)
{
    type_register_static(&ot_usbdev_info);
}

type_init(ot_usbdev_register_types);
