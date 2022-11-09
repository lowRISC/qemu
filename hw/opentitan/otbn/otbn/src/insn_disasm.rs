// Copyright 2021 Gregory Chadwick <mail@gregchadwick.co.uk>
// Copyright 2022-2023 Rivos, Inc.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use paste::paste;

use super::csrs;
use super::insn_format;
use super::insn_proc;

pub struct InstructionStringOutputter {
    /// PC of the instruction being output. Used to generate disassembly of
    /// instructions with PC relative fields (such as BEQ and JAL).
    pub insn_pc: u32,
}

// Macros to produce string outputs for various different instruction types
macro_rules! string_out_for_alu_reg_op {
    ($name:ident) => {
        paste! {
            fn [<process_ $name>](
                &mut self,
                dec_insn: insn_format::RType
            ) -> Self::InstructionResult {
                format!("{} x{}, x{}, x{}", stringify!($name), dec_insn.rd, dec_insn.rs1,
                    dec_insn.rs2)
            }
        }
    };
}

macro_rules! string_out_for_alu_imm_op {
    ($name:ident) => {
        paste! {
            fn [<process_ $name i>](
                &mut self,
                dec_insn: insn_format::IType
            ) -> Self::InstructionResult {
                format!("{}i x{}, x{}, {}", stringify!($name), dec_insn.rd, dec_insn.rs1,
                    dec_insn.imm)
            }
        }
    };
}

macro_rules! string_out_for_alu_imm_shamt_op {
    ($name:ident) => {
        paste! {
            fn [<process_ $name i>](
                &mut self,
                dec_insn: insn_format::ITypeShamt
            ) -> Self::InstructionResult {
                format!("{}i x{}, x{}, {}", stringify!($name), dec_insn.rd, dec_insn.rs1,
                    dec_insn.shamt)
            }
        }
    };
}

macro_rules! string_out_for_alu_ops {
    ($($name:ident),*) => {
        $(
            string_out_for_alu_reg_op! {$name}
            string_out_for_alu_imm_op! {$name}
        )*
    }
}

macro_rules! string_out_for_shift_ops {
    ($($name:ident),*) => {
        $(
            string_out_for_alu_reg_op! {$name}
            string_out_for_alu_imm_shamt_op! {$name}
        )*
    }
}

macro_rules! string_out_for_branch_ops {
    ($($name:ident),*) => {
        $(
            paste! {
                fn [<process_ $name>](
                    &mut self,
                    dec_insn: insn_format::BType
                ) -> Self::InstructionResult {
                    let branch_pc = self.insn_pc.wrapping_add(dec_insn.imm as u32);

                    format!("{} x{}, x{}, 0x{:08x}", stringify!($name), dec_insn.rs1, dec_insn.rs2,
                        branch_pc)
                }
            }
        )*
    }
}

macro_rules! string_out_for_load_ops {
    ($($name:ident),*) => {
        $(
            paste! {
                fn [<process_ $name>](
                    &mut self,
                    dec_insn: insn_format::IType
                ) -> Self::InstructionResult {
                    format!("{} x{}, {}(x{})", stringify!($name), dec_insn.rd, dec_insn.imm,
                        dec_insn.rs1)
                }
            }
        )*
    }
}

macro_rules! string_out_for_store_ops {
    ($($name:ident),*) => {
        $(
            paste! {
                fn [<process_ $name>](
                    &mut self,
                    dec_insn: insn_format::SType
                ) -> Self::InstructionResult {
                    format!("{} x{}, {}(x{})", stringify!($name), dec_insn.rs2, dec_insn.imm,
                        dec_insn.rs1)
                }
            }
        )*
    }
}

macro_rules! string_out_for_bn_alu_imm_op {
    ($name:ident) => {
        paste! {
            fn [<process_bn_ $name i>](
                &mut self,
                dec_insn: insn_format::IType
            ) -> Self::InstructionResult {
                let imm = dec_insn.imm & 0x3ff;
                let fg = (dec_insn.imm >> 11) & 0b1;
                format!("{}i x{}, x{}, {}, FG{}", stringify!($name), dec_insn.rd, dec_insn.rs1,
                        imm, fg)
            }
        }
    };
}

macro_rules! string_out_for_csr_rr_op {
    ($name:ident) => {
        paste! {
            fn [<process_ $name>](
                &mut self,
                dec_insn: insn_format::ITypeCSR
            ) -> Self::InstructionResult {
                format!("{} x{}, {}, x{}", stringify!($name), dec_insn.rd,
                    csrs::CSRAddr::string_name(dec_insn.csr),
                    dec_insn.rs1)
            }
        }
    };
}

macro_rules! string_out_for_csr_ops {
    ($($name:ident),*) => {
        $(
            string_out_for_csr_rr_op! {$name}
        )*
    };
}

// Macros to produce string outputs for various different BN instruction types
macro_rules! string_out_for_alu_reg_bn_op {
    ($name:ident) => {
        paste! {
            fn [<process_bn_ $name>](
                &mut self,
                dec_insn: insn_format::RType
            ) -> Self::InstructionResult {
                let sb_val = (dec_insn.funct7 & 0b0011111) << 3;
                let sb = if sb_val != 0 {
                    let st = if dec_insn.funct7 & 0b010_0000 != 0 {
                        ">>"
                    } else {
                        "<<"
                    };
                    format!(" {} {}", st, sb_val)
                } else {
                    "".to_owned()
                };
                let fg = (dec_insn.funct7 >> 6) & 0b1;
                format!("bn.{} w{}, w{}, w{}{}, FG{}", stringify!($name), dec_insn.rd, dec_insn.rs1,
                        dec_insn.rs2, sb, fg)
            }
        }
    };
}

// Macros to produce string outputs for various different BN instruction types
macro_rules! string_out_for_alu_reg_bn_mod_op {
    ($name:ident) => {
        paste! {
            fn [<process_bn_ $name m>](
                &mut self,
                dec_insn: insn_format::RType
            ) -> Self::InstructionResult {
                format!("bn.{}m w{}, w{}, w{}", stringify!($name), dec_insn.rd, dec_insn.rs1,
                        dec_insn.rs2)
            }
        }
    };
}

macro_rules! string_out_for_alu_reg_bn_cmp_op {
    ($name:ident) => {
        paste! {
            fn [<process_bn_ $name>](
                &mut self,
                dec_insn: insn_format::RType
            ) -> Self::InstructionResult {
                let sb_val = (dec_insn.funct7 & 0b0011111) << 3;
                let sb = if sb_val != 0 {
                    let st = if dec_insn.funct7 & 0b010_0000 != 0 {
                        ">>"
                    } else {
                        "<<"
                    };
                    format!(" {} {}", st, sb_val)
                } else {
                    "".to_owned()
                };
                let fg = (dec_insn.funct7 >> 6) & 0b1;
                format!("bn.{} w{}, w{}{}, FG{}", stringify!($name),
                        dec_insn.rs1, dec_insn.rs2, sb, fg)
            }
        }
    };
}

macro_rules! string_out_for_alu_reg_bn_ops {
    ($($name:ident),*) => {
        $(
            string_out_for_alu_reg_bn_op! {$name}
        )*
    };
}

macro_rules! string_out_for_alu_reg_bn_mod_ops {
    ($($name:ident),*) => {
        $(
            string_out_for_alu_reg_bn_mod_op! {$name}
        )*
    };
}

macro_rules! string_out_for_alu_reg_bn_cmp_ops {
    ($($name:ident),*) => {
        $(
            string_out_for_alu_reg_bn_cmp_op! {$name}
        )*
    };
}

impl insn_proc::InstructionProcessor for InstructionStringOutputter {
    type InstructionResult = String;

    // TODO: Make one macro that takes all names as arguments and generates all the functions
    // together
    string_out_for_alu_ops! {add, xor, or, and}
    string_out_for_alu_reg_op! {sub}
    string_out_for_shift_ops! {sll, srl, sra}

    fn process_lui(&mut self, dec_insn: insn_format::UType) -> Self::InstructionResult {
        format!("lui x{}, 0x{:08x}", dec_insn.rd, dec_insn.imm)
    }

    string_out_for_branch_ops! {beq, bne}
    string_out_for_load_ops! {lw}
    string_out_for_store_ops! {sw}

    string_out_for_bn_alu_imm_op! {add}
    string_out_for_bn_alu_imm_op! {sub}

    fn process_jal(&mut self, dec_insn: insn_format::JType) -> Self::InstructionResult {
        let target_pc = self.insn_pc.wrapping_add(dec_insn.imm as u32);
        format!("jal x{}, 0x{:08x}", dec_insn.rd, target_pc)
    }

    fn process_jalr(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult {
        format!(
            "jalr x{}, 0x{:03x}(x{})",
            dec_insn.rd, dec_insn.imm, dec_insn.rs1
        )
    }

    string_out_for_csr_ops! {csrrs}

    fn process_csrrw(&mut self, dec_insn: insn_format::ITypeCSR) -> Self::InstructionResult {
        format!(
            "csrrw x{}, {}, x{}",
            dec_insn.rd,
            csrs::CSRAddr::string_name(dec_insn.csr),
            dec_insn.rs1
        )
    }

    fn process_ecall(&mut self) -> Self::InstructionResult {
        String::from("ecall")
    }

    fn process_loop(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult {
        format!("loop x{}, {}", dec_insn.rs1, dec_insn.imm + 1)
    }

    fn process_loopi(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult {
        format!(
            "loopi {}, {}",
            (dec_insn.rs1 << 5) | dec_insn.rd,
            dec_insn.imm + 1
        )
    }

    fn process_bn_lid(&mut self, dec_insn: insn_format::WidType) -> Self::InstructionResult {
        let grs1 = dec_insn.rs1;
        let grd = dec_insn.rs2;
        let offset = dec_insn.imm << 5;

        let grs1_inc = if (dec_insn.inc & 0b10) != 0 { "++" } else { "" };
        let grd_inc = if (dec_insn.inc & 0b01) != 0 { "++" } else { "" };

        format!(
            "bn.lid x{}{}, {}(x{}{})",
            grd, grd_inc, offset, grs1, grs1_inc
        )
    }

    fn process_bn_sid(&mut self, dec_insn: insn_format::WidType) -> Self::InstructionResult {
        let grs1 = dec_insn.rs1;
        let grs2 = dec_insn.rs2;
        let offset = dec_insn.imm << 5;

        let grs1_inc = if (dec_insn.inc & 0b10) != 0 { "++" } else { "" };
        let grs2_inc = if (dec_insn.inc & 0b01) != 0 { "++" } else { "" };

        format!(
            "bn.sid x{}{}, {}(x{}{})",
            grs2, grs2_inc, offset, grs1, grs1_inc
        )
    }

    fn process_bn_sel(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let fg = (dec_insn.funct7 >> 6) & 0b1;
        let selbit = 1 << (dec_insn.funct7 & 0x3);
        let flag = match insn_proc::Flags::from_bits_truncate(selbit as u8) {
            insn_proc::Flags::CARRY => "C",
            insn_proc::Flags::MSB => "M",
            insn_proc::Flags::LSB => "L",
            insn_proc::Flags::ZERO => "Z",
            _ => "", // never happens. Is there a better way to handle this?
        };
        format!(
            "bn.sel w{}, w{}, w{}, FG{}.{}",
            dec_insn.rd, dec_insn.rs1, dec_insn.rs2, fg, flag
        )
    }

    string_out_for_alu_reg_bn_cmp_ops! {cmp, cmpb}

    fn process_bn_mov(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult {
        format!("bn.mov w{}, w{}", dec_insn.rd, dec_insn.rs1)
    }

    fn process_bn_movr(&mut self, dec_insn: insn_format::SType) -> Self::InstructionResult {
        let grd_inc = if dec_insn.imm & 0b0001 != 0 { "++" } else { "" };
        let grs_inc = if dec_insn.imm & 0b0100 != 0 { "++" } else { "" };

        format!(
            "bn.movr x{}{}, x{}{}",
            dec_insn.rs2, grd_inc, dec_insn.rs1, grs_inc
        )
    }

    fn process_bn_wsrr(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult {
        let wsr = dec_insn.imm & 0xff;

        format!("bn.wsrr {}, {}", dec_insn.rd, wsr)
    }

    fn process_bn_wsrw(&mut self, dec_insn: insn_format::IType) -> Self::InstructionResult {
        let wsr = dec_insn.imm & 0xff;

        format!("bn.wsrw {}, {}", wsr, dec_insn.rs1)
    }

    fn process_bn_mulqacc(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let zero_acc = if dec_insn.funct3 & 0b001 != 0 {
            ".z"
        } else {
            ""
        };
        let acc_shift = 64 * (dec_insn.funct3 >> 1);
        let qwsel1 = dec_insn.funct7 & 0b11;
        let qwsel2 = dec_insn.funct7 >> 2 & 0b11;

        format!(
            "bn.mulqacc{} w{}.{}, w{}.{}, {}",
            zero_acc, dec_insn.rs1, qwsel1, dec_insn.rs2, qwsel2, acc_shift
        )
    }

    fn process_bn_mulqacc_wo(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let zero_acc = if dec_insn.funct3 & 0b001 != 0 {
            ".z"
        } else {
            ""
        };
        let acc_shift = 64 * (dec_insn.funct3 >> 1);
        let fg = (dec_insn.funct7 >> 6) & 0b1;
        let qwsel1 = dec_insn.funct7 & 0b11;
        let qwsel2 = dec_insn.funct7 >> 2 & 0b11;

        format!(
            "bn.mulqacc.wo{} w{}, w{}.{}, w{}.{}, {} FG{}",
            zero_acc, dec_insn.rd, dec_insn.rs1, qwsel1, dec_insn.rs2, qwsel2, acc_shift, fg
        )
    }

    fn process_bn_mulqacc_so(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let zero_acc = if dec_insn.funct3 & 0b001 != 0 {
            ".z"
        } else {
            ""
        };
        let acc_shift = 64 * (dec_insn.funct3 >> 1);
        let fg = (dec_insn.funct7 >> 6) & 0b1;
        let qwsel1 = dec_insn.funct7 & 0b11;
        let qwsel2 = dec_insn.funct7 >> 2 & 0b11;
        let hwsel = if dec_insn.funct7 & 0b001_0000 != 0 {
            "u"
        } else {
            "l"
        };

        format!(
            "bn.mulqacc.so{} w{}.{}, w{}.{}, w{}.{}, {} FG{}",
            zero_acc, dec_insn.rd, hwsel, dec_insn.rs1, qwsel1, dec_insn.rs2, qwsel2, acc_shift, fg
        )
    }

    string_out_for_alu_reg_bn_ops! {and, or, xor, add, addc, sub, subb}
    string_out_for_alu_reg_bn_mod_ops! {add, sub}

    fn process_bn_not(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let sb_val = (dec_insn.funct7 & 0b0011111) << 3;
        let sb = if sb_val != 0 {
            let st = if dec_insn.funct7 & 0b010_0000 != 0 {
                ">>"
            } else {
                "<<"
            };
            format!(" {} {}", st, sb_val)
        } else {
            "".to_owned()
        };
        let fg = (dec_insn.funct7 >> 6) & 0b1;
        format!("bn.not w{}, w{}{} FG{}", dec_insn.rd, dec_insn.rs2, sb, fg)
    }

    fn process_bn_rshi(&mut self, dec_insn: insn_format::RType) -> Self::InstructionResult {
        let imm = (dec_insn.funct7 << 1) | (dec_insn.funct3 >> 2);
        let shimm = if imm != 0 {
            format!(" >> {}", imm)
        } else {
            "".to_owned()
        };

        format!(
            "bn.rshi w{}, w{}, w{}{}",
            dec_insn.rd, dec_insn.rs1, dec_insn.rs2, shimm
        )
    }
}
