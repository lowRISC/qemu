/*
 * QEMU OpenTitan BigNumber proxy API
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
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

#ifndef HW_OPENTITAN_OTBN_PROXY_H
#define HW_OPENTITAN_OTBN_PROXY_H

#include "qemu/osdep.h"
#include "qemu/typedefs.h"

/*
 * The Unsafe Code Guidelines also notably defines that usize and isize are
 * respectively compatible with uintptr_t and intptr_t defined in C, while rust
 * does not explicitly guarantee compatibility between usize and size_t
 */

typedef void *OTBNProxy;

typedef void (*ot_otbn_signal_completion_fn)(void *opaque);
typedef void (*ot_otbn_fetch_entropy_fn)(void *opaque);

enum OtOTBNCommand {
    OT_OTBN_CMD_NONE = 0x00,
    OT_OTBN_CMD_EXECUTE = 0xD8,
    OT_OTBN_CMD_SEC_WIPE_DMEM = 0xC3,
    OT_OTBN_CMD_SEC_WIDE_IMEM = 0x1E,
};

enum OtOTBNStatus {
    OT_OTBN_STATUS_IDLE = 0x00,
    OT_OTBN_STATUS_BUSY_EXECUTE = 0x01,
    OT_OTBN_STATUS_BUSY_SEC_WIPE_DMEM = 0x02,
    OT_OTBN_STATUS_BUSY_SEC_WIPE_IMEM = 0x03,
    OT_OTBN_STATUS_BUSY_SEC_WIPE_INT = 0x04,
    OT_OTBN_STATUS_LOCKED = 0xFF,
};

enum OtOTBNRandomSource {
    OT_OTBN_URND,
    OT_OTBN_RND,
    OT_OTBN_RND_COUNT
};

#define OT_OTBN_IMEM_SIZE (4U << 10U)
#define OT_OTBN_DMEM_SIZE (3U << 10U)

#define OT_OTBN_RANDOM_BIT_WIDTH 256u
#define OT_OTBN_RANDOM_WORD_COUNT \
    ((OT_OTBN_RANDOM_BIT_WIDTH) / (8u * sizeof(uint32_t)))

extern OTBNProxy ot_otbn_proxy_new(ot_otbn_fetch_entropy_fn urnd_req_entropy,
                                   void *urnd_opaque,
                                   ot_otbn_fetch_entropy_fn rnd_req_entropy,
                                   void *rnd_opaque,
                                   ot_otbn_signal_completion_fn signal,
                                   void *on_comp_opaque);
extern void ot_otbn_proxy_start(OTBNProxy proxy, bool test_mode,
                                const char *logname);
extern void ot_otbn_proxy_terminate(OTBNProxy proxy);
extern int ot_otbn_proxy_push_entropy(OTBNProxy proxy, uint32_t rndix,
    const uint8_t *seed, uint32_t len, bool fips);
extern int ot_otbn_proxy_execute(OTBNProxy proxy, bool dumpstate);
extern int ot_otbn_proxy_wipe_memory(OTBNProxy proxy, bool doi);
extern bool ot_otbn_proxy_acknowledge_execution(OTBNProxy proxy);
extern uint32_t
ot_otbn_proxy_read_memory(OTBNProxy proxy, bool doi, uint32_t addr);
extern bool ot_otbn_proxy_write_memory(OTBNProxy proxy, bool doi, uint32_t addr,
                                       uint32_t val);
extern enum OtOTBNStatus ot_otbn_proxy_get_status(OTBNProxy proxy);
extern uint32_t ot_otbn_proxy_get_instruction_count(OTBNProxy proxy);
extern void
ot_otbn_proxy_set_instruction_count(OTBNProxy proxy, uint32_t value);
extern uint32_t ot_otbn_proxy_get_err_bits(OTBNProxy proxy);
extern void ot_otbn_proxy_set_err_bits(OTBNProxy proxy, uint32_t value);
extern bool ot_otbn_proxy_get_ctrl(OTBNProxy proxy);
extern void ot_otbn_proxy_set_ctrl(OTBNProxy proxy, bool value);

#endif /* HW_OPENTITAN_OTBN_PROXY_H */
