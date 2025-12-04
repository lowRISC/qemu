/*
 * QEMU OpenTitan SPI Device controller
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Alice Ziuziakowska <a.ziuziakowska@lowrisc.org>
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
#include "qemu/module.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/opentitan/ot_spi_device.h"
#include "hw/opentitan/ot_spi_host.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

#define PARAM_SRAM_DEPTH         1024u
#define PARAM_SRAM_EGRESS_DEPTH  848u
#define PARAM_SRAM_INGRESS_DEPTH 112u
#define PARAM_NUM_CMD_INFO       24u
#define PARAM_NUM_LOCALITY       5u
#define PARAM_TPM_RD_FIFO_DEPTH  16u
#define PARAM_TPM_WR_FIFO_DEPTH  16u
#define PARAM_NUM_IRQS           8u
#define PARAM_NUM_ALERTS         1u
#define PARAM_REG_WIDTH          32u

/* SPI device registers */
/* clang-format off */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(INTR_UPLOAD_CMDFIFO_NOT_EMPTY, 0u, 1u)
    SHARED_FIELD(INTR_UPLOAD_PAYLOAD_NOT_EMPTY, 1u, 1u)
    SHARED_FIELD(INTR_UPLOAD_PAYLOAD_OVERFLOW, 2u, 1u)
    SHARED_FIELD(INTR_READBUF_WATERMARK, 3u, 1u)
    SHARED_FIELD(INTR_READBUF_FLIP, 4u, 1u)
    SHARED_FIELD(INTR_TPM_HEADER_NOT_EMPTY, 5u, 1u)
    SHARED_FIELD(INTR_TPM_RDFIFO_CMD_END, 6u, 1u)
    SHARED_FIELD(INTR_TPM_RDFIFO_DROP, 7u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(CONTROL, 0x10u)
    FIELD(CONTROL, FLASH_STATUS_FIFO_CLR, 0u, 1u)
    FIELD(CONTROL, FLASH_READ_BUFFER_CLR, 1u, 1u)
    FIELD(CONTROL, MODE, 4u, 2u)
REG32(CFG, 0x14u)
    FIELD(CFG, TX_ORDER, 2u, 1u)
    FIELD(CFG, RX_ORDER, 3u, 1u)
    FIELD(CFG, MAILBOX_EN, 24u, 1u)
REG32(STATUS, 0x18u)
    FIELD(STATUS, CSB, 5u, 1u)
    FIELD(STATUS, TPM_CSB, 6u, 1u)
REG32(INTERCEPT_EN, 0x1cu)
    FIELD(INTERCEPT_EN, STATUS, 0u, 1u)
    FIELD(INTERCEPT_EN, JEDEC, 1u, 1u)
    FIELD(INTERCEPT_EN, SFDP, 2u, 1u)
    FIELD(INTERCEPT_EN, MBX, 3u, 1u)
REG32(ADDR_MODE, 0x20u)
    FIELD(ADDR_MODE, ADDR_4B_EN, 0u, 1u)
    FIELD(ADDR_MODE, PENDING, 31u, 1u)
REG32(LAST_READ_ADDR, 0x24u)
REG32(FLASH_STATUS, 0x28u)
    FIELD(FLASH_STATUS, BUSY, 0u, 1u)
    FIELD(FLASH_STATUS, WEL, 1u, 1u)
    FIELD(FLASH_STATUS, STATUS, 2u, 22u)
REG32(JEDEC_CC, 0x2cu)
    FIELD(JEDEC_CC, CC, 0u, 8u)
    FIELD(JEDEC_CC, NUM_CC, 8u, 8u)
REG32(JEDEC_ID, 0x30u)
    FIELD(JEDEC_ID, DEVICE, 0u, 16u)
    FIELD(JEDEC_ID, MF, 16u, 8u)
REG32(READ_THRESHOLD, 0x34u)
    FIELD(READ_THRESHOLD, THRESHOLD, 0u, 10u)
REG32(MAILBOX_ADDR, 0x38u)
    FIELD(MAILBOX_ADDR, LOWER, 0u, 9u)
    FIELD(MAILBOX_ADDR, UPPER, 10u, 22u)
REG32(UPLOAD_STATUS, 0x3cu)
    FIELD(UPLOAD_STATUS, CMDFIFO_DEPTH, 0u, 5u)
    FIELD(UPLOAD_STATUS, CMDFIFO_NOTEMPTY, 7u, 1u)
    FIELD(UPLOAD_STATUS, ADDRFIFO_DEPTH, 8u, 5u)
    FIELD(UPLOAD_STATUS, ADDRFIFO_NOTEMPTY, 15u, 1u)
REG32(UPLOAD_STATUS2, 0x40u)
    FIELD(UPLOAD_STATUS2, PAYLOAD_DEPTH, 0u, 9u)
    FIELD(UPLOAD_STATUS2, PAYLOAD_START_IDX, 16u, 8u)
REG32(UPLOAD_CMDFIFO, 0x44u)
    FIELD(UPLOAD_CMDFIFO, DATA, 0u, 8u)
    FIELD(UPLOAD_CMDFIFO, BUSY, 13u, 1u)
    FIELD(UPLOAD_CMDFIFO, WEL, 14u, 1u)
    FIELD(UPLOAD_CMDFIFO, ADDR4B_MODE, 15u, 1u)
REG32(UPLOAD_ADDRFIFO, 0x48u)
REG32(CMD_FILTER_0, 0x4cu)
REG32(CMD_FILTER_1, 0x50u)
REG32(CMD_FILTER_2, 0x54u)
REG32(CMD_FILTER_3, 0x58u)
REG32(CMD_FILTER_4, 0x5cu)
REG32(CMD_FILTER_5, 0x60u)
REG32(CMD_FILTER_6, 0x64u)
REG32(CMD_FILTER_7, 0x68u)
REG32(ADDR_SWAP_MASK, 0x6cu)
REG32(ADDR_SWAP_DATA, 0x70u)
REG32(PAYLOAD_SWAP_MASK, 0x74u)
REG32(PAYLOAD_SWAP_DATA, 0x78u)
REG32(CMD_INFO_0, 0x7cu) /* ReadStatus1 */
    SHARED_FIELD(CMD_INFO_OPCODE, 0u, 8u)
    SHARED_FIELD(CMD_INFO_ADDR_MODE, 8u, 2u)
    SHARED_FIELD(CMD_INFO_ADDR_SWAP_EN, 10u, 1u) /* not used in Flash mode */
    SHARED_FIELD(CMD_INFO_MBYTE_EN, 11u, 1u)
    /* limited to bits, ignore in QEMU */
    SHARED_FIELD(CMD_INFO_DUMMY_SIZE, 12u, 3u)
    /* only use this bit for dummy cfg */
    SHARED_FIELD(CMD_INFO_DUMMY_EN, 15u, 1u)
    SHARED_FIELD(CMD_INFO_PAYLOAD_EN, 16u, 4u)
    /* not used in Flash mode (guess) */
    SHARED_FIELD(CMD_INFO_PAYLOAD_DIR, 20u, 1u)
    SHARED_FIELD(CMD_INFO_PAYLOAD_SWAP_EN, 21u, 1u) /* not used in Flash mode */
    SHARED_FIELD(CMD_INFO_READ_PIPELINE_MODE, 22u, 2u)
    SHARED_FIELD(CMD_INFO_UPLOAD, 24u, 1u)
    SHARED_FIELD(CMD_INFO_BUSY, 25u, 1u)
    SHARED_FIELD(CMD_INFO_VALID, 31u, 1u)
REG32(CMD_INFO_1, 0x80u) /* ReadStatus2 */
REG32(CMD_INFO_2, 0x84u) /* ReadStatus3 */
REG32(CMD_INFO_3, 0x88u) /* ReadJedecId */
REG32(CMD_INFO_4, 0x8cu) /* ReadSfdp */
REG32(CMD_INFO_5, 0x90u) /* Read */
REG32(CMD_INFO_6, 0x94u) /* FastRead */
REG32(CMD_INFO_7, 0x98u) /* FastReadDual */
REG32(CMD_INFO_8, 0x9cu) /* FastReadQuad */
REG32(CMD_INFO_9, 0xa0u) /* FastReadDualIO */
REG32(CMD_INFO_10, 0xa4u) /* FastReadQuadIO */
REG32(CMD_INFO_11, 0xa8u)
REG32(CMD_INFO_12, 0xacu)
REG32(CMD_INFO_13, 0xb0u)
REG32(CMD_INFO_14, 0xb4u)
REG32(CMD_INFO_15, 0xb8u)
REG32(CMD_INFO_16, 0xbcu)
REG32(CMD_INFO_17, 0xc0u)
REG32(CMD_INFO_18, 0xc4u)
REG32(CMD_INFO_19, 0xc8u)
REG32(CMD_INFO_20, 0xccu)
REG32(CMD_INFO_21, 0xd0u)
REG32(CMD_INFO_22, 0xd4u)
REG32(CMD_INFO_23, 0xd8u)
REG32(CMD_INFO_EN4B, 0xdcu)
REG32(CMD_INFO_EX4B, 0xe0u)
REG32(CMD_INFO_WREN, 0xe4u)
REG32(CMD_INFO_WRDI, 0xe8u)
/* clang-format on */

/* TPM registers */
/* clang-format off */
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
    FIELD(TPM_STATUS, WRFIFO_PENDING, 1u, 1u)
    FIELD(TPM_STATUS, RDFIFO_ABORTED, 2u, 1u)
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
/* clang-format on */

#define SPI_BUS_PROTO_VER   0
#define SPI_BUS_HEADER_SIZE (2u * sizeof(uint32_t))
#define SPI_TPM_READ_FIFO_SIZE_BYTES \
    (PARAM_TPM_RD_FIFO_DEPTH * sizeof(uint32_t))
/*
 * Pacing time to give hand back to the vCPU when a readbuf event is triggered.
 * This is needed so guest SW can respond to the interrupt and fill the buffer.
 *
 * The scheduled timer tell the CharDev backend not to consume (nor push back)
 * any more bytes from/to the SPI bus. The Chardev will resume its SPI bus
 * bytestream management as soon as the timer is cancelled/expires. Ideally,
 * guest SW will clear the readbuf interrupt causing the timer to expire,
 * usually once SW has filled in the read buffer. If Guest SW does not use this
 * (e.g. it writes across both buffers in advance and does not expect to handle
 * a buffer flip event) then this will not happen, and the timer must exhaust
 * on its own, thus we only set this delay to a small arbitrary value of 10 ms.
 *
 * @todo: A better approach might be to yield to the vCPU every few bytes/words
 * to better emulate the concurrency of large SPI transactions with SW,
 * potentially keeping a small additional delay on a buffer flip.
 */
#define SPI_BUS_FLASH_READ_DELAY_NS 10000000

/*
 * Memory layout extracted from the documentation:
 * opentitan.org/book/hw/ip/spi_device/doc/programmers_guide.html
 * #dual-port-sram-layout
 *
 *          New scheme (Egress + Ingress)      Old Scheme (DPSRAM)
 *         +--------------------------------+    +-----------------------+
 *         | Flash / Passthru modes         |    | Flash / Passthru modes|
 *  0x000 -+-------------------+------+-----+   -+----------------+------+
 *         | Read Command 0    | 1KiB | Out |    | Read Command 0 | 1KiB |
 *  0x400 -+-------------------+------+-----+   -+----------------+------+
 *         | Read Command 1    | 1KiB | Out |    | Read Command 1 | 1KiB |
 *  0x800 -+-------------------+------+-----+   -+----------------+------+
 *         | Mailbox           | 1KiB | Out |    | Mailbox        | 1KiB |
 *  0xc00 -+-------------------+------+-----+   -+----------------+------+
 *         | SFDP              | 256B | Out |    | SFDP           | 256B |
 *  0xd00 -+-------------------+------+-----+   -+----------------+------+
 *         | TPM Read Buffer   |  64B | Out |    | Payload FIFO   | 256B |
 *  0xd40 -+-------------------+------+-----+   -+----------------+------+
 *         |                   |      |     |    | Command FIFO   |  64B |
 *  0xe00 -+-------------------+------+-----+   -+----------------+------+
 *         | Payload FIFO      | 256B | In  |    | Address FIFO   |  64B |
 *  0xf00 -+-------------------+------+-----+   -+----------------+------+
 *         | Command FIFO      |  64B | In  |
 *  0xf40 -+-------------------+------+-----+
 *         | Address FIFO      |  64B | In  |
 *  0xf80 -+-------------------+------+-----+
 *         | TPM Write Buffer  |  64B | In  |
 *  0xfc0 -+-------------------+------+-----+
 *
 */
/* clang-format off */
#define SPI_SRAM_READ0_OFFSET      0x0
#define SPI_SRAM_READ_LOG2_SIZE    10u
#define SPI_SRAM_READ_SIZE         (1u << SPI_SRAM_READ_LOG2_SIZE)
#define SPI_SRAM_READ1_OFFSET      (SPI_SRAM_READ0_OFFSET + SPI_SRAM_READ_SIZE)
#define SPI_SRAM_READ1_SIZE        0x400u
#define SPI_SRAM_MBX_OFFSET        (SPI_SRAM_READ1_OFFSET + SPI_SRAM_READ_SIZE)
#define SPI_SRAM_MBX_SIZE          0x400u
#define SPI_SRAM_SFDP_OFFSET       (SPI_SRAM_MBX_OFFSET + SPI_SRAM_MBX_SIZE)
#define SPI_SRAM_SFDP_SIZE         0x100u
#define SPI_SRAM_TPM_READ_OFFSET   (SPI_SRAM_SFDP_OFFSET + SPI_SRAM_SFDP_SIZE)
#define SPI_SRAM_TPM_READ_SIZE     0x40u
#define SPI_SRAM_INGRESS_OFFSET    0xE00u
#define SPI_SRAM_PAYLOAD_OFFSET    SPI_SRAM_INGRESS_OFFSET
#define SPI_SRAM_PAYLOAD_SIZE      0x100u
#define SPI_SRAM_CMD_OFFSET \
    (SPI_SRAM_PAYLOAD_OFFSET + SPI_SRAM_PAYLOAD_SIZE)
#define SPI_SRAM_CMD_SIZE          0x40u
#define SPI_SRAM_ADDR_OFFSET       (SPI_SRAM_CMD_OFFSET + SPI_SRAM_CMD_SIZE)
#define SPI_SRAM_ADDR_SIZE         0x40u
#define SPI_SRAM_TPM_WRITE_OFFSET  (SPI_SRAM_ADDR_OFFSET + SPI_SRAM_ADDR_SIZE)
#define SPI_SRAM_TPM_WRITE_SIZE    0x40u
#define SPI_SRAM_ADDR_END \
    (SPI_SRAM_TPM_WRITE_OFFSET + SPI_SRAM_TPM_WRITE_SIZE)
#define SPI_SRAM_END_OFFSET        (SPI_SRAM_ADDR_END)
#define SPI_DEVICE_SIZE            0x2000u
#define SPI_DEVICE_SPI_REGS_OFFSET 0u
#define SPI_DEVICE_TPM_REGS_OFFSET 0x800u
#define SPI_DEVICE_SRAM_OFFSET     0x1000u
/* clang-format on */

#define SRAM_SIZE                 (PARAM_SRAM_DEPTH * sizeof(uint32_t))
#define EGRESS_BUFFER_SIZE_BYTES  (PARAM_SRAM_EGRESS_DEPTH * sizeof(uint32_t))
#define EGRESS_BUFFER_SIZE_WORDS  PARAM_SRAM_EGRESS_DEPTH
#define INGRESS_BUFFER_SIZE_BYTES (PARAM_SRAM_INGRESS_DEPTH * sizeof(uint32_t))
#define INGRESS_BUFFER_SIZE_WORDS PARAM_SRAM_INGRESS_DEPTH
#define FLASH_READ_BUFFER_SIZE    (2u * SPI_SRAM_READ_SIZE)

#define SPI_DEFAULT_TX_RX_VALUE ((uint8_t)0xffu)
#define SPI_FLASH_BUFFER_SIZE   256u

typedef enum {
    /* Internal HW command slots (0-10) */
    SLOT_HW_READ_STATUS1, /* slot 0, typically 0x05u */
    SLOT_HW_READ_STATUS2, /* slot 1, typically 0x35u */
    SLOT_HW_READ_STATUS3, /* slot 2, typically 0x15u */
    SLOT_HW_READ_JEDEC, /* slot 3, typically 0x9f */
    SLOT_HW_READ_SFDP, /* slot 4, typically 0x5a */
    SLOT_HW_READ_NORMAL, /* slot 5, typically 0x03u */
    SLOT_HW_READ_FAST, /* slot 6, typically 0x0bu */
    SLOT_HW_READ_DUAL, /* slot 7, typically 0x3bu */
    SLOT_HW_READ_QUAD, /* slot 8, typically 0x6bu */
    SLOT_HW_READ_DUAL_IO, /* slot 9, typically 0xbbu */
    SLOT_HW_READ_QUAD_IO, /* slot 10, typically 0xebu */
    /* SW command slots (11-23) */
    SLOT_SW_CMD_11,
    SLOT_SW_CMD_12,
    SLOT_SW_CMD_13,
    SLOT_SW_CMD_14,
    SLOT_SW_CMD_15,
    SLOT_SW_CMD_16,
    SLOT_SW_CMD_17,
    SLOT_SW_CMD_18,
    SLOT_SW_CMD_19,
    SLOT_SW_CMD_20,
    SLOT_SW_CMD_21,
    SLOT_SW_CMD_22,
    SLOT_SW_CMD_23,
    /* Configurable HW command slots (EN4B-WRDI, 24-27) */
    SLOT_HW_EN4B, /* slot 24, typically 0xb7u */
    SLOT_HW_EX4B, /* slot 25, typically 0xe9u */
    SLOT_HW_WREN, /* slot 26, typically 0x06u */
    SLOT_HW_WRDI, /* slot 27, typically 0x04u */
    SLOT_COUNT,
    SLOT_INVALID = SLOT_COUNT,
} OtSpiDeviceCommandSlot;

#define SLOT_SW_CMD_FIRST (SLOT_SW_CMD_11)
#define SLOT_SW_CMD_LAST  (SLOT_SW_CMD_23)

static_assert(SLOT_COUNT == 28u, "Invalid command slot count");

#define TPM_OPCODE_READ_BIT  7u
#define TPM_OPCODE_SIZE_MASK 0x3Fu
#define TPM_ADDR_HEADER      0xD4u
#define TPM_READY            0x01u

static_assert(SPI_SRAM_INGRESS_OFFSET >=
                  (SPI_SRAM_TPM_READ_OFFSET + SPI_SRAM_TPM_READ_SIZE),
              "SPI SRAM Egress buffers overflow into Ingress buffers");
static_assert(SPI_SRAM_END_OFFSET == 0xfc0u, "Invalid SRAM definition");

typedef enum {
    CTRL_MODE_DISABLED,
    CTRL_MODE_FLASH,
    CTRL_MODE_PASSTHROUGH,
    CTRL_MODE_INVALID,
} OtSpiDeviceMode;

typedef enum {
    SPI_BUS_IDLE,
    SPI_BUS_FLASH,
    SPI_BUS_TPM,
    SPI_BUS_DISCARD,
    SPI_BUS_ERROR,
} OtSpiBusState;

typedef enum {
    SPI_FLASH_IDLE, /* No command received */
    SPI_FLASH_COLLECT, /* Collecting address or additional info after cmd */
    SPI_FLASH_BUFFER, /* Reading out data from buffer or SFDP (-> SPI host) */
    SPI_FLASH_READ, /* Reading out data from SRAM (-> SPI host) */
    SPI_FLASH_UP_ADDR, /* Uploading address (<- SPI host) */
    SPI_FLASH_UP_DUMMY, /* Uploading dummy (<- SPI host) */
    SPI_FLASH_UP_PAYLOAD, /* Uploading payload (<- SPI host) */
    SPI_FLASH_PASSTHROUGH_UP_ADDR, /* Passthrough mode - Uploading address */
    SPI_FLASH_PASSTHROUGH_UP_DUMMY, /* Passthrough mode - Uploading dummy */
    SPI_FLASH_PASSTHROUGH_UP_PAYLOAD, /* Passthrough mode - Uploading payload */
    SPI_FLASH_DONE, /* No more clock expected for the current command */
    SPI_FLASH_ERROR, /* On error */
} OtSpiFlashState;

typedef enum {
    SPI_TPM_IDLE, /* Wait for the fist byte of the header.*/
    SPI_TPM_ADDR, /* Wait for the following 3 bytes of the header.*/
    SPI_TPM_WAIT, /* Not ready for the host to read.*/
    SPI_TPM_START_BYTE, /* Now ready for the host to read.*/
    SPI_TPM_READ, /* The host requested a read handled by Software.*/
    SPI_TPM_READ_HW_REG, /* Host requested a read handled by Hardware.*/
    SPI_TPM_WRITE, /* Host is writing.*/
    SPI_TPM_END, /* Finished the spi transaction.*/
} OtSpiTpmState;

typedef struct {
    unsigned address_size; /* Length of address field for current command */
    bool cmd_addr_swap; /* Address byte swapping is enabled */
    bool cmd_dummy; /* Command has dummy field */
    bool cmd_payload_swap; /* Payload byte swapping is enabled */
    bool cmd_payload_dir_out; /* Payload is from flash to host */
} OtSpiCommandParams;

typedef struct {
    OtSpiFlashState state;
    OtSpiDeviceCommandSlot slot; /* Command slot */
    OtSpiCommandParams cmd_params; /* Parameters for current command */
    unsigned pos; /* Current position in data buffer */
    unsigned len; /* Meaning depends on command and current state */
    uint32_t address; /* Address tracking */
    uint32_t last_read_addr; /* Last address read before increment */
    uint32_t next_buffer_addr; /* Next buffer boundary */
    uint32_t cmd_info; /* Selected command info slot */
    uint8_t *src; /* Selected read data source (alias) */
    uint8_t *payload; /* Selected write data sink (alias) */
    uint8_t *buffer; /* Temporary buffer to handle transfer */
    Fifo8 cmd_fifo; /* Command FIFO (HW uses 32-bit FIFO w/ 24-bit padding) */
    OtFifo32 address_fifo; /* Address FIFO */
    QEMUTimer *irq_timer; /* Timer to resume processing after a READBUF_* IRQ */
    bool loop; /* Keep reading the buffer if end is reached */
    bool watermark_crossed; /* Read watermark hit, used as flip-flop */
    bool new_cmd; /* New command has been pushed in current SPI transaction */
} SpiDeviceFlash;

typedef struct {
    OtSpiTpmState state;
    uint8_t *write_buffer;
    uint32_t opcode;
    unsigned write_pos;
    unsigned len;
    Fifo8 rdfifo;
    unsigned can_receive;
    unsigned transfer_size;
    unsigned reg;
    unsigned locality;
    bool read;
    bool should_sw_handle;
} SpiDeviceTpm;

typedef struct {
    unsigned addr;
    uint32_t offset;
    unsigned nbytes;
} SpiTpmHwAddrMap;

typedef struct {
    OtSpiBusState state;
    unsigned byte_count; /* Count of SPI payload to receive */
    Fifo8 chr_fifo; /* QEMU protocol input FIFO */
    uint8_t mode; /* Polarity/phase mismatch */
    bool release; /* Whether to release /CS on last byte */
    bool failed_transaction; /* Whether to discard until /CS is deasserted */
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
    SpiDeviceTpm tpm;

    /* CS signal for downstream flash in passthrough mode, active low */
    IbexIRQ passthrough_cs;
    IbexIRQ passthrough_en;

    uint32_t *spi_regs; /* Registers */
    uint32_t *tpm_regs; /* Registers */
    uint32_t *sram;

    /* Properties */
    char *ot_id;
    OtSPIHostState *spi_host; /* downstream SPI Host */
    CharBackend chr; /* communication device */
    guint watch_tag; /* tracker for comm device change */
};

struct OtSPIDeviceClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

#define REG_BITS     (8u * sizeof(uint32_t))
#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_SPI_LAST_REG (R_CMD_INFO_WRDI)
#define SPI_REGS_COUNT (R_SPI_LAST_REG + 1u)
#define SPI_REGS_SIZE  (SPI_REGS_COUNT * sizeof(uint32_t))
#define SPI_REG_NAME(_reg_) \
    ((((_reg_) < SPI_REGS_COUNT) && SPI_REG_NAMES[_reg_]) ? \
         SPI_REG_NAMES[_reg_] : \
         "?")

#define R_TPM_LAST_REG (R_TPM_READ_FIFO)
#define TPM_REGS_COUNT (R_TPM_LAST_REG + 1u)
#define TPM_REGS_SIZE  (TPM_REGS_COUNT * sizeof(uint32_t))
#define TPM_REG_NAME(_reg_) \
    ((((_reg_) < TPM_REGS_COUNT) && TPM_REG_NAMES[_reg_]) ? \
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
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(INTERCEPT_EN),
    REG_NAME_ENTRY(ADDR_MODE),
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
};
/* clang-format on */
#undef REG_NAME_ENTRY

#define INTR_MASK         ((1u << PARAM_NUM_IRQS) - 1u)
#define ALERT_TEST_MASK   (R_ALERT_TEST_FATAL_FAULT_MASK)
#define INTR_READBUF_MASK (INTR_READBUF_WATERMARK_MASK | INTR_READBUF_FLIP_MASK)
#define CONTROL_MASK \
    (R_CONTROL_FLASH_STATUS_FIFO_CLR_MASK | \
     R_CONTROL_FLASH_READ_BUFFER_CLR_MASK | R_CONTROL_MODE_MASK)
#define CMD_INFO_GEN_MASK \
    (CMD_INFO_OPCODE_MASK | CMD_INFO_ADDR_MODE_MASK | \
     CMD_INFO_ADDR_SWAP_EN_MASK | CMD_INFO_MBYTE_EN_MASK | \
     CMD_INFO_DUMMY_SIZE_MASK | CMD_INFO_DUMMY_EN_MASK | \
     CMD_INFO_PAYLOAD_EN_MASK | CMD_INFO_PAYLOAD_DIR_MASK | \
     CMD_INFO_PAYLOAD_SWAP_EN_MASK | CMD_INFO_UPLOAD_MASK | \
     CMD_INFO_BUSY_MASK | CMD_INFO_VALID_MASK)
#define CMD_INFO_SPC_MASK (CMD_INFO_OPCODE_MASK | CMD_INFO_VALID_MASK)
#define CFG_MASK \
    (R_CFG_TX_ORDER_MASK | R_CFG_RX_ORDER_MASK | R_CFG_MAILBOX_EN_MASK)
#define INTERCEPT_EN_MASK \
    (R_INTERCEPT_EN_STATUS_MASK | R_INTERCEPT_EN_JEDEC_MASK | \
     R_INTERCEPT_EN_SFDP_MASK | R_INTERCEPT_EN_MBX_MASK)
#define FLASH_STATUS_RW0C_MASK \
    (R_FLASH_STATUS_BUSY_MASK | R_FLASH_STATUS_WEL_MASK)
#define FLASH_STATUS_RW_MASK (R_FLASH_STATUS_STATUS_MASK)
#define JEDEC_CC_MASK        (R_JEDEC_CC_CC_MASK | R_JEDEC_CC_NUM_CC_MASK)
#define JEDEC_ID_MASK        (R_JEDEC_ID_DEVICE_MASK | R_JEDEC_ID_MF_MASK)

#define COMMAND_OPCODE(_cmd_info_) \
    ((uint8_t)((_cmd_info_) & CMD_INFO_OPCODE_MASK))

#define STATE_NAME_ENTRY(_st_) [_st_] = stringify(_st_)
/* clang-format off */
static const char *BUS_STATE_NAMES[] = {
    STATE_NAME_ENTRY(SPI_BUS_IDLE),
    STATE_NAME_ENTRY(SPI_BUS_FLASH),
    STATE_NAME_ENTRY(SPI_BUS_TPM),
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
    STATE_NAME_ENTRY(SPI_FLASH_PASSTHROUGH_UP_ADDR),
    STATE_NAME_ENTRY(SPI_FLASH_PASSTHROUGH_UP_DUMMY),
    STATE_NAME_ENTRY(SPI_FLASH_PASSTHROUGH_UP_PAYLOAD),
    STATE_NAME_ENTRY(SPI_FLASH_DONE),
    STATE_NAME_ENTRY(SPI_FLASH_ERROR),
};
/* clang-format on */
#undef STATE_NAME_ENTRY

#define BUS_STATE_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(BUS_STATE_NAMES) ? \
         BUS_STATE_NAMES[(_st_)] : \
         "?")
#define FLASH_STATE_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(FLASH_STATE_NAMES) ? \
         FLASH_STATE_NAMES[(_st_)] : \
         "?")

#define IRQ_NAME_ENTRY(_st_) [INTR_##_st_##_SHIFT] = stringify(_st_)
/* clang-format off */
static const char *IRQ_NAMES[] = {
    IRQ_NAME_ENTRY(UPLOAD_CMDFIFO_NOT_EMPTY),
    IRQ_NAME_ENTRY(UPLOAD_PAYLOAD_NOT_EMPTY),
    IRQ_NAME_ENTRY(UPLOAD_PAYLOAD_OVERFLOW),
    IRQ_NAME_ENTRY(READBUF_WATERMARK),
    IRQ_NAME_ENTRY(READBUF_FLIP),
    IRQ_NAME_ENTRY(TPM_HEADER_NOT_EMPTY),
    IRQ_NAME_ENTRY(TPM_RDFIFO_CMD_END),
    IRQ_NAME_ENTRY(TPM_RDFIFO_DROP)
};
/* clang-format on */
#undef IRQ_NAME_ENTRY

#define IRQ_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(IRQ_NAMES) ? IRQ_NAMES[(_st_)] : "?")

#define WORD_ALIGN(_x_) ((_x_) & ~0x3u)

#define BUS_CHANGE_STATE(_s_, _sst_) \
    ot_spi_device_bus_change_state_line(_s_, SPI_BUS_##_sst_, __LINE__)
#define FLASH_CHANGE_STATE(_s_, _sst_) \
    ot_spi_device_flash_change_state_line(_s_, SPI_FLASH_##_sst_, __LINE__)

static void ot_spi_device_bus_change_state_line(OtSPIDeviceState *s,
                                                OtSpiBusState state, int line)
{
    if (s->bus.state != state) {
        trace_ot_spi_device_bus_change_state(s->ot_id, line,
                                             BUS_STATE_NAME(state), state);
        s->bus.state = state;
    }
}

static void ot_spi_device_flash_change_state_line(
    OtSPIDeviceState *s, OtSpiFlashState state, int line)
{
    if (s->flash.state != state) {
        trace_ot_spi_device_flash_change_state(s->ot_id, line,
                                               FLASH_STATE_NAME(s->flash.state),
                                               s->flash.state,
                                               FLASH_STATE_NAME(state), state);
        s->flash.state = state;
    }
}

static bool ot_spi_device_is_sw_command(OtSpiDeviceCommandSlot slot)
{
    return (slot >= SLOT_SW_CMD_FIRST) && (slot <= SLOT_SW_CMD_LAST);
}

static bool ot_spi_device_flash_command_is_upload(const SpiDeviceFlash *f)
{
    return ot_spi_device_is_sw_command(f->slot) &&
           ((f->cmd_info & CMD_INFO_UPLOAD_MASK) != 0u);
}

static bool ot_spi_device_flash_command_is_payload_en(const SpiDeviceFlash *f)
{
    return (f->cmd_info & CMD_INFO_PAYLOAD_EN_MASK) != 0u;
}

static bool ot_spi_device_flash_command_has_payload(const SpiDeviceFlash *f)
{
    return ot_spi_device_is_sw_command(f->slot) &&
           ot_spi_device_flash_command_is_payload_en(f);
}

static bool ot_spi_device_flash_command_is_rx_payload(const SpiDeviceFlash *f)
{
    return ot_spi_device_is_sw_command(f->slot) &&
           ((f->cmd_info & CMD_INFO_PAYLOAD_DIR_MASK) == 0u);
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
    FLASH_CHANGE_STATE(s, IDLE);
    f->slot = SLOT_INVALID;
    f->cmd_info = 0u;
    f->address = 0u;
    f->last_read_addr = 0u;
    f->next_buffer_addr = SPI_SRAM_READ_SIZE;
    f->pos = 0u;
    f->len = 0u;
    f->watermark_crossed = false;
    f->new_cmd = false;
    g_assert(s->sram);
    f->payload = &((uint8_t *)s->sram)[SPI_SRAM_PAYLOAD_OFFSET];
    memset(f->buffer, 0u, SPI_FLASH_BUFFER_SIZE);

    SpiDeviceTpm *tpm = &s->tpm;
    tpm->state = SPI_TPM_IDLE;
    tpm->can_receive = 1u;
    tpm->transfer_size = 0u;
    tpm->read = false;
    tpm->reg = 0u;
    tpm->opcode = 0u;
    tpm->locality = 0u;
    tpm->write_buffer = &((uint8_t *)s->sram)[SPI_SRAM_TPM_WRITE_OFFSET];
    fifo8_reset(&tpm->rdfifo);

    memset(s->sram, 0u, SRAM_SIZE);
}

static void ot_spi_device_update_irqs(OtSPIDeviceState *s)
{
    uint32_t levels = s->spi_regs[R_INTR_STATE] & s->spi_regs[R_INTR_ENABLE];
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        bool level = (bool)((levels >> ix) & 0x1u);
        if (level != (bool)ibex_irq_get_level(&s->irqs[ix])) {
            trace_ot_spi_device_set_irq(s->ot_id, IRQ_NAME(ix), ix, level);
        }
        ibex_irq_set(&s->irqs[ix], (int)level);
    }
}

static void ot_spi_device_update_alerts(OtSPIDeviceState *s)
{
    uint32_t level = s->spi_regs[R_ALERT_TEST];

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
        ibex_irq_set(&s->alerts[ix], (int)((level >> ix) & 0x1u));
    }
}

static bool ot_spi_device_is_tpm_enabled(const OtSPIDeviceState *s)
{
    return (bool)FIELD_EX32(s->tpm_regs[R_TPM_CFG], TPM_CFG, EN);
}

/*
 * if the SW set this field to 1, the HW logic always pushes the command/addr
 * and write data to buffers. The logic does not compare the incoming address to
 * the list of managed-by-HW register addresses.
 */
static bool ot_spi_device_is_tpm_mode_crb(const OtSPIDeviceState *s)
{
    return (bool)FIELD_EX32(s->tpm_regs[R_TPM_CFG], TPM_CFG, TPM_MODE);
}

/*
 * If 0, TPM submodule directly returns the return-by-HW registers for the read
 * requests. If 1, TPM submodule uploads the TPM command regardless of the
 * address, and the SW may return the value through the read FIFO.
 */
static bool ot_spi_device_tpm_disable_hw_regs(const OtSPIDeviceState *s)
{
    return (bool)FIELD_EX32(s->tpm_regs[R_TPM_CFG], TPM_CFG, HW_REG_DIS);
}

static OtSpiDeviceMode ot_spi_device_get_mode(const OtSPIDeviceState *s)
{
    return (OtSpiDeviceMode)FIELD_EX32(s->spi_regs[R_CONTROL], CONTROL, MODE);
}

static bool ot_spi_device_is_addr4b_en(const OtSPIDeviceState *s)
{
    return (bool)(s->spi_regs[R_ADDR_MODE] & R_ADDR_MODE_ADDR_4B_EN_MASK);
}

static bool ot_spi_device_is_mailbox_en(const OtSPIDeviceState *s)
{
    return (bool)(s->spi_regs[R_CFG] & R_CFG_MAILBOX_EN_MASK);
}

static unsigned
ot_spi_device_get_command_address_size(const OtSPIDeviceState *s)
{
    const SpiDeviceFlash *f = &s->flash;

    switch (SHARED_FIELD_EX32(f->cmd_info, CMD_INFO_ADDR_MODE)) {
    case 0x0u: /* AddrDisabled */
        return 0u;
    case 0x1u: /* AddrCfg */
        return ot_spi_device_is_addr4b_en(s) ? 4u : 3u;
    case 0x2u: /* Addr3B */
        return 3u;
    case 0x3u: /* Addr4B */
        return 4u;
    default:
        g_assert_not_reached();
    }
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

static void ot_spi_device_release(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;
    SpiDeviceBus *bus = &s->bus;

    trace_ot_spi_device_release(s->ot_id);

    BUS_CHANGE_STATE(s, IDLE);
    bus->byte_count = 0u;
    bus->failed_transaction = false;

    s->spi_regs[R_STATUS] = R_STATUS_CSB_MASK | R_STATUS_TPM_CSB_MASK;

    bool update_irq = false;

    OtSpiDeviceMode mode = ot_spi_device_get_mode(s);

    switch (mode) {
    case CTRL_MODE_FLASH:
    case CTRL_MODE_PASSTHROUGH:
        /* new uploaded command */
        if (!fifo8_is_empty(&f->cmd_fifo) && f->new_cmd) {
            s->spi_regs[R_INTR_STATE] |= INTR_UPLOAD_CMDFIFO_NOT_EMPTY_MASK;
            update_irq = true;
        }
        /* update upload status register with payload information */
        if ((mode == CTRL_MODE_FLASH && f->state == SPI_FLASH_UP_PAYLOAD) ||
            (mode == CTRL_MODE_PASSTHROUGH &&
             f->state == SPI_FLASH_PASSTHROUGH_UP_PAYLOAD)) {
            unsigned payload_start, payload_len;
            if (f->pos > SPI_SRAM_PAYLOAD_SIZE) {
                payload_start = f->pos % SPI_SRAM_PAYLOAD_SIZE;
                payload_len = SPI_SRAM_PAYLOAD_SIZE;
                s->spi_regs[R_INTR_STATE] |= INTR_UPLOAD_PAYLOAD_OVERFLOW_MASK;
                update_irq = true;
                trace_ot_spi_device_flash_overflow(s->ot_id, "payload");
            } else {
                payload_start = 0u;
                payload_len = f->pos;
            }
            s->spi_regs[R_UPLOAD_STATUS2] =
                FIELD_DP32(0, UPLOAD_STATUS2, PAYLOAD_START_IDX, payload_start);
            s->spi_regs[R_UPLOAD_STATUS2] =
                FIELD_DP32(s->spi_regs[R_UPLOAD_STATUS2], UPLOAD_STATUS2,
                           PAYLOAD_DEPTH, payload_len);
            trace_ot_spi_device_flash_payload(s->ot_id, f->pos, payload_start,
                                              payload_len);
        }
        /*
         * "shows the last address accessed by the host system."
         * "does not show the commands falling into the mailbox region or
         *  Read SFDP command’s address."
         */
        if (f->slot >= SLOT_HW_READ_NORMAL && f->slot <= SLOT_HW_READ_QUAD_IO &&
            !ot_spi_device_is_mailbox_match(s, f->last_read_addr)) {
            trace_ot_spi_device_update_last_read_addr(s->ot_id,
                                                      f->last_read_addr);
            s->spi_regs[R_LAST_READ_ADDR] = f->last_read_addr;
        }
        FLASH_CHANGE_STATE(s, IDLE);
        break;
    default:
        break;
    }

    bool upload_intr = false;
    switch (mode) {
    case CTRL_MODE_FLASH:
        upload_intr = ot_spi_device_is_sw_command(f->slot);
        break;
    case CTRL_MODE_PASSTHROUGH:
        /*
         * @todo add support for payload interrupt management in Passthrough
         * mode.
         */
        if (f->pos) {
            upload_intr = true;
        }
        /* passthrough mode: release CS */
        ibex_irq_raise(&s->passthrough_cs);
        break;
    default:
        break;
    }

    f->new_cmd = false;

    if (upload_intr) {
        if (f->pos) {
            s->spi_regs[R_INTR_STATE] |= INTR_UPLOAD_PAYLOAD_NOT_EMPTY_MASK;
            update_irq = true;
        }
    }

    if (update_irq) {
        ot_spi_device_update_irqs(s);
    }
}

static void ot_spi_device_flash_pace_spibus(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    timer_del(f->irq_timer);
    int64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    trace_ot_spi_device_flash_pace(s->ot_id, "set",
                                   timer_pending(f->irq_timer));
    timer_mod(f->irq_timer, now + SPI_BUS_FLASH_READ_DELAY_NS);
}

static void ot_spi_device_flash_clear_readbuffer(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    f->next_buffer_addr = SPI_SRAM_READ_SIZE;
    f->watermark_crossed = false;
}

static bool
ot_spi_device_flash_match_command_slot(OtSPIDeviceState *s, uint8_t cmd)
{
    SpiDeviceFlash *f = &s->flash;

    g_assert(f->state == SPI_FLASH_IDLE);

    /*
     * Find and match the opcode in the CMD_INFO registers. In case of
     * multiple matching entries, the last one is used.
     */
    bool matched = false;
    for (unsigned ix = 0u; ix < SLOT_COUNT; ix++) {
        uint32_t cmd_info = s->spi_regs[R_CMD_INFO_0 + ix];
        if (cmd == (uint8_t)SHARED_FIELD_EX32(cmd_info, CMD_INFO_OPCODE)) {
            if (SHARED_FIELD_EX32(cmd_info, CMD_INFO_VALID)) {
                f->slot = ix;
                f->cmd_info = cmd_info;
                matched = true;
                const char *type =
                    ot_spi_device_is_sw_command(f->slot) ? "SW" : "HW";
                trace_ot_spi_device_flash_match(s->ot_id, type, cmd, f->slot);
            } else {
                trace_ot_spi_device_flash_disabled_slot(s->ot_id, cmd, ix);
            }
        }
    }

    if (matched) {
        const char *type = ot_spi_device_is_sw_command(f->slot) ? "SW" : "HW";
        trace_ot_spi_device_flash_new_command(s->ot_id, type, cmd, f->slot);
        return true;
    }

    trace_ot_spi_device_flash_unmatched_command(s->ot_id, cmd);
    return false;
}

static void ot_spi_device_flash_try_upload(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    if (ot_spi_device_flash_command_is_upload(f)) {
        bool busy = (bool)(f->cmd_info & CMD_INFO_BUSY_MASK);
        if (busy) {
            s->spi_regs[R_FLASH_STATUS] |= R_FLASH_STATUS_BUSY_MASK;
        }
        if (fifo8_is_full(&f->cmd_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: cmd fifo overflow",
                          __func__, s->ot_id);
        } else {
            fifo8_push(&f->cmd_fifo, COMMAND_OPCODE(f->cmd_info));
        }
        f->new_cmd = true;
        trace_ot_spi_device_flash_upload(s->ot_id, f->slot, f->cmd_info, busy);
    }
}

static void ot_spi_device_flash_decode_read_jedec(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    uint8_t cc_count =
        (uint8_t)FIELD_EX32(s->spi_regs[R_JEDEC_CC], JEDEC_CC, NUM_CC);
    uint8_t cc_code =
        (uint8_t)FIELD_EX32(s->spi_regs[R_JEDEC_CC], JEDEC_CC, CC);
    uint8_t jedec_manuf =
        (uint8_t)FIELD_EX32(s->spi_regs[R_JEDEC_ID], JEDEC_ID, MF);
    uint16_t jedec_device =
        (uint16_t)FIELD_EX32(s->spi_regs[R_JEDEC_ID], JEDEC_ID, DEVICE);
    memset(f->buffer, (int)(uint8_t)cc_code, (size_t)cc_count);
    f->len = cc_count;
    f->buffer[f->len++] = jedec_manuf;
    /*
     * For some reason, OpenTitan device byte-swaps the device field.
     * This is not documented, but the "flash density" field is sent first
     *
     * JEDEC_ID register:  |  00  |  MF  |  DEVICE_ID  |  content
     *                     |31..24|23..16|   15 .. 0   |  bits
     *                     |  B3  |  B2  |  B1  |  B0  |  bytes
     * is sent in the following order: B2-B0-B1
     */
    f->buffer[f->len++] = (uint8_t)(jedec_device >> 0u);
    f->buffer[f->len++] = (uint8_t)(jedec_device >> 8u);
    /* after the end of JEDEC ID is 0s on OpenTitan */
    memset(&f->buffer[f->len], (int)0u, SPI_FLASH_BUFFER_SIZE - f->len);
    f->len = SPI_FLASH_BUFFER_SIZE;
    f->src = f->buffer;
    FLASH_CHANGE_STATE(s, BUFFER);
}

static void ot_spi_device_flash_decode_write_enable(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    bool enable = f->slot == SLOT_HW_WREN;
    trace_ot_spi_device_flash_exec(s->ot_id, enable ? "WREN" : "WRDI");
    if (enable) {
        s->spi_regs[R_FLASH_STATUS] |= R_FLASH_STATUS_WEL_MASK;
    } else {
        s->spi_regs[R_FLASH_STATUS] &= ~R_FLASH_STATUS_WEL_MASK;
    }
    FLASH_CHANGE_STATE(s, DONE);
}

static void ot_spi_device_flash_decode_addr4_enable(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    bool enable = f->slot == SLOT_HW_EN4B;
    trace_ot_spi_device_flash_exec(s->ot_id, enable ? "EN4B" : "EX4B");

    if (enable) {
        s->spi_regs[R_ADDR_MODE] |= R_ADDR_MODE_ADDR_4B_EN_MASK;
    } else {
        s->spi_regs[R_ADDR_MODE] &= ~R_ADDR_MODE_ADDR_4B_EN_MASK;
    }
    FLASH_CHANGE_STATE(s, DONE);
}

static void ot_spi_device_flash_decode_read_status(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    g_assert(f->slot <= SLOT_HW_READ_STATUS3);

    uint32_t status = s->spi_regs[R_FLASH_STATUS];
    f->buffer[0] = (uint8_t)(status >> (f->slot * 8u));
    f->len = sizeof(uint8_t);
    f->src = f->buffer;
    f->loop = true;

    trace_ot_spi_device_flash_read_status(s->ot_id, f->slot, f->buffer[0]);

    FLASH_CHANGE_STATE(s, BUFFER);
}

static void ot_spi_device_flash_decode_read_sfdp(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    f->src = f->buffer;
    FLASH_CHANGE_STATE(s, COLLECT);
    f->loop = true;
    f->len = 4; /* 3-byte address + 1 dummy byte */
}

static void ot_spi_device_flash_decode_read_data(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    unsigned dummy = 1;

    switch (f->slot) {
    case SLOT_HW_READ_NORMAL:
        dummy = 0;
        break;
    case SLOT_HW_READ_FAST:
    case SLOT_HW_READ_DUAL:
    case SLOT_HW_READ_QUAD:
    case SLOT_HW_READ_DUAL_IO:
    case SLOT_HW_READ_QUAD_IO:
        dummy = 1u;
        break;
    default:
        g_assert_not_reached();
    }

    f->src = f->buffer;
    FLASH_CHANGE_STATE(s, COLLECT);
    f->len = dummy + (ot_spi_device_is_addr4b_en(s) ? 4u : 3u);
}

static void ot_spi_device_flash_decode_hw_command(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    switch (f->slot) {
    case SLOT_HW_READ_STATUS1:
    case SLOT_HW_READ_STATUS2:
    case SLOT_HW_READ_STATUS3:
        ot_spi_device_flash_decode_read_status(s);
        break;
    case SLOT_HW_READ_JEDEC:
        ot_spi_device_flash_decode_read_jedec(s);
        break;
    case SLOT_HW_READ_SFDP:
        ot_spi_device_flash_decode_read_sfdp(s);
        break;
    case SLOT_HW_READ_NORMAL:
    case SLOT_HW_READ_FAST:
    case SLOT_HW_READ_DUAL:
    case SLOT_HW_READ_QUAD:
    case SLOT_HW_READ_DUAL_IO:
    case SLOT_HW_READ_QUAD_IO:
        ot_spi_device_flash_decode_read_data(s);
        break;
    case SLOT_HW_EN4B:
    case SLOT_HW_EX4B:
        ot_spi_device_flash_decode_addr4_enable(s);
        break;
    case SLOT_HW_WREN:
    case SLOT_HW_WRDI:
        ot_spi_device_flash_decode_write_enable(s);
        break;
    default:
        g_assert_not_reached();
    }
}

static void ot_spi_device_flash_exec_read_sfdp(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    unsigned address = ldl_be_p(f->src) >> 8u; /* discard dummy byte */
    f->pos = address % SPI_SRAM_SFDP_SIZE;
    f->len = SPI_SRAM_SFDP_SIZE;
    f->src = &((uint8_t *)s->sram)[SPI_SRAM_SFDP_OFFSET];
    f->loop = true;
    FLASH_CHANGE_STATE(s, BUFFER);
}

static void ot_spi_device_flash_exec_read_data(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    uint32_t address = ldl_be_p(f->buffer);
    if (!ot_spi_device_is_addr4b_en(s)) {
        address >>= 8u;
    }

    trace_ot_spi_device_flash_set_read_addr(s->ot_id, (uint32_t)address);

    if ((address >> SPI_SRAM_READ_LOG2_SIZE) + 1u !=
        (f->next_buffer_addr >> SPI_SRAM_READ_LOG2_SIZE)) {
        /*
         * either the FLASH_READ_BUFFER_CLR has not been triggered, or
         * subsequent read commands do not cover a continuous address range
         * (% buffer size)
         */
        trace_ot_spi_device_flash_read_config_error(s->ot_id, address,
                                                    f->next_buffer_addr);
    }

    f->address = address;
    FLASH_CHANGE_STATE(s, READ);

    f->src = (uint8_t *)s->sram;
    f->loop = true;
}

static void ot_spi_device_flash_exec_command(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    switch (f->slot) {
    case SLOT_HW_READ_SFDP:
        ot_spi_device_flash_exec_read_sfdp(s);
        break;
    case SLOT_HW_READ_NORMAL:
    case SLOT_HW_READ_FAST:
    case SLOT_HW_READ_DUAL:
    case SLOT_HW_READ_QUAD:
    case SLOT_HW_READ_DUAL_IO:
    case SLOT_HW_READ_QUAD_IO:
        ot_spi_device_flash_exec_read_data(s);
        break;
    default:
        g_assert_not_reached();
    }
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

    uint8_t tx = (f->pos < f->len) ? f->src[f->pos] : SPI_DEFAULT_TX_RX_VALUE;

    f->pos++;
    if (f->pos >= f->len) {
        if (f->loop) {
            f->pos = 0u;
        } else {
            FLASH_CHANGE_STATE(s, DONE);
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
         * The current implementation therefore only substitutes the SPI MISO
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
            if (!f->watermark_crossed) {
                trace_ot_spi_device_flash_read_threshold(s->ot_id, f->address,
                                                         threshold);
                s->spi_regs[R_INTR_STATE] |= INTR_READBUF_WATERMARK_MASK;
                pace_spibus = true;
                ot_spi_device_update_irqs(s);
            }
            /* should be reset on buffer switch */
            f->watermark_crossed = true;
        }
    }

    f->last_read_addr = f->address;
    f->address += 1u;

    /*
     * "If a new read command crosses the current buffer boundary, the SW clears
     *  the cross event for the HW to detect the address cross event again."
     */
    bool flip = f->address == f->next_buffer_addr;
    if (flip) {
        f->watermark_crossed = false;
        f->next_buffer_addr += SPI_SRAM_READ_SIZE;
        s->spi_regs[R_INTR_STATE] |= INTR_READBUF_FLIP_MASK;
        trace_ot_spi_device_flash_cross_buffer(s->ot_id, f->address,
                                               f->next_buffer_addr);
        pace_spibus = true;
        ot_spi_device_update_irqs(s);
    }

    if (pace_spibus) {
        ot_spi_device_flash_pace_spibus(s);
    }

    return tx;
}

static void ot_spi_device_flash_init_upload(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    f->pos = 0;
    f->len =
        ot_spi_device_flash_command_has_payload(f) ? SPI_SRAM_PAYLOAD_SIZE : 0u;
    s->spi_regs[R_UPLOAD_STATUS2] = 0;
    g_assert(f->payload);
    FLASH_CHANGE_STATE(s, UP_PAYLOAD);
}

static void ot_spi_device_flash_decode_sw_command(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    unsigned addr_size = ot_spi_device_get_command_address_size(s);
    f->pos = 0u;
    if (addr_size != 0u) {
        f->len = addr_size;
        FLASH_CHANGE_STATE(s, UP_ADDR);
    } else if (f->cmd_info & CMD_INFO_DUMMY_EN_MASK) {
        f->len = 1u;
        FLASH_CHANGE_STATE(s, UP_DUMMY);
    } else if (ot_spi_device_flash_command_is_upload(f)) {
        ot_spi_device_flash_init_upload(s);
    } else {
        /*
         * Any payload sent with a non-uploaded SW command is ignored. The
         * behaviour of this error case is not well specified for the hardware.
         */
        s->spi_regs[R_UPLOAD_STATUS2] = 0;
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
                trace_ot_spi_device_flash_push_address(s->ot_id, f->address);
                ot_fifo32_push(&f->address_fifo, f->address);
            } else {
                /* waiting for answer from OT team here */
                g_assert_not_reached();
            }
            if (f->cmd_info & CMD_INFO_DUMMY_EN_MASK) {
                f->len = 1u;
                FLASH_CHANGE_STATE(s, UP_DUMMY);
            } else if (ot_spi_device_flash_command_is_upload(f)) {
                ot_spi_device_flash_init_upload(s);
            } else {
                /*
                 * Any payload sent with a non-uploaded SW command is ignored.
                 * The behaviour of this error case is not well specified for
                 * the hardware.
                 */
                FLASH_CHANGE_STATE(s, DONE);
            }
        }
        break;
    case SPI_FLASH_UP_DUMMY:
        f->pos++;
        g_assert(f->pos == f->len);
        f->pos = 0u;
        if (ot_spi_device_flash_command_is_upload(f)) {
            ot_spi_device_flash_init_upload(s);
        } else {
            /*
             * Any payload sent with a non-uploaded SW command is ignored. The
             * behaviour of this error case is not well specified for the
             * hardware.
             */
            FLASH_CHANGE_STATE(s, DONE);
        }
        break;
    case SPI_FLASH_UP_PAYLOAD:
        if (ot_spi_device_flash_command_is_rx_payload(f)) {
            f->payload[f->pos % SPI_SRAM_PAYLOAD_SIZE] = rx;
        }
        f->pos++;
        break;
    case SPI_FLASH_DONE:
        FLASH_CHANGE_STATE(s, ERROR);
    /* fallthrough */
    case SPI_FLASH_ERROR:
        trace_ot_spi_device_flash_byte_unexpected(s->ot_id, rx);
        BUS_CHANGE_STATE(s, DISCARD);
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

    trace_ot_spi_device_flash_transfer(s->ot_id, "->", rx);

    uint8_t tx = SPI_DEFAULT_TX_RX_VALUE;

    switch (f->state) {
    case SPI_FLASH_IDLE:
        f->slot = SLOT_INVALID;
        f->pos = 0;
        f->len = 0;
        f->src = NULL;
        f->loop = false;
        if (ot_spi_device_flash_match_command_slot(s, rx)) {
            if (ot_spi_device_is_sw_command(f->slot)) {
                ot_spi_device_flash_decode_sw_command(s);
                ot_spi_device_flash_try_upload(s);
            } else {
                ot_spi_device_flash_decode_hw_command(s);
            }
        } else {
            /* this command cannot be processed, discard all remaining bytes */
            trace_ot_spi_device_flash_unknown_command(s->ot_id, rx);
            FLASH_CHANGE_STATE(s, ERROR);
            BUS_CHANGE_STATE(s, DISCARD);
        }
        break;
    case SPI_FLASH_COLLECT:
        if (!ot_spi_device_flash_collect(s, rx)) {
            ot_spi_device_flash_exec_command(s);
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
        FLASH_CHANGE_STATE(s, ERROR);
        break;
    case SPI_FLASH_ERROR:
        break;
    default:
        error_setg(&error_fatal, "unexpected state %s[%d]\n",
                   FLASH_STATE_NAME(f->state), f->state);
        g_assert_not_reached();
    }

    trace_ot_spi_device_flash_transfer(s->ot_id, "<-", tx);

    return tx;
}

static uint8_t ot_spi_device_flash_spi_transfer(OtSPIDeviceState *s, uint8_t rx)
{
    OtSPIHostClass *spihostc = OT_SPI_HOST_GET_CLASS(s->spi_host);
    return spihostc->ssi_downstream_transfer(s->spi_host, rx);
}

static void ot_spi_device_flash_resume_read(void *opaque)
{
    OtSPIDeviceState *s = opaque;

    trace_ot_spi_device_flash_pace(s->ot_id, "release",
                                   timer_pending(s->flash.irq_timer));
    qemu_chr_fe_accept_input(&s->chr);
}

static bool
ot_spi_device_flash_command_is_filter(OtSPIDeviceState *s, uint8_t cmd)
{
    return (bool)(s->spi_regs[R_CMD_FILTER_0 + (cmd / REG_BITS)] &
                  (1u << (cmd % REG_BITS)));
}

static uint8_t ot_spi_device_swap_byte_data(
    uint8_t byte, unsigned byte_sel, uint32_t swap_mask, uint32_t swap_data)
{
    g_assert(byte_sel < 4u);
    uint8_t mask = (uint8_t)(swap_mask >> (byte_sel * 8u));
    uint8_t data = (uint8_t)(swap_data >> (byte_sel * 8u));
    return (byte & ~mask) | (data & mask);
}

static bool ot_spi_device_flash_try_intercept_hw_command(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    uint32_t intercept_val32 = s->spi_regs[R_INTERCEPT_EN];
    bool intercepted = false;

    switch (f->slot) {
    case SLOT_HW_READ_STATUS1:
    case SLOT_HW_READ_STATUS2:
    case SLOT_HW_READ_STATUS3:
        if (FIELD_EX32(intercept_val32, INTERCEPT_EN, STATUS)) {
            ot_spi_device_flash_decode_read_status(s);
            intercepted = true;
        }
        break;
    case SLOT_HW_READ_JEDEC:
        if (FIELD_EX32(intercept_val32, INTERCEPT_EN, JEDEC)) {
            ot_spi_device_flash_decode_read_jedec(s);
            intercepted = true;
        }
        break;
    case SLOT_HW_READ_SFDP:
        if (FIELD_EX32(intercept_val32, INTERCEPT_EN, SFDP)) {
            ot_spi_device_flash_decode_read_sfdp(s);
            intercepted = true;
        }
        break;
    case SLOT_HW_READ_NORMAL:
    case SLOT_HW_READ_FAST:
    case SLOT_HW_READ_DUAL:
    case SLOT_HW_READ_QUAD:
    case SLOT_HW_READ_DUAL_IO:
    case SLOT_HW_READ_QUAD_IO:
        /* We try to intercept these at every read after an address is given */
        break;
    case SLOT_HW_EN4B:
    case SLOT_HW_EX4B:
        /* Always intercepted */
        ot_spi_device_flash_decode_addr4_enable(s);
        intercepted = true;
        break;
    case SLOT_HW_WREN:
    case SLOT_HW_WRDI:
        /* Always intercepted */
        ot_spi_device_flash_decode_write_enable(s);
        intercepted = true;
        break;
    default:
        break;
    }

    if (intercepted) {
        trace_ot_spi_device_flash_intercepted_command(s->ot_id, f->slot);
    }

    return intercepted;
}

static void ot_spi_device_flash_passthrough_command_params(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    OtSpiCommandParams *p = &f->cmd_params;

    p->address_size = ot_spi_device_get_command_address_size(s);
    p->cmd_addr_swap =
        (bool)SHARED_FIELD_EX32(f->cmd_info, CMD_INFO_ADDR_SWAP_EN);
    p->cmd_payload_swap =
        (bool)SHARED_FIELD_EX32(f->cmd_info, CMD_INFO_PAYLOAD_SWAP_EN);
    p->cmd_payload_dir_out =
        (bool)SHARED_FIELD_EX32(f->cmd_info, CMD_INFO_PAYLOAD_DIR);

    /*
     * SPI transfers in QEMU are modelled at the granularity of bytes,
     * therefore we can only support either 0 dummy cycles (dummy_en = 0), or
     * 8 dummy cycles (dummy_en = 1, dummy_size = 7). Anything in between we
     * round up to 1 dummy byte, so we only check dummy_en.
     */
    p->cmd_dummy = (bool)SHARED_FIELD_EX32(f->cmd_info, CMD_INFO_DUMMY_EN);

    if (p->cmd_dummy &&
        ((uint8_t)SHARED_FIELD_EX32(f->cmd_info, CMD_INFO_DUMMY_SIZE) != 7u)) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: %s: slot %d: set non-zero dummy cycle count is "
                      "unsupported, using 8 cycles (1 byte)",
                      __func__, s->ot_id, f->slot);
    }

    if (SHARED_FIELD_EX32(f->cmd_info, CMD_INFO_READ_PIPELINE_MODE) != 0u) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: %s: slot %d: 2-stage read pipeline is unsupported",
                      __func__, s->ot_id, f->slot);
    }

    trace_ot_spi_device_flash_command_params(s->ot_id, p->address_size,
                                             p->cmd_addr_swap, p->cmd_dummy,
                                             p->cmd_payload_swap);
}

static void
ot_spi_device_flash_passthrough_address_phase(OtSPIDeviceState *s, uint8_t rx)
{
    SpiDeviceFlash *f = &s->flash;

    OtSpiCommandParams *p = &f->cmd_params;

    f->len = 4u;

    f->buffer[f->pos] = rx;
    if (p->cmd_addr_swap) {
        unsigned byte_sel = (f->len - f->pos - 1u);
        uint8_t swapped_rx =
            ot_spi_device_swap_byte_data(rx, byte_sel,
                                         s->spi_regs[R_ADDR_SWAP_MASK],
                                         s->spi_regs[R_ADDR_SWAP_DATA]);
        trace_ot_spi_device_flash_swap_byte(s->ot_id, "address", byte_sel, rx,
                                            swapped_rx);
        rx = swapped_rx;
    }

    (void)ot_spi_device_flash_spi_transfer(s, rx);

    f->pos++;
    if (f->pos == f->len) { /* end of address phase */
        f->pos = 0u;
        f->address = ldl_be_p(f->buffer);

        /* if upload command, push address FIFO */
        if (ot_spi_device_flash_command_is_upload(f)) {
            if (!ot_fifo32_is_full(&f->address_fifo)) {
                ot_fifo32_push(&f->address_fifo, f->address);
            } else {
                g_assert_not_reached();
            }
        }

        if (p->cmd_dummy) {
            FLASH_CHANGE_STATE(s, PASSTHROUGH_UP_DUMMY);
        } else {
            FLASH_CHANGE_STATE(s, PASSTHROUGH_UP_PAYLOAD);
        }
    }
}

static void ot_spi_device_flash_passthrough_dummy_phase(OtSPIDeviceState *s)
{
    SpiDeviceFlash *f = &s->flash;

    OtSpiCommandParams *p = &f->cmd_params;

    g_assert(p->cmd_dummy);

    (void)ot_spi_device_flash_spi_transfer(s, SPI_DEFAULT_TX_RX_VALUE);

    FLASH_CHANGE_STATE(s, PASSTHROUGH_UP_PAYLOAD);
}

static uint8_t
ot_spi_device_flash_passthrough_payload_phase(OtSPIDeviceState *s, uint8_t rx)
{
    SpiDeviceFlash *f = &s->flash;

    OtSpiCommandParams *p = &f->cmd_params;

    if (p->cmd_payload_dir_out) { /* flash -> SPI Host */

        /* try intercept read to mailbox region */
        if (FIELD_EX32(s->spi_regs[R_INTERCEPT_EN], INTERCEPT_EN, MBX) != 0u) {
            if (f->slot >= SLOT_HW_READ_NORMAL &&
                f->slot <= SLOT_HW_READ_QUAD_IO) {
                if (p->address_size != 0u) {
                    if (ot_spi_device_is_mailbox_match(s, f->address)) {
                        trace_ot_spi_device_flash_intercept_mailbox(s->ot_id,
                                                                    f->address);
                        unsigned word_idx =
                            (f->address >> 2u) & (SPI_SRAM_MBX_SIZE - 1u);
                        unsigned byte_idx = f->address % 4u;
                        uint32_t word =
                            s->sram[(SPI_SRAM_MBX_OFFSET >> 2u) + word_idx];
                        uint8_t tx = (uint8_t)(word >> (8u * byte_idx));

                        f->address += 1u;
                        return tx;
                    }
                } else {
                    qemu_log_mask(
                        LOG_GUEST_ERROR,
                        "%s: %s: Mailbox read intercept enabled but HW READ "
                        "CMD %d has no address field",
                        __func__, s->ot_id, f->slot);
                }
            }
        }

        /* common out path: read from flash, update last read address */
        f->last_read_addr = f->address;
        f->address += 1u;
        return ot_spi_device_flash_spi_transfer(s, SPI_DEFAULT_TX_RX_VALUE);

    } /* SPI Host -> flash */

    /* if this command is to be uploaded, upload payload */
    if (ot_spi_device_flash_command_is_upload(f)) {
        f->payload[f->pos % SPI_SRAM_PAYLOAD_SIZE] = rx;
    }

    /* if payload swap is enabled, swap the first 4 bytes of payload */
    if (f->pos < 4u && p->cmd_payload_swap) {
        uint8_t swapped_rx =
            ot_spi_device_swap_byte_data(rx, f->pos,
                                         s->spi_regs[R_PAYLOAD_SWAP_MASK],
                                         s->spi_regs[R_PAYLOAD_SWAP_DATA]);
        trace_ot_spi_device_flash_swap_byte(s->ot_id, "payload", f->pos, rx,
                                            swapped_rx);
        rx = swapped_rx;
    }

    f->pos++;

    (void)ot_spi_device_flash_spi_transfer(s, rx);

    return SPI_DEFAULT_TX_RX_VALUE;
}


static uint8_t
ot_spi_device_flash_transfer_passthrough(OtSPIDeviceState *s, uint8_t rx)
{
    SpiDeviceFlash *f = &s->flash;

    OtSpiCommandParams *p = &f->cmd_params;

    trace_ot_spi_device_flash_transfer(s->ot_id, "->", rx);

    uint8_t tx = SPI_DEFAULT_TX_RX_VALUE;

    switch (f->state) {
    case SPI_FLASH_IDLE:
        f->slot = SLOT_INVALID;
        f->cmd_info = 0u;
        f->pos = 0u;
        f->len = 0u;
        f->address = 0u;
        f->src = NULL;
        f->loop = false;
        memset(f->buffer, 0u, 4u);
        /*
         * Unmatched commands are not necessarily erroneous and the HW
         * will continue the transfer with these default parameters,
         * unless it is filtered later on.
         */
        memset(&f->cmd_params, 0u, sizeof(OtSpiCommandParams));

        if (ot_spi_device_flash_match_command_slot(s, rx)) {
            if (ot_spi_device_flash_try_intercept_hw_command(s)) {
                break;
            }
            /* only matched software/not intercepted commands can be uploaded */
            ot_spi_device_flash_try_upload(s);
            ot_spi_device_flash_passthrough_command_params(s);
        }

        if (ot_spi_device_flash_command_is_filter(s, rx)) {
            /* command opcode is filtered, do not send to downstream */
            trace_ot_spi_device_flash_filtered_command(s->ot_id, rx);
            ibex_irq_raise(&s->passthrough_cs);
        } else {
            /* issue the command: assert chip select (active low) */
            ibex_irq_lower(&s->passthrough_cs);
            /* pass the opcode through */
            tx = ot_spi_device_flash_spi_transfer(s, rx);
        }

        if (p->address_size != 0u) {
            /* command has address field */
            f->pos = 4u - p->address_size;
            FLASH_CHANGE_STATE(s, PASSTHROUGH_UP_ADDR);
            break;
        }
        if (p->cmd_dummy) {
            /* no address field, but has dummy */
            FLASH_CHANGE_STATE(s, PASSTHROUGH_UP_DUMMY);
            break;
        }
        /* no address or dummy field, rest is payload */
        FLASH_CHANGE_STATE(s, PASSTHROUGH_UP_PAYLOAD);
        break;
    case SPI_FLASH_PASSTHROUGH_UP_ADDR:
        ot_spi_device_flash_passthrough_address_phase(s, rx);
        break;
    case SPI_FLASH_PASSTHROUGH_UP_DUMMY:
        ot_spi_device_flash_passthrough_dummy_phase(s);
        break;
    case SPI_FLASH_PASSTHROUGH_UP_PAYLOAD:
        tx = ot_spi_device_flash_passthrough_payload_phase(s, rx);
        break;
    /* HW intercepted commands */
    case SPI_FLASH_COLLECT:
        if (!ot_spi_device_flash_collect(s, rx)) {
            g_assert(f->slot == SLOT_HW_READ_SFDP);
            ot_spi_device_flash_exec_read_sfdp(s);
        }
        break;
    case SPI_FLASH_BUFFER:
        g_assert(f->slot <= SLOT_HW_READ_SFDP);
        tx = ot_spi_device_flash_read_buffer(s);
        break;
    case SPI_FLASH_DONE:
        FLASH_CHANGE_STATE(s, ERROR);
        break;
    case SPI_FLASH_ERROR:
        break;
    default:
        error_setg(&error_fatal, "unexpected state %s[%d]",
                   FLASH_STATE_NAME(f->state), f->state);
        g_assert_not_reached();
    }

    trace_ot_spi_device_flash_transfer(s->ot_id, "<-", tx);

    return tx;
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
    case R_INTERCEPT_EN:
    case R_ADDR_MODE:
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
    case R_STATUS:
        val32 = s->spi_regs[reg];
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
            qemu_log_mask(LOG_UNIMP, "%s: %s: CMD_FIFO is empty\n", __func__,
                          s->ot_id);
            val32 = 0;
        }
        break;
    case R_UPLOAD_ADDRFIFO:
        if (!ot_fifo32_is_empty(&f->address_fifo)) {
            val32 = ot_fifo32_pop(&f->address_fifo);
        } else {
            qemu_log_mask(LOG_UNIMP, "%s: %s: ADDR_FIFO is empty\n", __func__,
                          s->ot_id);
            val32 = 0;
        }
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: W/O register 0x%02x (%s)\n",
                      __func__, s->ot_id, (uint32_t)addr, SPI_REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        val32 = 0;
        break;
    }

    if (reg != R_INTR_STATE || val32 != 0) {
        uint32_t pc = ibex_get_current_pc();
        trace_ot_spi_device_io_spi_read_out(s->ot_id, (uint32_t)addr,
                                            SPI_REG_NAME(reg), val32, pc);
    }

    return (uint64_t)val32;
};

static void ot_spi_device_spi_regs_write(void *opaque, hwaddr addr,
                                         uint64_t val64, unsigned size)
{
    OtSPIDeviceState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_spi_device_io_spi_write_in(s->ot_id, (uint32_t)addr,
                                        SPI_REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK & ~(INTR_TPM_HEADER_NOT_EMPTY_MASK);
        s->spi_regs[reg] &= ~val32; /* RW1C */
        ot_spi_device_update_irqs(s);
        if (!ot_spi_device_is_tpm_enabled(s) &&
            !ot_spi_device_flash_is_readbuf_irq(s)) {
            /*
             * no need to trigger the timer if in tpm mode or readbuf IRQs have
             * been cleared.
             */
            trace_ot_spi_device_flash_pace(s->ot_id, "clear",
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
        s->spi_regs[reg] = val32;
        ot_spi_device_update_alerts(s);
        break;
    case R_CONTROL:
        if (val32 & R_CONTROL_FLASH_STATUS_FIFO_CLR_MASK) {
            if (s->bus.state != SPI_BUS_IDLE) {
                /*
                 * "The reset should only be used when the upstream SPI host is
                 *  known to be inactive."
                 */
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: %s: Flash status FIFO cleared while SPI "
                              "host not idle\n",
                              __func__, s->ot_id);
                /* clear it anyway, with undefined consequences */
            }
            s->spi_regs[R_FLASH_STATUS] = 0x0u; /* reset value */
        }
        if (val32 & R_CONTROL_FLASH_READ_BUFFER_CLR_MASK) {
            if (s->bus.state != SPI_BUS_IDLE) {
                /*
                 * "The reset should only be used when the upstream SPI host is
                 *  known to be inactive."
                 */
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: %s: Flash read buffer cleared while SPI "
                              "host not idle",
                              __func__, s->ot_id);
                /* clear it anyway, with undefined consequences */
            }
            ot_spi_device_flash_clear_readbuffer(s);
        }
        val32 &= R_CONTROL_MODE_MASK;
        if (val32 != (s->spi_regs[reg] & R_CONTROL_MODE_MASK)) {
            ot_spi_device_clear_modes(s);
            if (s->bus.state == SPI_BUS_FLASH) {
                /*
                 * Hardware assumes that control mode does not change during a
                 * transaction, so the behaviour is undefined if that happens.
                 * We choose to cancel any ongoing SPI transfer until the next
                 * CS.
                 */
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: %s: Flash mode changed during transfer, "
                              "discarding rest of transfer\n",
                              __func__, s->ot_id);
                BUS_CHANGE_STATE(s, DISCARD);
            }
        }
        s->spi_regs[reg] = val32;
        switch (ot_spi_device_get_mode(s)) {
        case CTRL_MODE_INVALID:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: invalid mode\n", __func__,
                          s->ot_id);
        /* fallthrough */
        case CTRL_MODE_FLASH:
        case CTRL_MODE_DISABLED:
            ibex_irq_lower(&s->passthrough_en);
            break;
        case CTRL_MODE_PASSTHROUGH:
            ibex_irq_raise(&s->passthrough_en);
            break;
        }
        break;
    case R_CFG:
        val32 &= CFG_MASK;
        s->spi_regs[reg] = val32;
        break;
    case R_INTERCEPT_EN:
        val32 &= INTERCEPT_EN_MASK;
        s->spi_regs[reg] = val32;
        break;
    case R_ADDR_MODE:
        s->spi_regs[reg] &= ~R_ADDR_MODE_ADDR_4B_EN_MASK;
        s->spi_regs[reg] |= val32 & R_ADDR_MODE_ADDR_4B_EN_MASK; /* RW */
        break;
    case R_FLASH_STATUS:
        s->spi_regs[reg] &= val32 & FLASH_STATUS_RW0C_MASK; /* RW0C */
        s->spi_regs[reg] &= ~FLASH_STATUS_RW_MASK;
        s->spi_regs[reg] |= val32 & FLASH_STATUS_RW_MASK; /* RW */
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
    case R_LAST_READ_ADDR:
    case R_STATUS:
    case R_UPLOAD_STATUS:
    case R_UPLOAD_STATUS2:
    case R_UPLOAD_CMDFIFO:
    case R_UPLOAD_ADDRFIFO:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: R/O register 0x%02x (%s)\n",
                      __func__, s->ot_id, (uint32_t)addr, SPI_REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
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
    case R_TPM_CMD_ADDR:
        s->tpm_regs[R_TPM_STATUS] &= ~R_TPM_STATUS_CMDADDR_NOTEMPTY_MASK;
        s->spi_regs[R_INTR_STATE] &= ~INTR_TPM_HEADER_NOT_EMPTY_MASK;
        ot_spi_device_update_irqs(s);
        /* fall through*/
    case R_TPM_STATUS:
    case R_TPM_CFG:
    case R_TPM_CAP:
    case R_TPM_ACCESS_0:
    case R_TPM_ACCESS_1:
    case R_TPM_STS:
    case R_TPM_INTF_CAPABILITY:
    case R_TPM_INT_ENABLE:
    case R_TPM_INT_VECTOR:
    case R_TPM_INT_STATUS:
    case R_TPM_DID_VID:
    case R_TPM_RID:
        val32 = s->tpm_regs[reg];
        break;
    case R_TPM_READ_FIFO:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: W/O register 0x%02x (%s)\n",
                      __func__, s->ot_id, (uint32_t)addr, TPM_REG_NAME(reg));
        val32 = 0u;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        val32 = 0u;
        break;
    }


    uint32_t pc = ibex_get_current_pc();
    trace_ot_spi_device_io_tpm_read_out(s->ot_id, (uint32_t)addr,
                                        TPM_REG_NAME(reg), val32, pc);

    return (uint64_t)val32;
};

static void ot_spi_device_tpm_regs_write(void *opaque, hwaddr addr,
                                         uint64_t val64, unsigned size)
{
    OtSPIDeviceState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_spi_device_io_tpm_write_in(s->ot_id, (uint32_t)addr,
                                        TPM_REG_NAME(reg), val32, pc);

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
        s->tpm_regs[reg] = val32;
        break;
    case R_TPM_STATUS:
        val32 &= R_TPM_STATUS_WRFIFO_PENDING_MASK;
        s->tpm_regs[reg] &=
            val32 | ~R_TPM_STATUS_WRFIFO_PENDING_MASK; /* RW0C */
        break;
    case R_TPM_READ_FIFO:
        fifo8_push(&s->tpm.rdfifo, (uint8_t)(val32 >> 0) & 0xffu);
        fifo8_push(&s->tpm.rdfifo, (uint8_t)(val32 >> 8u) & 0xffu);
        fifo8_push(&s->tpm.rdfifo, (uint8_t)(val32 >> 16u) & 0xffu);
        fifo8_push(&s->tpm.rdfifo, (uint8_t)(val32 >> 24u) & 0xffu);
        break;
    case R_TPM_CAP:
    case R_TPM_CMD_ADDR:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: R/O register 0x%02x (%s)\n",
                      __func__, s->ot_id, (uint32_t)addr, TPM_REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        break;
    }
};

static MemTxResult ot_spi_device_buf_read_with_attrs(
    void *opaque, hwaddr addr, uint64_t *val64, unsigned size, MemTxAttrs attrs)
{
    OtSPIDeviceState *s = opaque;
    (void)attrs;
    uint32_t val32;

    hwaddr last = addr + (hwaddr)(size - 1u);

    if (addr < SPI_SRAM_INGRESS_OFFSET) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: cannot read egress buffer 0x%03x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        return MEMTX_DECODE_ERROR;
    }

    if ((addr >= SPI_SRAM_PAYLOAD_OFFSET &&
         last < (SPI_SRAM_PAYLOAD_OFFSET + SPI_SRAM_PAYLOAD_SIZE)) ||
        (addr >= SPI_SRAM_TPM_WRITE_OFFSET &&
         last < (SPI_SRAM_TPM_WRITE_OFFSET + SPI_SRAM_TPM_WRITE_SIZE))) {
        /* flash payload and tpm write buffers */
        val32 = s->sram[addr >> 2u];
    } else if (addr >= SPI_SRAM_CMD_OFFSET &&
               last < (SPI_SRAM_CMD_OFFSET + SPI_SRAM_CMD_SIZE)) {
        /* flash command FIFO */
        val32 = ((const uint32_t *)s->flash.cmd_fifo.data)[addr >> 2u];
    } else if (addr >= SPI_SRAM_ADDR_OFFSET &&
               last < (SPI_SRAM_ADDR_OFFSET + SPI_SRAM_ADDR_SIZE)) {
        /* flash address FIFO */
        val32 = s->flash.address_fifo.data[addr >> 2u];
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Invalid ingress buffer access to "
                      "0x%03x-0x%03x\n",
                      __func__, s->ot_id, (uint32_t)addr, (uint32_t)last);
        val32 = 0;
    }

    /* TODO: check which buffers can only be accessed as 32-bit locations */

    unsigned addr_offset = (addr & 3u);
    g_assert((addr_offset + size) <= 4u);
    val32 >>= addr_offset << 3u;

    uint32_t pc = ibex_get_current_pc();
    trace_ot_spi_device_io_buf_read_out(s->ot_id, (uint32_t)addr, size, val32,
                                        pc);

    *val64 = (uint64_t)val32;

    return MEMTX_OK;
}

static MemTxResult ot_spi_device_buf_write_with_attrs(
    void *opaque, hwaddr addr, uint64_t val64, unsigned size, MemTxAttrs attrs)
{
    OtSPIDeviceState *s = opaque;
    (void)attrs;

    uint32_t val32 = (uint32_t)val64;
    uint32_t pc = ibex_get_current_pc();
    trace_ot_spi_device_io_buf_write_in(s->ot_id, (uint32_t)addr, size, val32,
                                        pc);

    hwaddr last = addr + (hwaddr)(size - 1u);

    if (last >= SPI_SRAM_INGRESS_OFFSET) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: cannot write ingress buffer 0x%03x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        return MEMTX_DECODE_ERROR;
    }
    s->sram[addr >> 2u] = val32;

    return MEMTX_OK;
}

static void ot_spi_device_tpm_init_buffers(OtSPIDeviceState *s)
{
    s->tpm.write_pos = 0u;
    s->tpm.len = SPI_SRAM_TPM_WRITE_SIZE;
}

static void ot_spi_device_chr_handle_header(OtSPIDeviceState *s)
{
    SpiDeviceBus *bus = &s->bus;

    uint32_t size = 0u;
    const uint8_t *hdr =
        fifo8_pop_bufptr(&bus->chr_fifo, SPI_BUS_HEADER_SIZE, &size);

    if (size != SPI_BUS_HEADER_SIZE) {
        trace_ot_spi_device_chr_error(s->ot_id, "invalid header size");
        s->spi_regs[R_STATUS] |= R_STATUS_CSB_MASK;
        BUS_CHANGE_STATE(s, ERROR);
        return;
    }

    if (hdr[0] != '/' || hdr[1u] != 'C' || hdr[2u] != 'S' ||
        hdr[3u] != SPI_BUS_PROTO_VER) {
        trace_ot_spi_device_chr_error(s->ot_id, "invalid header");
        s->spi_regs[R_STATUS] |= R_STATUS_CSB_MASK;
        BUS_CHANGE_STATE(s, ERROR);
        return;
    }

    unsigned word = ldl_le_p(&hdr[4u]);
    bus->byte_count = word >> 16u;
    uint8_t mode = word & 0xfu;
    bus->release = !((word >> 7) & 0x1);

    bus->rev_rx = (bool)(mode & R_CFG_RX_ORDER_MASK);
    bus->rev_tx = (bool)(mode & R_CFG_TX_ORDER_MASK);
    /* if phase or polarity is not mode 0, corrupt data */
    uint8_t comm = mode ^ (uint8_t)s->spi_regs[R_CFG];
    bus->mode = (comm != 0) ? 0xFF : 0x00;

    trace_ot_spi_device_chr_handle_packet(s->ot_id, bus->byte_count,
                                          bus->release, bus->rev_rx ? 'l' : 'm',
                                          bus->rev_tx ? 'l' : 'm',
                                          bus->mode ? "mismatch" : "ok");

    if (!bus->byte_count) {
        /* no payload, stay in IDLE (handle_header is only called from IDLE) */
        return;
    }

    /* @todo: also update the TPM CSB value when used in the protocol */
    s->spi_regs[R_STATUS] &= ~R_STATUS_CSB_MASK;

    /* discard the packet if we're within a failed transaction */
    if (bus->failed_transaction) {
        BUS_CHANGE_STATE(s, DISCARD);
        return;
    }

    /* @todo: Check that the tpm chip-select was assigned.*/
    if (ot_spi_device_is_tpm_enabled(s)) {
        ot_spi_device_tpm_init_buffers(s);
        BUS_CHANGE_STATE(s, TPM);
        return;
    }

    switch (ot_spi_device_get_mode(s)) {
    case CTRL_MODE_FLASH:
    case CTRL_MODE_PASSTHROUGH:
        BUS_CHANGE_STATE(s, FLASH);
        break;
    case CTRL_MODE_DISABLED:
    case CTRL_MODE_INVALID:
    default:
        BUS_CHANGE_STATE(s, DISCARD);
        break;
    }
}

static void ot_spi_device_chr_send_discard(OtSPIDeviceState *s, unsigned count)
{
    const uint8_t buf[1u] = { SPI_DEFAULT_TX_RX_VALUE };

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
    s->bus.byte_count -= size;
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
        uint8_t tx;
        switch (ot_spi_device_get_mode(s)) {
        case CTRL_MODE_FLASH:
            tx = ot_spi_device_flash_transfer(s, rx);
            break;
        case CTRL_MODE_PASSTHROUGH:
            tx = ot_spi_device_flash_transfer_passthrough(s, rx);
            break;
        default:
            g_assert_not_reached();
        }
        tx ^= bus->mode;
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

static bool
ot_spi_device_tpm_get_hw_register(OtSPIDeviceState *s, uint32_t *reg_val)
{
    /* clang-format off */
    static const SpiTpmHwAddrMap TPM_HW_ADDR[] = {
      /*{ register_addr, tpm_register_offset, size }*/
        { 0x000u,        R_TPM_ACCESS_0,        4u },
        { 0x004u,        R_TPM_ACCESS_1,        4u },
        { 0x008u,        R_TPM_INT_ENABLE,      4u },
        { 0x00Cu,        R_TPM_INT_VECTOR,      1u },
        { 0x010u,        R_TPM_INT_STATUS,      4u },
        { 0x014u,        R_TPM_INTF_CAPABILITY, 4u },
        { 0x018u,        R_TPM_STS,             4u },
        { 0x028u,        0u,                    0u }, /* Hash Start */
        { 0xF00u,        R_TPM_DID_VID,         4u },
        { 0xF04u,        R_TPM_RID,             4u },
    };
    /* clang-format on */

    if (reg_val) {
        *reg_val = UINT32_MAX;
    }
    for (uint32_t idx = 0u; idx < ARRAY_SIZE(TPM_HW_ADDR); idx++) {
        if (TPM_HW_ADDR[idx].addr == (s->tpm.reg & ~0x3u)) {
            unsigned nbytes = TPM_HW_ADDR[idx].nbytes;
            g_assert(nbytes <= sizeof(uint32_t));
            if (reg_val && nbytes != 0u) {
                uint32_t mask =
                    UINT32_MAX >> (8u * (sizeof(uint32_t) - nbytes));
                *reg_val &= ~mask;
                *reg_val |= s->tpm_regs[TPM_HW_ADDR[idx].offset];
            }
            return true;
        }
    }
    return false;
}

static void ot_spi_device_tpm_idle_state(OtSPIDeviceState *s,
                                         const uint8_t *buf, unsigned size)
{
    g_assert(size == 1u);
    SpiDeviceTpm *tpm = &s->tpm;
    fifo8_reset(&tpm->rdfifo);
    tpm->opcode = buf[0u];
    tpm->read = tpm->opcode >> TPM_OPCODE_READ_BIT;
    tpm->transfer_size = (tpm->opcode & TPM_OPCODE_SIZE_MASK) + 1u;
    tpm->state = SPI_TPM_ADDR;
    tpm->can_receive = 3u;
}

static void ot_spi_device_tpm_addr_state(OtSPIDeviceState *s,
                                         const uint8_t *buf, unsigned size)
{
    g_assert(size == 3u);
    SpiDeviceTpm *tpm = &s->tpm;
    tpm->reg = buf[1u] << 8u | buf[2u];
    tpm->locality = tpm->reg >> 12u;
    s->tpm_regs[R_TPM_CMD_ADDR] =
        (tpm->opcode << R_TPM_CMD_ADDR_CMD_SHIFT) | tpm->reg;

    /* When read, we immediately signal the software to fill the FIFO */
    if (tpm->read) {
        s->tpm_regs[R_TPM_STATUS] |= R_TPM_STATUS_CMDADDR_NOTEMPTY_MASK;
        s->spi_regs[R_INTR_STATE] |= INTR_TPM_HEADER_NOT_EMPTY_MASK;
        ot_spi_device_update_irqs(s);
    }

    tpm->can_receive = 1u;
    tpm->should_sw_handle = true;
    if (!ot_spi_device_is_tpm_mode_crb(s) &&
        !ot_spi_device_tpm_disable_hw_regs(s) && buf[0u] == TPM_ADDR_HEADER) {
        bool is_hw_register = ot_spi_device_tpm_get_hw_register(s, NULL);
        tpm->should_sw_handle = !is_hw_register;
    }

    tpm->state = tpm->should_sw_handle ? SPI_TPM_WAIT : SPI_TPM_START_BYTE;
}

static void ot_spi_device_tpm_write_state(OtSPIDeviceState *s,
                                          const uint8_t *buf, unsigned size)
{
    SpiDeviceTpm *tpm = &s->tpm;
    memcpy(&tpm->write_buffer[tpm->write_pos], buf, size);
    tpm->write_pos += size;
    tpm->state =
        tpm->write_pos >= tpm->transfer_size ? SPI_TPM_IDLE : tpm->state;

    s->tpm_regs[R_TPM_STATUS] |= R_TPM_STATUS_CMDADDR_NOTEMPTY_MASK;
    s->spi_regs[R_INTR_STATE] |= INTR_TPM_HEADER_NOT_EMPTY_MASK;
    ot_spi_device_update_irqs(s);
}

static void ot_spi_device_tpm_read_state(OtSPIDeviceState *s, unsigned size,
                                         uint8_t *tx_buf)
{
    SpiDeviceTpm *tpm = &s->tpm;
    g_assert(fifo8_pop_buf(&tpm->rdfifo, tx_buf, size) == size);
    tpm->state = SPI_TPM_IDLE;
}

static void ot_spi_device_tpm_read_hw_state(OtSPIDeviceState *s, unsigned size,
                                            uint8_t *tx_buf)
{
    SpiDeviceTpm *tpm = &s->tpm;
    uint32_t val32;
    ot_spi_device_tpm_get_hw_register(s, &val32);
    g_assert(size == sizeof(uint32_t));
    stl_le_p(tx_buf, val32);
    tpm->state = SPI_TPM_IDLE;
}

static void ot_spi_device_tpm_wait_state(OtSPIDeviceState *s)
{
    SpiDeviceTpm *tpm = &s->tpm;
    if (!tpm->read || (fifo8_num_used(&tpm->rdfifo) >= tpm->transfer_size)) {
        tpm->state = SPI_TPM_START_BYTE;
    }
}

static void ot_spi_device_tpm_start_byte_state(OtSPIDeviceState *s,
                                               unsigned size, uint8_t *tx_buf)
{
    SpiDeviceTpm *tpm = &s->tpm;
    tpm->state = SPI_TPM_WRITE;
    if (tpm->read) {
        tpm->state = tpm->should_sw_handle ? SPI_TPM_READ : SPI_TPM_READ_HW_REG;
    }
    tx_buf[size - 1u] = TPM_READY;
    tpm->can_receive = tpm->transfer_size;
}

static void ot_spi_device_tpm_state_machine(OtSPIDeviceState *s,
                                            const uint8_t *buf, unsigned size)
{
    g_assert(size <= SPI_TPM_READ_FIFO_SIZE_BYTES);
    uint8_t tx_buf[SPI_TPM_READ_FIFO_SIZE_BYTES] = { 0u };

    SpiDeviceTpm *tpm = &s->tpm;
    switch (tpm->state) {
    case SPI_TPM_IDLE:
        ot_spi_device_tpm_idle_state(s, buf, size);
        break;
    case SPI_TPM_ADDR:
        ot_spi_device_tpm_addr_state(s, buf, size);
        break;
    case SPI_TPM_WRITE:
        ot_spi_device_tpm_write_state(s, buf, size);
        break;
    case SPI_TPM_READ:
        ot_spi_device_tpm_read_state(s, size, tx_buf);
        break;
    case SPI_TPM_READ_HW_REG:
        ot_spi_device_tpm_read_hw_state(s, size, tx_buf);
        break;
    case SPI_TPM_WAIT:
        ot_spi_device_tpm_wait_state(s);
        break;
    case SPI_TPM_START_BYTE:
        ot_spi_device_tpm_start_byte_state(s, size, tx_buf);
        break;
    case SPI_TPM_END:
        /* Wait for the cs de-assertion.*/
        s->bus.byte_count = 0u;
        tpm->state = SPI_TPM_IDLE;
        return;
    default:
        break;
    };
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_write(&s->chr, tx_buf, (int)size);
    }
    s->bus.byte_count -= size;
    tpm->can_receive = tpm->state == SPI_TPM_IDLE ? 1u : tpm->can_receive;
}

static int ot_spi_device_chr_can_receive(void *opaque)
{
    OtSPIDeviceState *s = opaque;
    SpiDeviceBus *bus = &s->bus;
    unsigned length = 0u;

    switch (bus->state) {
    case SPI_BUS_IDLE:
        length = fifo8_num_free(&bus->chr_fifo);
        break;
    case SPI_BUS_FLASH:
        length = timer_pending(s->flash.irq_timer) ? 0 : 1u;
        break;
    case SPI_BUS_TPM:
        length = s->tpm.can_receive;
        break;
    case SPI_BUS_DISCARD:
        length = 1u;
        break;
    case SPI_BUS_ERROR:
        length = 0;
        break;
    default:
        error_setg(&error_fatal, "unexpected state %d\n", bus->state);
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
            /* a full header has been received, it can be decoded */
            ot_spi_device_chr_handle_header(s);
        }
        break;
    case SPI_BUS_FLASH:
        ot_spi_device_chr_recv_flash(s, buf, (unsigned)size);
        break;
    case SPI_BUS_TPM:
        ot_spi_device_tpm_state_machine(s, buf, (unsigned)size);
        break;
    case SPI_BUS_DISCARD:
    case SPI_BUS_ERROR:
        ot_spi_device_chr_recv_discard(s, buf, (unsigned)size);
        /* we need to discard the other packets in this transaction */
        bus->failed_transaction = true;
        break;
    default:
        g_assert_not_reached();
        break;
    }

    if (!bus->byte_count && bus->state != SPI_BUS_ERROR) {
        if (bus->release) {
            ot_spi_device_release(s);
        } else {
            BUS_CHANGE_STATE(s, IDLE);
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

        ot_spi_device_release(s);
    }

    if (event == CHR_EVENT_CLOSED) {
        ot_spi_device_release(s);
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


    fifo8_reset(&bus->chr_fifo);

    ot_spi_device_release(s);

    if (s->watch_tag > 0) {
        g_source_remove(s->watch_tag);
        /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                             &ot_spi_device_chr_watch_cb, s);
    }

    return 0;
}

static const Property ot_spi_device_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtSPIDeviceState, ot_id),
    DEFINE_PROP_CHR("chardev", OtSPIDeviceState, chr),
    DEFINE_PROP_LINK("spi-host", OtSPIDeviceState, spi_host, TYPE_OT_SPI_HOST,
                     OtSPIHostState *),
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
    .impl.min_access_size = 1u,
    .impl.max_access_size = 4u,
};

static void ot_spi_device_reset_enter(Object *obj, ResetType type)
{
    OtSPIDeviceClass *c = OT_SPI_DEVICE_GET_CLASS(obj);
    OtSPIDeviceState *s = OT_SPI_DEVICE(obj);
    SpiDeviceFlash *f = &s->flash;
    SpiDeviceBus *bus = &s->bus;

    trace_ot_spi_device_reset(s->ot_id, "enter");

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    ot_spi_device_clear_modes(s);

    memset(s->spi_regs, 0u, SPI_REGS_SIZE);
    memset(s->tpm_regs, 0u, TPM_REGS_SIZE);

    fifo8_reset(&bus->chr_fifo);
    /* not sure if the following FIFOs should be reset on clear_modes instead */
    fifo8_reset(&f->cmd_fifo);
    ot_fifo32_reset(&f->address_fifo);

    ot_spi_device_release(s);
    s->spi_regs[R_CONTROL] = 0x10u;
    s->spi_regs[R_STATUS] = 0x60u;
    s->spi_regs[R_JEDEC_CC] = 0x7fu;
    for (unsigned ix = 0u; ix < PARAM_NUM_CMD_INFO; ix++) {
        s->spi_regs[R_CMD_INFO_0 + ix] = 0x7000u;
    }

    memset(&f->cmd_params, 0u, sizeof(OtSpiCommandParams));

    s->tpm_regs[R_TPM_CAP] = 0x660100u;

    ibex_irq_lower(&s->passthrough_en);
    ibex_irq_raise(&s->passthrough_cs);

    ot_spi_device_update_irqs(s);
    ot_spi_device_update_alerts(s);
}

static void ot_spi_device_realize(DeviceState *dev, Error **errp)
{
    OtSPIDeviceState *s = OT_SPI_DEVICE(dev);
    (void)errp;

    g_assert(s->ot_id);

    g_assert(s->spi_host);

    qemu_chr_fe_set_handlers(&s->chr, &ot_spi_device_chr_can_receive,
                             &ot_spi_device_chr_receive,
                             &ot_spi_device_chr_event_hander,
                             &ot_spi_device_chr_be_change, s, NULL, true);
}

static void ot_spi_device_init(Object *obj)
{
    OtSPIDeviceState *s = OT_SPI_DEVICE(obj);
    SpiDeviceFlash *f = &s->flash;
    SpiDeviceBus *bus = &s->bus;

    memory_region_init(&s->mmio.main, obj, TYPE_OT_SPI_DEVICE ".mmio",
                       SPI_DEVICE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio.main);
    memory_region_init_io(&s->mmio.spi, obj, &ot_spi_device_spi_regs_ops, s,
                          TYPE_OT_SPI_DEVICE ".spi-regs", SPI_REGS_SIZE);
    memory_region_add_subregion(&s->mmio.main, SPI_DEVICE_SPI_REGS_OFFSET,
                                &s->mmio.spi);
    memory_region_init_io(&s->mmio.tpm, obj, &ot_spi_device_tpm_regs_ops, s,
                          TYPE_OT_SPI_DEVICE ".tpm-regs", TPM_REGS_SIZE);
    memory_region_add_subregion(&s->mmio.main, SPI_DEVICE_TPM_REGS_OFFSET,
                                &s->mmio.tpm);
    memory_region_init_io(&s->mmio.buf, obj, &ot_spi_device_buf_ops, s,
                          TYPE_OT_SPI_DEVICE ".buf", SRAM_SIZE);
    memory_region_add_subregion(&s->mmio.main, SPI_DEVICE_SRAM_OFFSET,
                                &s->mmio.buf);

    s->spi_regs = g_new0(uint32_t, SPI_REGS_COUNT);
    s->tpm_regs = g_new0(uint32_t, TPM_REGS_COUNT);
    s->sram = g_new(uint32_t, SRAM_SIZE / sizeof(uint32_t));

    fifo8_create(&bus->chr_fifo, SPI_BUS_HEADER_SIZE);
    fifo8_create(&f->cmd_fifo, SPI_SRAM_CMD_SIZE / sizeof(uint32_t));
    fifo8_create(&s->tpm.rdfifo, SPI_TPM_READ_FIFO_SIZE_BYTES);
    ot_fifo32_create(&f->address_fifo, SPI_SRAM_ADDR_SIZE / sizeof(uint32_t));
    f->buffer =
        (uint8_t *)g_new0(uint32_t, SPI_FLASH_BUFFER_SIZE / sizeof(uint32_t));

    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }

    /* Passthrough enable is active high, CS is active low */
    ibex_qdev_init_irq_default(OBJECT(s), &s->passthrough_en,
                               OT_SPI_DEVICE_PASSTHROUGH_EN, 0);
    ibex_qdev_init_irq_default(OBJECT(s), &s->passthrough_cs,
                               OT_SPI_DEVICE_PASSTHROUGH_CS, 1);

    /*
     * This timer is used to hand over to the vCPU whenever a READBUF_* irq is
     * raised, otherwide the vCPU would not be able to get notified that a
     * buffer refill is required by the HW. In other words, this is poor man's
     * co-operative multitasking between the vCPU and the IO thread
     */
    f->irq_timer =
        timer_new_ns(OT_VIRTUAL_CLOCK, &ot_spi_device_flash_resume_read, s);
}

static void ot_spi_device_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_spi_device_realize;
    device_class_set_props(dc, ot_spi_device_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtSPIDeviceClass *sc = OT_SPI_DEVICE_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_spi_device_reset_enter, NULL,
                                       NULL, &sc->parent_phases);
}

static const TypeInfo ot_spi_device_info = {
    .name = TYPE_OT_SPI_DEVICE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtSPIDeviceState),
    .instance_init = &ot_spi_device_init,
    .class_size = sizeof(OtSPIDeviceClass),
    .class_init = &ot_spi_device_class_init,
};

static void ot_spi_device_register_types(void)
{
    type_register_static(&ot_spi_device_info);
}

type_init(ot_spi_device_register_types);
