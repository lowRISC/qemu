# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""ELF helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from io import BytesIO
from logging import getLogger
from os.path import basename
from typing import BinaryIO, Iterator, NamedTuple, Optional
import re

try:
    _ELF_ERROR = None
    # note: pyelftools package is an OpenTitan toolchain requirement, see
    # python-requirements.txt file from OT top directory.
    from elftools.common.exceptions import ELFError
    from elftools.elf.constants import SH_FLAGS
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import Section, SymbolTableSection
    from elftools.elf.segments import Segment
except ImportError as elf_exc:
    _ELF_ERROR = str(elf_exc)

try:
    _RUST_ERROR = None
    from rust_demangler import demangle as rust_demangle
except ImportError as rust_exc:
    _RUST_ERROR = rust_exc
    def rust_demangle(sym):  # noqa: E301
        """Fake demangler."""
        return sym


class ElfSymbolExtent(NamedTuple):
    """ELF symbol extent."""

    start: int  # start address
    end: int  # end address
    func: str  # function name
    module: str  # ELF name


class ElfBlob:
    """Load ELF application."""

    ELF_ERROR = _ELF_ERROR
    """Report whether ELF tools have been loaded."""

    RUST_ERROR = _RUST_ERROR
    """Report whether Rust tools have been loaded."""

    RUST_TRAIL_CRE = re.compile(r'::h[0-9a-f]{16}$')
    """Regex to get rid of Rust trailing symbol string."""

    def __init__(self):
        if self.ELF_ERROR:
            raise ImportError('pyelftools package not available')
        self._log = getLogger('elf')
        self._elf: Optional[ELFFile] = None
        self._payload_address: int = 0
        self._payload_size: int = 0
        self._payload: bytes = b''
        self._name = ''

    def load(self, efp: BinaryIO) -> None:
        """Load the content of an ELF file.

           The ELF file stream is no longer accessed once this method
           completes.

           :param efp: a File-like (binary read access)
        """
        # use a copy of the stream to release the file pointer.
        try:
            self._elf = ELFFile(BytesIO(efp.read()))
            self._name = efp.name
        except ELFError as exc:
            raise ValueError(f'Invalid ELF file: {exc}') from exc
        if self._elf['e_machine'] != 'EM_RISCV':
            raise ValueError('Not an RISC-V ELF file')
        if self._elf['e_type'] != 'ET_EXEC':
            raise ValueError('Not an executable ELF file')
        self._log.debug('entry point: 0x%X', self.entry_point)
        self._log.debug('data size: %d', self.size)

    @property
    def address_size(self) -> int:
        """Provide the width of address value used in the ELFFile.

           :return: the address width in bits (not bytes!)
        """
        return self._elf.elfclass if self._elf else 0

    @property
    def entry_point(self) -> Optional[int]:
        """Provide the entry point of the application, if any.

           :return: the entry point address
        """
        return self._elf and self._elf.header.get('e_entry', None)

    @property
    def size(self) -> int:
        """Provide the size of the payload section, if any.

           :return: the data/payload size in bytes
        """
        if not self._payload_size:
            self._payload_address, self._payload_size = self._parse_segments()
        return self._payload_size

    @property
    def load_address(self) -> int:
        """Provide the first destination address on target to copy the
           application blob.

           :return: the load address
        """
        if not self._payload_address:
            self._payload_address, self._payload_size = self._parse_segments()
        return self._payload_address

    @property
    def blob(self) -> bytes:
        """Provide the application blob, i.e. the whole loadable binary.

           :return: the raw application binary.
        """
        if not self._payload:
            self._payload = self._build_payload()
        if len(self._payload) != self.size:
            raise RuntimeError('Internal error: size mismatch')
        return self._payload

    @property
    def name(self) -> str:
        """Return the filename of the loaded ELF."""
        return basename(self._name)

    @property
    def code_span(self) -> tuple[int, int]:
        """Report the extent of the executable portion of the ELF file.

           :return: (start address, end address)
        """
        loadable_segments = list(self._loadable_segments())
        base_addr = None
        last_addr = None
        for section in self._elf.iter_sections():
            if not self.is_section_executable(section):
                continue
            for segment in loadable_segments:
                if segment.section_in_segment(section):
                    break
            else:
                continue
            addr = section.header['sh_addr']
            size = section.header['sh_size']
            if base_addr is None or base_addr > addr:
                base_addr = addr
            last = addr + size
            if last_addr is None or last_addr < last:
                last_addr = last
            self._log.debug('Code section @ 0x%08x 0x%08x bytes', addr, size)
        return base_addr, last_addr

    def is_section_executable(self, section: 'Section') -> bool:
        """Report whether the section is flagged as executable.

           :return: True is section is executable
        """
        return bool(section.header['sh_flags'] & SH_FLAGS.SHF_EXECINSTR)

    def get_symbols(self) -> list[ElfSymbolExtent]:
        """Retrieve symbols from ELF.

           :return: a list of symbol extents
        """
        symbols = []
        name = self.name
        for sect in self._elf.iter_sections():
            if not isinstance(sect, SymbolTableSection):
                continue
            for symb in sect.iter_symbols():
                if not symb.name or symb.name.startswith('.L'):
                    continue
                if symb.entry.st_info.type != 'STT_FUNC':
                    continue
                func = symb.name
                if func.startswith('_ZN'):
                    func = rust_demangle(func)
                    func = self.RUST_TRAIL_CRE.sub('', func)
                symbols.append((symb.entry.st_value,
                                symb.entry.st_value + symb.entry.st_size, func,
                                name))
        symbols.sort(key=lambda s: (s[0], s[1]))
        for addr, end, func, _ in symbols:
            self._log.debug('%s: %08x..%08x %s', name, addr, end, func)
        return symbols

    def _loadable_segments(self) -> Iterator['Segment']:
        """Provide an iterator on segments that should be loaded into the final
           binary.
        """
        if not self._elf:
            raise RuntimeError('No ELF file loaded')
        for segment in sorted(self._elf.iter_segments(),
                              key=lambda seg: seg['p_paddr']):
            if segment['p_type'] not in ('PT_LOAD', ):
                continue
            if not segment['p_filesz']:
                continue
            yield segment

    def _parse_segments(self) -> tuple[int, int]:
        """Parse ELF segments and extract physical location and size.

           :return: the location of the first byte and the overall payload size
                    in bytes
        """
        size = 0
        phy_start = None
        for segment in self._loadable_segments():
            seg_size = segment['p_filesz']
            if not seg_size:
                continue
            phy_addr = segment['p_paddr']
            if phy_start is None:
                phy_start = phy_addr
            else:
                if phy_addr > phy_start+size:
                    self._log.debug('fill gap with previous segment')
                    size = phy_addr-phy_start
            size += seg_size
        if phy_start is None:
            raise ValueError('No loadable segment found')
        return phy_start, size

    def _build_payload(self) -> bytes:
        """Extract the loadable payload from the ELF file and generate a
           unique, contiguous binary buffer.

           :return: the payload to store as the application blob
        """
        buf = BytesIO()
        phy_start = None
        for segment in self._loadable_segments():
            phy_addr = segment['p_paddr']
            if phy_start is None:
                phy_start = phy_addr
            else:
                current_addr = phy_start+buf.tell()
                if phy_addr > current_addr:
                    fill_size = phy_addr-current_addr
                    buf.write(bytes(fill_size))
            buf.write(segment.data())
        data = buf.getvalue()
        buf.close()
        return data
