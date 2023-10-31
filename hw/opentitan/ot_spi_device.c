/*
 * QEMU OpenTitan SPI Device controller
 *
 * Copyright (c) 2023 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
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
#include "qemu/bswap.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/opentitan/ot_spi_device.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

#define PARAM_SRAM_DEPTH         1024u
#define PARAM_SRAM_OFFSET        4096u
#define PARAM_SRAM_EGRESS_DEPTH  832u
#define PARAM_SRAM_INGRESS_DEPTH 104u
#define PARAM_NUM_CMD_INFO       24u
#define PARAM_NUM_LOCALITY       5u
#define PARAM_TPM_WR_FIFO_PTR_W  7u
#define PARAM_TPM_RD_FIFO_PTR_W  5u
#define PARAM_TPM_RD_FIFO_WIDTH  32u
#define PARAM_NUM_IRQS           12u
#define PARAM_NUM_ALERTS         1u
#define PARAM_REG_WIDTH          32u

/* SPI device registers */
REG32(INTR_STATE, 0x0u)
SHARED_FIELD(INTR_GENERIC_RX_FULL, 0u, 1u)
SHARED_FIELD(INTR_GENERIC_RX_WATERMARK, 1u, 1u)
SHARED_FIELD(INTR_GENERIC_TX_WATERMARK, 2u, 1u)
SHARED_FIELD(INTR_GENERIC_RX_ERROR, 3u, 1u)
SHARED_FIELD(INTR_GENERIC_RX_OVERFLOW, 4u, 1u)
SHARED_FIELD(INTR_GENERIC_TX_UNDERFLOW, 5u, 1u)
SHARED_FIELD(INTR_UPLOAD_CMDFIFO_NOT_EMPTY, 6u, 1u)
SHARED_FIELD(INTR_UPLOAD_PAYLOAD_NOT_EMPTY, 7u, 1u)
SHARED_FIELD(INTR_UPLOAD_PAYLOAD_OVERFLOW, 8u, 1u)
SHARED_FIELD(INTR_READBUF_WATERMARK, 9u, 1u)
SHARED_FIELD(INTR_READBUF_FLIP, 10u, 1u)
SHARED_FIELD(INTR_TPM_HEADER_NOT_EMPTY, 11u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(CONTROL, 0x10u)
FIELD(CONTROL, ABORT, 0u, 1u)
FIELD(CONTROL, MODE, 4u, 2u)
FIELD(CONTROL, RST_TXFIFO, 16u, 1u)
FIELD(CONTROL, RST_RXFIFO, 17u, 1u)
FIELD(CONTROL, SRAM_CLK_EN, 31u, 1u)
REG32(CFG, 0x14u)
FIELD(CFG, CPOL, 0u, 1u)
FIELD(CFG, CPHA, 1u, 1u)
FIELD(CFG, TX_ORDER, 2u, 1u)
FIELD(CFG, RX_ORDER, 3u, 1u)
FIELD(CFG, TIMER_V, 8u, 8u)
FIELD(CFG, ADDR_4B_EN, 16u, 1u)
FIELD(CFG, MAILBOX_EN, 24u, 1u)
REG32(FIFO_LEVEL, 0x18u)
FIELD(FIFO_LEVEL, RXLVL, 0u, 16u)
FIELD(FIFO_LEVEL, TXLVL, 16u, 16u)
REG32(ASYNC_FIFO_LEVEL, 0x1cu)
FIELD(ASYNC_FIFO_LEVEL, RXLVL, 0u, 8u)
FIELD(ASYNC_FIFO_LEVEL, TXLVL, 16, 8u)
REG32(STATUS, 0x20u)
FIELD(STATUS, RXF_FULL, 0u, 1u)
FIELD(STATUS, RXF_EMPTY, 1u, 1u)
FIELD(STATUS, TXF_FULL, 2u, 1u)
FIELD(STATUS, TXF_EMPTY, 3u, 1u)
FIELD(STATUS, ABORT_DONE, 4u, 1u)
FIELD(STATUS, CSB, 5u, 1u)
FIELD(STATUS, TPM_CSB, 6u, 1u)
REG32(RXF_PTR, 0x24u)
FIELD(RXF_PTR, RPTR, 0u, 16u)
FIELD(RXF_PTR, WPTR, 16u, 16u)
REG32(TXF_PTR, 0x28u)
FIELD(TXF_PTR, RPTR, 0u, 16u)
FIELD(TXF_PTR, WPTR, 16u, 16u)
REG32(RXF_ADDR, 0x2cu)
FIELD(RXF_ADDR, BASE, 0u, 16u)
FIELD(RXF_ADDR, LIMIT, 16u, 16u)
REG32(TXF_ADDR, 0x30u)
FIELD(TXF_ADDR, BASE, 0u, 16u)
FIELD(TXF_ADDR, LIMIT, 16u, 16u)
REG32(INTERCEPT_EN, 0x34u)
FIELD(INTERCEPT_EN, STATUS, 0u, 1u)
FIELD(INTERCEPT_EN, JEDEC, 1u, 1u)
FIELD(INTERCEPT_EN, SFDP, 2u, 1u)
FIELD(INTERCEPT_EN, MBX, 3u, 1u)
REG32(LAST_READ_ADDR, 0x38u)
REG32(FLASH_STATUS, 0x3cu)
FIELD(FLASH_STATUS, BUSY, 0u, 1u)
FIELD(FLASH_STATUS, WEL, 1u, 1u)
FIELD(FLASH_STATUS, BP0, 2u, 1u)
FIELD(FLASH_STATUS, BP1, 3u, 1u)
FIELD(FLASH_STATUS, BP2, 4u, 1u)
FIELD(FLASH_STATUS, TB, 5u, 1u) /* beware actual bits depend on emulated dev. */
FIELD(FLASH_STATUS, SEC, 6u, 1u)
FIELD(FLASH_STATUS, SRP0, 7u, 1u)
FIELD(FLASH_STATUS, SRP1, 8u, 1u)
FIELD(FLASH_STATUS, QE, 9u, 1u)
FIELD(FLASH_STATUS, LB1, 11u, 1u)
FIELD(FLASH_STATUS, LB2, 12u, 1u)
FIELD(FLASH_STATUS, LB3, 13u, 1u)
FIELD(FLASH_STATUS, CMP, 14u, 1u)
FIELD(FLASH_STATUS, SUS, 15u, 1u)
FIELD(FLASH_STATUS, WPS, 18u, 1u)
FIELD(FLASH_STATUS, DRV0, 21u, 1u)
FIELD(FLASH_STATUS, DRV1, 22u, 1u)
FIELD(FLASH_STATUS, HOLD_NRST, 23u, 1u)
REG32(JEDEC_CC, 0x40u)
FIELD(JEDEC_CC, CC, 0u, 8u)
FIELD(JEDEC_CC, NUM_CC, 8u, 8u)
REG32(JEDEC_ID, 0x44u)
FIELD(JEDEC_ID, ID, 0u, 16u)
FIELD(JEDEC_ID, MF, 16u, 8u)
REG32(READ_THRESHOLD, 0x48u)
FIELD(READ_THRESHOLD, THRESHOLD, 0u, 10u)
REG32(MAILBOX_ADDR, 0x4cu)
FIELD(MAILBOX_ADDR, LOWER, 0u, 9u)
FIELD(MAILBOX_ADDR, UPPER, 10u, 22u)
REG32(UPLOAD_STATUS, 0x50u)
FIELD(UPLOAD_STATUS, CMDFIFO_DEPTH, 0u, 5u)
FIELD(UPLOAD_STATUS, CMDFIFO_NOTEMPTY, 7u, 1u)
FIELD(UPLOAD_STATUS, ADDRFIFO_DEPTH, 8u, 5u)
FIELD(UPLOAD_STATUS, ADDRFIFO_NOTEMPTY, 15u, 1u)
REG32(UPLOAD_STATUS2, 0x54u)
FIELD(UPLOAD_STATUS2, PAYLOAD_DEPTH, 0u, 9u)
FIELD(UPLOAD_STATUS2, PAYLOAD_START_IDX, 16u, 8u)
REG32(UPLOAD_CMDFIFO, 0x58u)
FIELD(UPLOAD_CMDFIFO, DATA, 0u, 8u)
REG32(UPLOAD_ADDRFIFO, 0x5cu)
REG32(CMD_FILTER_0, 0x60u)
REG32(CMD_FILTER_1, 0x64u)
REG32(CMD_FILTER_2, 0x68u)
REG32(CMD_FILTER_3, 0x6cu)
REG32(CMD_FILTER_4, 0x70u)
REG32(CMD_FILTER_5, 0x74u)
REG32(CMD_FILTER_6, 0x78u)
REG32(CMD_FILTER_7, 0x7cu)
REG32(ADDR_SWAP_MASK, 0x80u)
REG32(ADDR_SWAP_DATA, 0x84u)
REG32(PAYLOAD_SWAP_MASK, 0x88u)
REG32(PAYLOAD_SWAP_DATA, 0x8cu)
REG32(CMD_INFO_0, 0x90u) /* ReadStatus1 */
SHARED_FIELD(CMD_INFO_OPCODE, 0u, 8u)
SHARED_FIELD(CMD_INFO_ADDR_MODE, 8u, 2u)
SHARED_FIELD(CMD_INFO_ADDR_SWAP_EN, 10u, 1u) /* not used in Flash mode */
SHARED_FIELD(CMD_INFO_MBYTE_EN, 11u, 1u)
SHARED_FIELD(CMD_INFO_DUMMY_SIZE, 12u, 3u) /* limited to bits, ignore in QEMU */
SHARED_FIELD(CMD_INFO_DUMMY_EN, 15u, 1u) /* only use this bit for dummy cfg */
SHARED_FIELD(CMD_INFO_PAYLOAD_EN, 16u, 4u)
SHARED_FIELD(CMD_INFO_PAYLOAD_DIR, 20u, 1u) /* not used in Flash mode (guess) */
SHARED_FIELD(CMD_INFO_PAYLOAD_SWAP_EN, 21u, 1u) /* not used in Flash mode */
SHARED_FIELD(CMD_INFO_UPLOAD, 24u, 1u)
SHARED_FIELD(CMD_INFO_BUSY, 25u, 1u)
SHARED_FIELD(CMD_INFO_VALID, 31u, 1u)
REG32(CMD_INFO_1, 0x94u) /* ReadStatus2 */
REG32(CMD_INFO_2, 0x98u) /* ReadStatus3 */
REG32(CMD_INFO_3, 0x9cu) /* ReadJedecId */
REG32(CMD_INFO_4, 0xa0u) /* ReadSfdp */
REG32(CMD_INFO_5, 0xa4u) /* Read */
REG32(CMD_INFO_6, 0xa8u) /* FastRead */
REG32(CMD_INFO_7, 0xacu) /* FastReadDual */
REG32(CMD_INFO_8, 0xb0u) /* FastReadQuad */
REG32(CMD_INFO_9, 0xb4u) /* FastReadDualIO */
REG32(CMD_INFO_10, 0xb8u) /* FastReadQuadIO */
REG32(CMD_INFO_11, 0xbcu)
REG32(CMD_INFO_12, 0xc0u)
REG32(CMD_INFO_13, 0xc4u)
REG32(CMD_INFO_14, 0xc8u)
REG32(CMD_INFO_15, 0xccu)
REG32(CMD_INFO_16, 0xd0u)
REG32(CMD_INFO_17, 0xd4u)
REG32(CMD_INFO_18, 0xd8u)
REG32(CMD_INFO_19, 0xdcu)
REG32(CMD_INFO_20, 0xe0u)
REG32(CMD_INFO_21, 0xe4u)
REG32(CMD_INFO_22, 0xe8u)
REG32(CMD_INFO_23, 0xecu)
REG32(CMD_INFO_EN4B, 0xf0u)
REG32(CMD_INFO_EX4B, 0xf4u)
REG32(CMD_INFO_WREN, 0xf8u)
REG32(CMD_INFO_WRDI, 0xfcu)

/* TPM registers */
REG32(TPM_CAP, 0x00u)
FIELD(TPM_CAP, REV, 0u, 8u)
FIELD(TPM_CAP, LOCALITY, 8u, 1u)
FIELD(TPM_CAP, MAX_WR_SIZE, 16u, 3u)
FIELD(TPM_CAP, MAX_RD_SIZE, 20u, 3u)
REG32(TPM_CFG, 0x04u)
FIELD(TPM_CFG, EN, 0u, 1u)
FIELD(TPM_CFG, TPM_MODE, 1u, 1u)
FIELD(TPM_CFG, HW_REG_DIS, 2u, 1u)
FIELD(TPM_CFG, TPM_REG_CHK_DIS, 3u, 1u)
FIELD(TPM_CFG, INVALID_LOCALITY, 4u, 1u)
REG32(TPM_STATUS, 0x08u)
FIELD(TPM_STATUS, CMDADDR_NOTEMPTY, 0u, 1u)
FIELD(TPM_STATUS, WRFIFO_DEPTH, 16u, 7u)
REG32(TPM_ACCESS_0, 0x0cu)
FIELD(TPM_ACCESS_0, ACCESS_0, 0u, 8u)
FIELD(TPM_ACCESS_0, ACCESS_1, 8u, 8u)
FIELD(TPM_ACCESS_0, ACCESS_2, 16u, 8u)
FIELD(TPM_ACCESS_0, ACCESS_3, 24u, 8u)
REG32(TPM_ACCESS_1, 0x10u)
FIELD(TPM_ACCESS_1, ACCESS_4, 0u, 8u)
REG32(TPM_STS, 0x14u)
REG32(TPM_INTF_CAPABILITY, 0x18u)
REG32(TPM_INT_ENABLE, 0x1cu)
REG32(TPM_INT_VECTOR, 0x20u)
FIELD(TPM_INT_VECTOR, INT_VECTOR, 0u, 8u)
REG32(TPM_INT_STATUS, 0x24u)
REG32(TPM_DID_VID, 0x28u)
FIELD(TPM_DID_VID, VID, 0u, 16u)
FIELD(TPM_DID_VID, DID, 16u, 16u)
REG32(TPM_RID, 0x2cu)
FIELD(TPM_RID, RID, 0u, 8u)
REG32(TPM_CMD_ADDR, 0x30u)
FIELD(TPM_CMD_ADDR, ADDR, 0u, 24u)
FIELD(TPM_CMD_ADDR, CMD, 24u, 8u)
REG32(TPM_READ_FIFO, 0x34u)
REG32(TPM_WRITE_FIFO, 0x38u)
FIELD(TPM_WRITE_FIFO, VALUE, 0u, 8u)

#define SPI_BUS_PROTO_VER   0
#define SPI_BUS_HEADER_SIZE (2u * sizeof(uint32_t))
/**
 * Delay for handling non-aligned generic data transfer and flush the FIFO.
 * Generic mode is deprecated anyway. Arbitrarily set to 1 ms.
 */
#define SPI_BUS_TIMEOUT_NS 1000000u
/*
 * Pacing time to give hand back to the vCPU when a readbuf event is triggered.
 * The scheduler timer tell the CharDev backend not to consume (nor push back)
 * any more bytes from/to the SPI bus. The timer can either exhausts on its own,
 * which should never happen, and much more likely when the readbuf interruption
 * is cleared by the guest SW, which should usually happen once the SW has
 * filled in the read buffer. As soon as the timer is cancelled/over, the
 * CharDev resumes its SPI bus bytestream management. Arbitrarily set to 100 ms.
 */
#define SPI_BUS_FLASH_READ_DELAY_NS 100000000u

/*
 *          New scheme (Egress + Ingress)      Old Scheme (DPSRAM)
 *         +-----------------------------+    +-----------------------+
 *         | Flash / Passthru modes      |    | Flash / Passthru modes|
 *  0x000 -+----------------+------+-----+   -+----------------+------+
 *         | Read Command 0 | 1KiB | Out |    | Read Command 0 | 1KiB |
 *  0x400 -+----------------+------+-----+   -+----------------+------+
 *         | Read Command 1 | 1KiB | Out |    | Read Command 1 | 1KiB |
 *  0x800 -+----------------+------+-----+   -+----------------+------+
 *         | Mailbox        | 1KiB | Out |    | Mailbox        | 1KiB |
 *  0xc00 -+----------------+------+-----+   -+----------------+------+
 *         | SFDP           | 256B | Out |    | SFDP           | 256B |
 *  0xd00 -+----------------+------+-----+   -+----------------+------+
 *         |                             |    | Payload FIFO   | 256B |
 *  0xe00 -+----------------+------+-----+   -+----------------+------+
 *         | Payload FIFO   | 256B | In  |    | Command FIFO   |  64B |
 *  0xe40 -+----------------+------+-----+   -+----------------+------+
 *         | Command FIFO   |  64B | In  |    | Address FIFO   |  64B |
 *  0xe80 -+----------------+------+-----+   -+----------------+------+
 *         | Address FIFO   |  64B | In  |
 *  0xe80 -+----------------+------+-----+
 *
 *
 */
#define SPI_SRAM_READ0_OFFSET 0x0
#define SPI_SRAM_READ_SIZE    0x400u
#define SPI_SRAM_READ1_OFFSET (SPI_SRAM_READ0_OFFSET + SPI_SRAM_READ_SIZE)
#define SPI_SRAM_READ1_SIZE   0x400u
#define SPI_SRAM_MBX_OFFSET   (SPI_SRAM_READ1_OFFSET + SPI_SRAM_READ_SIZE)
#define SPI_SRAM_MBX_SIZE     0x400u
#define SPI_SRAM_SFDP_OFFSET  (SPI_SRAM_MBX_OFFSET + SPI_SRAM_MBX_SIZE)
#define SPI_SRAM_SFDP_SIZE    0x100u
/* with new scheme (no dual part SRAM, the following offsets are shifted...) */
#define SPI_SRAM_INGRESS_OFFSET 0x100u
#define SPI_SRAM_PAYLOAD_OFFSET (SPI_SRAM_SFDP_OFFSET + SPI_SRAM_SFDP_SIZE)
#define SPI_SRAM_PAYLOAD_SIZE   0x100u
#define SPI_SRAM_CMD_OFFSET     (SPI_SRAM_PAYLOAD_OFFSET + SPI_SRAM_PAYLOAD_SIZE)
#define SPI_SRAM_CMD_SIZE       0x40u
#define SPI_SRAM_ADDR_OFFSET    (SPI_SRAM_CMD_OFFSET + SPI_SRAM_CMD_SIZE)
#define SPI_SRAM_ADDR_SIZE      0x40u
#define SPI_SRAM_ADDR_END       (SPI_SRAM_ADDR_OFFSET + SPI_SRAM_ADDR_SIZE)
#define SPI_SRAM_END_OFFSET     (SPI_SRAM_ADDR_END)
static_assert(SPI_SRAM_END_OFFSET == 0xe80u, "Invalid SRAM definition");

#define SPI_DEVICE_SIZE            0x2000u
#define SPI_DEVICE_SPI_REGS_OFFSET 0u
#define SPI_DEVICE_TPM_REGS_OFFSET 0x800u
#define SPI_DEVICE_SRAM_OFFSET     0x1000u

#define SRAM_SIZE PARAM_SRAM_OFFSET
#define EGRESS_BUFFER_SIZE_BYTES \
    (SPI_SRAM_PAYLOAD_OFFSET - SPI_SRAM_READ0_OFFSET)
#define EGRESS_BUFFER_SIZE_WORDS (EGRESS_BUFFER_SIZE_BYTES / sizeof(uint32_t))
#define INGRESS_BUFFER_SIZE_BYTES \
    (SPI_SRAM_END_OFFSET - SPI_SRAM_PAYLOAD_OFFSET)
#define INGRESS_BUFFER_SIZE_WORDS (INGRESS_BUFFER_SIZE_BYTES / sizeof(uint32_t))

#define GENERIC_BUFFER_SIZE    (2u * SPI_SRAM_READ_SIZE)
#define FLASH_READ_BUFFER_SIZE (2u * SPI_SRAM_READ_SIZE)

#define FIFO_PHASE_BIT 12u
static_assert((1u << FIFO_PHASE_BIT) >= GENERIC_BUFFER_SIZE,
              "Invalid phase bit");
#define FIFO_PTR_MASK     ((1u << FIFO_PHASE_BIT) - 1u)
#define FIFO_PTR(_ptr_)   ((_ptr_)&FIFO_PTR_MASK)
#define FIFO_PHASE(_ptr_) ((bool)((_ptr_) >> (FIFO_PHASE_BIT)))
#define FIFO_MAKE_PTR(_phase_, _ptr_) \
    ((((unsigned)(bool)(_phase_)) << FIFO_PHASE_BIT) | FIFO_PTR(_ptr_))

#define RXFIFO_LEN sizeof(uint32_t)
#define TXFIFO_LEN sizeof(uint32_t)

#define SPI_DEFAULT_TX_VALUE  0xffu
#define SPI_FLASH_BUFFER_SIZE 256u

typedef enum {
    READ_STATUS1,
    READ_STATUS2,
    READ_STATUS3,
    READ_JEDEC,
    READ_SFDP,
    READ_NORMAL,
    READ_FAST,
    READ_DUAL,
    READ_QUAD,
    READ_DUAL_IO,
    READ_QUAD_IO,
} SpiDeviceHwCommand;

static const uint8_t SPI_DEVICE_HW_COMMANDS[] = {
    /* clang-format off */
    [READ_STATUS1] = 0x05u,
    [READ_STATUS2] = 0x35u,
    [READ_STATUS3] = 0x15u,
    [READ_JEDEC] = 0x9fu,
    [READ_SFDP] = 0x5au,
    [READ_NORMAL] = 0x03u,
    [READ_FAST] = 0x0bu,
    [READ_DUAL] = 0x3bu,
    [READ_QUAD] = 0x6bu,
    [READ_DUAL_IO] = 0xbbu,
    [READ_QUAD_IO] = 0xebu,
    /* clang-format on */
};

#define SPI_DEVICE_CMD_HW_STA_COUNT ARRAY_SIZE(SPI_DEVICE_HW_COMMANDS)
#define SPI_DEVICE_CMD_HW_STA_FIRST 0
#define SPI_DEVICE_CMD_HW_STA_LAST  (SPI_DEVICE_CMD_HW_STA_COUNT - 1u)
#define SPI_DEVICE_CMD_HW_CFG_FIRST (R_CMD_INFO_EN4B - R_CMD_INFO_0)
#define SPI_DEVICE_CMD_HW_CFG_LAST  (R_CMD_INFO_WRDI - R_CMD_INFO_0)
#define SPI_DEVICE_CMD_HW_CFG_COUNT \
    (SPI_DEVICE_CMD_HW_CFG_LAST - SPI_DEVICE_CMD_HW_CFG_FIRST + 1u)
#define SPI_DEVICE_CMD_SW_FIRST (SPI_DEVICE_CMD_HW_STA_LAST + 1u)
#define SPI_DEVICE_CMD_SW_LAST  (SPI_DEVICE_CMD_HW_CFG_FIRST - 1u)
#define SPI_DEVICE_CMD_SW_COUNT \
    (SPI_DEVICE_CMD_SW_LAST - SPI_DEVICE_CMD_SW_FIRST + 1u)

static_assert((SPI_DEVICE_CMD_HW_STA_COUNT + SPI_DEVICE_CMD_SW_COUNT +
               SPI_DEVICE_CMD_HW_CFG_COUNT) == 28u,
              "Invalid command info definitions");
static_assert(PARAM_NUM_CMD_INFO ==
                  SPI_DEVICE_CMD_HW_CFG_FIRST - SPI_DEVICE_CMD_HW_STA_FIRST,
              "Invalid command info definitions");

typedef enum {
    CTRL_MODE_FW,
    CTRL_MODE_FLASH,
    CTRL_MODE_PASSTHROUGH,
    CTRL_MODE_INVALID,
} OtSpiDeviceMode;

typedef enum {
    ADDR_MODE_ADDRDISABLED,
    ADDR_MODE_ADDRCFG,
    ADDR_MODE_ADDR3B,
    ADDR_MODE_ADDR4B,
} OtSpiDeviceAddrMode;

typedef enum {
    SPI_BUS_IDLE,
    SPI_BUS_GENERIC,
    SPI_BUS_FLASH,
    SPI_BUS_DISCARD,
    SPI_BUS_ERROR,
} OtSpiBusState;

typedef enum {
    SPI_FLASH_CMD_NONE, /* Not decoded / unknown */
    SPI_FLASH_CMD_HW_STA, /* Hardcoded HW-handled commands */
    SPI_FLASH_CMD_HW_CFG, /* Configurable HW-handled commands */
    SPI_FLASH_CMD_SW, /* Configurable SW-handled commands */
} OtSpiFlashCommand;

typedef enum {
    SPI_FLASH_IDLE, /* No command received */
    SPI_FLASH_COLLECT, /* Collecting address or additional info after cmd */
    SPI_FLASH_BUFFER, /* Reading out data from buffer or SFDP (-> SPI host) */
    SPI_FLASH_READ, /* Reading out data from SRAM (-> SPI host) */
    SPI_FLASH_UP_ADDR, /* Uploading address (<- SPI host) */
    SPI_FLASH_UP_DUMMY, /* Uploading dummy (<- SPI host) */
    SPI_FLASH_UP_PAYLOAD, /* Uploading payload (<- SPI host) */
    SPI_FLASH_DONE, /* No more clock expected for the current command */
    SPI_FLASH_ERROR, /* On error */
} OtSpiFlashState;

typedef struct {
    OtSpiFlashState state;
    OtSpiFlashCommand type;
    unsigned pos; /* Current position in data buffer */
    unsigned len; /* Meaning depends on command and current state */
    unsigned slot; /* Command slot */
    uint32_t address; /* Address tracking */
    uint32_t cmd_info; /* Selected command info slot */
    uint8_t *src; /* Selected read data source (alias) */
    uint8_t *payload; /* Selected write data sink (alias) */
    uint8_t *buffer; /* Temporary buffer to handle transfer */
    Fifo8 cmd_fifo; /* Command FIFO (HW uses 32-bit FIFO w/ 24-bit padding) */
    OtFifo32 address_fifo; /* Address FIFO */
    QEMUTimer *irq_timer; /* Timer to resume processing after a READBUF_* IRQ */
    bool loop; /* Keep reading the buffer if end is reached */
    bool watermark; /* Read watermark hit, used as flip-flop */
} SpiDeviceFlash;

typedef struct {
    uint32_t *buf;
    uint32_t *ptr;
    uint32_t *addr;
} SpiFifo;

typedef struct {
    SpiFifo rxf; /* DPRAM input */
    SpiFifo txf; /* DPRAM output */
    Fifo8 rx_fifo; /* Input comm port */
    Fifo8 tx_fifo; /* Output comm port */
    QEMUTimer *rx_timer; /* RX input timeout for filling in SRAM */
} SpiDeviceGeneric;

typedef struct {
    OtSpiBusState state;
    unsigned byte_count; /* Count of SPI payload to receive */
    Fifo8 chr_fifo; /* QEMU protocol input FIFO */
    uint8_t mode; /* Polarity/phase mismatch */
    bool release; /* Whether to release /CS on last byte */
    bool rev_rx; /* Reverse RX bits */
    bool rev_tx; /* Reverse TX bits */
} SpiDeviceBus;

struct OtSPIDeviceState {
    SysBusDevice parent_obj;

    struct {
        MemoryRegion main;
        MemoryRegion spi;
        MemoryRegion tpm;
        MemoryRegion buf;
    } mmio;
    IbexIRQ irqs[PARAM_NUM_IRQS];
    IbexIRQ alerts[PARAM_NUM_ALERTS];

    SpiDeviceBus bus;
    SpiDeviceFlash flash;
    SpiDeviceGeneric generic;

    uint32_t *spi_regs; /* Registers */
    uint32_t *tpm_regs; /* Registers */
    uint32_t *sram; /* SRAM (DPRAM on EG, E/I on DJ) */

    /* Properties */
    CharBackend chr; /* communication device */
    guint watch_tag; /* tracker for comm device change */
    bool dpsram; /* support for deprecated DPSRAM and generic mode */
};

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_SPI_LAST_REG (R_CMD_INFO_WRDI)
#define SPI_REGS_COUNT (R_SPI_LAST_REG + 1u)
#define SPI_REGS_SIZE  (SPI_REGS_COUNT * sizeof(uint32_t))
#define SPI_REG_NAME(_reg_) \
    ((((_reg_) <= SPI_REGS_COUNT) && SPI_REG_NAMES[_reg_]) ? \
         SPI_REG_NAMES[_reg_] : \
         "?")

#define R_TPM_LAST_REG (R_TPM_WRITE_FIFO)
#define TPM_REGS_COUNT (R_TPM_LAST_REG + 1u)
#define TPM_REGS_SIZE  (TPM_REGS_COUNT * sizeof(uint32_t))
#define TPM_REG_NAME(_reg_) \
    ((((_reg_) <= TPM_REGS_COUNT) && TPM_REG_NAMES[_reg_]) ? \
         TPM_REG_NAMES[_reg_] : \
         "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
/* clang-format off */
static const char *SPI_REG_NAMES[SPI_REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(CONTROL),
    REG_NAME_ENTRY(CFG),
    REG_NAME_ENTRY(FIFO_LEVEL),
    REG_NAME_ENTRY(ASYNC_FIFO_LEVEL),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(RXF_PTR),
    REG_NAME_ENTRY(TXF_PTR),
    REG_NAME_ENTRY(RXF_ADDR),
    REG_NAME_ENTRY(TXF_ADDR),
    REG_NAME_ENTRY(INTERCEPT_EN),
    REG_NAME_ENTRY(LAST_READ_ADDR),
    REG_NAME_ENTRY(FLASH_STATUS),
    REG_NAME_ENTRY(JEDEC_CC),
    REG_NAME_ENTRY(JEDEC_ID),
    REG_NAME_ENTRY(READ_THRESHOLD),
    REG_NAME_ENTRY(MAILBOX_ADDR),
    REG_NAME_ENTRY(UPLOAD_STATUS),
    REG_NAME_ENTRY(UPLOAD_STATUS2),
    REG_NAME_ENTRY(UPLOAD_CMDFIFO),
    REG_NAME_ENTRY(UPLOAD_ADDRFIFO),
    REG_NAME_ENTRY(CMD_FILTER_0),
    REG_NAME_ENTRY(CMD_FILTER_1),
    REG_NAME_ENTRY(CMD_FILTER_2),
    REG_NAME_ENTRY(CMD_FILTER_3),
    REG_NAME_ENTRY(CMD_FILTER_4),
    REG_NAME_ENTRY(CMD_FILTER_5),
    REG_NAME_ENTRY(CMD_FILTER_6),
    REG_NAME_ENTRY(CMD_FILTER_7),
    REG_NAME_ENTRY(ADDR_SWAP_MASK),
    REG_NAME_ENTRY(ADDR_SWAP_DATA),
    REG_NAME_ENTRY(PAYLOAD_SWAP_MASK),
    REG_NAME_ENTRY(PAYLOAD_SWAP_DATA),
    REG_NAME_ENTRY(CMD_INFO_0),
    REG_NAME_ENTRY(CMD_INFO_1),
    REG_NAME_ENTRY(CMD_INFO_2),
    REG_NAME_ENTRY(CMD_INFO_3),
    REG_NAME_ENTRY(CMD_INFO_4),
    REG_NAME_ENTRY(CMD_INFO_5),
    REG_NAME_ENTRY(CMD_INFO_6),
    REG_NAME_ENTRY(CMD_INFO_7),
    REG_NAME_ENTRY(CMD_INFO_8),
    REG_NAME_ENTRY(CMD_INFO_9),
    REG_NAME_ENTRY(CMD_INFO_10),
    REG_NAME_ENTRY(CMD_INFO_11),
    REG_NAME_ENTRY(CMD_INFO_12),
    REG_NAME_ENTRY(CMD_INFO_13),
    REG_NAME_ENTRY(CMD_INFO_14),
    REG_NAME_ENTRY(CMD_INFO_15),
    REG_NAME_ENTRY(CMD_INFO_16),
    REG_NAME_ENTRY(CMD_INFO_17),
    REG_NAME_ENTRY(CMD_INFO_18),
    REG_NAME_ENTRY(CMD_INFO_19),
    REG_NAME_ENTRY(CMD_INFO_20),
    REG_NAME_ENTRY(CMD_INFO_21),
    REG_NAME_ENTRY(CMD_INFO_22),
    REG_NAME_ENTRY(CMD_INFO_23),
    REG_NAME_ENTRY(CMD_INFO_EN4B),
    REG_NAME_ENTRY(CMD_INFO_EX4B),
    REG_NAME_ENTRY(CMD_INFO_WREN),
    REG_NAME_ENTRY(CMD_INFO_WRDI),
};

static const char *TPM_REG_NAMES[TPM_REGS_COUNT] = {
    REG_NAME_ENTRY(TPM_CAP),
    REG_NAME_ENTRY(TPM_CFG),
    REG_NAME_ENTRY(TPM_STATUS),
    REG_NAME_ENTRY(TPM_ACCESS_0),
    REG_NAME_ENTRY(TPM_ACCESS_1),
    REG_NAME_ENTRY(TPM_STS),
    REG_NAME_ENTRY(TPM_INTF_CAPABILITY),
    REG_NAME_ENTRY(TPM_INT_ENABLE),
    REG_NAME_ENTRY(TPM_INT_VECTOR),
    REG_NAME_ENTRY(TPM_INT_STATUS),
    REG_NAME_ENTRY(TPM_DID_VID),
    REG_NAME_ENTRY(TPM_RID),
    REG_NAME_ENTRY(TPM_CMD_ADDR),
    REG_NAME_ENTRY(TPM_READ_FIFO),
    REG_NAME_ENTRY(TPM_WRITE_FIFO),
};
/* clang-format on */
#undef REG_NAME_ENTRY

#define INTR_MASK         ((1u << PARAM_NUM_IRQS) - 1u)
#define ALERT_TEST_MASK   (R_ALERT_TEST_FATAL_FAULT_MASK)
#define INTR_READBUF_MASK (INTR_READBUF_WATERMARK_MASK | INTR_READBUF_FLIP_MASK)
#define CONTROL_MASK \
    (R_CONTROL_ABORT_MASK | R_CONTROL_MODE_MASK | R_CONTROL_RST_TXFIFO_MASK | \
     R_CONTROL_RST_RXFIFO_MASK | R_CONTROL_SRAM_CLK_EN_MASK)
#define CMD_INFO_GEN_MASK \
    (CMD_INFO_OPCODE_MASK | CMD_INFO_ADDR_MODE_MASK | \
     CMD_INFO_ADDR_SWAP_EN_MASK | CMD_INFO_MBYTE_EN_MASK | \
     CMD_INFO_DUMMY_SIZE_MASK | CMD_INFO_DUMMY_EN_MASK | \
     CMD_INFO_PAYLOAD_EN_MASK | CMD_INFO_PAYLOAD_DIR_MASK | \
     CMD_INFO_PAYLOAD_SWAP_EN_MASK | CMD_INFO_UPLOAD_MASK | \
     CMD_INFO_BUSY_MASK | CMD_INFO_VALID_MASK)
#define CMD_INFO_SPC_MASK (CMD_INFO_OPCODE_MASK | CMD_INFO_VALID_MASK)
#define CFG_MASK \
    (R_CFG_CPOL_MASK | R_CFG_CPHA_MASK | R_CFG_TX_ORDER_MASK | \
     R_CFG_RX_ORDER_MASK | R_CFG_TIMER_V_MASK | R_CFG_ADDR_4B_EN_MASK | \
     R_CFG_MAILBOX_EN_MASK)
#define INTERCEPT_EN_MASK \
    (R_INTERCEPT_EN_STATUS_MASK | R_INTERCEPT_EN_JEDEC_MASK | \
     R_INTERCEPT_EN_SFDP_MASK | R_INTERCEPT_EN_MBX_MASK)
#define FLASH_STATUS_STATUS_MASK \
    (R_FLASH_STATUS_WEL_MASK | R_FLASH_STATUS_BP0_MASK | \
     R_FLASH_STATUS_BP1_MASK | R_FLASH_STATUS_BP2_MASK | \
     R_FLASH_STATUS_TB_MASK | R_FLASH_STATUS_SEC_MASK | \
     R_FLASH_STATUS_SRP0_MASK | R_FLASH_STATUS_SRP1_MASK | \
     R_FLASH_STATUS_QE_MASK | R_FLASH_STATUS_LB1_MASK | \
     R_FLASH_STATUS_LB2_MASK | R_FLASH_STATUS_LB3_MASK | \
     R_FLASH_STATUS_CMP_MASK | R_FLASH_STATUS_SUS_MASK | \
     R_FLASH_STATUS_WPS_MASK | R_FLASH_STATUS_DRV0_MASK | \
     R_FLASH_STATUS_DRV1_MASK | R_FLASH_STATUS_HOLD_NRST_MASK)
#define FLASH_STATUS_MASK (R_FLASH_STATUS_BUSY_MASK | FLASH_STATUS_STATUS_MASK)
#define JEDEC_CC_MASK     (R_JEDEC_CC_CC_MASK | R_JEDEC_CC_NUM_CC_MASK)
#define JEDEC_ID_MASK     (R_JEDEC_ID_ID_MASK | R_JEDEC_ID_MF_MASK)

#define COMMAND_OPCODE(_cmd_info_) \
    ((uint8_t)((_cmd_info_)&CMD_INFO_OPCODE_MASK))
#define FLASH_SLOT(_name_) ((R_CMD_INFO_##_name_) - R_CMD_INFO_0)

#define STATE_NAME_ENTRY(_st_) [_st_] = stringify(_st_)
/* clang-format off */
static const char *BUS_STATE_NAMES[] = {
    STATE_NAME_ENTRY(SPI_BUS_IDLE),
    STATE_NAME_ENTRY(SPI_BUS_GENERIC),
    STATE_NAME_ENTRY(SPI_BUS_FLASH),
    STATE_NAME_ENTRY(SPI_BUS_DISCARD),
    STATE_NAME_ENTRY(SPI_BUS_ERROR),
};

static const char *FLASH_STATE_NAMES[] = {
    STATE_NAME_ENTRY(SPI_FLASH_IDLE),
    STATE_NAME_ENTRY(SPI_FLASH_COLLECT),
    STATE_NAME_ENTRY(SPI_FLASH_BUFFER),
    STATE_NAME_ENTRY(SPI_FLASH_READ),
    STATE_NAME_ENTRY(SPI_FLASH_UP_ADDR),
    STATE_NAME_ENTRY(SPI_FLASH_UP_DUMMY),
    STATE_NAME_ENTRY(SPI_FLASH_UP_PAYLOAD),
    STATE_NAME_ENTRY(SPI_FLASH_DONE),
    STATE_NAME_ENTRY(SPI_FLASH_ERROR),
};
/* clang-format on */
#undef STATE_NAME_ENTRY

#define BUS_STATE_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(BUS_STATE_NAMES) ? \
         BUS_STATE_NAMES[(_st_)] : \
         "?")
#define FLASH_STATE_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(FLASH_STATE_NAMES) ? \
         FLASH_STATE_NAMES[(_st_)] : \
         "?")

#define IRQ_NAME_ENTRY(_st_) [INTR_##_st_##_SHIFT] = stringify(_st_)
/* clang-format off */
static const char *IRQ_NAMES[] = {
    IRQ_NAME_ENTRY(GENERIC_RX_FULL),
    IRQ_NAME_ENTRY(GENERIC_RX_WATERMARK),
    IRQ_NAME_ENTRY(GENERIC_TX_WATERMARK),
    IRQ_NAME_ENTRY(GENERIC_RX_ERROR),
    IRQ_NAME_ENTRY(GENERIC_RX_OVERFLOW),
    IRQ_NAME_ENTRY(GENERIC_TX_UNDERFLOW),
    IRQ_NAME_ENTRY(UPLOAD_CMDFIFO_NOT_EMPTY),
    IRQ_NAME_ENTRY(UPLOAD_PAYLOAD_NOT_EMPTY),
    IRQ_NAME_ENTRY(UPLOAD_PAYLOAD_OVERFLOW),
    IRQ_NAME_ENTRY(READBUF_WATERMARK),
    IRQ_NAME_ENTRY(READBUF_FLIP),
    IRQ_NAME_ENTRY(TPM_HEADER_NOT_EMPTY),
};
/* clang-format on */
#undef IRQ_NAME_ENTRY

#define IRQ_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(IRQ_NAMES) ? IRQ_NAMES[(_st_)] : "?")

#define WORD_ALIGN(_x_) ((_x_) & ~0x3u)

#define BUS_CHANGE_STATE(_b_, _sst_) \
    ot_spi_device_bus_change_state_line(_b_, SPI_BUS_##_sst_, __LINE__)
#define FLASH_CHANGE_STATE(_f_, _sst_) \
    ot_spi_device_flash_change_state_line(_f_, SPI_FLASH_##_sst_, __LINE__)

static void spi_fifo_init(SpiFifo *f, OtSPIDeviceState *s, bool tx)
{
    f->buf = s->sram;
    f->ptr = &s->spi_regs[tx ? R_TXF_PTR : R_RXF_PTR];
    f->addr = &s->spi_regs[tx ? R_TXF_ADDR : R_RXF_ADDR];
}

static unsigned spi_fifo_count_to_word(const SpiFifo *f)
{
    unsigned wptr = (*f->ptr) >> 16u;
    unsigned bytes = (wptr & (sizeof(uint32_t) - 1u));

    return (unsigned)(sizeof(uint32_t) - bytes);
}

static void spi_fifo_push(SpiFifo *f, uint8_t data)
{
    unsigned wptr = (*f->ptr) >> 16u;
    unsigned base = (*f->addr) & UINT16_MAX;
    unsigned lim = (*f->addr) >> 16u;
    unsigned max = lim - base;
    unsigned woff = FIFO_PTR(wptr);
    bool phase = FIFO_PHASE(wptr);
    ((uint8_t *)f->buf)[base + woff] = data;
    woff += sizeof(uint8_t);
    if (WORD_ALIGN(woff) > max) {
        trace_ot_spi_device_gen_phase(__func__, woff, lim, phase);
        woff = 0;
        phase = !phase;
    }
    *f->ptr &= UINT16_MAX;
    *f->ptr |= FIFO_MAKE_PTR(phase, woff) << 16u;
}

static void spi_fifo_push_w(SpiFifo *f, uint32_t data)
{
    unsigned wptr = (*f->ptr) >> 16u;
    unsigned base = (*f->addr) & UINT16_MAX;
    unsigned lim = (*f->addr) >> 16u;
    unsigned max = lim - base;
    unsigned woff = FIFO_PTR(wptr);
    g_assert(!(woff & 0x3u));
    bool phase = FIFO_PHASE(wptr);
    f->buf[(base + woff) >> 2u] = data;
    woff += sizeof(uint32_t);
    if (WORD_ALIGN(woff) > max) {
        trace_ot_spi_device_gen_phase(__func__, woff, lim, phase);
        woff = 0;
        phase = !phase;
    }
    *f->ptr &= UINT16_MAX;
    *f->ptr |= FIFO_MAKE_PTR(phase, woff) << 16u;
}

static uint8_t spi_fifo_pop(SpiFifo *f)
{
    unsigned rptr = (*f->ptr) & UINT16_MAX;
    unsigned base = (*f->addr) & UINT16_MAX;
    unsigned lim = (*f->addr) >> 16u;
    unsigned max = lim - base;
    unsigned roff = FIFO_PTR(rptr);
    bool phase = FIFO_PHASE(rptr);
    uint8_t data = ((uint8_t *)f->buf)[base + roff];
    roff += sizeof(uint8_t);
    if (WORD_ALIGN(roff) > max) {
        trace_ot_spi_device_gen_phase(__func__, roff, lim, phase);
        roff = 0;
        phase = !phase;
    }
    *f->ptr &= UINT16_MAX << 16u;
    *f->ptr |= FIFO_MAKE_PTR(phase, roff);
    return data;
}

static void spi_fifo_reset(SpiFifo *f)
{
    unsigned base = (*f->addr) & UINT16_MAX;
    unsigned ptr = FIFO_MAKE_PTR(false, base);
    *f->ptr = ptr | (ptr << 16u);
}

static unsigned spi_fifo_capacity(const SpiFifo *f)
{
    unsigned lim = (*f->addr) >> 16u;
    unsigned base = (*f->addr) & UINT16_MAX;

    return lim + sizeof(uint32_t) - base;
}

static unsigned spi_fifo_num_free(const SpiFifo *f)
{
    unsigned wptr = (*f->ptr) >> 16u;
    unsigned rptr = (*f->ptr) & UINT16_MAX;
    unsigned woff = FIFO_PTR(wptr);
    unsigned roff = FIFO_PTR(rptr);
    bool wph = FIFO_PHASE(wptr);
    bool rph = FIFO_PHASE(rptr);
    if (wph == rph) {
        roff += spi_fifo_capacity(f);
    }
    int count = (int)roff - (int)woff;
    g_assert(count >= 0);
    return (unsigned)count;
}

static unsigned spi_fifo_num_used(const SpiFifo *f)
{
    unsigned wptr = (*f->ptr) >> 16u;
    unsigned rptr = (*f->ptr) & UINT16_MAX;
    unsigned woff = FIFO_PTR(wptr);
    unsigned roff = FIFO_PTR(rptr);
    bool wph = FIFO_PHASE(wptr);
    bool rph = FIFO_PHASE(rptr);
    if (wph != rph) {
        woff += spi_fifo_capacity(f);
    }
    int count = (int)woff - (int)roff;
    g_assert(count >= 0);
    return (unsigned)count;
}

static bool spi_fifo_is_empty(const SpiFifo *f)
{
    return spi_fifo_num_used(f) == 0;
}

static bool spi_fifo_is_full(const SpiFifo *f)
{
    return spi_fifo_num_free(f) == 0;
}

static unsigned ot_spi_device_rxf_threshold(const OtSPIDeviceState *s)
{
    return (unsigned)FIELD_EX32(s->spi_regs[R_FIFO_LEVEL], FIFO_LEVEL, RXLVL);
}

static unsigned ot_spi_device_txf_threshold(const OtSPIDeviceState *s)
{
    return (unsigned)FIELD_EX32(s->spi_regs[R_FIFO_LEVEL], FIFO_LEVEL, TXLVL);
}

static bool ot_spi_device_is_rx_fifo_in_reset(const OtSPIDeviceState *s)
{
    return (bool)(s->spi_regs[R_CONTROL] & R_CONTROL_RST_RXFIFO_MASK);
}

static bool ot_spi_device_is_tx_fifo_in_reset(const OtSPIDeviceState *s)
{
    return (bool)(s->spi_regs[R_CONTROL] & R_CONTROL_RST_TXFIFO_MASK);
}

static bool ot_spi_device_is_cs_active(const OtSPIDeviceState *s)
{
    const SpiDeviceBus *bus = &s->bus;
    return bus->state != SPI_BUS_IDLE && bus->state != SPI_BUS_ERROR;
}

static void ot_spi_device_bus_change_state_line(SpiDeviceBus *b,
                                                OtSpiBusState state, int line)
{
    if (b->state != state) {
        trace_ot_spi_device_bus_change_state(line, BUS_STATE_NAME(state),
                                             state);
        b->state = state;
    }
}

static void ot_spi_device_flash_change_state_line(
    SpiDeviceFlash *f, OtSpiFlashState state, int line)
{
    if (f->state != state) {
        trace_ot_spi_device_flash_change_state(line, FLASH_STATE_NAME(state),
                                               state);
        f->state = state;
    }
}

static bool ot_spi_device_flash_has_input_payload(uint32_t cmd_info)
{
    return (cmd_info & CMD_INFO_PAYLOAD_EN_MASK) != 0 &&
           !(cmd_info & CMD_INFO_PAYLOAD_DIR_MASK);
}

static bool ot_spi_device_flash_is_upload(const SpiDeviceFlash *f)
{
    return (f->cmd_info & CMD_INFO_UPLOAD_MASK) != 0 &&
           (f->slot >= SPI_DEVICE_CMD_SW_FIRST) &&
           (f->slot <= SPI_DEVICE_CMD_SW_LAST);
}

static bool ot_spi_device_flash_is_readbuf_irq(const OtSPIDeviceState *s)
{
    /*
     * ignore R_INTR_ENABLE as the device may be used in poll mode, but this
     * device nevertheless needs to hand back execution to vCPU when a readbuf
     * interrupt is set
     */
    return (bool)(s->spi_regs[R_INTR_STATE] & INTR_READBUF_MASK);
}

static void ot_spi_device_clear_modes(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    timer_del(f->irq_timer);
    FLASH_CHANGE_STATE(f, IDLE);
    f->address = 0;
    f->cmd_info = UINT32_MAX;
    f->pos = 0;
    f->len = 0;
    f->type = SPI_FLASH_CMD_NONE;
    g_assert(s->sram);
    f->payload = &((uint8_t *)s->sram)[SPI_SRAM_PAYLOAD_OFFSET];
    if (!s->dpsram) {
        f->payload += SPI_SRAM_INGRESS_OFFSET;
    }
    memset(f->buffer, 0, SPI_FLASH_BUFFER_SIZE);

    if (s->dpsram) {
        SpiDeviceGeneric *g = &s->generic;
        timer_del(g->rx_timer);
        fifo8_reset(&g->rx_fifo);
        fifo8_reset(&g->tx_fifo);
    }

    memset(s->sram, 0, SRAM_SIZE);
}

static uint32_t ot_spi_device_get_status(const OtSPIDeviceState *s)
{
    /*
     * "Current version does not implement abort_done logic. It is tied to 1
     *  always."
     */
    uint32_t status = R_STATUS_ABORT_DONE_MASK;

    if (ot_spi_device_is_cs_active(s)) {
        status |= R_STATUS_CSB_MASK;
    }

    if (s->dpsram) {
        const SpiDeviceGeneric *g = &s->generic;
        if (spi_fifo_is_empty(&g->txf)) {
            status |= R_STATUS_TXF_EMPTY_MASK;
        }
        if (spi_fifo_is_full(&g->txf)) {
            status |= R_STATUS_TXF_FULL_MASK;
        }
        if (spi_fifo_is_empty(&g->rxf)) {
            status |= R_STATUS_RXF_EMPTY_MASK;
        }
        if (spi_fifo_is_full(&g->rxf)) {
            status |= R_STATUS_RXF_FULL_MASK;
        }
    }

    return status;
}

static void ot_spi_device_update_irqs(OtSPIDeviceState *s)
{
    uint32_t levels = s->spi_regs[R_INTR_STATE] & s->spi_regs[R_INTR_ENABLE];
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        bool level = (bool)((levels >> ix) & 0x1u);
        if (level && !ibex_irq_get_level(&s->irqs[ix])) {
            trace_ot_spi_device_set_irq(IRQ_NAME(ix), ix);
        }
        ibex_irq_set(&s->irqs[ix], (int)level);
    }
}

static OtSpiDeviceMode ot_spi_device_get_mode(const OtSPIDeviceState *s)
{
    return (OtSpiDeviceMode)FIELD_EX32(s->spi_regs[R_CONTROL], CONTROL, MODE);
}

static bool ot_spi_device_is_addr4b_en(const OtSPIDeviceState *s)
{
    return (bool)(s->spi_regs[R_CFG] & R_CFG_ADDR_4B_EN_MASK);
}

static bool ot_spi_device_is_mailbox_en(const OtSPIDeviceState *s)
{
    return (bool)(s->spi_regs[R_CFG] & R_CFG_MAILBOX_EN_MASK);
}

static bool
ot_spi_device_is_mailbox_match(const OtSPIDeviceState *s, uint32_t addr)
{
    if (!ot_spi_device_is_mailbox_en(s)) {
        return false;
    }

    uint32_t mailbox_addr =
        s->spi_regs[R_MAILBOX_ADDR] & R_MAILBOX_ADDR_UPPER_MASK;
    return (addr & R_MAILBOX_ADDR_UPPER_MASK) == mailbox_addr;
}

static bool ot_spi_device_is_hw_read_command(const OtSPIDeviceState *s)
{
    const SpiDeviceFlash *f = &s->flash;

    switch (f->slot) {
    case READ_NORMAL:
    case READ_FAST:
    case READ_DUAL:
    case READ_QUAD:
    case READ_DUAL_IO:
    case READ_QUAD_IO:
        return true;
    default:
        return false;
    }
}

static void ot_spi_device_release_cs(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;
    SpiDeviceBus *bus = &s->bus;

    BUS_CHANGE_STATE(bus, IDLE);
    bus->byte_count = 0;

    bool update_irq = false;
    switch (ot_spi_device_get_mode(s)) {
    case CTRL_MODE_FLASH:
        if (!fifo8_is_empty(&f->cmd_fifo)) {
            s->spi_regs[R_INTR_STATE] |= INTR_UPLOAD_CMDFIFO_NOT_EMPTY_MASK;
            update_irq = true;
        }
        if (f->state == SPI_FLASH_UP_PAYLOAD) {
            unsigned pos;
            unsigned len;
            if (f->pos) {
                s->spi_regs[R_INTR_STATE] |= INTR_UPLOAD_PAYLOAD_NOT_EMPTY_MASK;
                update_irq = true;
            }
            if (f->pos > f->len) {
                pos = f->pos % SPI_SRAM_PAYLOAD_SIZE;
                len = SPI_SRAM_PAYLOAD_SIZE;
                s->spi_regs[R_INTR_STATE] |= INTR_UPLOAD_PAYLOAD_OVERFLOW_MASK;
                update_irq = true;
                trace_ot_spi_device_flash_overflow("payload");
            } else {
                pos = 0;
                len = f->pos;
            }
            s->spi_regs[R_UPLOAD_STATUS2] =
                FIELD_DP32(0, UPLOAD_STATUS2, PAYLOAD_START_IDX, pos);
            s->spi_regs[R_UPLOAD_STATUS2] =
                FIELD_DP32(s->spi_regs[R_UPLOAD_STATUS2], UPLOAD_STATUS2,
                           PAYLOAD_DEPTH, len);
            trace_ot_spi_device_flash_payload(f->pos, pos, len);
        }
        /*
         * "shows the last address accessed by the host system."
         * "does not show the commands falling into the mailbox region or
         *  Read SFDP command’s address."
         */
        if (ot_spi_device_is_hw_read_command(s) &&
            !ot_spi_device_is_mailbox_match(s, f->address)) {
            trace_ot_spi_device_update_last_read_addr(f->address);
            s->spi_regs[R_LAST_READ_ADDR] = f->address;
        }
        FLASH_CHANGE_STATE(f, IDLE);
        break;
    case CTRL_MODE_PASSTHROUGH:
        s->spi_regs[R_LAST_READ_ADDR] = f->address;
        break;
    default:
        break;
    }

    if (update_irq) {
        ot_spi_device_update_irqs(s);
    }
}

static void ot_spi_device_flash_pace_spibus(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    timer_del(f->irq_timer);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    trace_ot_spi_device_flash_pace("set", timer_pending(f->irq_timer));
    timer_mod(f->irq_timer, (int64_t)(now + SPI_BUS_FLASH_READ_DELAY_NS));
}

static void ot_spi_device_flash_decode_command(OtSPIDeviceState *s, uint8_t cmd)
{
    SpiDeviceFlash *f = &s->flash;

    /* search command slot in HW-handling commands (static group) */
    if (f->state == SPI_FLASH_IDLE) {
        for (unsigned ix = 0; ix < SPI_DEVICE_CMD_HW_STA_COUNT; ix++) {
            if (cmd == SPI_DEVICE_HW_COMMANDS[ix]) {
                f->type = SPI_FLASH_CMD_HW_STA;
                f->slot = ix;
                f->cmd_info = SHARED_FIELD_DP32(s->spi_regs[R_CMD_INFO_0 + ix],
                                                CMD_INFO_OPCODE, cmd);
                trace_ot_spi_device_flash_new_command("hw", cmd, f->slot);
                break;
            }
        }
    }

    /* search command in other slots */
    if (f->state == SPI_FLASH_IDLE) {
        for (unsigned ix = SPI_DEVICE_CMD_HW_STA_COUNT;
             ix < PARAM_NUM_CMD_INFO + SPI_DEVICE_CMD_HW_CFG_COUNT; ix++) {
            uint32_t val32 = s->spi_regs[R_CMD_INFO_0 + ix];
            if (cmd == SHARED_FIELD_EX32(val32, CMD_INFO_OPCODE)) {
                if (SHARED_FIELD_EX32(val32, CMD_INFO_VALID)) {
                    f->type = ix < PARAM_NUM_CMD_INFO ? SPI_FLASH_CMD_SW :
                                                        SPI_FLASH_CMD_HW_CFG;
                    f->slot = ix;
                    f->cmd_info = val32;
                    trace_ot_spi_device_flash_new_command(
                        f->type == SPI_FLASH_CMD_SW ? "sw" : "hw_cfg", cmd,
                        f->slot);
                    break;
                }
                trace_ot_spi_device_flash_disabled_slot(cmd, ix);
            }
        }
    }

    if (f->type == SPI_FLASH_CMD_NONE) {
        trace_ot_spi_device_flash_ignored_command("unmanaged", cmd);
        return;
    }

    bool upload = ot_spi_device_flash_is_upload(f);
    if (upload) {
        if (fifo8_is_full(&f->cmd_fifo)) {
            error_setg(&error_warn, "%s: command FIFO overflow\n", __func__);
            return;
        }

        bool set_busy = (bool)f->cmd_info & CMD_INFO_BUSY_MASK;
        if (set_busy) {
            s->spi_regs[R_FLASH_STATUS] |= R_FLASH_STATUS_BUSY_MASK;
        }
        trace_ot_spi_device_flash_upload(f->slot, f->cmd_info, set_busy);
        fifo8_push(&f->cmd_fifo, COMMAND_OPCODE(f->cmd_info));
    }
}

static void ot_spi_device_flash_decode_read_jedec(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    f->len = 3u;
    uint32_t cc_count = FIELD_EX32(s->spi_regs[R_JEDEC_CC], JEDEC_CC, NUM_CC);
    uint32_t cc_code = FIELD_EX32(s->spi_regs[R_JEDEC_CC], JEDEC_CC, CC);
    uint32_t jedec = s->spi_regs[R_JEDEC_ID];
    /* use len field to count continuation code */
    memset(f->buffer, (int)cc_code, cc_count);
    stl_le_p(&f->buffer[cc_count], bswap32(jedec << 8));
    f->len += cc_count;
    memset(&f->buffer[f->len], (int)SPI_DEFAULT_TX_VALUE,
           SPI_FLASH_BUFFER_SIZE - f->len);
    f->src = f->buffer;
    FLASH_CHANGE_STATE(f, BUFFER);
}

static void ot_spi_device_flash_decode_write_enable(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    bool enable = f->slot == FLASH_SLOT(WREN);
    trace_ot_spi_device_flash_exec(enable ? "WREN" : "WRDI");
    if (enable) {
        s->spi_regs[R_FLASH_STATUS] |= R_FLASH_STATUS_WEL_MASK;
    } else {
        s->spi_regs[R_FLASH_STATUS] &= ~R_FLASH_STATUS_WEL_MASK;
    }
    FLASH_CHANGE_STATE(f, DONE);
}

static void ot_spi_device_flash_decode_addr4_enable(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    bool enable = f->slot == FLASH_SLOT(EN4B);
    trace_ot_spi_device_flash_exec(enable ? "EN4B" : "EX4B");

    if (enable) {
        s->spi_regs[R_CFG] |= R_CFG_ADDR_4B_EN_MASK;
    } else {
        s->spi_regs[R_CFG] &= ~R_CFG_ADDR_4B_EN_MASK;
    }
    FLASH_CHANGE_STATE(f, DONE);
}

static void ot_spi_device_flash_decode_read_status(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    g_assert(f->slot < 3u);

    uint32_t status = s->spi_regs[R_FLASH_STATUS];
    f->buffer[0] = (uint8_t)(status >> (f->slot * 8u));
    f->len = sizeof(uint8_t);
    f->src = f->buffer;
    f->loop = true;

    trace_ot_spi_device_flash_read_status(f->slot, f->buffer[0]);

    FLASH_CHANGE_STATE(f, BUFFER);
}

static void ot_spi_device_flash_decode_read_sfdp(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    f->src = f->buffer;
    FLASH_CHANGE_STATE(f, COLLECT);
    f->loop = true;
    f->len = 4; /* 3-byte address + 1 dummy byte */
}

static void ot_spi_device_flash_decode_read_data(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    unsigned dummy = 1;

    switch (f->slot) {
    case READ_NORMAL:
        dummy = 0;
        break;
    case READ_FAST:
    case READ_DUAL:
    case READ_QUAD:
    case READ_DUAL_IO:
    case READ_QUAD_IO:
        dummy = 1u;
        break;
    default:
        g_assert_not_reached();
    }

    f->src = f->buffer;
    f->watermark = false;
    FLASH_CHANGE_STATE(f, COLLECT);
    f->len = dummy + (ot_spi_device_is_addr4b_en(s) ? 4u : 3u);
}

static void ot_spi_device_flash_decode_hw_static_command(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    switch (COMMAND_OPCODE(f->cmd_info)) {
    case 0x05u: /* READ_STATUS_1 */
    case 0x35u: /* READ_STATUS_2 */
    case 0x15u: /* READ_STATUS_3 */
        ot_spi_device_flash_decode_read_status(s);
        break;
    case 0x9fu: /* READ_JEDEC    */
        ot_spi_device_flash_decode_read_jedec(s);
        break;
    case 0x5au: /* READ_SFDP     */
        ot_spi_device_flash_decode_read_sfdp(s);
        break;
    case 0x03u: /* READ_NORMAL */
    case 0x0bu: /* READ_FAST   */
    case 0x3bu: /* READ_DUAL   */
    case 0x6bu: /* READ_QUAD   */
    case 0xbbu: /* READ_DUALIO */
    case 0xebu: /* READ_QUADIO */
        ot_spi_device_flash_decode_read_data(s);
        break;
    default:
        g_assert_not_reached();
    }
}

static void ot_spi_device_flash_exec_read_sfdp(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    unsigned address = ldl_be_p(f->src);
    address &= (1u << 24u) - 1u; /* discard dummy byte */
    f->pos = address % SPI_SRAM_SFDP_SIZE;
    f->len = SPI_SRAM_SFDP_SIZE;
    f->src = &((uint8_t *)s->sram)[SPI_SRAM_SFDP_OFFSET];
    f->loop = true;
    FLASH_CHANGE_STATE(f, BUFFER);
}

static void ot_spi_device_flash_exec_read_data(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    uint32_t address = ldl_be_p(f->buffer);
    if (!ot_spi_device_is_addr4b_en(s)) {
        address >>= 8u;
    }

    trace_ot_spi_device_flash_set_read_addr(address);

    f->address = address;
    FLASH_CHANGE_STATE(f, READ);

    f->src = (uint8_t *)s->sram;
    f->loop = true;
}

static void ot_spi_device_exec_command(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    switch (COMMAND_OPCODE(f->cmd_info)) {
    case 0x5Au: /* READ_SFDP */
        ot_spi_device_flash_exec_read_sfdp(s);
        break;
    case 0x03u: /* READ_NORMAL */
    case 0x0bu: /* READ_FAST   */
    case 0x3bu: /* READ_DUAL   */
    case 0x6bu: /* READ_QUAD   */
    case 0xbbu: /* READ_DUALIO */
    case 0xebu: /* READ_QUADIO */
        ot_spi_device_flash_exec_read_data(s);
        break;
    default:
        g_assert_not_reached();
    }
}

static uint8_t ot_spi_device_flash_exec_hw_cfg_command(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    uint8_t tx = SPI_DEFAULT_TX_VALUE;
    unsigned cmdinfo = f->slot - (R_CMD_INFO_EN4B - R_CMD_INFO_0);

    switch (cmdinfo) {
    case 0: /* EN4B (typ. 0xB7) */
    case 1u: /* EX4B (typ. 0xE9u) */
        ot_spi_device_flash_decode_addr4_enable(s);
        break;
    case 2u: /* WREN (typ. 0x06u) */
    case 3u: /* WRDI (typ. 0x04u) */
        ot_spi_device_flash_decode_write_enable(s);
        break;
    default:
        error_setg(&error_fatal, "%s: invalid command info %u %u", __func__,
                   f->slot, cmdinfo);
        g_assert_not_reached();
    }

    return tx;
}

static bool ot_spi_device_flash_collect(OtSPIDeviceState *s, uint8_t rx)
{
    SpiDeviceFlash *f = &s->flash;

    g_assert(f->src);

    f->src[f->pos++] = rx;

    return f->pos != f->len;
}

static uint8_t ot_spi_device_flash_read_buffer(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    g_assert(f->src);

    uint8_t tx = (f->pos < f->len) ? f->src[f->pos] : SPI_DEFAULT_TX_VALUE;

    f->pos++;
    if (f->pos >= f->len) {
        if (f->loop) {
            f->pos = 0;
        } else {
            FLASH_CHANGE_STATE(f, DONE);
        }
    }

    return tx;
}

static uint8_t ot_spi_device_flash_read_data(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    g_assert(f->src);

    bool pace_spibus = false;
    uint8_t tx;

    f->pos = f->address & (FLASH_READ_BUFFER_SIZE - 1u);

    if (ot_spi_device_is_mailbox_match(s, f->address)) {
        /*
         * Sequencing is the very same whether mailbox is matched or not,
         * otherwise, readbuf event would not be emitted, pages would not
         * be reloaded and HW buffer not refilled by the FW for the pages that
         * follow the mailbox (address-wide).
         * Not sure this is how the HW actually works, and there is no SW
         * example that fully demontrates how the mailbox vs. regular pages are
         * supposed to work.
         * The current implementation therefore only subsitutes the SPI MISO
         * value, but acts exactly as if the virtual flash pages where used.
         * This might be right or wrong.
         */
        uint8_t *src = f->src + SPI_SRAM_MBX_OFFSET;
        unsigned pos = f->address & (SPI_SRAM_MBX_SIZE - 1u);
        tx = src[pos];
    } else {
        uint8_t *src = f->src + SPI_SRAM_READ0_OFFSET;
        tx = src[f->pos];
    }

    uint32_t threshold = s->spi_regs[R_READ_THRESHOLD];
    /* "If 0, disable the watermark."" */
    if (threshold) {
        uint32_t lowaddr = f->address & (SPI_SRAM_READ_SIZE - 1u);

        /* "when the host access above or equal to the threshold" */
        if (lowaddr >= threshold) {
            if (!f->watermark) {
                trace_ot_spi_device_flash_read_threshold(f->address, threshold);
                s->spi_regs[R_INTR_STATE] |= INTR_READBUF_WATERMARK_MASK;
                pace_spibus = true;
                ot_spi_device_update_irqs(s);
            }
            /* should be reset on buffer switch */
            f->watermark = true;
        }
    }

    f->address += 1u;

    /*
     * "If a new read command crosses the current buffer boundary, the SW clears
     *  the cross event for the HW to detect the address cross event again."
     */
    bool flip = (f->address & (SPI_SRAM_READ_SIZE - 1u)) == 0;
    if (flip) {
        f->watermark = false;
        s->spi_regs[R_INTR_STATE] |= INTR_READBUF_FLIP_MASK;
        trace_ot_spi_device_flash_cross_buffer("run", f->address);
        pace_spibus = true;
        ot_spi_device_update_irqs(s);
    }

    if (pace_spibus) {
        ot_spi_device_flash_pace_spibus(s);
    }

    return tx;
}

static void ot_spi_device_flash_init_payload(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    f->pos = 0;
    f->len = SPI_SRAM_PAYLOAD_SIZE;
    s->spi_regs[R_UPLOAD_STATUS2] = 0;
    g_assert(f->payload);
    FLASH_CHANGE_STATE(f, UP_PAYLOAD);
}

static void ot_spi_device_flash_decode_sw_command(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    unsigned addr_count;
    uint32_t addr_mode = SHARED_FIELD_EX32(f->cmd_info, CMD_INFO_ADDR_MODE);
    switch ((int)addr_mode) {
    case ADDR_MODE_ADDRDISABLED:
        addr_count = 0;
        break;
    case ADDR_MODE_ADDRCFG:
        addr_count = ot_spi_device_is_addr4b_en(s) ? 4u : 3u;
        break;
    case ADDR_MODE_ADDR3B:
        addr_count = 3u;
        break;
    case ADDR_MODE_ADDR4B:
        addr_count = 4u;
        break;
    default:
        g_assert_not_reached();
        break;
    }

    f->pos = 0;
    if (addr_count != 0) {
        f->len = addr_count;
        FLASH_CHANGE_STATE(f, UP_ADDR);
    } else if (f->cmd_info & CMD_INFO_DUMMY_EN_MASK) {
        f->len = 1u;
        FLASH_CHANGE_STATE(f, UP_DUMMY);
    } else if (ot_spi_device_flash_has_input_payload(f->cmd_info)) {
        ot_spi_device_flash_init_payload(s);
    }
}

static void ot_spi_device_flash_exec_sw_command(OtSPIDeviceState *s, uint8_t rx)
{
    SpiDeviceFlash *f = &s->flash;

    switch (f->state) {
    case SPI_FLASH_UP_ADDR:
        if (f->pos < f->len) {
            f->buffer[f->pos] = rx;
        }
        f->pos++;
        if (f->pos == f->len) {
            f->address = ldl_be_p(f->buffer);
            if (!ot_spi_device_is_addr4b_en(s)) {
                f->address >>= 8u;
            }
            if (!ot_fifo32_is_full(&f->address_fifo)) {
                trace_ot_spi_device_flash_push_address(f->address);
                ot_fifo32_push(&f->address_fifo, f->address);
            } else {
                /* waiting for answer from OT team here */
                g_assert_not_reached();
            }
            if (f->cmd_info & CMD_INFO_DUMMY_EN_MASK) {
                f->len = 1u;
                FLASH_CHANGE_STATE(f, UP_DUMMY);
            } else if (ot_spi_device_flash_has_input_payload(f->cmd_info)) {
                ot_spi_device_flash_init_payload(s);
            } else {
                FLASH_CHANGE_STATE(f, DONE);
            }
        }
        break;
    case SPI_FLASH_UP_DUMMY:
        f->pos++;
        g_assert(f->pos == f->len);
        if (ot_spi_device_flash_has_input_payload(f->cmd_info)) {
            ot_spi_device_flash_init_payload(s);
        } else {
            FLASH_CHANGE_STATE(f, DONE);
        }
        break;
    case SPI_FLASH_UP_PAYLOAD:
        f->payload[f->pos % SPI_SRAM_PAYLOAD_SIZE] = rx;
        f->pos++;
        break;
    case SPI_FLASH_DONE:
        FLASH_CHANGE_STATE(f, ERROR);
    /* fallthrough */
    case SPI_FLASH_ERROR:
        trace_ot_spi_device_flash_byte_unexpected(rx);
        BUS_CHANGE_STATE(&s->bus, DISCARD);
        break;
    case SPI_FLASH_COLLECT:
    case SPI_FLASH_BUFFER:
    case SPI_FLASH_READ:
    default:
        g_assert_not_reached();
        break;
    }
}

static uint8_t ot_spi_device_flash_transfer(OtSPIDeviceState *s, uint8_t rx)
{
    SpiDeviceFlash *f = &s->flash;

    (void)rx;

    uint8_t tx = SPI_DEFAULT_TX_VALUE;

    switch (f->state) {
    case SPI_FLASH_IDLE:
        f->slot = UINT_MAX;
        f->pos = 0;
        f->len = 0;
        f->src = NULL;
        f->loop = false;
        f->type = SPI_FLASH_CMD_NONE;
        ot_spi_device_flash_decode_command(s, rx);
        switch (f->type) {
        case SPI_FLASH_CMD_HW_STA:
            ot_spi_device_flash_decode_hw_static_command(s);
            break;
        case SPI_FLASH_CMD_HW_CFG:
            ot_spi_device_flash_exec_hw_cfg_command(s);
            break;
        case SPI_FLASH_CMD_SW:
            ot_spi_device_flash_decode_sw_command(s);
            break;
        case SPI_FLASH_CMD_NONE:
            /* this command cannot be processed, discard all remaining bytes */
            FLASH_CHANGE_STATE(f, ERROR);
            BUS_CHANGE_STATE(&s->bus, DISCARD);
            break;
        default:
            g_assert_not_reached();
        }
        break;
    case SPI_FLASH_COLLECT:
        if (!ot_spi_device_flash_collect(s, rx)) {
            ot_spi_device_exec_command(s);
        }
        break;
    case SPI_FLASH_BUFFER:
        tx = ot_spi_device_flash_read_buffer(s);
        break;
    case SPI_FLASH_READ:
        tx = ot_spi_device_flash_read_data(s);
        break;
    case SPI_FLASH_UP_ADDR:
    case SPI_FLASH_UP_DUMMY:
    case SPI_FLASH_UP_PAYLOAD:
        ot_spi_device_flash_exec_sw_command(s, rx);
        break;
    case SPI_FLASH_DONE:
        FLASH_CHANGE_STATE(f, ERROR);
        break;
    case SPI_FLASH_ERROR:
        break;
    default:
        error_setg(&error_fatal, "%s: unexpected state %s[%d]\n", __func__,
                   FLASH_STATE_NAME(f->state), f->state);
        g_assert_not_reached();
    }

    return tx;
}

static void ot_spi_device_flash_resume_read(void *opaque)
{
    OtSPIDeviceState *s = opaque;

    trace_ot_spi_device_flash_pace("release",
                                   timer_pending(s->flash.irq_timer));
    qemu_chr_fe_accept_input(&s->chr);
}

static uint64_t
ot_spi_device_spi_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtSPIDeviceState *s = opaque;
    (void)size;
    SpiDeviceFlash *f = &s->flash;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_CONTROL:
    case R_CFG:
    case R_FIFO_LEVEL:
    case R_ASYNC_FIFO_LEVEL:
    case R_RXF_PTR:
    case R_TXF_PTR:
    case R_RXF_ADDR:
    case R_TXF_ADDR:
    case R_INTERCEPT_EN:
    case R_LAST_READ_ADDR:
    case R_FLASH_STATUS:
    case R_JEDEC_CC:
    case R_JEDEC_ID:
    case R_READ_THRESHOLD:
    case R_MAILBOX_ADDR:
    case R_UPLOAD_STATUS2:
    case R_CMD_FILTER_0:
    case R_CMD_FILTER_1:
    case R_CMD_FILTER_2:
    case R_CMD_FILTER_3:
    case R_CMD_FILTER_4:
    case R_CMD_FILTER_5:
    case R_CMD_FILTER_6:
    case R_CMD_FILTER_7:
    case R_ADDR_SWAP_MASK:
    case R_ADDR_SWAP_DATA:
    case R_PAYLOAD_SWAP_MASK:
    case R_PAYLOAD_SWAP_DATA:
    case R_CMD_INFO_0:
    case R_CMD_INFO_1:
    case R_CMD_INFO_2:
    case R_CMD_INFO_3:
    case R_CMD_INFO_4:
    case R_CMD_INFO_5:
    case R_CMD_INFO_6:
    case R_CMD_INFO_7:
    case R_CMD_INFO_8:
    case R_CMD_INFO_9:
    case R_CMD_INFO_10:
    case R_CMD_INFO_11:
    case R_CMD_INFO_12:
    case R_CMD_INFO_13:
    case R_CMD_INFO_14:
    case R_CMD_INFO_15:
    case R_CMD_INFO_16:
    case R_CMD_INFO_17:
    case R_CMD_INFO_18:
    case R_CMD_INFO_19:
    case R_CMD_INFO_20:
    case R_CMD_INFO_21:
    case R_CMD_INFO_22:
    case R_CMD_INFO_23:
    case R_CMD_INFO_EN4B:
    case R_CMD_INFO_EX4B:
    case R_CMD_INFO_WREN:
    case R_CMD_INFO_WRDI:
        val32 = s->spi_regs[reg];
        break;
    case R_STATUS:
        val32 = ot_spi_device_get_status(s);
        break;
    case R_UPLOAD_STATUS:
        val32 = 0;
        val32 = FIELD_DP32(val32, UPLOAD_STATUS, CMDFIFO_DEPTH,
                           fifo8_num_used(&f->cmd_fifo));
        val32 = FIELD_DP32(val32, UPLOAD_STATUS, CMDFIFO_NOTEMPTY,
                           !fifo8_is_empty(&f->cmd_fifo));
        val32 = FIELD_DP32(val32, UPLOAD_STATUS, ADDRFIFO_DEPTH,
                           ot_fifo32_num_used(&f->address_fifo));
        val32 = FIELD_DP32(val32, UPLOAD_STATUS, ADDRFIFO_NOTEMPTY,
                           !ot_fifo32_is_empty(&f->address_fifo));
        break;
    case R_UPLOAD_CMDFIFO:
        if (!fifo8_is_empty(&f->cmd_fifo)) {
            val32 = (uint32_t)fifo8_pop(&f->cmd_fifo);
        } else {
            qemu_log_mask(LOG_UNIMP, "%s: CMD_FIFO is empty\n", __func__);
            val32 = 0;
        }
        break;
    case R_UPLOAD_ADDRFIFO:
        if (!ot_fifo32_is_empty(&f->address_fifo)) {
            val32 = ot_fifo32_pop(&f->address_fifo);
        } else {
            qemu_log_mask(LOG_UNIMP, "%s: ADDR_FIFO is empty\n", __func__);
            val32 = 0;
        }
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: W/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, SPI_REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_spi_device_io_spi_read_out((unsigned)addr, SPI_REG_NAME(reg),
                                        (uint64_t)val32, pc);

    return (uint64_t)val32;
};

static void ot_spi_device_spi_regs_write(void *opaque, hwaddr addr,
                                         uint64_t val64, unsigned size)
{
    OtSPIDeviceState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_spi_device_io_spi_write_in((unsigned)addr, SPI_REG_NAME(reg),
                                        val64, pc);

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK & ~(INTR_TPM_HEADER_NOT_EMPTY_MASK);
        s->spi_regs[reg] &= ~val32; /* RW1C */
        ot_spi_device_update_irqs(s);
        if (!ot_spi_device_flash_is_readbuf_irq(s)) {
            /* no need to trigger the timer if readbuf IRQs have been cleared */
            trace_ot_spi_device_flash_pace("clear",
                                           timer_pending(s->flash.irq_timer));
            timer_del(s->flash.irq_timer);
            qemu_chr_fe_accept_input(&s->chr);
        }
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->spi_regs[reg] = val32;
        ot_spi_device_update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->spi_regs[R_INTR_STATE] |= val32;
        ot_spi_device_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        if (val32) {
            for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
                ibex_irq_set(&s->alerts[ix], (int)((val32 >> ix) & 0x1u));
            }
        }
        break;
    case R_CONTROL:
        val32 &= CONTROL_MASK;
        if ((val32 & R_CONTROL_MODE_MASK) !=
            (s->spi_regs[reg] & R_CONTROL_MODE_MASK)) {
            ot_spi_device_clear_modes(s);
        }
        s->spi_regs[reg] = val32;
        switch (ot_spi_device_get_mode(s)) {
        case CTRL_MODE_FW:
            if (!s->dpsram) {
                qemu_log_mask(LOG_UNIMP, "%s: generic mode disabled\n",
                              __func__);
            }
            break;
        case CTRL_MODE_FLASH:
            break;
        case CTRL_MODE_PASSTHROUGH:
        default:
            qemu_log_mask(LOG_UNIMP, "%s: unsupported mode\n", __func__);
            break;
        }
        if (val32 & R_CONTROL_ABORT_MASK) {
            /* however, TXFIFO is unlikely to block */
            qemu_log_mask(LOG_UNIMP, "%s: abort unsupported\n", __func__);
            break;
        }
        if (val32 & R_CONTROL_RST_RXFIFO_MASK) {
            fifo8_reset(&s->generic.rx_fifo);
        }
        if (val32 & R_CONTROL_RST_TXFIFO_MASK) {
            fifo8_reset(&s->generic.tx_fifo);
        }
        break;
    case R_CFG:
        val32 &= CFG_MASK;
        s->spi_regs[reg] = val32;
        break;
    case R_RXF_PTR:
        s->spi_regs[reg] &= R_RXF_PTR_WPTR_MASK;
        val32 &= R_RXF_PTR_RPTR_MASK;
        s->spi_regs[reg] |= val32;
        break;
    case R_TXF_PTR:
        s->spi_regs[reg] &= R_RXF_PTR_RPTR_MASK;
        val32 &= R_TXF_PTR_WPTR_MASK;
        s->spi_regs[reg] |= val32;
        break;
    case R_INTERCEPT_EN:
        val32 &= INTERCEPT_EN_MASK;
        s->spi_regs[reg] = val32;
        break;
    case R_FLASH_STATUS:
        s->spi_regs[reg] &= val32 & R_FLASH_STATUS_BUSY_MASK; /* RW0C */
        s->spi_regs[reg] |= val32 & FLASH_STATUS_STATUS_MASK; /* RW */
        break;
    case R_JEDEC_CC:
        val32 &= JEDEC_CC_MASK;
        s->spi_regs[reg] = val32;
        break;
    case R_JEDEC_ID:
        val32 &= JEDEC_ID_MASK;
        s->spi_regs[reg] = val32;
        break;
    case R_READ_THRESHOLD:
        val32 &= R_READ_THRESHOLD_THRESHOLD_MASK;
        s->spi_regs[reg] = val32;
        break;
    case R_RXF_ADDR:
    case R_TXF_ADDR:
        val32 &= 0xfffcfffcu;
        if ((val32 >> 16u) >= (val32 & UINT16_MAX)) {
            s->spi_regs[reg] = val32;
        } else {
            /*
             * not sure about the HW behavior, but easier to discard here rather
             * than testing each time a FIFO is used
             */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: invalid limit/base for %s, ignoring\n", __func__,
                          SPI_REG_NAME(reg));
        }
        break;
    case R_FIFO_LEVEL:
    case R_LAST_READ_ADDR:
    case R_MAILBOX_ADDR:
    case R_CMD_FILTER_0:
    case R_CMD_FILTER_1:
    case R_CMD_FILTER_2:
    case R_CMD_FILTER_3:
    case R_CMD_FILTER_4:
    case R_CMD_FILTER_5:
    case R_CMD_FILTER_6:
    case R_CMD_FILTER_7:
    case R_ADDR_SWAP_MASK:
    case R_ADDR_SWAP_DATA:
    case R_PAYLOAD_SWAP_MASK:
    case R_PAYLOAD_SWAP_DATA:
        s->spi_regs[reg] = val32;
        break;
    case R_CMD_INFO_0:
    case R_CMD_INFO_1:
    case R_CMD_INFO_2:
    case R_CMD_INFO_3:
    case R_CMD_INFO_4:
    case R_CMD_INFO_5:
    case R_CMD_INFO_6:
    case R_CMD_INFO_7:
    case R_CMD_INFO_8:
    case R_CMD_INFO_9:
    case R_CMD_INFO_10:
    case R_CMD_INFO_11:
    case R_CMD_INFO_12:
    case R_CMD_INFO_13:
    case R_CMD_INFO_14:
    case R_CMD_INFO_15:
    case R_CMD_INFO_16:
    case R_CMD_INFO_17:
    case R_CMD_INFO_18:
    case R_CMD_INFO_19:
    case R_CMD_INFO_20:
    case R_CMD_INFO_21:
    case R_CMD_INFO_22:
    case R_CMD_INFO_23:
        val32 &= CMD_INFO_GEN_MASK;
        s->spi_regs[reg] = val32;
        break;
    case R_CMD_INFO_EN4B:
    case R_CMD_INFO_EX4B:
    case R_CMD_INFO_WREN:
    case R_CMD_INFO_WRDI:
        val32 &= CMD_INFO_SPC_MASK;
        s->spi_regs[reg] = val32;
        break;
    case R_ASYNC_FIFO_LEVEL:
    case R_STATUS:
    case R_UPLOAD_STATUS:
    case R_UPLOAD_STATUS2:
    case R_UPLOAD_CMDFIFO:
    case R_UPLOAD_ADDRFIFO:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: R/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, SPI_REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static uint64_t
ot_spi_device_tpm_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtSPIDeviceState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_TPM_CAP:
    case R_TPM_CFG:
    case R_TPM_STATUS:
    case R_TPM_ACCESS_0:
    case R_TPM_ACCESS_1:
    case R_TPM_STS:
    case R_TPM_INTF_CAPABILITY:
    case R_TPM_INT_ENABLE:
    case R_TPM_INT_VECTOR:
    case R_TPM_INT_STATUS:
    case R_TPM_DID_VID:
    case R_TPM_RID:
    case R_TPM_CMD_ADDR:
    case R_TPM_WRITE_FIFO:
        qemu_log_mask(LOG_UNIMP, "%s: %s: not supported\n", __func__,
                      TPM_REG_NAME(reg));
        val32 = s->tpm_regs[reg];
        break;
    case R_TPM_READ_FIFO:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: W/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, SPI_REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_spi_device_io_tpm_read_out((unsigned)addr, TPM_REG_NAME(reg),
                                        (uint64_t)val32, pc);

    return (uint64_t)val32;
};

static void ot_spi_device_tpm_regs_write(void *opaque, hwaddr addr,
                                         uint64_t val64, unsigned size)
{
    OtSPIDeviceState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_spi_device_io_tpm_write_in((unsigned)addr, TPM_REG_NAME(reg),
                                        val64, pc);

    switch (reg) {
    case R_TPM_CFG:
    case R_TPM_ACCESS_0:
    case R_TPM_ACCESS_1:
    case R_TPM_STS:
    case R_TPM_INTF_CAPABILITY:
    case R_TPM_INT_ENABLE:
    case R_TPM_INT_VECTOR:
    case R_TPM_INT_STATUS:
    case R_TPM_DID_VID:
    case R_TPM_RID:
    case R_TPM_READ_FIFO:
        qemu_log_mask(LOG_UNIMP, "%s: %s: not supported\n", __func__,
                      TPM_REG_NAME(reg));
        s->tpm_regs[reg] = val32;
        break;
    case R_TPM_CAP:
    case R_TPM_STATUS:
    case R_TPM_CMD_ADDR:
    case R_TPM_WRITE_FIFO:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: R/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, TPM_REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static MemTxResult ot_spi_device_buf_read_with_attrs(
    void *opaque, hwaddr addr, uint64_t *val64, unsigned size, MemTxAttrs attrs)
{
    OtSPIDeviceState *s = opaque;
    (void)attrs;
    uint32_t val32;

    hwaddr last = addr + size - 1u;

    if (s->dpsram) {
        if (last >= SRAM_SIZE) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad buffer offset 0x%" HWADDR_PRIx "\n",
                          __func__, addr);
            return MEMTX_DECODE_ERROR;
        }

        if (last >= SPI_SRAM_SFDP_OFFSET + SPI_SRAM_SFDP_SIZE) {
            if (last < SPI_SRAM_ADDR_OFFSET) {
                /* command FIFO */
                val32 = ((const uint32_t *)s->flash.cmd_fifo.data)[addr >> 2u];
            } else if (last < SPI_SRAM_ADDR_END) {
                /* address FIFO */
                val32 = s->flash.address_fifo.data[addr >> 2u];
            } else {
                val32 = s->sram[addr >> 2u];
            }
        } else {
            val32 = s->sram[addr >> 2u];
        }

    } else {
        if (last < SPI_SRAM_PAYLOAD_OFFSET + SPI_SRAM_INGRESS_OFFSET) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: cannot read egress buffer 0x%" HWADDR_PRIx "\n",
                          __func__, addr);
            return MEMTX_DECODE_ERROR;
        }
        if (last < SPI_SRAM_CMD_OFFSET + SPI_SRAM_INGRESS_OFFSET) {
            /* payload buffer */
            val32 = s->sram[addr >> 2u];
        } else if (last < SPI_SRAM_ADDR_OFFSET + SPI_SRAM_INGRESS_OFFSET) {
            /* command FIFO */
            val32 = ((const uint32_t *)s->flash.cmd_fifo.data)[addr >> 2u];
        } else if (last < SPI_SRAM_ADDR_END + SPI_SRAM_INGRESS_OFFSET) {
            /* address FIFO */
            val32 = s->flash.address_fifo.data[addr >> 2u];
        } else {
            /* TPM or not used area */
            qemu_log_mask(LOG_UNIMP,
                          "%s: TPM not supported 0x%" HWADDR_PRIx "\n",
                          __func__, addr);
            val32 = 0;
        }
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_spi_device_buf_read_out((unsigned)addr, size, (uint64_t)val32, pc);

    *val64 = (uint64_t)val32;

    return MEMTX_OK;
}

static MemTxResult ot_spi_device_buf_write_with_attrs(
    void *opaque, hwaddr addr, uint64_t val64, unsigned size, MemTxAttrs attrs)
{
    OtSPIDeviceState *s = opaque;
    (void)attrs;

    uint64_t pc = ibex_get_current_pc();
    trace_ot_spi_device_buf_write_in((unsigned)addr, size, val64, pc);

    hwaddr last = addr + size - 1u;

    if (s->dpsram) {
        if (last >= SRAM_SIZE) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad buffer offset 0x%" HWADDR_PRIx "\n",
                          __func__, addr);
            return MEMTX_DECODE_ERROR;
        }

        s->sram[addr >> 2u] = (uint32_t)val64;
    } else {
        if (last >= SPI_SRAM_PAYLOAD_OFFSET) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: cannot write ingress buffer 0x%" HWADDR_PRIx
                          "\n",
                          __func__, addr);
            return MEMTX_DECODE_ERROR;
        }
        s->sram[addr >> 2u] = (uint32_t)val64;
    }

    return MEMTX_OK;
}

static void ot_spi_device_chr_handle_header(OtSPIDeviceState *s)
{
    SpiDeviceBus *bus = &s->bus;

    uint32_t size = 0;
    const uint8_t *hdr =
        fifo8_pop_buf(&bus->chr_fifo, SPI_BUS_HEADER_SIZE, &size);

    if (size != SPI_BUS_HEADER_SIZE) {
        trace_ot_spi_device_chr_error("invalid header size");
        BUS_CHANGE_STATE(bus, ERROR);
        return;
    }

    if (hdr[0] != '/' || hdr[1u] != 'C' || hdr[2u] != 'S' ||
        hdr[3u] != SPI_BUS_PROTO_VER) {
        trace_ot_spi_device_chr_error("invalid header");
        BUS_CHANGE_STATE(bus, ERROR);
        return;
    }

    unsigned word = ldl_le_p(&hdr[4u]);
    bus->byte_count = word >> 16u;
    uint8_t mode = word & 0xfu;
    bus->release = !((word >> 7) & 0x1);

    bus->rev_rx = (bool)(mode & R_CFG_RX_ORDER_MASK);
    bus->rev_tx = (bool)(mode & R_CFG_TX_ORDER_MASK);
    /* if phase or polarity does not match, corrupt data */
    uint8_t comm = mode ^ (uint8_t)s->spi_regs[R_CFG];
    bus->mode = (comm & (R_CFG_CPOL_MASK | R_CFG_CPHA_MASK)) ? 0xFF : 0x00;

    trace_ot_spi_device_chr_cs_assert(bus->byte_count, bus->release,
                                      bus->rev_rx ? 'l' : 'm',
                                      bus->rev_tx ? 'l' : 'm',
                                      bus->mode ? "mismatch" : "ok");

    if (!bus->byte_count) {
        /* no payload, stay in IDLE */
        return;
    }

    switch (ot_spi_device_get_mode(s)) {
    case CTRL_MODE_FW:
        BUS_CHANGE_STATE(bus, GENERIC);
        break;
    case CTRL_MODE_FLASH:
        BUS_CHANGE_STATE(bus, FLASH);
        break;
    case CTRL_MODE_PASSTHROUGH:
    default:
        BUS_CHANGE_STATE(bus, DISCARD);
        break;
    }
}

static void ot_spi_device_chr_send_discard(OtSPIDeviceState *s, unsigned count)
{
    const uint8_t buf[1u] = { (uint8_t)0xffu };

    while (count--) {
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_write(&s->chr, buf, (int)sizeof(buf));
        }
    }
}

static void ot_spi_device_chr_recv_discard(OtSPIDeviceState *s,
                                           const uint8_t *buf, unsigned size)
{
    (void)buf;

    ot_spi_device_chr_send_discard(s, size);
}

static void ot_spi_device_chr_recv_flash(OtSPIDeviceState *s,
                                         const uint8_t *buf, unsigned size)
{
    SpiDeviceBus *bus = &s->bus;
    while (size) {
        uint8_t rx = *buf++ ^ bus->mode;
        if (bus->rev_rx) {
            rx = revbit8(rx);
        }
        uint8_t tx = ot_spi_device_flash_transfer(s, rx) ^ bus->mode;
        if (bus->rev_tx) {
            tx = revbit8(tx);
        }
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_write(&s->chr, &tx, (int)sizeof(tx));
        }
        bus->byte_count--;
        size--;
    }
}

static void ot_spi_device_chr_send_generic(OtSPIDeviceState *s, unsigned count)
{
    if (ot_spi_device_is_tx_fifo_in_reset(s)) {
        uint8_t buf[] = { 0xff };
        trace_ot_spi_device_gen_fifo_error("TXF in reset");
        while (count--) {
            qemu_chr_fe_write(&s->chr, buf, sizeof(buf));
        }
        return;
    }

    SpiDeviceGeneric *g = &s->generic;

    while (count) {
        uint8_t buf[TXFIFO_LEN];
        unsigned len = 0;
        while (len < TXFIFO_LEN && len < count) {
            if (fifo8_is_empty(&g->tx_fifo)) {
                break;
            }
            buf[len++] = fifo8_pop(&g->tx_fifo);
        }
        if (len && qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_write(&s->chr, buf, (int)len);
        }
        count -= len;
        g_assert(fifo8_is_empty(&g->tx_fifo));
        unsigned rem = count;
        while (rem && !fifo8_is_full(&g->tx_fifo)) {
            if (!spi_fifo_is_empty(&g->txf)) {
                fifo8_push(&g->tx_fifo, spi_fifo_pop(&g->txf));
                trace_ot_spi_device_gen_update_fifo("txf", __LINE__,
                                                    *g->txf.ptr);
            } else {
                trace_ot_spi_device_gen_fifo_error("TXF underflow");
                fifo8_push(&g->tx_fifo, 0xFF); /* "lingering data" */
            }
            rem--;
        }
    }

    if (spi_fifo_num_used(&g->txf) < ot_spi_device_txf_threshold(s)) {
        s->spi_regs[R_INTR_STATE] |= INTR_GENERIC_TX_WATERMARK_MASK;
        ot_spi_device_update_irqs(s);
    }
}

static void ot_spi_device_chr_recv_generic(OtSPIDeviceState *s,
                                           const uint8_t *buf, unsigned size)
{
    SpiDeviceGeneric *g = &s->generic;
    SpiDeviceBus *bus = &s->bus;

    unsigned count = size;

    timer_del(g->rx_timer);
    unsigned bcount = spi_fifo_count_to_word(&g->rxf);
    g_assert(bcount <= fifo8_num_free(&g->rx_fifo));
    bool rx_ignore = ot_spi_device_is_rx_fifo_in_reset(s);
    /* cpol, cpha, bit order not handled in generic mode, as it is deprecated */
    while (bcount && count) {
        if (!rx_ignore) {
            fifo8_push(&g->rx_fifo, *buf);
        }
        buf++;
        count--;
        bcount--;
    }
    if (!bcount) {
        while (!fifo8_is_empty(&g->rx_fifo)) {
            g_assert(!spi_fifo_is_full(&g->rxf));
            if (!rx_ignore) {
                spi_fifo_push(&g->rxf, fifo8_pop(&g->rx_fifo));
                trace_ot_spi_device_gen_update_fifo("rxf", __LINE__,
                                                    *g->rxf.ptr);
            }
        }
    }
    while (count >= sizeof(uint32_t)) {
        /* bypass RXFIFO */
        uint32_t word = ldl_le_p(buf);
        buf += sizeof(word);
        count -= sizeof(word);
        g_assert(spi_fifo_num_free(&g->rxf) >= sizeof(word));
        if (!rx_ignore) {
            spi_fifo_push_w(&g->rxf, word);
            trace_ot_spi_device_gen_update_fifo("rxf", __LINE__, *g->rxf.ptr);
        }
    }
    while (count) {
        g_assert(!fifo8_is_full(&g->rx_fifo));
        if (!rx_ignore) {
            fifo8_push(&g->rx_fifo, *buf++);
            trace_ot_spi_device_gen_update_fifo("rxf", __LINE__, *g->rxf.ptr);
        }
        count--;
    }
    if (!fifo8_is_empty(&g->rx_fifo) && bus->byte_count) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        /* todo: use R_CFG_TIMER_V field to change the timeout */
        timer_mod(g->rx_timer, (int64_t)(now + SPI_BUS_TIMEOUT_NS));
    }

    if (spi_fifo_num_used(&g->rxf) > ot_spi_device_rxf_threshold(s)) {
        s->spi_regs[R_INTR_STATE] |= INTR_GENERIC_RX_WATERMARK_MASK;
    }

    if (spi_fifo_is_full(&g->rxf)) {
        s->spi_regs[R_INTR_STATE] |= INTR_GENERIC_RX_FULL_MASK;
    }

    ot_spi_device_update_irqs(s);

    unsigned tx_size;
    if ((unsigned)size <= bus->byte_count) {
        tx_size = size;
        bus->byte_count -= size;
    } else {
        trace_ot_spi_device_chr_error("packet overflow");
        tx_size = bus->byte_count;
        bus->byte_count = 0;
    }

    ot_spi_device_chr_send_generic(s, tx_size);
}

static void ot_spi_device_recv_generic_timeout(void *opaque)
{
    OtSPIDeviceState *s = opaque;
    SpiDeviceGeneric *g = &s->generic;
    SpiDeviceBus *bus = &s->bus;

    trace_ot_spi_device_gen_rx_timeout(fifo8_num_used(&g->rx_fifo));

    bool rx_ignore = ot_spi_device_is_rx_fifo_in_reset(s);
    while (!fifo8_is_empty(&g->rx_fifo)) {
        uint8_t byte = fifo8_pop(&g->rx_fifo);
        bus->byte_count -= sizeof(uint8_t);
        if (!rx_ignore) {
            spi_fifo_push(&g->rxf, byte);
            trace_ot_spi_device_gen_update_fifo("rxf", __LINE__, *g->rxf.ptr);
        }
    }
}

static int ot_spi_device_chr_can_receive(void *opaque)
{
    OtSPIDeviceState *s = opaque;
    SpiDeviceBus *bus = &s->bus;
    unsigned length;

    switch (bus->state) {
    case SPI_BUS_IDLE:
        length = fifo8_num_free(&bus->chr_fifo);
        break;
    case SPI_BUS_GENERIC:
        length = fifo8_num_free(&s->generic.rx_fifo);
        break;
    case SPI_BUS_FLASH:
        length = timer_pending(s->flash.irq_timer) ? 0 : 1u;
        break;
    case SPI_BUS_DISCARD:
        length = 1u;
        break;
    case SPI_BUS_ERROR:
        length = 0;
        break;
    default:
        error_setg(&error_fatal, "%s: unexpected state %d\n", __func__,
                   bus->state);
        /* linter does not know error_setg never returns */
        g_assert_not_reached();
    }

    return (int)length;
}

static void ot_spi_device_chr_receive(void *opaque, const uint8_t *buf,
                                      int size)
{
    OtSPIDeviceState *s = opaque;
    SpiDeviceBus *bus = &s->bus;

    switch (bus->state) {
    case SPI_BUS_IDLE:
        g_assert(size <= fifo8_num_free(&bus->chr_fifo));
        while (size--) {
            fifo8_push(&bus->chr_fifo, *buf++);
        }
        if (fifo8_is_full(&bus->chr_fifo)) {
            ot_spi_device_chr_handle_header(s);
        }
        break;
    case SPI_BUS_GENERIC:
        if (s->dpsram) {
            ot_spi_device_chr_recv_generic(s, buf, (unsigned)size);
        }
        break;
    case SPI_BUS_FLASH:
        ot_spi_device_chr_recv_flash(s, buf, (unsigned)size);
        break;
    case SPI_BUS_DISCARD:
    case SPI_BUS_ERROR:
        ot_spi_device_chr_recv_discard(s, buf, (unsigned)size);
        break;
    default:
        g_assert_not_reached();
        break;
    }

    if (!bus->byte_count) {
        if (bus->release) {
            ot_spi_device_release_cs(s);
        } else {
            BUS_CHANGE_STATE(bus, IDLE);
        }
    }
}

static void ot_spi_device_chr_event_hander(void *opaque, QEMUChrEvent event)
{
    OtSPIDeviceState *s = opaque;

    if (event == CHR_EVENT_OPENED) {
        if (object_dynamic_cast(OBJECT(s->chr.chr), TYPE_CHARDEV_SERIAL)) {
            ot_common_ignore_chr_status_lines(&s->chr);
        }

        if (!qemu_chr_fe_backend_connected(&s->chr)) {
            return;
        }

        ot_spi_device_release_cs(s);
    }

    if (event == CHR_EVENT_CLOSED) {
        ot_spi_device_release_cs(s);
    }
}

static gboolean
ot_spi_device_chr_watch_cb(void *do_not_use, GIOCondition cond, void *opaque)
{
    OtSPIDeviceState *s = opaque;
    (void)do_not_use;
    (void)cond;

    s->watch_tag = 0;

    return FALSE;
}

static int ot_spi_device_chr_be_change(void *opaque)
{
    OtSPIDeviceState *s = opaque;
    SpiDeviceBus *bus = &s->bus;

    qemu_chr_fe_set_handlers(&s->chr, &ot_spi_device_chr_can_receive,
                             &ot_spi_device_chr_receive,
                             &ot_spi_device_chr_event_hander,
                             &ot_spi_device_chr_be_change, s, NULL, true);

    if (s->dpsram) {
        SpiDeviceGeneric *g = &s->generic;
        fifo8_reset(&g->rx_fifo);
        fifo8_reset(&g->tx_fifo);
    }

    fifo8_reset(&bus->chr_fifo);

    ot_spi_device_release_cs(s);

    if (s->watch_tag > 0) {
        g_source_remove(s->watch_tag);
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                             &ot_spi_device_chr_watch_cb, s);
    }

    return 0;
}

static Property ot_spi_device_properties[] = {
    DEFINE_PROP_CHR("chardev", OtSPIDeviceState, chr),
    DEFINE_PROP_BOOL("dpsram", OtSPIDeviceState, dpsram, false),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_spi_device_spi_regs_ops = {
    .read = &ot_spi_device_spi_regs_read,
    .write = &ot_spi_device_spi_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static const MemoryRegionOps ot_spi_device_tpm_regs_ops = {
    .read = &ot_spi_device_tpm_regs_read,
    .write = &ot_spi_device_tpm_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static const MemoryRegionOps ot_spi_device_buf_ops = {
    .read_with_attrs = &ot_spi_device_buf_read_with_attrs,
    .write_with_attrs = &ot_spi_device_buf_write_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_spi_device_reset(DeviceState *dev)
{
    OtSPIDeviceState *s = OT_SPI_DEVICE(dev);
    SpiDeviceFlash *f = &s->flash;
    SpiDeviceGeneric *g = &s->generic;
    SpiDeviceBus *bus = &s->bus;

    ot_spi_device_clear_modes(s);

    memset(s->spi_regs, 0, SPI_REGS_SIZE);
    memset(s->tpm_regs, 0, TPM_REGS_SIZE);

    fifo8_reset(&bus->chr_fifo);
    spi_fifo_reset(&g->rxf);
    spi_fifo_reset(&g->txf);
    /* not sure if the following FIFOs should be reset on clear_modes instead */
    fifo8_reset(&f->cmd_fifo);
    ot_fifo32_reset(&f->address_fifo);

    ot_spi_device_release_cs(s);
    f->watermark = false;
    s->spi_regs[R_CONTROL] = 0x80000010u;
    s->spi_regs[R_CFG] = 0x7f00u;
    s->spi_regs[R_FIFO_LEVEL] = 0x80u;
    s->spi_regs[R_STATUS] = 0x7au;
    s->spi_regs[R_RXF_ADDR] = 0x1fc0000u;
    s->spi_regs[R_TXF_ADDR] = 0x3fc0200u;
    s->spi_regs[R_JEDEC_CC] = 0x7fu;
    for (unsigned ix = 0; ix < PARAM_NUM_CMD_INFO; ix++) {
        s->spi_regs[R_CMD_INFO_0 + ix] = 0x7000u;
    }

    s->tpm_regs[R_TPM_CAP] = 0x660100u;

    ot_spi_device_update_irqs(s);
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], 0);
    }
}

static void ot_spi_device_realize(DeviceState *dev, Error **errp)
{
    OtSPIDeviceState *s = OT_SPI_DEVICE(dev);
    (void)errp;

    qemu_chr_fe_set_handlers(&s->chr, &ot_spi_device_chr_can_receive,
                             &ot_spi_device_chr_receive,
                             &ot_spi_device_chr_event_hander,
                             &ot_spi_device_chr_be_change, s, NULL, true);
}

static void ot_spi_device_init(Object *obj)
{
    OtSPIDeviceState *s = OT_SPI_DEVICE(obj);
    SpiDeviceGeneric *g = &s->generic;
    SpiDeviceFlash *f = &s->flash;
    SpiDeviceBus *bus = &s->bus;

    memory_region_init(&s->mmio.main, obj, TYPE_OT_SPI_DEVICE "-mmio",
                       SPI_DEVICE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio.main);
    memory_region_init_io(&s->mmio.spi, obj, &ot_spi_device_spi_regs_ops, s,
                          TYPE_OT_SPI_DEVICE "-spi-regs", SPI_REGS_SIZE);
    memory_region_add_subregion(&s->mmio.main, SPI_DEVICE_SPI_REGS_OFFSET,
                                &s->mmio.spi);
    memory_region_init_io(&s->mmio.tpm, obj, &ot_spi_device_tpm_regs_ops, s,
                          TYPE_OT_SPI_DEVICE "-tpm-regs", TPM_REGS_SIZE);
    memory_region_add_subregion(&s->mmio.main, SPI_DEVICE_TPM_REGS_OFFSET,
                                &s->mmio.tpm);
    memory_region_init_io(&s->mmio.buf, obj, &ot_spi_device_buf_ops, s,
                          TYPE_OT_SPI_DEVICE "-buf", SRAM_SIZE);
    memory_region_add_subregion(&s->mmio.main, SPI_DEVICE_SRAM_OFFSET,
                                &s->mmio.buf);

    s->spi_regs = g_new0(uint32_t, SPI_REGS_COUNT);
    s->tpm_regs = g_new0(uint32_t, TPM_REGS_COUNT);
    s->sram = g_new(uint32_t, SRAM_SIZE / sizeof(uint32_t));

    spi_fifo_init(&g->rxf, s, false);
    spi_fifo_init(&g->txf, s, true);
    fifo8_create(&g->rx_fifo, RXFIFO_LEN);
    fifo8_create(&g->tx_fifo, TXFIFO_LEN);
    fifo8_create(&bus->chr_fifo, SPI_BUS_HEADER_SIZE);
    fifo8_create(&f->cmd_fifo, SPI_SRAM_CMD_SIZE / sizeof(uint32_t));
    ot_fifo32_create(&f->address_fifo, SPI_SRAM_ADDR_SIZE / sizeof(uint32_t));
    f->buffer =
        (uint8_t *)g_new0(uint32_t, SPI_FLASH_BUFFER_SIZE / sizeof(uint32_t));

    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OPENTITAN_DEVICE_ALERT);
    }

    /*
     * This timer is used to hand over to the vCPU whenever a READBUF_* irq is
     * raised, otherwide the vCPU would not be able to get notified that a
     * buffer refill is required by the HW. In other words, this is poor man's
     * co-operative multitasking between the vCPU and the IO thread
     */
    f->irq_timer =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, &ot_spi_device_flash_resume_read, s);
    g->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                               &ot_spi_device_recv_generic_timeout, s);
}

static void ot_spi_device_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_spi_device_reset;
    dc->realize = &ot_spi_device_realize;
    device_class_set_props(dc, ot_spi_device_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_spi_device_info = {
    .name = TYPE_OT_SPI_DEVICE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtSPIDeviceState),
    .instance_init = &ot_spi_device_init,
    .class_init = &ot_spi_device_class_init,
};

static void ot_spi_device_register_types(void)
{
    type_register_static(&ot_spi_device_info);
}

type_init(ot_spi_device_register_types)
