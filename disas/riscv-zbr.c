/*
 * QEMU RISC-V Disassembler for Zbr
 *
 * Copyright (c) 2023 Rivos Inc
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "disas/riscv.h"
#include "disas/riscv-zbr.h"

typedef enum {
    /* 0 is reserved for rv_op_illegal. */
    rv_op_crc32_b = 1,
    rv_op_crc32_h = 2,
    rv_op_crc32_w = 3,
    rv_op_crc32_d = 4,
    rv_op_crc32c_b = 5,
    rv_op_crc32c_h = 6,
    rv_op_crc32c_w = 7,
    rv_op_crc32c_d = 8,
} rv_zbr_op;

const rv_opcode_data rv_zbr_opcode_data[] = {
    { "illegal", rv_codec_illegal, rv_fmt_none, NULL, 0, 0, 0 },
    { "crc32.b", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
    { "crc32.h", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
    { "crc32.w", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
    { "crc32.d", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
    { "crc32c.b", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
    { "crc32c.h", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
    { "crc32c.w", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
    { "crc32c.d", rv_codec_r, rv_fmt_rd_rs1, NULL, 0, 0, 0 },
};

void decode_zbr(rv_decode *dec, rv_isa isa)
{
    rv_inst inst = dec->inst;
    rv_opcode op = rv_op_illegal;

    switch ((inst >> 0) & 0b1111111) {
    case 0b0010011:
        switch ((inst >> 12) & 0b111) {
        case 0b001:
            switch ((inst >> 20 & 0b111111111111)) {
            case 0b011000010000:
                op = rv_op_crc32_b;
                break;
            case 0b011000010001:
                op = rv_op_crc32_h;
                break;
            case 0b011000010010:
                op = rv_op_crc32_w;
                break;
            case 0b011000010011:
                op = rv_op_crc32_d;
                break;
            case 0b011000011000:
                op = rv_op_crc32c_b;
                break;
            case 0b011000011001:
                op = rv_op_crc32c_h;
                break;
            case 0b011000011010:
                op = rv_op_crc32c_w;
                break;
            case 0b011000011011:
                op = rv_op_crc32c_d;
                break;
            }
            break;
        }
        break;
    }
    dec->op = op;
}
