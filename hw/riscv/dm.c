/*
 * QEMU Debug Module Interface and Controller
 *
 * Copyright (c) 2022-2024 Rivos, Inc.
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without (limitation) the rights
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
 * Generated code for absract commands has been extracted from the PULP Debug
 * module whose file (dm_mem.sv) contains the following copyright. This piece of
 * code is self contained within the `riscv_dm_access_register` function:
 *
 *   Copyright and related rights are licensed under the Solderpad Hardware
 *   License, Version 0.51 (the “License”); you may not use this file except in
 *   compliance with the License. You may obtain a copy of the License at
 *   http://solderpad.org/licenses/SHL-0.51. Unless required by applicable law
 *   or agreed to in writing, software, hardware and materials distributed under
 *   this License is distributed on an “AS IS” BASIS, WITHOUT WARRANTIES OR
 *   CONDITIONS OF ANY KIND, either express or implied. See the License for the
 *   specific language governing permissions and limitations under the License.
 *
 * Limitations:
 * - Unsupported features:
 *   - PMP management
 *   - DCSR.STEPIE
 *   - Cancellation of outstanding halt request
 *   - Halt on reset
 * - Not tested:
 *   - User mode debugging
 */

#include "qemu/osdep.h"
#include "qemu/compiler.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "disas/dis-asm.h"
#include "exec/cpu_ldst.h"
#include "hw/boards.h"
#include "hw/core/cpu.h"
#include "hw/jtag/tap_ctrl.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/debug.h"
#include "hw/riscv/dm.h"
#include "hw/riscv/dtm.h"
#include "sysemu/hw_accel.h"
#include "sysemu/runstate.h"
#include "target/riscv/cpu.h"
#include "trace.h"


#undef TRACE_CPU_STATES

/*
 * Register definitions
 */

#define RISCV_DEBUG_DM_VERSION     2u /* Debug Module v0.13.x */
#define RISCV_DEBUG_SB_VERSION     1u /* System Bus v1.0 */
#define RISCVDM_ABSTRACTDATA_SLOTS 10u
#define ADDRESS_BITS               7u

/* clang-format off */

/* Debug Module registers */
REG32(DATA0, 0x04u)
REG32(DATA1, 0x05u)
REG32(DATA2, 0x06u)
REG32(DATA3, 0x07u)
REG32(DATA4, 0x08u)
REG32(DATA5, 0x09u)
REG32(DATA6, 0x0au)
REG32(DATA7, 0x0bu)
REG32(DATA8, 0x0cu)
REG32(DATA9, 0x0du)
REG32(DATA10, 0x0eu)
REG32(DATA11, 0x0fu)
REG32(DMCONTROL, 0x10u)
    FIELD(DMCONTROL, DMACTIVE, 0u, 1u)
    FIELD(DMCONTROL, NDMRESET, 1u, 1u)
    FIELD(DMCONTROL, CLRRESETHALTREQ, 2u, 1u)
    FIELD(DMCONTROL, SETRESETHALTREQ, 3u, 1u)
    FIELD(DMCONTROL, HARTSELHI, 6u, 10u)
    FIELD(DMCONTROL, HARTSELLO, 16u, 10u)
    FIELD(DMCONTROL, HASEL, 26u, 1u)
    FIELD(DMCONTROL, ACKHAVERESET, 28u, 1u)
    FIELD(DMCONTROL, HARTRESET, 29u, 1u)
    FIELD(DMCONTROL, RESUMEREQ, 30u, 1u)
    FIELD(DMCONTROL, HALTREQ, 31u, 1u)
REG32(DMSTATUS, 0x11u)
    FIELD(DMSTATUS, VERSION, 0u, 4u)
    FIELD(DMSTATUS, CONFSTRPTRVALID, 4u, 1u)
    FIELD(DMSTATUS, HASRESETHALTREQ, 5u, 1u)
    FIELD(DMSTATUS, AUTHBUSY, 6u, 1u)
    FIELD(DMSTATUS, AUTHENTICATED, 7u, 1u)
    FIELD(DMSTATUS, ANYHALTED, 8u, 1u)
    FIELD(DMSTATUS, ALLHALTED, 9u, 1u)
    FIELD(DMSTATUS, ANYRUNNING, 10u, 1u)
    FIELD(DMSTATUS, ALLRUNNING, 11u, 1u)
    FIELD(DMSTATUS, ANYUNAVAIL, 12u, 1u)
    FIELD(DMSTATUS, ALLUNAVAIL, 13u, 1u)
    FIELD(DMSTATUS, ANYNONEXISTENT, 14u, 1u)
    FIELD(DMSTATUS, ALLNONEXISTENT, 15u, 1u)
    FIELD(DMSTATUS, ANYRESUMEACK, 16u, 1u)
    FIELD(DMSTATUS, ALLRESUMEACK, 17u, 1u)
    FIELD(DMSTATUS, ANYHAVERESET, 18u, 1u)
    FIELD(DMSTATUS, ALLHAVERESET, 19u, 1u)
    FIELD(DMSTATUS, IMPEBREAK, 22u, 1u)
REG32(HARTINFO, 0x12u)
    FIELD(HARTINFO, DATAADDR, 0u, 12u)
    FIELD(HARTINFO, DATASIZE, 12u, 4u)
    FIELD(HARTINFO, DATAACCESS, 16u, 1u)
    FIELD(HARTINFO, NSCRATCH, 20u, 4u)
REG32(ABSTRACTCS, 0x16u)
    FIELD(ABSTRACTCS, DATACOUNT, 0u, 4u)
    FIELD(ABSTRACTCS, CMDERR, 8u, 3u)
    FIELD(ABSTRACTCS, BUSY, 12u, 1u)
    FIELD(ABSTRACTCS, PROGBUFSIZE, 24u, 5u)
REG32(COMMAND, 0x17u)
    /* muxed fields: access register */
    FIELD(COMMAND, CONTROL, 0u, 24u)
    FIELD(COMMAND, CMDTYPE, 24u, 8u)
    FIELD(COMMAND, REG_REGNO, 0u, 16u)
    FIELD(COMMAND, REG_WRITE, 16u, 1u)
    FIELD(COMMAND, REG_TRANSFER, 17u, 1u)
    FIELD(COMMAND, REG_POSTEXEC, 18u, 1u)
    FIELD(COMMAND, REG_AARPOSTINCREMENT, 19u, 1u)
    FIELD(COMMAND, REG_AARSIZE, 20u, 3u)
    /* muxed fields: access memory */
    FIELD(COMMAND, MEM_WRITE, 16u, 1u)
    FIELD(COMMAND, MEM_AAMPOSTINCREMENT, 19u, 1u)
    FIELD(COMMAND, REG_AAMSIZE, 20u, 3u)
    FIELD(COMMAND, MEM_AAMVIRTUAL, 23u, 1u)
REG32(ABSTRACTAUTO, 0x18u)
    FIELD(ABSTRACTAUTO, AUTOEXECDATA, 0u, 12u)
    FIELD(ABSTRACTAUTO, AUTOEXECPROGBUF, 16u, 16u)
REG32(NEXTDM, 0x1d)
REG32(PROGBUF0, 0x20u)
REG32(PROGBUF1, 0x21u)
REG32(PROGBUF2, 0x22u)
REG32(PROGBUF3, 0x23u)
REG32(PROGBUF4, 0x24u)
REG32(PROGBUF5, 0x25u)
REG32(PROGBUF6, 0x26u)
REG32(PROGBUF7, 0x27u)
REG32(PROGBUF8, 0x28u)
REG32(PROGBUF9, 0x29u)
REG32(PROGBUF10, 0x2au)
REG32(PROGBUF11, 0x2bu)
REG32(PROGBUF12, 0x2cu)
REG32(PROGBUF13, 0x2du)
REG32(PROGBUF14, 0x2eu)
REG32(PROGBUF15, 0x2fu)
REG32(SBCS, 0x38u)
    FIELD(SBCS, SBACCESS8, 0u, 1u)
    FIELD(SBCS, SBACCESS16, 1u, 1u)
    FIELD(SBCS, SBACCESS32, 2u, 1u)
    FIELD(SBCS, SBACCESS64, 3u, 1u)
    FIELD(SBCS, SBACCESS128, 4u, 1u)
    FIELD(SBCS, SBASIZE, 5u, 7u)
    FIELD(SBCS, SBERROR, 12u, 3u)
    FIELD(SBCS, SBREADONDATA, 15u, 1u)
    FIELD(SBCS, SBAUTOINCREMENT, 16u, 1u)
    FIELD(SBCS, SBACCESS, 17u, 3u)
    FIELD(SBCS, SBREADONADDR, 20u, 1u)
    FIELD(SBCS, SBBUSY, 21u, 1u)
    FIELD(SBCS, SBBUSYERROR, 22u, 1u)
    FIELD(SBCS, SBVERSION, 29u, 3u)
REG32(SBADDRESS0, 0x39u)
REG32(SBADDRESS1, 0x3au)
REG32(SBDATA0, 0x3cu)
REG32(SBDATA1, 0x3du)
REG32(HALTSUM0, 0x40u)

#define A_FIRST A_DATA0
#define A_LAST  A_HALTSUM0

/* Debug CSRs */
REG32(DCSR, CSR_DCSR)
    FIELD(DCSR, PRV, 0u, 2u)
    FIELD(DCSR, STEP, 2u, 1u)
    FIELD(DCSR, NMIP, 3u, 1u)
    FIELD(DCSR, MPRVEN, 4u, 1u)
    FIELD(DCSR, CAUSE, 6u, 3u)
    FIELD(DCSR, STOPTIME, 9u, 1u)
    FIELD(DCSR, STOPCOUNT, 10u, 1u)
    FIELD(DCSR, STEPIE, 11u, 1u)
    FIELD(DCSR, EBREAKU, 12u, 1u)
    FIELD(DCSR, EBREAKS, 13u, 1u)
    FIELD(DCSR, EBREAKM, 15u, 1u)
    FIELD(DCSR, XDEBUGVER, 28u, 4u)

/* Debug module remote data */
REG32(HALTED, RISCV_DM_HALTED_OFFSET)
    FIELD(HALTED, HALTED, 0u, 1u)
REG32(GOING, RISCV_DM_GOING_OFFSET)
    FIELD(GOING, GOING, 0u, 1u)
REG32(RESUMING, RISCV_DM_RESUMING_OFFSET)
    FIELD(RESUMING, RESUMING, 0u, 1u)
REG32(EXCEPTION, RISCV_DM_EXCEPTION_OFFSET)
    FIELD(EXCEPTION, EXCEPTION, 0u, 1u)
REG32(FLAGS, RISCV_DM_FLAGS_OFFSET)
    FIELD(FLAGS, FLAG_GO, 0u, 1u)
    FIELD(FLAGS, FLAG_RESUME, 1u, 1u)
/* clang-format on */

/*
 * Macros
 */

/* clang-format off */
#define SBCS_WRITE_MASK (R_SBCS_SBERROR_MASK         | \
                         R_SBCS_SBREADONDATA_MASK    | \
                         R_SBCS_SBAUTOINCREMENT_MASK | \
                         R_SBCS_SBACCESS_MASK        | \
                         R_SBCS_SBREADONADDR_MASK    | \
                         R_SBCS_SBBUSYERROR_MASK)
/* clang-format on */

#define GPR_ZERO 0u /* zero = x0 */
#define GPR_S0   8u /* s0 = x8 */
#define GPR_A0   10u /* a0 = x10 */

static_assert((A_LAST - A_FIRST) < 64u, "too many registers");

#define REG_BIT(_addr_)    (1ull << ((_addr_) - A_FIRST))
#define REG_BIT_DEF(_reg_) REG_BIT(A_##_reg_)

#define DM_REG_COUNT (1u << (ADDRESS_BITS))
#define xtrace_riscv_dm_error(_soc_, _msg_) \
    trace_riscv_dm_error(_soc_, __func__, __LINE__, _msg_)
#define xtrace_riscv_dm_info(_soc_, _msg_, _val_) \
    trace_riscv_dm_info(_soc_, __func__, __LINE__, _msg_, _val_)
#define xtrace_reg(_soc_, _msg_, _reg_, _off_) \
    trace_riscv_dm_access_register(_soc_, _msg_, \
                                   get_riscv_debug_reg_name(_reg_), \
                                   (_reg_) - (_off_));

#define RISCVDM_DEFAULT_MTA 0x1ull /* "MEMTXATTRS_UNSPECIFIED" */

/*
 * Type definitions
 */

/** Debug Module command errors */
typedef enum RISCVDMCmdErr {
    CMD_ERR_NONE = 0,
    CMD_ERR_BUSY = 1,
    CMD_ERR_NOT_SUPPORTED = 2,
    CMD_ERR_EXCEPTION = 3,
    CMD_ERR_HALT_RESUME = 4,
    CMD_ERR_BUS = 5,
    _CMD_ERR_RSV1 = 6,
    CMD_ERR_OTHER = 7,
} RISCVDMCmdErr;

/* For debug purpose only, used to only dump traces on any change */
typedef struct RISCVDMStateCache {
    struct {
        unsigned ix;
        bool halted;
        bool stopped;
        bool running;
    } cpu;
    struct {
        unsigned halted;
        unsigned running;
        unsigned unavail;
        unsigned nonexistent;
        unsigned resumeack;
        unsigned havereset;
    } dm;
} RISCVDMStateCache;

typedef struct RISCVDMHartState {
    RISCVCPU *cpu; /* Associated hart */
    target_ulong hartid; /** Hart ID */
    bool halted; /* Hart has halted execution */
    bool resumed; /* Hart has resumed execution */
    bool have_reset; /* Hart has reset, not yet supported */
    bool unlock_reset; /* Whether DM may reset CPU */
#ifdef TRACE_CPU_STATES
    RISCVDMStateCache dbgcache;
#endif
} RISCVDMHartState;

typedef struct RISCVDMConfig {
    unsigned nscratch;
    unsigned progbuf_count;
    unsigned data_count;
    unsigned abstractcmd_count;
    uint32_t dmi_addr; /* note: next_dm imposes that DM use 32-bit only addr. */
    uint32_t dmi_next; /* next_dm */
    hwaddr dm_phyaddr;
    hwaddr rom_phyaddr;
    hwaddr whereto_phyaddr;
    hwaddr data_phyaddr;
    hwaddr progbuf_phyaddr;
    uint64_t mta_dm; /* MemTxAttrs */
    uint64_t mta_sba; /* MemTxAttrs */
    uint16_t resume_offset;
    bool sysbus_access;
    bool abstractauto;
} RISCVDMConfig;

/** Debug Module */
struct RISCVDMState {
    RISCVDebugDeviceState parent;

    RISCVDMCmdErr cmd_err; /* Command result */
    RISCVDMHartState *hart; /* Currently selected hart for debug, if any */
    RISCVDMHartState *harts; /* Hart states */
    AddressSpace *as; /* Hart address space */
    const char *soc; /* Subsystem name, for debug */
    uint64_t nonexistent_bm; /* Selected harts that are not existent */
    uint64_t unavailable_bm; /* Selected harts that are not available */
    uint64_t to_go_bm; /* Harts that have been flagged for debug exec */
    uint32_t address; /* DM register addr: only bADDRESS_BITS..b0 are used */
    uint32_t *regs; /* Debug module register values */
    uint64_t sbdata; /* Last sysbus data */
    MemTxAttrs mta_dm; /* MemTxAttrs to access debug module implementation */
    MemTxAttrs mta_sba; /* MemTxAttrs to access system bus devices */
    bool cmd_busy; /* A command is being executed */
    bool dtm_ok; /* DTM is available */

    /* config */
    RISCVDTMState *dtm;
    RISCVDMConfig cfg;
    uint32_t hart_count; /* Count of harts */
    uint32_t *cpu_idx; /* array of hart_count CPU index */
};

typedef RISCVDMCmdErr CmdErr;

/** Abstract command types */
typedef enum RISCVDMAbstractCommand {
    CMD_ACCESS_REGISTER = 0,
    CMD_QUICK_ACCESS = 1,
    CMD_ACCESS_MEMORY = 2,
} RISCVDMAbstractCommand;

/** System bus access error */
typedef enum RISCVDMSysbusError {
    SYSBUS_NONE,
    SYSBUS_TIMEOUT,
    SYSBUS_BADADDR,
    SYSBUS_BADALIGN,
    SYSBUS_ASIZE,
    SYSBUS_OTHER = 7,
} RISCVDMSysbusError;

/** Debug Module register */
struct RISCVDMDMReg;
typedef struct RISCVDMDMReg RISCVDMDMReg;

/** Handlers for a Debug Module register */
struct RISCVDMDMReg {
    const char *name; /* register name, for debugging */
    const uint32_t value; /* preset bits */
    CmdErr (*read)(RISCVDMState *dm, uint32_t *value);
    CmdErr (*write)(RISCVDMState *dm, uint32_t value);
};

/*
 * Forward declarations
 */

static void riscv_dm_reset(DeviceState *dev);

static bool riscv_dm_cond_autoexec(RISCVDMState *dm, bool prgbf,
                                   unsigned regix);
static CmdErr riscv_dm_read_absdata(RISCVDMState *dm, unsigned woffset,
                                    unsigned wcount, hwaddr *value);
static CmdErr riscv_dm_write_absdata(RISCVDMState *dm, unsigned woffset,
                                     unsigned wcount, hwaddr value);
static CmdErr riscv_dm_read_progbuf(RISCVDMState *dm, unsigned woffset,
                                    hwaddr *value);
static CmdErr riscv_dm_write_progbuf(RISCVDMState *dm, unsigned woffset,
                                     hwaddr value);
static CmdErr riscv_dm_exec_command(RISCVDMState *dm, uint32_t value);

static CmdErr riscv_dm_dmcontrol_write(RISCVDMState *dm, uint32_t value);
static CmdErr riscv_dm_command_write(RISCVDMState *dm, uint32_t value);
static CmdErr riscv_dm_abstractauto_read(RISCVDMState *dm, uint32_t *value);
static CmdErr riscv_dm_abstractauto_write(RISCVDMState *dm, uint32_t value);
static CmdErr riscv_dm_dmstatus_read(RISCVDMState *dm, uint32_t *value);
static CmdErr riscv_dm_sbcs_write(RISCVDMState *dm, uint32_t value);
static CmdErr riscv_dm_sbaddress0_write(RISCVDMState *dm, uint32_t value);
static CmdErr riscv_dm_sbaddress1_write(RISCVDMState *dm, uint32_t value);
static CmdErr riscv_dm_sbdata0_read(RISCVDMState *dm, uint32_t *value);
static CmdErr riscv_dm_sbdata0_write(RISCVDMState *dm, uint32_t value);
static CmdErr riscv_dm_sbdata1_read(RISCVDMState *dm, uint32_t *value);
static CmdErr riscv_dm_sbdata1_write(RISCVDMState *dm, uint32_t value);
static CmdErr riscv_dm_hartinfo_read(RISCVDMState *dm, uint32_t *value);
static CmdErr riscv_dm_abstractcs_read(RISCVDMState *dm, uint32_t *value);
static CmdErr riscv_dm_abstractcs_write(RISCVDMState *dm, uint32_t value);
static CmdErr riscv_dm_haltsum0_read(RISCVDMState *dm, uint32_t *value);

static CmdErr riscv_dm_access_register(RISCVDMState *dm, uint32_t value);
static CmdErr riscv_dm_access_memory(RISCVDMState *dm, uint32_t value);
static CmdErr riscv_dm_quick_access(RISCVDMState *dm, uint32_t value);

static void riscv_dm_ensure_running(RISCVDMState *dm);
static void riscv_dm_halt_hart(RISCVDMState *dm, unsigned hartsel);
static void riscv_dm_resume_hart(RISCVDMState *dm, unsigned hartsel);

/*
 * Constants
 */

/* DM update/capture registers that should not be traced in trace log */
static const uint64_t RISCVDM_REG_IGNORE_TRACES =
    /* the remote debugger keeps polling dmstatus (to get hart status) */
    REG_BIT_DEF(DMSTATUS) |
    /* the remote debugger polls abstractcs quite often (to get busy/cmderr) */
    REG_BIT_DEF(ABSTRACTCS);

static const RISCVDMDMReg RISCVDM_DMS[DM_REG_COUNT] = {
    [A_DMCONTROL] = {
        .name = "dmcontrol",
        .write = &riscv_dm_dmcontrol_write,
    },
    [A_DMSTATUS] = {
        .name = "dmstatus",
        .value = (RISCV_DEBUG_DM_VERSION << R_DMSTATUS_VERSION_SHIFT) |
                 (1u << R_DMSTATUS_AUTHENTICATED_SHIFT),
        .read = &riscv_dm_dmstatus_read,
    },
    [A_HARTINFO] = {
        .name = "hartinfo",
        .read = &riscv_dm_hartinfo_read,
    },
    [A_ABSTRACTCS] = {
        .name = "abstractcs",
        .read = &riscv_dm_abstractcs_read,
        .write = &riscv_dm_abstractcs_write,
    },
    [A_COMMAND] = {
        .name = "command",
        .write = &riscv_dm_command_write,
    },
    [A_ABSTRACTAUTO] = {
        .name = "abstractauto",
        .read = &riscv_dm_abstractauto_read,
        .write = &riscv_dm_abstractauto_write,
    },
    [A_NEXTDM] = {
        .name = "nextdm",
    },
    [A_SBCS] = {
        .name = "sbcs",
        .write = &riscv_dm_sbcs_write,
    },
    [A_SBADDRESS0] = {
        .name = "sbaddress0",
        .write = &riscv_dm_sbaddress0_write,
    },
    [A_SBADDRESS1] = {
        .name = "sbaddress1",
        .write = &riscv_dm_sbaddress1_write,
    },
    [A_SBDATA0] = {
        .name = "sbdata0",
        .read = &riscv_dm_sbdata0_read,
        .write = &riscv_dm_sbdata0_write,
    },
    [A_SBDATA1] = {
        .name = "sbdata1",
        .read = &riscv_dm_sbdata1_read,
        .write = &riscv_dm_sbdata1_write,
    },
    [A_HALTSUM0] = {
        .name = "haltsum0",
        .read = &riscv_dm_haltsum0_read,
    },
};

#define MAKE_NAME_ENTRY(_pfx_, _ent_) [_pfx_##_##_ent_] = stringify(_ent_)
static const char *DCSR_CAUSE_NAMES[8u] = {
    /* clang-format off */
    MAKE_NAME_ENTRY(DCSR_CAUSE, NONE),
    MAKE_NAME_ENTRY(DCSR_CAUSE, EBREAK),
    MAKE_NAME_ENTRY(DCSR_CAUSE, BREAKPOINT),
    MAKE_NAME_ENTRY(DCSR_CAUSE, HALTREQ),
    MAKE_NAME_ENTRY(DCSR_CAUSE, STEP),
    MAKE_NAME_ENTRY(DCSR_CAUSE, RESETHALTREQ),
    /* clang-format on */
};
#undef MAKE_NAME_ENTRY

#define MAKE_REG_ENTRY(_ent_) stringify(_ent_)
static const char *RISCVDM_DM_DATA_NAMES[12u] = {
    /* clang-format off */
    MAKE_REG_ENTRY(data0),
    MAKE_REG_ENTRY(data1),
    MAKE_REG_ENTRY(data2),
    MAKE_REG_ENTRY(data3),
    MAKE_REG_ENTRY(data4),
    MAKE_REG_ENTRY(data5),
    MAKE_REG_ENTRY(data6),
    MAKE_REG_ENTRY(data7),
    MAKE_REG_ENTRY(data8),
    MAKE_REG_ENTRY(data9),
    MAKE_REG_ENTRY(data10),
    MAKE_REG_ENTRY(data11),
    /* clang-format on */
};

static const char *RISCVDM_DM_PROGBUF_NAMES[16u] = {
    /* clang-format off */
    MAKE_REG_ENTRY(progbuf0),
    MAKE_REG_ENTRY(progbuf1),
    MAKE_REG_ENTRY(progbuf2),
    MAKE_REG_ENTRY(progbuf3),
    MAKE_REG_ENTRY(progbuf4),
    MAKE_REG_ENTRY(progbuf5),
    MAKE_REG_ENTRY(progbuf6),
    MAKE_REG_ENTRY(progbuf7),
    MAKE_REG_ENTRY(progbuf8),
    MAKE_REG_ENTRY(progbuf9),
    MAKE_REG_ENTRY(progbuf10),
    MAKE_REG_ENTRY(progbuf11),
    MAKE_REG_ENTRY(progbuf12),
    MAKE_REG_ENTRY(progbuf13),
    MAKE_REG_ENTRY(progbuf14),
    MAKE_REG_ENTRY(progbuf15),
    /* clang-format on */
};
#undef MAKE_REG_ENTRY

static const char *riscv_dm_get_reg_name(unsigned addr);

/* -------------------------------------------------------------------------- */
/* DMI interface implementation */
/* -------------------------------------------------------------------------- */

static RISCVDebugResult
riscv_dm_write_rq(RISCVDebugDeviceState *dev, uint32_t addr, uint32_t value)
{
    RISCVDMState *dm = RISCV_DM(dev);

    CmdErr ret;
    bool autoexec = false;

    /* store address for next read back */
    dm->address = addr;

    if ((addr >= A_DATA0) && (addr < (A_DATA0 + dm->cfg.data_count))) {
        unsigned dix = addr - A_DATA0;
        if (!(ret = riscv_dm_write_absdata(dm, dix, 1u, (hwaddr)value))) {
            dm->regs[addr] = value;
            autoexec = riscv_dm_cond_autoexec(dm, false, dix);
        }
    } else if ((addr >= A_PROGBUF0) &&
               (addr < (A_PROGBUF0 + dm->cfg.progbuf_count))) {
        unsigned pbix = addr - A_PROGBUF0;
        if (!(ret = riscv_dm_write_progbuf(dm, pbix, (hwaddr)value))) {
            dm->regs[addr] = value;
            autoexec = riscv_dm_cond_autoexec(dm, true, pbix);
        }
    } else if (RISCVDM_DMS[addr].write) {
        ret = RISCVDM_DMS[addr].write(dm, value);
    } else {
        xtrace_riscv_dm_info(dm->soc, "write request ignored @", addr);
        ret = CMD_ERR_NONE;
    }
    if (ret != CMD_ERR_NONE) {
        xtrace_riscv_dm_error(dm->soc, "fail to write");
    }

    if ((ret == CMD_ERR_NONE) && autoexec) {
        xtrace_riscv_dm_info(dm->soc, "autoexec last command",
                             dm->regs[A_COMMAND]);
        ret = riscv_dm_exec_command(dm, dm->regs[A_COMMAND]);
    }

    /* do not override a previous error, which should be explicitly cleared */
    if (dm->cmd_err == CMD_ERR_NONE) {
        dm->cmd_err = ret;
    }

    if ((addr >= A_FIRST) && !(RISCVDM_REG_IGNORE_TRACES & REG_BIT(addr))) {
        trace_riscv_dm_reg_update(dm->soc, riscv_dm_get_reg_name(addr), addr,
                                  value, "write", ret);
    }

    return RISCV_DEBUG_NOERR;
}

static RISCVDebugResult
riscv_dm_read_rq(RISCVDebugDeviceState *dev, uint32_t addr)
{
    RISCVDMState *dm = RISCV_DM(dev);

    CmdErr ret;
    bool autoexec = false;
    uint32_t value = 0;

    /* store address for next read back */
    dm->address = addr;

    if ((addr >= A_DATA0) && (addr < (A_DATA0 + dm->cfg.data_count))) {
        unsigned dix = addr - A_DATA0;
        hwaddr val = 0u;
        if (!(ret = riscv_dm_read_absdata(dm, dix, 1u, &val))) {
            dm->regs[addr] = (uint32_t)val;
            autoexec = riscv_dm_cond_autoexec(dm, false, dix);
        }
    } else if ((addr >= A_PROGBUF0) &&
               (addr < (A_PROGBUF0 + dm->cfg.progbuf_count))) {
        unsigned pbix = addr - A_PROGBUF0;
        hwaddr val;
        if (!(ret = riscv_dm_read_progbuf(dm, pbix, &val))) {
            dm->regs[addr] = (uint32_t)val;
            autoexec = riscv_dm_cond_autoexec(dm, true, pbix);
        }
    } else if (RISCVDM_DMS[addr].read) {
        ret = RISCVDM_DMS[addr].read(dm, &value);
    } else {
        ret = CMD_ERR_NONE;
        value = dm->regs[addr];
    }

    if (ret != CMD_ERR_NONE) {
        xtrace_riscv_dm_error(dm->soc, "fail to read");
    } else if (autoexec) {
        xtrace_riscv_dm_info(dm->soc, "autoexec last command",
                             dm->regs[A_COMMAND]);
        ret = riscv_dm_exec_command(dm, dm->regs[A_COMMAND]);
    }

    if ((addr >= A_FIRST) && !(RISCVDM_REG_IGNORE_TRACES & REG_BIT(addr))) {
        trace_riscv_dm_reg_update(dm->soc, riscv_dm_get_reg_name(addr), addr,
                                  value, "read", ret);
    }

    /* do not override a previous error, which should be explicitly cleared */
    if (dm->cmd_err == CMD_ERR_NONE) {
        dm->cmd_err = ret;
    }

    return RISCV_DEBUG_NOERR;
}

static uint32_t riscv_dm_read_value(RISCVDebugDeviceState *dev)
{
    RISCVDMState *dm = RISCV_DM(dev);

    if (dm->address >= DM_REG_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid address: 0x%x\n", __func__,
                      dm->address);

        return 0;
    }

    uint32_t value = dm->regs[dm->address];

    if (!(RISCVDM_REG_IGNORE_TRACES & REG_BIT(dm->address))) {
        trace_riscv_dm_reg_capture(dm->soc, riscv_dm_get_reg_name(dm->address),
                                   dm->address, value);
    }

    return value;
}

static void riscv_dm_set_next_dm(RISCVDebugDeviceState *dev, uint32_t addr)
{
    RISCVDMState *dm = RISCV_DM(dev);

    dm->regs[A_NEXTDM] = addr;
}

/* -------------------------------------------------------------------------- */
/* DM implementation */
/* -------------------------------------------------------------------------- */

static const char *riscv_dm_get_reg_name(unsigned addr)
{
    if ((addr >= A_DATA0) && (addr <= A_DATA11)) {
        return RISCVDM_DM_DATA_NAMES[addr - A_DATA0];
    }

    if ((addr >= A_PROGBUF0) && (addr <= A_PROGBUF15)) {
        return RISCVDM_DM_PROGBUF_NAMES[addr - A_PROGBUF0];
    }

    if (addr < DM_REG_COUNT) {
        return RISCVDM_DMS[addr].name;
    }

    return "INVALID";
}


static bool riscv_dm_cond_autoexec(RISCVDMState *dm, bool prgbf, unsigned regix)
{
    uint32_t autoexec =
        prgbf ?
            FIELD_EX32(dm->regs[A_ABSTRACTAUTO], ABSTRACTAUTO,
                       AUTOEXECPROGBUF) :
            FIELD_EX32(dm->regs[A_ABSTRACTAUTO], ABSTRACTAUTO, AUTOEXECDATA);
    return (bool)(autoexec & (1u << regix));
}


static CmdErr riscv_dm_read_absdata(RISCVDMState *dm, unsigned woffset,
                                    unsigned wcount, hwaddr *value)
{
    if (!dm->cfg.data_phyaddr) {
        /* CSR-shadowed implementation is not supported */
        xtrace_riscv_dm_error(dm->soc, "no support");
        return CMD_ERR_NOT_SUPPORTED;
    }

    if ((woffset + wcount > dm->cfg.data_count) || (wcount > 2u)) {
        xtrace_riscv_dm_error(dm->soc, "invalid arg");
        return CMD_ERR_OTHER;
    }

    /* use a memory location to store abstract data */
    MemTxResult res;
    res = address_space_rw(dm->as, dm->cfg.data_phyaddr + (woffset << 2u),
                           dm->mta_dm, value, wcount << 2u, false);
    trace_riscv_dm_absdata(dm->soc, "read", woffset, wcount, *value, res);
    if (res != MEMTX_OK) {
        xtrace_riscv_dm_error(dm->soc, "memtx");
        return CMD_ERR_BUS;
    }

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_write_absdata(RISCVDMState *dm, unsigned woffset,
                                     unsigned wcount, hwaddr value)
{
    if (!dm->cfg.data_phyaddr) {
        /* CSR-shadowed implementation is not supported */
        xtrace_riscv_dm_error(dm->soc, "no support");
        return CMD_ERR_NOT_SUPPORTED;
    }

    if ((woffset + wcount > dm->cfg.data_count) || (wcount > 2u)) {
        xtrace_riscv_dm_error(dm->soc, "invalid arg");
        return CMD_ERR_OTHER;
    }

    /* use a memory location to store abstract data */
    MemTxResult res;
    res = address_space_rw(dm->as, dm->cfg.data_phyaddr + (woffset << 2u),
                           dm->mta_dm, &value, wcount << 2u, true);
    trace_riscv_dm_absdata(dm->soc, "write", woffset, wcount, value, res);
    if (res != MEMTX_OK) {
        xtrace_riscv_dm_error(dm->soc, "memtx");
        return CMD_ERR_BUS;
    }

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_read_progbuf(RISCVDMState *dm, unsigned woffset,
                                    hwaddr *value)
{
    if (!dm->cfg.progbuf_phyaddr) {
        /* CSR-shadowed implementation is not supported */
        xtrace_riscv_dm_error(dm->soc, "no support");
        return CMD_ERR_NOT_SUPPORTED;
    }

    if (woffset >= dm->cfg.progbuf_count) {
        xtrace_riscv_dm_error(dm->soc, "invalid arg");
        return CMD_ERR_OTHER;
    }

    /* use a memory location to store abstract data */
    MemTxResult res;
    res = address_space_rw(dm->as, dm->cfg.progbuf_phyaddr + (woffset << 2u),
                           dm->mta_dm, value, sizeof(uint32_t), false);
    trace_riscv_dm_progbuf(dm->soc, "read", woffset, *value, res);
    if (res != MEMTX_OK) {
        xtrace_riscv_dm_error(dm->soc, "memtx");
        return CMD_ERR_BUS;
    }

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_write_progbuf(RISCVDMState *dm, unsigned woffset,
                                     hwaddr value)
{
    if (!dm->cfg.progbuf_phyaddr) {
        /* CSR-shadowed implementation is not supported */
        xtrace_riscv_dm_error(dm->soc, "no support");
        return CMD_ERR_NOT_SUPPORTED;
    }

    if (woffset >= dm->cfg.progbuf_count) {
        xtrace_riscv_dm_error(dm->soc, "invalid arg");
        return CMD_ERR_OTHER;
    }

    /* use a memory location to store abstract data */
    MemTxResult res;
    res = address_space_rw(dm->as, dm->cfg.progbuf_phyaddr + (woffset << 2u),
                           dm->mta_dm, &value, sizeof(uint32_t), true);
    trace_riscv_dm_progbuf(dm->soc, "write", woffset, value, res);
    if (res != MEMTX_OK) {
        xtrace_riscv_dm_error(dm->soc, "memtx");
        return CMD_ERR_BUS;
    }

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_write_whereto(RISCVDMState *dm, uint32_t value)
{
    /* use a memory location to store the where-to-jump location */
    if (address_space_rw(dm->as, dm->cfg.whereto_phyaddr, dm->mta_dm, &value,
                         sizeof(value), true) != MEMTX_OK) {
        xtrace_riscv_dm_error(dm->soc, "memtx");
        return CMD_ERR_BUS;
    }

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_update_flags(RISCVDMState *dm, unsigned hartnum,
                                    bool set, uint32_t flag_mask)
{
    if (!dm->cfg.dm_phyaddr) {
        /* CSR-shadowed implementation is not supported */
        xtrace_riscv_dm_error(dm->soc, "no support");
        return CMD_ERR_NOT_SUPPORTED;
    }

    if (hartnum >= dm->hart_count) {
        xtrace_riscv_dm_error(dm->soc, "internal error");
        return CMD_ERR_OTHER;
    }

    /*
     * optional second scratch register is used in Debug ROM to use a different
     * location for each hart
     */
    unsigned foffset = dm->cfg.nscratch > 1u ? hartnum * sizeof(uint32_t) : 0u;

    /*
     * note: not sure whether this read-modify-write sequence is required,
     * as it seems that flag values (GO/RESUME) are exclusive; a simple write
     * might be enough
     */
    MemTxResult res;
    uint32_t flag_bm;
    hwaddr flagaddr = dm->cfg.dm_phyaddr + A_FLAGS + foffset;
    res = address_space_rw(dm->as, flagaddr, dm->mta_dm, &flag_bm,
                           sizeof(flag_bm), false);
    if (res != MEMTX_OK) {
        xtrace_riscv_dm_error(dm->soc, "memrx");
        return CMD_ERR_BUS;
    }
    if (set) {
        flag_bm |= flag_mask;
    } else {
        flag_bm &= ~flag_mask;
    }
    res = address_space_rw(dm->as, flagaddr, dm->mta_dm, &flag_bm,
                           sizeof(flag_bm), true);
    if (res != MEMTX_OK) {
        xtrace_riscv_dm_error(dm->soc, "memtx");
        return CMD_ERR_BUS;
    }

    return CMD_ERR_NONE;
}

/*
 * DM status acknowledgement
 */

static RISCVDMHartState *
riscv_dm_get_hart_from_id(RISCVDMState *dm, unsigned hartid)
{
    /* hart debugger index is not equivalent to the hartid */
    unsigned hix = 0;
    while (hix < dm->hart_count) {
        if (dm->harts[hix].hartid == (target_ulong)hartid) {
            return &dm->harts[hix];
        }
        hix++;
    }
    return NULL;
}

static void riscv_dm_set_busy(RISCVDMState *dm, bool busy)
{
    dm->cmd_busy = busy;
    trace_riscv_dm_busy(dm->soc, busy);
}

static void riscv_dm_set_cs(RISCVDMState *dm, bool enable)
{
    dm->hart->cpu->env.debug_cs = enable;
    trace_riscv_dm_cs(dm->soc, enable);
}

static uint32_t risc_dmi_get_debug_cause(RISCVCPU *cpu)
{
    return FIELD_EX32(cpu->env.dcsr, DCSR, CAUSE);
}

static const char *risc_dmi_get_debug_cause_name(RISCVCPU *cpu)
{
    return DCSR_CAUSE_NAMES[risc_dmi_get_debug_cause(cpu)];
}

static void riscv_dm_acknowledge(void *opaque, int irq, int level)
{
    /*
     * Note: this function is called from the VCPU thread, whereas the other
     * functions are run from the main/iothread.
     * Nevertheless, all runs w/ iothread_locked, so there should not be race
     * conditions (TBC...)
     */
    RISCVDMState *dm = opaque;
    RISCVDMHartState *hart;
    unsigned hartnum;

    assert(bql_locked());

    switch (irq) {
    case ACK_HALTED:
        hartnum = (unsigned)level;
        if ((hart = riscv_dm_get_hart_from_id(dm, hartnum))) {
            hart->halted = true;
            uint64_t hbm = 1u << hartnum;
            if (dm->unavailable_bm & hbm) {
                qemu_log("%s: ERROR, an unavailable hart should not be halted",
                         __func__);
                /* ensure hart can only be a single state */
                dm->unavailable_bm &= ~hbm;
            }
            riscv_dm_set_busy(dm, false);
            trace_riscv_dm_halted(dm->soc, hart - &dm->harts[0],
                                  hart->cpu->env.dpc,
                                  risc_dmi_get_debug_cause_name(hart->cpu));
        }
        break;
    case ACK_GOING:
        /* level value is meaningless */
        if (!dm->to_go_bm) {
            /* internal error */
            xtrace_riscv_dm_error(dm->soc, "Go ack w/o action");
            hart = NULL;
            break;
        }
        while (dm->to_go_bm) {
            hartnum = __builtin_ctz(dm->to_go_bm);
            if (hartnum >= dm->hart_count) {
                /* internal error, should never occur */
                xtrace_riscv_dm_error(dm->soc, "incoherent go bitmap");
                hart = NULL;
            } else {
                hart = &dm->harts[hartnum];
                if (riscv_dm_update_flags(dm, hartnum, false,
                                          R_FLAGS_FLAG_GO_MASK)) {
                    /* nothing we can do here */
                    xtrace_riscv_dm_error(dm->soc,
                                          "unable to lower going flag");
                    hart = NULL;
                }
            }
            dm->to_go_bm &= ~(1u << hartnum);
        }
        trace_riscv_dm_hart_state(dm->soc, hartnum, "debug ongoing");
        break;
    case ACK_RESUMING:
        hartnum = (unsigned)level;
        if ((hart = riscv_dm_get_hart_from_id(dm, hartnum))) {
            if (riscv_dm_update_flags(dm, (unsigned)level, false,
                                      R_FLAGS_FLAG_RESUME_MASK)) {
                /* nothing we can do here */
                xtrace_riscv_dm_error(dm->soc, "unable to lower resume flag");
            }
            hart->halted = false;
            hart->resumed = true;
            uint64_t hbm = 1u << hartnum;
            if (dm->unavailable_bm & hbm) {
                qemu_log("%s: ERROR, an unavailable hart should not be resumed",
                         __func__);
                /* ensure hart can only be a single state */
                dm->unavailable_bm &= ~hbm;
            }
            bool sstep = (bool)FIELD_EX32(hart->cpu->env.dcsr, DCSR, STEP);
            riscv_dm_set_cs(dm, sstep);
            trace_riscv_dm_hart_state(dm->soc, hartnum, "has resumed");
        }
        break;
    case ACK_EXCEPTION:
        /* level value is meaningless */
        hart = dm->hart;
        dm->cmd_err = CMD_ERR_EXCEPTION;
        riscv_dm_set_cs(dm, false);
        riscv_dm_set_busy(dm, false);
        trace_riscv_dm_hart_state(dm->soc, hart - &dm->harts[0],
                                  "exception in debug");
        break;
    default:
        xtrace_riscv_dm_error(dm->soc, "unknown ack line");
        return;
    }

    if (!hart) {
        xtrace_riscv_dm_error(dm->soc, "no hart to acknowledge");
        return;
    }
}

/*
 * Instruction generation
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static uint32_t riscv_dm_rm(uint32_t reg)
{
    return reg & 0x1fu;
}

static uint32_t riscv_dm_csr(uint32_t reg)
{
    return reg & 0xfffu;
}

static uint32_t riscv_dm_r3(uint32_t reg)
{
    return reg & 0x7u;
}

static uint32_t riscv_dm_bm(uint32_t bit)
{
    return 1u << bit;
}

static uint32_t riscv_dm_bmr(uint32_t msb, uint32_t lsb)
{
    return ((1u << msb) - 1u) & ~((1u << lsb) - 1u);
}

static uint32_t riscv_dm_insn_jal(uint32_t rd, uint32_t imm)
{
    return (get_field(imm, riscv_dm_bm(20)) << 31u) |
           (get_field(imm, riscv_dm_bmr(10, 1)) << 21u) |
           (get_field(imm, riscv_dm_bm(11)) << 20u) |
           (get_field(imm, riscv_dm_bmr(19, 12)) << 12u) |
           (riscv_dm_rm(rd) << 7u) | 0x6fu;
}

static uint32_t riscv_dm_insn_jalr(uint32_t rd, uint32_t rs1, uint32_t offset)
{
    return (get_field(offset, riscv_dm_bmr(11, 0)) << 20u) |
           (riscv_dm_rm(rs1) << 15u) | (0b000u << 12u) |
           (riscv_dm_rm(rd) << 7u) | 0x67u;
}

static uint32_t riscv_dm_insn_andi(uint32_t rd, uint32_t rs1, uint32_t imm)
{
    return (get_field(imm, riscv_dm_bmr(11, 0)) << 20u) |
           (riscv_dm_rm(rs1) << 15u) | (0b111u << 12u) |
           (riscv_dm_rm(rd) << 7u) | 0x13u;
}

static uint32_t riscv_dm_insn_slli(uint32_t rd, uint32_t rs1, uint32_t shamt)
{
    return (get_field(shamt, riscv_dm_bmr(5, 0)) << 20u) |
           (riscv_dm_rm(rs1) << 15u) | (0b001u << 12u) |
           (riscv_dm_rm(rd) << 7u) | 0x13u;
}

static uint32_t riscv_dm_insn_srli(uint32_t rd, uint32_t rs1, uint32_t shamt)
{
    return (get_field(shamt, riscv_dm_bmr(5, 0)) << 20u) |
           (riscv_dm_rm(rs1) << 15u) | (0b101u << 12u) |
           (riscv_dm_rm(rd) << 7u) | 0x13u;
}

static uint32_t riscv_dm_insn_load(uint32_t size, uint32_t dst, uint32_t base,
                                   uint32_t offset)
{
    return (get_field(offset, riscv_dm_bmr(11, 0)) << 20u) |
           (riscv_dm_rm(base) << 15u) | (riscv_dm_r3(size) << 12u) |
           (riscv_dm_rm(dst) << 7u) | 0x03u;
}

static uint32_t riscv_dm_insn_auipc(uint32_t rd, uint32_t imm)
{
    return (get_field(imm, riscv_dm_bm(20)) << 31u) |
           (get_field(imm, riscv_dm_bmr(10, 1)) << 21u) |
           (get_field(imm, riscv_dm_bm(11)) << 20u) |
           (get_field(imm, riscv_dm_bmr(19, 12)) << 12u) |
           (riscv_dm_rm(rd) << 7u) | 0x17u;
}

static uint32_t riscv_dm_insn_store(uint32_t size, uint32_t src, uint32_t base,
                                    uint32_t offset)
{
    return (get_field(offset, riscv_dm_bmr(11, 5)) << 25u) |
           (riscv_dm_rm(src) << 20u) | (riscv_dm_rm(base) << 15u) |
           (riscv_dm_r3(size) << 12u) |
           (get_field(offset, riscv_dm_bmr(4, 0)) << 7u) | 0x23u;
}

static uint32_t riscv_dm_insn_float_load(uint32_t size, uint32_t dst,
                                         uint32_t base, uint32_t offset)
{
    return (get_field(offset, riscv_dm_bmr(11, 0)) << 20u) |
           (riscv_dm_rm(base) << 15u) | (riscv_dm_r3(size) << 10u) |
           (riscv_dm_rm(dst) << 7u) | 0b0000111u;
}

static uint32_t riscv_dm_insn_float_store(uint32_t size, uint32_t src,
                                          uint32_t base, uint32_t offset)
{
    return (get_field(offset, riscv_dm_bmr(11, 5)) << 25u) |
           (riscv_dm_rm(src) << 20u) | (riscv_dm_rm(base) << 15u) |
           (riscv_dm_r3(size) << 12u) |
           (get_field(offset, riscv_dm_bmr(4, 0)) << 7u) | 0b0100111u;
}

static uint32_t riscv_dm_insn_csrw(uint32_t csr, uint32_t rs1)
{
    return (riscv_dm_csr(csr) << 20u) | (riscv_dm_rm(rs1) << 15u) |
           (0b001u << 12u) | (riscv_dm_csr(0) << 7u) | 0x73u;
}

static uint32_t riscv_dm_insn_csrr(uint32_t csr, uint32_t dst)
{
    return (riscv_dm_csr(csr) << 20u) | (riscv_dm_rm(0) << 15u) |
           (0b010u << 12u) | (riscv_dm_csr(dst) << 7u) | 0x73u;
}

static uint32_t riscv_dm_insn_branch(uint32_t src2, uint32_t src1,
                                     uint32_t funct3, uint32_t offset)
{
    return (get_field(offset, riscv_dm_bm(11)) << 31u) |
           (get_field(offset, riscv_dm_bmr(9, 4)) << 25u) |
           (riscv_dm_rm(src2) << 20u) | (riscv_dm_rm(src1) << 15u) |
           (riscv_dm_r3(funct3) << 12u) |
           (get_field(offset, riscv_dm_bmr(3, 0)) << 8u) |
           (get_field(offset, riscv_dm_bm(10)) << 7u) | 0b1100011u;
}

static uint16_t riscv_dm_insn_c_ebreak(void)
{
    return 0x9002u;
}

static uint32_t riscv_dm_insn_ebreak(void)
{
    return 0x00100073u;
}

static uint32_t riscv_dm_insn_wfi(void)
{
    return 0x10500073u;
}

static uint32_t riscv_dm_insn_nop(void)
{
    return 0x00000013u;
}

static uint32_t riscv_dm_insn_illegal(void)
{
    return 0x00000000u;
}

#pragma GCC diagnostic pop

/*
 * DM register implementation
 */

static CmdErr riscv_dm_dmcontrol_write(RISCVDMState *dm, uint32_t value)
{
    bool hasel = (bool)FIELD_EX32(value, DMCONTROL, HASEL);

    uint32_t hartsel = FIELD_EX32(value, DMCONTROL, HARTSELLO) |
                       (FIELD_EX32(value, DMCONTROL, HARTSELHI)
                        << R_DMCONTROL_HARTSELLO_LENGTH);

    /* mask any bits that cannot be used for hart selection */
    hartsel &= dm->hart_count - 1u; /* index start @ 0 */

    dm->hart = NULL;

    /* hart array not supported */
    if (!hasel) {
        uint64_t hbit = 1u << hartsel;
        if (!(hartsel < dm->hart_count)) {
            /* max supported harts: 64 */
            dm->nonexistent_bm |= hbit;
            /* ensure hart can only be in one state */
            dm->unavailable_bm &= ~hbit;
        } else {
            RISCVDMHartState *hart = &dm->harts[hartsel];
            dm->hart = hart;
            CPUState *cs = CPU(dm->hart->cpu);

            if (value & R_DMCONTROL_HARTRESET_MASK) {
                if (!cs->held_in_reset) {
                    trace_riscv_dm_hart_reset("assert", dm->soc, cs->cpu_index,
                                              dm->hart->hartid);
                    if (hart->unlock_reset) {
                        /*
                         * if hart is started in active reset, prevent from
                         * resetting it since it should not be released from
                         * reset (see below). Allowing reset w/ blocking reset
                         * release would leave the Resettable API count with
                         * a forever-locked reset count.
                         */
                        resettable_assert_reset(OBJECT(cs), RESET_TYPE_COLD);
                        dm->unavailable_bm |= hbit;
                    }
                }
            } else {
                if (cs->held_in_reset) {
                    if (hart->unlock_reset) {
                        /*
                         * if hart is started in active reset, prevent from
                         * releasing it from reset, otherwise it may start
                         * executing guest code not yet loaded, leading to an
                         * exception. It is up to the guest code to manage the
                         * initial out-of-reset sequence. Not sure how real HW
                         * manages this corner case.
                         */
                        trace_riscv_dm_hart_reset("release", dm->soc,
                                                  cs->cpu_index,
                                                  dm->hart->hartid);
                        resettable_release_reset(OBJECT(cs), RESET_TYPE_COLD);
                    }
                }
            }

            if (dm->unavailable_bm & hbit) {
                if (!cs->held_in_reset) {
                    /* hart exited from reset, became available */
                    dm->unavailable_bm &= ~hbit;
                    hart->have_reset = true;
                    hart->halted = false;
                    trace_riscv_dm_hart_reset("exited", dm->soc, cs->cpu_index,
                                              dm->hart->hartid);
                }
            }
        }

        if (value & R_DMCONTROL_ACKHAVERESET_MASK) {
            unsigned hix = 0;
            while (hix < dm->hart_count) {
                dm->harts->have_reset = false;
                hix++;
            }
        }
    }

    if (unlikely(value & R_DMCONTROL_NDMRESET_MASK)) {
        /* full system reset (but the Debug Module) */
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    } else if (dm->hart) {
        if (hartsel < dm->hart_count) {
            if (value & R_DMCONTROL_HALTREQ_MASK) {
                riscv_dm_halt_hart(dm, hartsel);
            } else {
                if (dm->hart->halted) {
                    /*
                     * resumereq is explicitly ignored if haltreq is set, by the
                     * specs
                     */
                    if (value & R_DMCONTROL_RESUMEREQ_MASK) {
                        /* it also clears the resume ack bit for those harts. */
                        dm->hart->resumed = false;
                        riscv_dm_resume_hart(dm, hartsel);
                    }
                }
            }
        }
    }

    dm->regs[A_DMCONTROL] &=
        ~(R_DMCONTROL_HARTSELLO_MASK | R_DMCONTROL_HARTSELHI_MASK |
          R_DMCONTROL_NDMRESET_MASK | R_DMCONTROL_DMACTIVE_MASK |
          R_DMCONTROL_HARTRESET_MASK);
    value &= R_DMCONTROL_NDMRESET_MASK | R_DMCONTROL_DMACTIVE_MASK |
             R_DMCONTROL_HARTRESET_MASK;
    /* HARTSELHI never used, since HARTSELLO already encodes up to 1K harts */
    dm->regs[A_DMCONTROL] = FIELD_DP32(value, DMCONTROL, HARTSELLO, hartsel);

    if (unlikely(!FIELD_EX32(dm->regs[A_DMCONTROL], DMCONTROL, DMACTIVE))) {
        /* Debug Module reset */
        trace_riscv_dm_reset(dm->soc, "debugger requested DM reset");
        riscv_dm_reset(DEVICE(dm));
    }

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_exec_command(RISCVDMState *dm, uint32_t value)
{
    if (!dm->hart) {
        /* no hart has been selected for debugging */
        xtrace_riscv_dm_error(dm->soc, "no hart");
        return CMD_ERR_OTHER;
    }

    if (!dm->cfg.data_phyaddr) {
        /*
         * CSR-shadowed implementation is not supported
         * abstract command slots are required
         */
        xtrace_riscv_dm_error(dm->soc, "no support");
        return CMD_ERR_NOT_SUPPORTED;
    }

    if (dm->cmd_busy) {
        xtrace_riscv_dm_error(dm->soc, "already busy");
        return CMD_ERR_BUSY;
    }

    if (!dm->hart->halted) {
        xtrace_riscv_dm_error(dm->soc, "cannot exec command if not halted");
        return CMD_ERR_HALT_RESUME;
    }

    /* "This bit is set as soon as command is written" */
    riscv_dm_set_busy(dm, true);

    int cmdtype = (int)FIELD_EX32(value, COMMAND, CMDTYPE);
    CmdErr ret;
    switch (cmdtype) {
    case CMD_ACCESS_REGISTER:
        ret = riscv_dm_access_register(dm, value);
        break;
    case CMD_QUICK_ACCESS:
        ret = riscv_dm_quick_access(dm, value);
        break;
    case CMD_ACCESS_MEMORY:
        ret = riscv_dm_access_memory(dm, value);
        break;
    default:
        ret = CMD_ERR_NOT_SUPPORTED;
        break;
    }

    if (ret != CMD_ERR_NONE) {
        xtrace_riscv_dm_error(dm->soc, "cmd exec failed");
        /* "and [this bit] is not cleared until that command has completed." */
        riscv_dm_set_busy(dm, false);
    }

    return ret;
}

static CmdErr riscv_dm_command_write(RISCVDMState *dm, uint32_t value)
{
    if (dm->cmd_err != CMD_ERR_NONE) {
        /* if cmderr is non-zero, writes to this register are ignored. */
        return CMD_ERR_NONE;
    }

    /* save command as it may be repeated w/ abstractauto command */
    dm->regs[A_COMMAND] = value;

    /* busy status is asserted in riscv_dm_command_write */
    return riscv_dm_exec_command(dm, value);
}

static CmdErr riscv_dm_abstractauto_read(RISCVDMState *dm, uint32_t *value)
{
    *value = dm->regs[A_ABSTRACTAUTO];

    /*
     * this function is only for debug, to be removed since simple read out
     * does not need a dedicated handler
     */
    xtrace_riscv_dm_info(dm->soc, "abstract auto read back", *value);

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_abstractauto_write(RISCVDMState *dm, uint32_t value)
{
    if (!dm->cfg.abstractauto) {
        xtrace_riscv_dm_info(dm->soc, "abstractauto support is disabled",
                             value);
        /*
         * Peer should check the content of ABSTRACTAUTO (which is initialized
         * and stuck to 0) to discover the feature is not supported.
         *
         * It seems OpenOCD does not perform this check and resume anyway.
         */
        return CMD_ERR_NONE;
    }

    if (dm->cmd_busy) {
        xtrace_riscv_dm_error(dm->soc, "already busy");
        return CMD_ERR_BUSY;
    }

    xtrace_riscv_dm_info(dm->soc, "abstractauto attempt", value);

    uint32_t mask = (((1u << dm->cfg.data_count) - 1u)
                     << R_ABSTRACTAUTO_AUTOEXECDATA_SHIFT) |
                    (((1u << dm->cfg.progbuf_count) - 1u)
                     << R_ABSTRACTAUTO_AUTOEXECPROGBUF_SHIFT);

    dm->regs[A_ABSTRACTAUTO] = value & mask;

    if (dm->regs[A_ABSTRACTAUTO] != value) {
        xtrace_riscv_dm_info(dm->soc, "abstractauto selected", value & mask);
    }

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_dmstatus_read(RISCVDMState *dm, uint32_t *value)
{
    unsigned halted = 0u;
    unsigned running = 0u;
    unsigned unavail = 0u;
    unsigned nonexistent = 0u;
    unsigned resumeack = 0u;
    unsigned havereset = 0u;

    unsigned hcount = dm->hart_count;
    /*
     * "3.4 Hart States
     *  Every hart that can be selected is in exactly one of the following four
     *  DM states: non-existent, unavailable, running, or halted."
     */
    unsigned hix = 0u;
    uint64_t mask = 1u;
    for (; hix < hcount; hix++, mask <<= 1u) {
        RISCVDMHartState *hart = &dm->harts[hix];
        if (hart->resumed) {
            resumeack += 1;
        }
        if (hart->have_reset) {
            havereset += 1;
        }
        if (dm->nonexistent_bm & mask) {
            nonexistent += 1;
            continue;
        }
        if (dm->unavailable_bm & mask) {
            /*
             * The hart may have been started since last poll. There is no way
             * for the hart to inform the DM in this case, so rely on polling
             * for now.
             */
            if (CPU(hart->cpu)->halted) {
                unavail += 1;
                continue;
            }
#ifdef TRACE_CPU_STATES
            qemu_log("%s: %s became available %p: %u\n", __func__, dm->soc,
                     CPU(hart->cpu), CPU(hart->cpu)->cpu_index);
#endif
            /* clear the unavailability flag and resume w/ "regular" states */
            dm->unavailable_bm &= ~mask;
        }
        if (hart->halted) {
            halted += 1;
        } else {
            running += 1;
        }
        mask <<= 1u;
        hix++;

#ifdef TRACE_CPU_STATES
        CPUState *cpu;
        cpu = CPU(hart->cpu);
        RISCVDMStateCache current = {
            .cpu = {
                .ix = cpu->cpu_index,
                .halted = cpu->halted,
                .stopped = cpu->stopped,
                .running = cpu->running,
            },
            .dm = {
                .halted = halted,
                .running = running,
                .unavail = unavail,
                .nonexistent = nonexistent,
                .resumeack = resumeack,
                .havereset = havereset,
            },
        };
        /* NOLINTNEXTLINE */
        if (memcmp(&current, &hart->dbgcache, sizeof(RISCVDMStateCache))) {
            qemu_log("%s: %s[%u] [H:%u S:%u R:%u] "
                     "DM [h:%u r:%u u:%u x:%u a:%u z:%u]\n",
                     __func__, dm->soc, hart->hartid, cpu->halted, cpu->stopped,
                     cpu->running, halted, running, unavail, nonexistent,
                     resumeack, havereset);
            hart->dbgcache = current;
        }
#endif /* #ifdef TRACE_CPU_STATES */
    }

    uint32_t val = dm->regs[A_DMSTATUS];
    val = FIELD_DP32(val, DMSTATUS, ANYHALTED, (halted ? 1 : 0));
    val = FIELD_DP32(val, DMSTATUS, ANYRUNNING, (running ? 1 : 0));
    val = FIELD_DP32(val, DMSTATUS, ANYUNAVAIL, (unavail ? 1 : 0));
    val = FIELD_DP32(val, DMSTATUS, ANYNONEXISTENT, (nonexistent ? 1 : 0));
    val = FIELD_DP32(val, DMSTATUS, ANYRESUMEACK, (resumeack ? 1 : 0));
    val = FIELD_DP32(val, DMSTATUS, ANYHAVERESET, (havereset ? 1 : 0));
    val = FIELD_DP32(val, DMSTATUS, ALLHALTED, (halted == hcount ? 1 : 0));
    val = FIELD_DP32(val, DMSTATUS, ALLRUNNING, (running == hcount ? 1 : 0));
    val = FIELD_DP32(val, DMSTATUS, ALLUNAVAIL, (unavail == hcount ? 1 : 0));
    val = FIELD_DP32(val, DMSTATUS, ALLNONEXISTENT,
                     (nonexistent == hcount ? 1 : 0));
    val =
        FIELD_DP32(val, DMSTATUS, ALLRESUMEACK, (resumeack == hcount ? 1 : 0));
    val =
        FIELD_DP32(val, DMSTATUS, ALLHAVERESET, (havereset == hcount ? 1 : 0));

    if (val != dm->regs[A_DMSTATUS]) {
        CPUState *cpu = CPU(dm->harts[0].cpu);
        trace_riscv_dm_dmstatus_read(dm->soc, val, halted, cpu->halted, running,
                                     cpu->running, resumeack, cpu->stopped,
                                     (uint32_t)dm->harts[0].cpu->env.pc);
    }

    *value = dm->regs[A_DMSTATUS] = val;

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_sbcs_write(RISCVDMState *dm, uint32_t value)
{
    /* mask out the preset, R/O bits */
    value &= SBCS_WRITE_MASK;

    /* clear error bits (if flagged as W1C) */
    value &= ~(value & (R_SBCS_SBERROR_MASK | R_SBCS_SBBUSYERROR_MASK));

    dm->regs[A_SBCS] &= ~SBCS_WRITE_MASK;
    dm->regs[A_SBCS] |= value;

    if (trace_event_get_state(TRACE_RISCV_DM_SBCS_WRITE)) {
        bool err = (bool)(value & R_SBCS_SBERROR_MASK);
        bool rdondata = (bool)(value & R_SBCS_SBREADONDATA_MASK);
        bool autoinc = (bool)(value & R_SBCS_SBAUTOINCREMENT_MASK);
        bool rdonaddr = (bool)(value & R_SBCS_SBREADONADDR_MASK);
        bool busyerr = (bool)(value & R_SBCS_SBBUSYERROR_MASK);
        unsigned access = 1u << FIELD_EX32(value, SBCS, SBACCESS);
        trace_riscv_dm_sbcs_write(dm->soc, err, busyerr, access, rdonaddr,
                                  rdondata, autoinc);
    }

    return CMD_ERR_NONE;
}

static uint32_t riscv_dm_sysbus_get_byte_count(RISCVDMState *dm)
{
    uint32_t size = 1u << FIELD_EX32(dm->regs[A_SBCS], SBCS, SBACCESS);
    /* LSBs of A_SBCS define supported sizes as a bitmap */
    if (!(dm->regs[A_SBCS] & size)) {
        dm->regs[A_SBCS] =
            FIELD_DP32(dm->regs[A_SBCS], SBCS, SBERROR, SYSBUS_ASIZE);
        xtrace_riscv_dm_error(dm->soc, "asize");
        return 0;
    }
    return size;
}

static void riscv_dm_sysbus_set_busy(RISCVDMState *dm, bool busy)
{
    dm->regs[A_SBCS] =
        FIELD_DP32(dm->regs[A_SBCS], SBCS, SBBUSY, (uint32_t)busy);
}

static void riscv_dm_sysbus_increment_address(RISCVDMState *dm)
{
    uint32_t size;
    if (!(size = riscv_dm_sysbus_get_byte_count(dm))) {
        /* invalid size case has already been handled by the caller */
        return;
    }

    uint32_t old = dm->regs[A_SBADDRESS0];
    dm->regs[A_SBADDRESS0] += size;
    if (old > dm->regs[A_SBADDRESS0] &&
        dm->hart->cpu->env.misa_mxl > MXL_RV32) {
        dm->regs[A_SBADDRESS1] += 1u;
    }
}

static CmdErr riscv_dm_sysbus_read(RISCVDMState *dm)
{
    uint32_t size;
    if (!(size = riscv_dm_sysbus_get_byte_count(dm))) {
        /*
         * note: the spec is fuzzy about how sysbus errors should be managed:
         * should cmderr always be flagged, or is sberror enough? TBC..
         */
        return CMD_ERR_NONE;
    }

    riscv_dm_sysbus_set_busy(dm, true);

    CmdErr ret = CMD_ERR_NONE;
    MemTxResult res;
    hwaddr address = (hwaddr)dm->regs[A_SBADDRESS0];
    if (dm->hart->cpu->env.misa_mxl > MXL_RV32) {
        address |= ((hwaddr)dm->regs[A_SBADDRESS1]) << 32u;
    }

    if (address & (size - 1u)) {
        dm->regs[A_SBCS] =
            FIELD_DP32(dm->regs[A_SBCS], SBCS, SBERROR, SYSBUS_BADALIGN);
        xtrace_riscv_dm_error(dm->soc, "align");
        ret = CMD_ERR_BUS;
        goto end;
    }

    /*
     * if the width of the read access is less than the width of sbdata, the
     * contents of the remaining high bits may take on any value
     */
    uint64_t val64 = 0; /* however 0 is easier for debugging */
    res = address_space_rw(dm->as, address, dm->mta_sba, &val64, size, false);
    trace_riscv_dm_sysbus_data_read(dm->soc, address, size, val64, res);
    if (res != MEMTX_OK) {
        dm->regs[A_SBCS] =
            FIELD_DP32(dm->regs[A_SBCS], SBCS, SBERROR, SYSBUS_BADADDR);
        xtrace_riscv_dm_error(dm->soc, "memtx");
        ret = CMD_ERR_BUS;
        goto end;
    }
    dm->sbdata = val64;
end:
    riscv_dm_sysbus_set_busy(dm, false);

    return ret;
}

static CmdErr riscv_dm_sbaddress0_write(RISCVDMState *dm, uint32_t value)
{
    if (!dm->cfg.sysbus_access) {
        xtrace_riscv_dm_error(dm->soc, "no support");
        return CMD_ERR_NONE;
    }

    if (FIELD_EX32(dm->regs[A_SBCS], SBCS, SBERROR)) {
        xtrace_riscv_dm_error(dm->soc, "sberror");
        return CMD_ERR_NONE;
    }

    if (FIELD_EX32(dm->regs[A_SBCS], SBCS, SBBUSY)) {
        FIELD_DP32(dm->regs[A_SBCS], SBCS, SBBUSYERROR, 1u);
        xtrace_riscv_dm_error(dm->soc, "sbbusy");
        return CMD_ERR_NONE;
    }

    dm->regs[A_SBADDRESS0] = value;
    trace_riscv_dm_sbaddr_write(dm->soc, 0, value);

    /*
     * "When 1, every write to sbaddress0 automatically triggers a system bus
     *  read at the new address."
     */
    if (!FIELD_EX32(dm->regs[A_SBCS], SBCS, SBREADONADDR)) {
        return CMD_ERR_NONE;
    }

    CmdErr ret;

    ret = riscv_dm_sysbus_read(dm);
    /*
     * "If the read succeeded and sbautoincrement is set,
     * increment sbaddress."
     */
    if ((ret == CMD_ERR_NONE) &&
        FIELD_EX32(dm->regs[A_SBCS], SBCS, SBAUTOINCREMENT)) {
        riscv_dm_sysbus_increment_address(dm);
    }

    return ret;
}

static CmdErr riscv_dm_sbaddress1_write(RISCVDMState *dm, uint32_t value)
{
    if ((!dm->cfg.sysbus_access) || (dm->hart->cpu->env.misa_mxl < MXL_RV64)) {
        xtrace_riscv_dm_error(dm->soc, "no support");
        return CMD_ERR_NONE;
    }

    if (FIELD_EX32(dm->regs[A_SBCS], SBCS, SBERROR)) {
        xtrace_riscv_dm_error(dm->soc, "sberror");
        return CMD_ERR_NONE;
    }

    if (FIELD_EX32(dm->regs[A_SBCS], SBCS, SBBUSY)) {
        FIELD_DP32(dm->regs[A_SBCS], SBCS, SBBUSYERROR, 1u);
        xtrace_riscv_dm_error(dm->soc, "sbbusy");
        return CMD_ERR_NONE;
    }

    dm->regs[A_SBADDRESS1] = value;
    trace_riscv_dm_sbaddr_write(dm->soc, 1, value);

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_sbdata0_read(RISCVDMState *dm, uint32_t *value)
{
    if (!dm->cfg.sysbus_access) {
        xtrace_riscv_dm_error(dm->soc, "no support");
        *value = 0;
        return CMD_ERR_NONE;
    }

    if (FIELD_EX32(dm->regs[A_SBCS], SBCS, SBERROR) ||
        FIELD_EX32(dm->regs[A_SBCS], SBCS, SBBUSYERROR)) {
        xtrace_riscv_dm_error(dm->soc, "sberror");
        return CMD_ERR_NONE;
    }

    if (FIELD_EX32(dm->regs[A_SBCS], SBCS, SBBUSY)) {
        FIELD_DP32(dm->regs[A_SBCS], SBCS, SBBUSYERROR, 1u);
        xtrace_riscv_dm_error(dm->soc, "sbbusy");
        return CMD_ERR_NONE;
    }

    /*
     * "Reads from this register start the following:
     *  1. Return the data.
     * i.e. the actual content has been read from the previous call, hence the
     * sbdata cache
     */
    *value = dm->regs[A_SBDATA0] = (uint32_t)dm->sbdata;
    trace_riscv_dm_sbdata_read(dm->soc, 0, *value);

    CmdErr ret = CMD_ERR_NONE;

    if (FIELD_EX32(dm->regs[A_SBCS], SBCS, SBREADONDATA)) {
        ret = riscv_dm_sysbus_read(dm);
    }

    /*
     * "When 1, sbaddress is incremented by the access size (in bytes) selected
     *  in sbaccess after every system bus access."
     */
    if ((ret == CMD_ERR_NONE) &&
        FIELD_EX32(dm->regs[A_SBCS], SBCS, SBAUTOINCREMENT)) {
        riscv_dm_sysbus_increment_address(dm);
    }

    return ret;
}

static CmdErr riscv_dm_sbdata0_write(RISCVDMState *dm, uint32_t value)
{
    if (!dm->cfg.sysbus_access) {
        xtrace_riscv_dm_error(dm->soc, "no support");
        return CMD_ERR_NONE;
    }

    if (FIELD_EX32(dm->regs[A_SBCS], SBCS, SBERROR) ||
        FIELD_EX32(dm->regs[A_SBCS], SBCS, SBBUSYERROR)) {
        xtrace_riscv_dm_error(dm->soc, "sberror");
        return CMD_ERR_NONE;
    }

    if (FIELD_EX32(dm->regs[A_SBCS], SBCS, SBBUSY)) {
        FIELD_DP32(dm->regs[A_SBCS], SBCS, SBBUSYERROR, 1u);
        xtrace_riscv_dm_error(dm->soc, "sbbusy");
        return CMD_ERR_NONE;
    }

    uint32_t size;
    if (!(size = riscv_dm_sysbus_get_byte_count(dm))) {
        return CMD_ERR_BUS;
    }

    CmdErr ret = CMD_ERR_NONE;

    riscv_dm_sysbus_set_busy(dm, true);
    MemTxResult res;
    hwaddr address = (hwaddr)dm->regs[A_SBADDRESS0];
    if (dm->hart->cpu->env.misa_mxl > MXL_RV32) {
        address |= ((hwaddr)dm->regs[A_SBADDRESS1]) << 32u;
    }
    if (address & (size - 1u)) {
        dm->regs[A_SBCS] =
            FIELD_DP32(dm->regs[A_SBCS], SBCS, SBERROR, SYSBUS_BADALIGN);
        xtrace_riscv_dm_error(dm->soc, "asize");
        goto end;
    }
    dm->regs[A_SBDATA0] = value;
    /*
     * If the width of the read access is less than the width of sbdata, the
     * contents of the remaining high bits may take on any value
     */
    uint64_t val64;
    val64 = (uint64_t)dm->regs[A_SBDATA0];
    if (size > sizeof(uint32_t)) {
        val64 = ((uint64_t)dm->regs[A_SBDATA1]) << 32u;
    }
    res = address_space_rw(dm->as, address, dm->mta_sba, &val64, size, true);
    trace_riscv_dm_sysbus_data_write(dm->soc, address, size, val64, res);
    if (res != MEMTX_OK) {
        dm->regs[A_SBCS] =
            FIELD_DP32(dm->regs[A_SBCS], SBCS, SBERROR, SYSBUS_BADADDR);
        xtrace_riscv_dm_error(dm->soc, "memtx");
        ret = CMD_ERR_BUS;
    }
end:
    riscv_dm_sysbus_set_busy(dm, false);

    if ((ret == CMD_ERR_NONE) &&
        FIELD_EX32(dm->regs[A_SBCS], SBCS, SBAUTOINCREMENT)) {
        riscv_dm_sysbus_increment_address(dm);
    }

    return ret;
}

static CmdErr riscv_dm_sbdata1_read(RISCVDMState *dm, uint32_t *value)
{
    if ((!dm->cfg.sysbus_access) || (dm->hart->cpu->env.misa_mxl < MXL_RV64)) {
        *value = 0;
        xtrace_riscv_dm_error(dm->soc, "no support");
        return CMD_ERR_NONE;
    }
    if (FIELD_EX32(dm->regs[A_SBCS], SBCS, SBBUSY)) {
        FIELD_DP32(dm->regs[A_SBCS], SBCS, SBBUSYERROR, 1u);
        xtrace_riscv_dm_error(dm->soc, "sbbusy");
        return CMD_ERR_NONE;
    }

    *value = dm->regs[A_SBDATA1] = (uint32_t)(dm->sbdata >> 32u);
    trace_riscv_dm_sbdata_read(dm->soc, 1, *value);

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_sbdata1_write(RISCVDMState *dm, uint32_t value)
{
    if ((!dm->cfg.sysbus_access) || (dm->hart->cpu->env.misa_mxl < MXL_RV64)) {
        xtrace_riscv_dm_error(dm->soc, "no support");
        return CMD_ERR_NONE;
    }
    if (FIELD_EX32(dm->regs[A_SBCS], SBCS, SBBUSY)) {
        FIELD_DP32(dm->regs[A_SBCS], SBCS, SBBUSYERROR, 1u);
        xtrace_riscv_dm_error(dm->soc, "sbbusy");
        return CMD_ERR_NONE;
    }

    dm->regs[A_SBDATA1] = value;
    trace_riscv_dm_sbdata_write(dm->soc, 1, value);

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_hartinfo_read(RISCVDMState *dm, uint32_t *value)
{
    uint32_t val;

    /* note that CSR-shadowing mode is not supported (data access == 0) */
    val = FIELD_DP32(0, HARTINFO, DATAADDR, dm->cfg.data_phyaddr);
    val = FIELD_DP32(val, HARTINFO, DATASIZE, dm->cfg.data_count);
    val = FIELD_DP32(val, HARTINFO, DATAACCESS, dm->cfg.data_phyaddr != 0u);
    val = FIELD_DP32(val, HARTINFO, NSCRATCH, dm->cfg.nscratch);

    *value = dm->regs[A_HARTINFO] = val;

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_abstractcs_read(RISCVDMState *dm, uint32_t *value)
{
    uint32_t val;

    val = FIELD_DP32(0, ABSTRACTCS, DATACOUNT, dm->cfg.data_count);
    val = FIELD_DP32(val, ABSTRACTCS, PROGBUFSIZE, dm->cfg.progbuf_count);
    val = FIELD_DP32(val, ABSTRACTCS, BUSY, (uint32_t)dm->cmd_busy);
    val = FIELD_DP32(val, ABSTRACTCS, CMDERR, (uint32_t)dm->cmd_err);

    *value = dm->regs[A_ABSTRACTCS] = val;

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_abstractcs_write(RISCVDMState *dm, uint32_t value)
{
    if (dm->cmd_busy) {
        xtrace_riscv_dm_error(dm->soc, "already busy");
        return CMD_ERR_BUSY;
    }

    /*
     * The bits in this field remain set until they are cleared by writing 1 to
     * them -> it is not clear whether any bit clears all cmderr bits or if
     * the error code may be changed when only some of them are cleared out...
     */
    uint32_t cmderr_mask = (value & R_ABSTRACTCS_CMDERR_MASK);
    value &= ~cmderr_mask;

    dm->regs[A_ABSTRACTCS] = value;
    dm->cmd_err &= ~(cmderr_mask >> R_ABSTRACTCS_CMDERR_SHIFT);

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_haltsum0_read(RISCVDMState *dm, uint32_t *value)
{
    unsigned hix = 0;
    unsigned halted_bm = 0;

    while (hix < dm->hart_count) {
        if (dm->harts[hix].halted) {
            halted_bm = 1u << hix;
        }
    }

    *value = dm->regs[A_HALTSUM0] = halted_bm;
    return CMD_ERR_NONE;
}

/* this function contains code retrieved from PUL dm_mem.sv file */
static CmdErr riscv_dm_access_register(RISCVDMState *dm, uint32_t value)
{
    /*
     * for now, only LE-RISC-V and LE-hosts are supported,
     * RV128 are not supp.
     */
    RISCVCPU *cpu = dm->hart->cpu;
    CPURISCVState *env = &cpu->env;

    if (!dm->cfg.progbuf_phyaddr ||
        dm->cfg.abstractcmd_count < RISCVDM_ABSTRACTDATA_SLOTS) {
        /* abstract command slots and progbuf address are required */
        xtrace_riscv_dm_error(dm->soc, "no support");
        return CMD_ERR_NOT_SUPPORTED;
    }

    unsigned regno = (unsigned)FIELD_EX32(value, COMMAND, REG_REGNO);
    bool write = (bool)FIELD_EX32(value, COMMAND, REG_WRITE);
    bool transfer = (bool)FIELD_EX32(value, COMMAND, REG_TRANSFER);
    bool postexec = (bool)FIELD_EX32(value, COMMAND, REG_POSTEXEC);
    bool aarpostinc = (bool)FIELD_EX32(value, COMMAND, REG_AARPOSTINCREMENT);
    unsigned aarsize = (unsigned)FIELD_EX32(value, COMMAND, REG_AARSIZE);
    unsigned maxarr = env->misa_mxl + 1u;

    if (transfer) {
        if (aarsize > maxarr) {
            /*
             * If aarsize specifies a size larger than the register’s actual
             * size, then the access must fail.
             */
            trace_riscv_dm_aarsize_error(dm->soc, aarsize);
            return CMD_ERR_NOT_SUPPORTED;
        }
    }

    uint32_t abscmd[RISCVDM_ABSTRACTDATA_SLOTS];
    memset(abscmd, 0u, sizeof(abscmd)); /* fill up the buf with illegal insns */

    /*
     * if ac_ar.transfer is not set then we can take a shortcut to the program
     * buffer, load debug module base address into a0, this is shared among all
     * commands
     */
    abscmd[1u] = (dm->cfg.nscratch > 1u) ? riscv_dm_insn_auipc(GPR_A0, 0) :
                                           riscv_dm_insn_nop();
    /* clr lowest 12b -> DM base offset */
    abscmd[2u] = (dm->cfg.nscratch > 1u) ?
                     riscv_dm_insn_srli(GPR_A0, GPR_A0, 12u) :
                     riscv_dm_insn_nop();
    abscmd[3u] = (dm->cfg.nscratch > 1u) ?
                     riscv_dm_insn_slli(GPR_A0, GPR_A0, 12u) :
                     riscv_dm_insn_nop();
    abscmd[4u] = riscv_dm_insn_nop();
    abscmd[5u] = riscv_dm_insn_nop();
    abscmd[6u] = riscv_dm_insn_nop();
    abscmd[7u] = riscv_dm_insn_nop();
    abscmd[8u] = (dm->cfg.nscratch > 1u) ?
                     riscv_dm_insn_csrr(CSR_DSCRATCH1, GPR_A0) :
                     riscv_dm_insn_nop();
    abscmd[9u] = riscv_dm_insn_ebreak();

    bool unsupported = false;
    /*
     * Depending on whether we are at the zero page or not we either use `x0` or
     * `x10/a0`
     */
    uint32_t regaddr = dm->cfg.dm_phyaddr == 0u ? GPR_ZERO : GPR_A0;

    if ((aarsize <= maxarr) && transfer) {
        if (write) {
            /* store a0 in dscratch1 */
            abscmd[0u] = (dm->cfg.nscratch > 1u) ?
                             riscv_dm_insn_csrw(CSR_DSCRATCH1, GPR_A0) :
                             riscv_dm_insn_nop();
            /* this range is reserved */
            if (regno >= 0xc000) {
                abscmd[0u] = riscv_dm_insn_ebreak(); /* leave asap */
                unsupported = true;
                /*
                 * A0 access needs to be handled separately, as we use A0 to
                 * load the DM address offset need to access DSCRATCH1 in this
                 * case
                 */
            } else if ((dm->cfg.nscratch > 1u) && (regno == 0x1000u + GPR_A0)) {
                xtrace_reg(dm->soc, "write GPR", regno, 0x1000u);
                /* store s0 in dscratch */
                abscmd[4u] = riscv_dm_insn_csrw(CSR_DSCRATCH0, GPR_S0);
                /* load from data register */
                abscmd[5u] = riscv_dm_insn_load(aarsize, GPR_S0, regaddr,
                                                dm->cfg.data_phyaddr);
                /* and store it in the corresponding CSR */
                abscmd[6u] = riscv_dm_insn_csrw(CSR_DSCRATCH1, GPR_S0);
                /* restore s0 again from dscratch */
                abscmd[7u] = riscv_dm_insn_csrr(CSR_DSCRATCH0, GPR_S0);
                /* GPR/FPR access */
            } else if (regno & 0x1000u) {
                /*
                 * determine whether we want to access the floating point
                 * register or not
                 */
                if (regno & 0x20u) {
                    xtrace_reg(dm->soc, "write FPR", regno, 0x1020u);
                    abscmd[4u] =
                        riscv_dm_insn_float_load(aarsize, riscv_dm_rm(regno),
                                                 regaddr, dm->cfg.data_phyaddr);
                } else {
                    xtrace_reg(dm->soc, "write GPR", regno, 0x1000u);
                    abscmd[4u] =
                        riscv_dm_insn_load(aarsize, riscv_dm_rm(regno), regaddr,
                                           dm->cfg.data_phyaddr);
                }
                /* CSR access */
            } else {
                /* data register to CSR */
                xtrace_reg(dm->soc, "write CSR", regno, 0u);
                /* store s0 in dscratch */
                abscmd[4u] = riscv_dm_insn_csrw(CSR_DSCRATCH0, GPR_S0);
                /* load from data register */
                abscmd[5u] = riscv_dm_insn_load(aarsize, GPR_S0, regaddr,
                                                dm->cfg.data_phyaddr);
                /* and store it in the corresponding CSR */
                abscmd[6u] = riscv_dm_insn_csrw(riscv_dm_csr(regno), GPR_S0);
                /* restore s0 again from dscratch */
                abscmd[7u] = riscv_dm_insn_csrr(CSR_DSCRATCH0, GPR_S0);
            }
        } else { /* read */

            /* store a0 in dscratch1 */
            abscmd[0u] = (dm->cfg.nscratch > 1u) ?
                             riscv_dm_insn_csrw(CSR_DSCRATCH1, regaddr) :
                             riscv_dm_insn_nop();
            /* this range is reserved */
            if (regno >= 0xc000) {
                abscmd[0u] = riscv_dm_insn_ebreak(); /* leave asap */
                unsupported = true;
                /* A0 access needs to be handled separately, as we use A0 to
                 * load the DM address offset need to access DSCRATCH1 in this
                 * case
                 */
            } else if ((dm->cfg.nscratch > 1u) && (regno == 0x1000u + GPR_A0)) {
                xtrace_reg(dm->soc, "read GPR", regno, 0x1000u);
                /* store s0 in dscratch */
                abscmd[4u] = riscv_dm_insn_csrw(CSR_DSCRATCH0, GPR_S0);
                /* read value from CSR into s0 */
                abscmd[5u] = riscv_dm_insn_csrr(CSR_DSCRATCH1, GPR_S0);
                /* and store s0 into data section */
                abscmd[6u] = riscv_dm_insn_store(aarsize, GPR_S0, regaddr,
                                                 dm->cfg.data_phyaddr);
                /* restore s0 again from dscratch */
                abscmd[7u] = riscv_dm_insn_csrr(CSR_DSCRATCH0, GPR_S0);
                /* GPR/FPR access */
            } else if (regno & 0x1000u) {
                /*
                 * determine whether we want to access the floating point
                 * register or not
                 */
                if (regno & 0x20u) {
                    xtrace_reg(dm->soc, "read FPR", regno, 0x1020u);
                    abscmd[4u] =
                        riscv_dm_insn_float_store(aarsize, riscv_dm_rm(regno),
                                                  regaddr,
                                                  dm->cfg.data_phyaddr);
                } else {
                    xtrace_reg(dm->soc, "read GPR", regno, 0x1000u);
                    abscmd[4u] =
                        riscv_dm_insn_store(aarsize, riscv_dm_rm(regno),
                                            regaddr, dm->cfg.data_phyaddr);
                }
                /* CSR access */
            } else {
                /* CSR register to data */
                xtrace_reg(dm->soc, "read CSR", regno, 0u);
                /* store s0 in dscratch */
                abscmd[4u] = riscv_dm_insn_csrw(CSR_DSCRATCH0, GPR_S0);
                /* read value from CSR into s0 */
                abscmd[5u] = riscv_dm_insn_csrr(riscv_dm_csr(regno), GPR_S0);
                /* and store s0 into data section */
                abscmd[6u] = riscv_dm_insn_store(aarsize, GPR_S0, regaddr,
                                                 dm->cfg.data_phyaddr);
                /* restore s0 again from dscratch */
                abscmd[7u] = riscv_dm_insn_csrr(CSR_DSCRATCH0, GPR_S0);
            }
        }
    } else if ((aarsize > maxarr) || aarpostinc) {
        /*
         * this should happend when e.g. aarsize > maxaar
         * Openocd will try to do an access with aarsize=64 bits
         * first before falling back to 32 bits.
         */
        abscmd[0u] = riscv_dm_insn_ebreak(); /* leave asap */
        unsupported = true;
    }
    if (postexec && !unsupported) {
        /* issue a nop, we will automatically run into the program buffer */
        abscmd[9u] = riscv_dm_insn_nop();
    }

    if (unsupported) {
        xtrace_riscv_dm_error(dm->soc, "unsupported abstract command");
    }

    /* copy the abstract command opcode into executable memory */
    hwaddr abscmd_addr = dm->cfg.progbuf_phyaddr - sizeof(abscmd);

    if (MEMTX_OK != address_space_rw(dm->as, abscmd_addr, dm->mta_dm,
                                     &abscmd[0], sizeof(abscmd), true)) {
        xtrace_riscv_dm_error(dm->soc, "write to abtract commands to mem");
        return CMD_ERR_BUS;
    }

    for (unsigned ix = 0; ix < RISCVDM_ABSTRACTDATA_SLOTS; ix++) {
        trace_riscv_dm_abstract_cmd(dm->soc,
                                    abscmd_addr + ix * sizeof(uint32_t),
                                    abscmd[ix]);
    }

    /* generate the "whereto" instruction */
    uint32_t offset;
    if (!transfer && postexec) {
        offset = dm->cfg.progbuf_phyaddr - dm->cfg.whereto_phyaddr;
    } else {
        offset = abscmd_addr - dm->cfg.whereto_phyaddr;
    }
    uint32_t whereto = riscv_dm_insn_jal(GPR_ZERO, offset);

    CmdErr res;
    if ((res = riscv_dm_write_whereto(dm, whereto))) {
        return res;
    };

    /* now kick off execution */
    unsigned hartsel = (unsigned)(dm->hart - &dm->harts[0]);
    CPUState *cs = CPU(dm->hart->cpu);
    trace_riscv_dm_change_hart(dm->soc, "GO", hartsel, cs->halted, cs->running,
                               cs->stopped, dm->hart->resumed);
    dm->to_go_bm |= 1u << hartsel;
    res = riscv_dm_update_flags(dm, hartsel, true, R_FLAGS_FLAG_GO_MASK);
    if (res != CMD_ERR_NONE) {
        xtrace_riscv_dm_error(dm->soc, "cannot go");
        dm->to_go_bm &= ~(1u << hartsel);
        return res;
    }

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_access_memory(RISCVDMState *dm, uint32_t value)
{
    /*
     * Arg Width | arg0/ret val |    arg1  |  arg2     |
     * ----------+--------------+----------+-----------+
     *        32 | data0        | data1    | data2     |
     *        64 | data0..1     | data2..3 | data4..5  |
     *       128 | data0..3     | data4..7 | data8..11 |
     * The Argument Width of the Access Memory abstract command is determined by
     * DXLEN, and not by aamsize.
     *   - arg0 is the data
     *   - arg1 is the address
     */
    RISCVCPU *cpu = dm->hart->cpu;
    CPURISCVState *env = &cpu->env;

    bool write = (bool)FIELD_EX32(value, COMMAND, MEM_WRITE);
    bool aampostinc = (bool)FIELD_EX32(value, COMMAND, MEM_AAMPOSTINCREMENT);
    unsigned aamsize = (unsigned)FIELD_EX32(value, COMMAND, REG_AAMSIZE);
    bool virt = (bool)FIELD_EX32(value, COMMAND, MEM_AAMVIRTUAL);
    hwaddr size = 1u << aamsize;

    if (aamsize > env->misa_mxl + 1u) {
        xtrace_riscv_dm_error(dm->soc, "ammsize");
        return CMD_ERR_NOT_SUPPORTED;
    }

    unsigned argwidth = env->misa_mxl; /* in 32-bit word count */
    unsigned datawcount = aamsize > 2u ? 2u : 1u;
    /* zero-init is only useful to help w/ debug on RV32 */
    hwaddr val = 0u;
    hwaddr addr = 0u;
    CmdErr res;
    if ((res = riscv_dm_read_absdata(dm, argwidth, argwidth, &addr))) {
        xtrace_riscv_dm_error(dm->soc, "read mem address (arg1)");
        return res;
    }
    if (virt) {
        hwaddr phyaddr;
        phyaddr = riscv_cpu_get_phys_page_debug(CPU(cpu), addr);
        if (phyaddr == -1) {
            xtrace_riscv_dm_error(dm->soc, "virtual mem");
            return CMD_ERR_BUS;
        }
        addr = phyaddr;
    }
    if (write) {
        /* read value from arg0 */
        if ((res = riscv_dm_read_absdata(dm, 0, datawcount, &val))) {
            xtrace_riscv_dm_error(dm->soc, "read mem data (arg0)");
            return res;
        }
        /* store value into main memory */
        if (MEMTX_OK !=
            address_space_rw(dm->as, addr, dm->mta_sba, &val, size, true)) {
            xtrace_riscv_dm_error(dm->soc, "write to mem");
            return CMD_ERR_BUS;
        }
    } else {
        /* read value from main memory */
        if (MEMTX_OK !=
            address_space_rw(dm->as, addr, dm->mta_sba, &val, size, false)) {
            xtrace_riscv_dm_error(dm->soc, "read from mem");
            return CMD_ERR_BUS;
        }
        /* write value to arg0 */
        if ((res = riscv_dm_write_absdata(dm, 0, datawcount, val))) {
            xtrace_riscv_dm_error(dm->soc, "write mem data (arg0)");
            return res;
        }
    }

    if (aampostinc) {
        addr += argwidth << 2u; /* convert to bytes */
        if (riscv_dm_write_absdata(dm, argwidth, argwidth, addr)) {
            xtrace_riscv_dm_error(dm->soc, "address postinc");
        }
    }

    riscv_dm_set_busy(dm, false);

    return CMD_ERR_NONE;
}

static CmdErr riscv_dm_quick_access(RISCVDMState *dm, uint32_t value)
{
    (void)dm;
    (void)value;

    return CMD_ERR_NOT_SUPPORTED;
}

/*
 * Debugger implementation
 */

static void riscv_dm_ensure_running(RISCVDMState *dm)
{
    /*
     * Hang on: "halted" has many different meanings, depending on the context.
     *
     * There are -at least- three indicators that a hart is not running:
     *   1. CPU may be halted: for example, it enters a WFI: `CPUState.halted`
     *   2. CPU may be stopped: `CPUState.stopped`, which differs from
     *      `CPUState.stop`
     *   3. VM may be not running: global VM state `current_run_state` !=
     *      `RUN_STATE_RUNNING`, which is common to all vCPUs.
     *
     * Debug module adds just another "halted" state which means the CPU is ...
     * actively running the park loop of the Debug ROM. This is the only state
     * in this file that is considered as the "halted" state of a hart.
     *
     * As the debug module needs the vCPU to be actively running to execute
     * Debug ROM code, ensure that the VM is running and that the vCPU is
     * running whenever the remote debugger requests to "halt" (run the park
     * loop) or "resume" (run the guest code).
     */

    if (runstate_needs_reset()) {
        xtrace_riscv_dm_error(dm->soc, "cannot change VM now");
        return;
    }

    RISCVCPU *cpu = dm->hart->cpu;
    CPUState *cs = CPU(cpu);

    cpu_synchronize_state(cs);

    if (!runstate_is_running()) {
        /*
         * the VM may be stopped
         * (for example, at startup, waiting for debugger initial request)
         */
        xtrace_riscv_dm_info(dm->soc, "(re)starting the VM", 0);
        vm_prepare_start(false);
        vm_start();
    }

    if (cs->stopped && !cs->held_in_reset) {
        cpu_resume(cs);
    }
}


static void riscv_dm_halt_hart(RISCVDMState *dm, unsigned hartsel)
{
    RISCVCPU *cpu = dm->harts[hartsel].cpu;
    CPUState *cs = CPU(cpu);

    trace_riscv_dm_change_hart(dm->soc, "HALT", hartsel, cs->halted,
                               cs->running, cs->stopped, dm->hart->resumed);

    /* Note: NMI are not yet supported */
    cpu_exit(cs);
    /* not sure if the real HW clear this flag on halt */
    dm->hart->resumed = false;
    riscv_dm_set_cs(dm, true);
    riscv_cpu_store_debug_cause(cs, DCSR_CAUSE_HALTREQ);
    cpu_interrupt(cs, CPU_INTERRUPT_DEBUG);
    /* vCPU should always be "running" - halt mode runs the park loop */
    riscv_dm_ensure_running(dm);
}

static void riscv_dm_resume_hart(RISCVDMState *dm, unsigned hartsel)
{
    RISCVCPU *cpu = dm->harts[hartsel].cpu;
    CPUState *cs = CPU(cpu);

    trace_riscv_dm_change_hart(dm->soc, "RESUME", hartsel, cs->halted,
                               cs->running, cs->stopped, dm->hart->resumed);

    /* generate "whereto" opcode */
    uint32_t offset =
        dm->cfg.rom_phyaddr + dm->cfg.resume_offset - dm->cfg.whereto_phyaddr;
    uint32_t whereto = riscv_dm_insn_jal(GPR_ZERO, offset);
    if (MEMTX_OK !=
        address_space_rw(dm->as, dm->cfg.whereto_phyaddr, dm->mta_dm, &whereto,
                         sizeof(whereto), true)) {
        xtrace_riscv_dm_error(dm->soc, "write whereto to mem");
        return;
    }

    CPURISCVState *env = &cpu->env;
    bool sstep = (bool)FIELD_EX32(env->dcsr, DCSR, STEP);

    if (sstep) {
        /*
         * it is not possible to single-step on an ebreak instruction
         * disable single stepping on such an error condition
         * note that the debugger is in charge of updating DPC on the next
         * instruction whenever an ebreak instruction is reached.
         */
        uint32_t insn = cpu_ldl_code(env, env->dpc);
        if ((insn == riscv_dm_insn_ebreak()) ||
            (((uint16_t)insn) == riscv_dm_insn_c_ebreak())) {
            /* cannot single-step an ebreak/c.break instruction */
            xtrace_riscv_dm_error(dm->soc, "clear single-step on ebreak");
            env->dcsr = FIELD_DP32(env->dcsr, DCSR, STEP, 0);
        }
    }

    if (riscv_dm_update_flags(dm, hartsel, true, R_FLAGS_FLAG_RESUME_MASK)) {
        xtrace_riscv_dm_error(dm->soc, "cannot resume");
    }

    cpu_exit(cs);
    cpu_reset_interrupt(cs, CPU_INTERRUPT_DEBUG);

    const char *cause = risc_dmi_get_debug_cause_name(cpu);
    trace_riscv_dm_resume_hart(dm->soc, sstep, cause);

    riscv_dm_ensure_running(dm);
}

static int riscv_dm_discover_cpus(RISCVDMState *dm)
{
    unsigned hartix = 0;
    CPUState *cpu;
    /* NOLINTNEXTLINE */
    CPU_FOREACH(cpu) {
        /* skips CPUs/harts that are not associated to this DM */
        bool skip = true;
        for (unsigned ix = 0; ix < dm->hart_count; ix++) {
            if (cpu->cpu_index == dm->cpu_idx[ix]) {
                skip = false;
                break;
            }
        }
        if (skip) {
            continue;
        }
        if (hartix >= dm->hart_count) {
            error_setg(&error_fatal, "Incoherent hart count");
        }
        RISCVDMHartState *hart = &dm->harts[hartix];
        hart->cpu = RISCV_CPU(cpu);
        hart->hartid = hart->cpu->env.mhartid;
        hart->unlock_reset = !cpu->held_in_reset;
        if (!dm->as) {
            /* address space is unknown till first hart is realized */
            dm->as = cpu->as;
        } else if (dm->as != cpu->as) {
            /* for now, all harts should share the same address space */
            error_setg(&error_fatal, "Incoherent address spaces");
        }
        hartix++;
    }

    return hartix ? 0 : -1;
}

static void riscv_dm_internal_reset(RISCVDMState *dm)
{
    dm->address = 0;
    dm->to_go_bm = 0;

    riscv_dm_set_busy(dm, false);

    bool is_running = runstate_is_running();

    for (unsigned ix = 0; ix < dm->hart_count; ix++) {
        RISCVDMHartState *hart = &dm->harts[ix];
        RISCVCPU *cpu = hart->cpu;
        if (!cpu) {
            continue;
        }

        CPURISCVState *env = &cpu->env;
        if (!env) {
            continue;
        }

        /*
         * inform the hart a remote debugger/debugger module is
         * available, as it changes how debug exceptions and trigger CSRs
         * behave
         */
        env->debug_dm = dm->dtm_ok;

        /* External debug support exists */
        env->dcsr = FIELD_DP32(env->dcsr, DCSR, XDEBUGVER, 4u);
        /* No support for MPRV */
        env->dcsr = FIELD_DP32(env->dcsr, DCSR, MPRVEN, 0u);
        /* Initial value */
        env->dcsr = FIELD_DP32(env->dcsr, DCSR, STOPTIME, 0u);
        env->dcsr = FIELD_DP32(env->dcsr, DCSR, STOPCOUNT, 0u);

        CPUState *cs = CPU(cpu);
        if (cs->halted) {
            if (cs->held_in_reset) {
                dm->unavailable_bm |= 1u << ix;
                trace_riscv_dm_unavailable(dm->soc, true);
                /* a hart cannot be halted and unavailable at once */
                hart->halted = false;
            } else {
                /* hart not explicitly halted, ready to run in parked mode */
                hart->halted = false;
            }
        }

        /*
         * fix DCSR at VM initialization:
         * 1. if the VM is started as soon as QEMU is started, do nothing
         * 2. if the VM is idled when QEMU is started, flag all harts as
         *    "halt-on-reset" as the debugger requires a reason for the
         *    harts being initially stopped
         */
        if (is_running) {
            /* called from vm_state change, running */
            if (risc_dmi_get_debug_cause(cpu) == DCSR_CAUSE_RESETHALTREQ) {
                env->dcsr = FIELD_DP32(env->dcsr, DCSR, CAUSE, DCSR_CAUSE_NONE);
            }
        } else {
            /* called from DMI reset */
            if (risc_dmi_get_debug_cause(cpu) == DCSR_CAUSE_NONE) {
                env->dcsr =
                    FIELD_DP32(env->dcsr, DCSR, CAUSE, DCSR_CAUSE_RESETHALTREQ);
            }
        }

        xtrace_riscv_dm_info(dm->soc, "cause", risc_dmi_get_debug_cause(cpu));
    }

    /* TODO: should we clear progbug, absdata, ...? */

    /* set dmactive once ready */
    dm->regs[A_DMCONTROL] |= R_DMCONTROL_DMACTIVE_MASK;

    /* consider all harts for this DM share the same capabilities */
    CPURISCVState *env = &dm->harts[0u].cpu->env;

    uint32_t value = 0u;
    if (dm->cfg.sysbus_access && env) {
        value = FIELD_DP32(value, SBCS, SBVERSION, RISCV_DEBUG_SB_VERSION);
        value = FIELD_DP32(value, SBCS, SBASIZE, 1u << (4u + env->misa_mxl));
        value =
            FIELD_DP32(value, SBCS, SBACCESS64, (uint32_t)(env->misa_mxl > 1u));
        value = FIELD_DP32(value, SBCS, SBACCESS32, 1u);
        value = FIELD_DP32(value, SBCS, SBACCESS16, 1u);
        value = FIELD_DP32(value, SBCS, SBACCESS8, 1u);
    }

    dm->regs[A_SBCS] = value;
}

static Property riscv_dm_properties[] = {
    DEFINE_PROP_LINK("dtm", RISCVDMState, dtm, TYPE_RISCV_DTM, RISCVDTMState *),
    DEFINE_PROP_ARRAY("hart", RISCVDMState, hart_count, cpu_idx,
                      qdev_prop_uint32, uint32_t),
    DEFINE_PROP_UINT32("dmi_addr", RISCVDMState, cfg.dmi_addr, 0),
    DEFINE_PROP_UINT32("dmi_next", RISCVDMState, cfg.dmi_next, 0),
    DEFINE_PROP_UINT32("nscratch", RISCVDMState, cfg.nscratch, 1u),
    DEFINE_PROP_UINT32("progbuf_count", RISCVDMState, cfg.progbuf_count, 0),
    DEFINE_PROP_UINT32("data_count", RISCVDMState, cfg.data_count, 2u),
    DEFINE_PROP_UINT32("abstractcmd_count", RISCVDMState, cfg.abstractcmd_count,
                       0),
    DEFINE_PROP_UINT64("dm_phyaddr", RISCVDMState, cfg.dm_phyaddr, 0),
    DEFINE_PROP_UINT64("rom_phyaddr", RISCVDMState, cfg.rom_phyaddr, 0),
    DEFINE_PROP_UINT64("whereto_phyaddr", RISCVDMState, cfg.whereto_phyaddr, 0),
    DEFINE_PROP_UINT64("data_phyaddr", RISCVDMState, cfg.data_phyaddr, 0),
    DEFINE_PROP_UINT64("progbuf_phyaddr", RISCVDMState, cfg.progbuf_phyaddr, 0),
    DEFINE_PROP_UINT16("resume_offset", RISCVDMState, cfg.resume_offset, 0),
    DEFINE_PROP_BOOL("sysbus_access", RISCVDMState, cfg.sysbus_access, true),
    /* beware that OpenOCD (RISC-V 2024/04) assumes this is always supported */
    DEFINE_PROP_BOOL("abstractauto", RISCVDMState, cfg.abstractauto, true),
    DEFINE_PROP_UINT64("mta_dm", RISCVDMState, cfg.mta_dm, RISCVDM_DEFAULT_MTA),
    DEFINE_PROP_UINT64("mta_sba", RISCVDMState, cfg.mta_sba,
                       RISCVDM_DEFAULT_MTA),
    DEFINE_PROP_END_OF_LIST(),
};

static void riscv_dm_reset(DeviceState *dev)
{
    RISCVDMState *dm = RISCV_DM(dev);

    g_assert(dm->dtm != NULL);
    dm->dtm_ok = riscv_dtm_register_dm(DEVICE(dm->dtm), RISCV_DEBUG_DEVICE(dev),
                                       dm->cfg.dmi_addr, DM_REG_COUNT);

    for (unsigned ix = 0; ix < DM_REG_COUNT; ix++) {
        if (ix != A_NEXTDM) {
            dm->regs[ix] = RISCVDM_DMS[ix].value;
        }
    }

    if (riscv_dm_discover_cpus(dm)) {
        error_setg(&error_fatal, "Cannot identify harts");
    }

    dm->nonexistent_bm = 0;
    dm->unavailable_bm = 0;

    riscv_dm_internal_reset(dm);
}

static void riscv_dm_realize(DeviceState *dev, Error **errp)
{
    RISCVDMState *dm = RISCV_DM(dev);

    MachineState *ms = MACHINE(qdev_get_machine());
    unsigned max_cpus = ms->smp.max_cpus;
    g_assert((dm->hart_count > 0) && (dm->hart_count <= max_cpus));
    dm->harts = g_new0(RISCVDMHartState, dm->hart_count);
    dm->regs = g_new0(uint32_t, DM_REG_COUNT);
    dm->as = NULL;

    if (dm->cfg.data_count > R_ABSTRACTAUTO_AUTOEXECDATA_LENGTH) {
        error_setg(errp, "Invalid data count property");
        return;
    }
    if (dm->cfg.progbuf_count > R_ABSTRACTAUTO_AUTOEXECPROGBUF_LENGTH) {
        error_setg(errp, "Invalid progbuf count property");
        return;
    }

    qdev_init_gpio_in_named(dev, &riscv_dm_acknowledge, RISCV_DM_ACK_LINES,
                            ACK_COUNT);

    dm->soc = object_get_canonical_path_component(OBJECT(dev)->parent);
    dm->unavailable_bm = (1u << dm->hart_count) - 1u;
    dm->regs[A_NEXTDM] = dm->cfg.dmi_next;

    dm->mta_dm = ((RISCVDMMemAttrs){ .value = dm->cfg.mta_dm }).attrs;
    dm->mta_sba = ((RISCVDMMemAttrs){ .value = dm->cfg.mta_sba }).attrs;
}

static void riscv_dm_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &riscv_dm_reset;
    dc->realize = &riscv_dm_realize;
    device_class_set_props(dc, riscv_dm_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    RISCVDebugDeviceClass *dmc = RISCV_DEBUG_DEVICE_CLASS(klass);
    dmc->write_rq = &riscv_dm_write_rq;
    dmc->read_rq = &riscv_dm_read_rq;
    dmc->read_value = &riscv_dm_read_value;
    dmc->set_next_dm = &riscv_dm_set_next_dm;

    /*
     * unfortunately, MemTxtAttrs is a bitfield and there is no built-time way
     * to define nor check its contents vs. an integral value. Run a quick
     * sanity check at runtime.
     */
    RISCVDMMemAttrs mta = { .attrs = MEMTXATTRS_UNSPECIFIED };
    g_assert(mta.value == RISCVDM_DEFAULT_MTA);
}

static const TypeInfo riscv_dm_info = {
    .name = TYPE_RISCV_DM,
    .parent = TYPE_RISCV_DEBUG_DEVICE,
    .instance_size = sizeof(RISCVDMState),
    .class_init = &riscv_dm_class_init,
};

static void riscv_dm_register_types(void)
{
    type_register_static(&riscv_dm_info);
}

type_init(riscv_dm_register_types);
