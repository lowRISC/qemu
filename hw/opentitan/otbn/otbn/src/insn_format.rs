// Copyright 2021 Gregory Chadwick <mail@gregchadwick.co.uk>
// Copyright 2022-2023 Rivos, Inc.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

//! Structures for instruction decoding

#[derive(Debug, PartialEq, Eq)]
pub struct RType {
    pub funct7: u32,
    pub rs2: usize,
    pub rs1: usize,
    pub funct3: u32,
    pub rd: usize,
}

impl RType {
    pub fn new(insn: u32) -> RType {
        RType {
            funct7: (insn >> 25) & 0x7f,
            rs2: ((insn >> 20) & 0x1f) as usize,
            rs1: ((insn >> 15) & 0x1f) as usize,
            funct3: (insn >> 12) & 0x7,
            rd: ((insn >> 7) & 0x1f) as usize,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct IType {
    pub imm: i32,
    pub rs1: usize,
    pub funct3: u32,
    pub rd: usize,
}

impl IType {
    pub fn new(insn: u32) -> IType {
        let uimm: i32 = ((insn >> 20) & 0x7ff) as i32;

        let imm: i32 = if (insn & 0x8000_0000) != 0 {
            uimm - (1 << 11)
        } else {
            uimm
        };

        IType {
            imm,
            rs1: ((insn >> 15) & 0x1f) as usize,
            funct3: (insn >> 12) & 0x7,
            rd: ((insn >> 7) & 0x1f) as usize,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct ITypeShamt {
    pub funct7: u32,
    pub shamt: u32,
    pub rs1: usize,
    pub funct3: u32,
    pub rd: usize,
}

impl ITypeShamt {
    pub fn new(insn: u32) -> ITypeShamt {
        let itype = IType::new(insn);

        ITypeShamt {
            funct7: (insn >> 25) & 0x7f,
            shamt: (itype.imm as u32) & 0x1f,
            rs1: itype.rs1,
            funct3: itype.funct3,
            rd: itype.rd,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct WidType {
    pub imm: i32,
    pub rs2: usize,
    pub rs1: usize,
    pub funct3: u32,
    pub inc: u32,
}

impl WidType {
    pub fn new(insn: u32) -> WidType {
        let uimm: i32 = (((insn >> 25) & 0x7f) | ((insn >> 2) & 0x380)) as i32;

        let imm: i32 = if (insn & 0x800) != 0 {
            uimm - (1 << 10)
        } else {
            uimm
        };

        WidType {
            imm,
            rs2: ((insn >> 20) & 0x1f) as usize,
            rs1: ((insn >> 15) & 0x1f) as usize,
            funct3: (insn >> 12) & 0x7,
            inc: (insn >> 7) & 0x3,
        }
    }
}

pub struct ITypeCSR {
    pub csr: u32,
    pub rs1: usize,
    pub funct3: u32,
    pub rd: usize,
}

impl ITypeCSR {
    pub fn new(insn: u32) -> ITypeCSR {
        let csr: u32 = (insn >> 20) & 0xfff;

        ITypeCSR {
            csr,
            rs1: ((insn >> 15) & 0x1f) as usize,
            funct3: (insn >> 12) & 0x7,
            rd: ((insn >> 7) & 0x1f) as usize,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct SType {
    pub imm: i32,
    pub rs2: usize,
    pub rs1: usize,
    pub funct3: u32,
}

impl SType {
    pub fn new(insn: u32) -> SType {
        let uimm: i32 = (((insn >> 20) & 0x7e0) | ((insn >> 7) & 0x1f)) as i32;

        let imm: i32 = if (insn & 0x8000_0000) != 0 {
            uimm - (1 << 11)
        } else {
            uimm
        };

        SType {
            imm,
            rs2: ((insn >> 20) & 0x1f) as usize,
            rs1: ((insn >> 15) & 0x1f) as usize,
            funct3: (insn >> 12) & 0x7,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct BType {
    pub imm: i32,
    pub rs2: usize,
    pub rs1: usize,
    pub funct3: u32,
}

impl BType {
    pub fn new(insn: u32) -> BType {
        let uimm: i32 =
            (((insn >> 20) & 0x7e0) | ((insn >> 7) & 0x1e) | ((insn & 0x80) << 4)) as i32;

        let imm: i32 = if (insn & 0x8000_0000) != 0 {
            uimm - (1 << 12)
        } else {
            uimm
        };

        BType {
            imm,
            rs2: ((insn >> 20) & 0x1f) as usize,
            rs1: ((insn >> 15) & 0x1f) as usize,
            funct3: (insn >> 12) & 0x7,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct UType {
    pub imm: i32,
    pub rd: usize,
}

impl UType {
    pub fn new(insn: u32) -> UType {
        UType {
            imm: (insn & 0xffff_f000) as i32,
            rd: ((insn >> 7) & 0x1f) as usize,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct JType {
    pub imm: i32,
    pub rd: usize,
}

impl JType {
    pub fn new(insn: u32) -> JType {
        let uimm: i32 =
            ((insn & 0xff000) | ((insn & 0x100000) >> 9) | ((insn >> 20) & 0x7fe)) as i32;

        let imm: i32 = if (insn & 0x8000_0000) != 0 {
            uimm - (1 << 20)
        } else {
            uimm
        };

        JType {
            imm,
            rd: ((insn >> 7) & 0x1f) as usize,
        }
    }
}
