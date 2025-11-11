# Copyright (c) 2025 Rivos, Inc.
# Copyright (c) 2025 lowRISC contributors.
# SPDX-License-Identifier: Apache2

"""QEMU OT tool to verify Key Manager DPE test execution

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from binascii import unhexlify
from collections.abc import Iterator
from configparser import ConfigParser, NoOptionError
from logging import getLogger
from typing import BinaryIO, NamedTuple, Optional, TextIO, Union

import re
import sys

from .dpe import KeyManagerDpe
from ..util.misc import to_bool, HexInt

# ruff: noqa: E402
_CRYPTO_EXC: Optional[Exception] = None
try:
    from Crypto.Cipher import AES
    from Crypto.Hash import KMAC256
except ModuleNotFoundError:
    try:
        # see pip3 install -r requirements.txt
        from Cryptodome.Cipher import AES
        from Cryptodome.Hash import KMAC256
    except ModuleNotFoundError as exc:
        _CRYPTO_EXC = exc


KeyManagerDpeStep = NamedTuple
"""Key Manager DPE test step."""


class KeyManagerDpeStepInitialize(KeyManagerDpeStep):
    """Key Manager DPE test initialization step."""

    dst: int
    """Destination slot."""


class KeyManagerDpeStepErase(KeyManagerDpeStep):
    """Key Manager DPE test erase step."""

    dst: int
    """Destination slot."""


class KeyManagerDpeStepAdvance(KeyManagerDpeStep):
    """Key Manager DPE test advance step."""

    src: int
    """Source slot."""

    dst: int
    """Destination slot."""

    max_key_version: int
    """Maximum key version."""

    binding: bytes
    """Software bindings."""

    allow_child: bool
    """Whether this context allows derivation of further children."""

    exportable: bool
    """Whether the key for the target slot is exportable."""

    reatin_parent: bool
    """Whether further advance operations force erasure of the slot."""


class KeyManagerDpeStepGenerate(KeyManagerDpeStep):
    """Key Manager DPE test generate step"""

    src: int
    """Source slot."""

    dst: str
    """Destination device."""

    output: Optional[str]
    """Type of output."""

    key_version: int
    """Key version."""

    salt: bytes
    """Salt."""


class KeyManagerDpeEngine:
    """
    Key Manager DPE test executer
    """

    ANSI_CRE = re.compile(r'(\x9B|\x1B\[)[0-?]*[ -\/]*[@-~]')
    """Filter out ANSI escape sequences from input stream (colors, etc.)."""

    LOG_CRE = re.compile(r'^(?:.*\s)?T>\s(.*)$')
    """Lines of interest in the log file."""

    def __init__(self, kmd: KeyManagerDpe):
        self._log = getLogger('keymgr.eng')
        self._kmd = kmd
        self._steps: dict[str, KeyManagerDpeStep] = []
        self._exp_results: dict[str, bytes] = {}

    def execute(self, ifp: TextIO) -> None:
        """Verify Key Manager DPE test execution

           :param ifp: ini-style sequence definition stream
        """
        seq = ConfigParser()
        seq.read_file(ifp)
        self._steps = self._build_steps_from_seq(seq)
        self._exp_results = self._execute_steps()

    def verify(self, lfp: BinaryIO) -> None:
        """Verify Key Manager DPE test execution

           :param lfp: bianry log stream
        """
        exec_log = self._filter_log(lfp)
        self._steps, results = self._build_steps_from_log(exec_log)
        if not self._steps:
            raise ValueError('No step found in log')
        self._exp_results = self._execute_steps()
        error_count = self._verify_results(results)
        if error_count:
            raise ValueError(f'{error_count} errors found')

    def _filter_log(self, lfp: BinaryIO) -> Iterator[KeyManagerDpeStep]:
        """Filter log stream

           :param lfp: log stream
        """
        for line in lfp:
            line = self.ANSI_CRE.sub('', line.decode(errors='ignore')).strip()
            lmo = self.LOG_CRE.match(line)
            if not lmo:
                continue
            yield lmo.group(1)

    @classmethod
    def _parse_bytes(cls, value: str) -> bytes:
        """Parse bytes from string

           :param value: string representation of bytes
           :return: the parsed bytes
        """
        value = value.strip('"')
        if value.startswith('[') and value.endswith(']'):
            value = value[1:-1]
            if len(value) & 1:
                value = f'0{value}'
            return unhexlify(value)
        if value.startswith('0x'):
            ivalue = int(value, 16)
            return ivalue.to_bytes((ivalue.bit_length() + 7) // 8, 'big')
        raise ValueError(f'invalid bytes: {value}')

    def _build_steps_from_seq(self, seq: ConfigParser) -> \
            dict[str, KeyManagerDpeStep]:
        module = sys.modules[__name__]
        steps: dict[str, KeyManagerDpeStep] = {}
        for step in seq.sections():
            if step in steps:
                raise ValueError(f'Duplicate step: {step}')
            mode = seq.get(step, 'mode')
            try:
                step_cls = getattr(module, f'KeyManagerDpeStep{mode.title()}')
            except AttributeError as exc:
                raise ValueError(f'Unknown mode: {mode}') from exc
            attrs = []
            for name, type_ in step_cls.__annotations__.items():
                if type_ is bool:
                    val = seq.getboolean(step, name, fallback=True)
                elif type_ is int:
                    val = seq.getint(step, name, fallback=None)
                    if val is None:
                        if name == 'max_key_version':
                            val = (1 << 32) - 1
                        else:
                            raise ValueError(f'Step {step} is missing {name}')
                elif type_ is str:
                    val = seq.get(step, name, fallback=None)
                    if val is None:
                        raise ValueError(f'Step {step} is missing {name}')
                elif type_ is Optional[str]:
                    val = seq.get(step, name, fallback=None)
                elif type_ is bytes:
                    try:
                        val = self._parse_bytes(seq.get(step, name))
                    except (KeyError, NoOptionError) as exc:
                        raise ValueError(f'Step {step} is missing attribute '
                                         f"'{name}'") from exc
                    except (AttributeError, ValueError) as exc:
                        raise ValueError(f'Step {step}, invalid bytes for '
                                         f'{name}: {exc}') from exc
                else:
                    raise ValueError(f'Step {step}, invalid type for {name}: '
                                     f'{type_}')
                if not isinstance(val, type_):
                    raise TypeError(f'Step {step}, invalid type for {name}: '
                                    f'{type(val)}, expected {type_}')
                self._log.debug("Step %s: attr '%s': type '%s', val= %s",
                                step, name, type_.__name__, val)
                attrs.append(val)
            kmd_step = step_cls(*attrs)
            steps[step] = kmd_step
        return steps

    def _build_steps_from_log(self, itlog: Iterator[str]) -> \
            tuple[dict[str, KeyManagerDpeStep], dict[str, bytes]]:
        module = sys.modules[__name__]
        steps: dict[str, KeyManagerDpeStep] = {}
        results: dict[str, bytes] = {}
        for step, values, result in self._enumerate_steps_from_log(itlog):
            self._log.info('Parsing %s log', step)
            if step in steps:
                raise ValueError(f'Duplicate step: {step}')
            try:
                mode = values['mode']
                step_cls = getattr(module, f'KeyManagerDpeStep{mode.title()}')
            except KeyError as exc:
                raise ValueError('Missing mode in result') from exc
            except AttributeError as exc:
                raise ValueError(f'Unknown mode: {mode}') from exc
            attrs = []
            for name, type_ in step_cls.__annotations__.items():
                if type_ is bool:
                    val = to_bool(values.get(name, True), permissive=False)
                elif type_ is int:
                    val = values.get(name, None)
                    if val is None:
                        if name == 'max_key_version':
                            val = (1 << 32) - 1
                        else:
                            raise ValueError(f'Step {step} is missing {name}')
                    else:
                        val = HexInt.parse(val)
                elif type_ is str:
                    val = values.get(name, None)
                    if val is None:
                        raise ValueError(f'Step {step} is missing {name}')
                elif type_ is Optional[str]:
                    val = values.get(name, None)
                elif type_ is bytes:
                    try:
                        val = self._parse_bytes(values[name])
                    except (KeyError, ValueError) as exc:
                        raise ValueError(f'Step {step}, invalid bytes for '
                                         f'{name}: {exc}') from exc
                else:
                    raise ValueError(f'Step {step}, invalid type for {name}: '
                                     f'{type_}')
                if not isinstance(val, type_):
                    raise TypeError(f'Step {step}, invalid type for {name}: '
                                    f'{type(val)}, expected {type_}')
                attrs.append(val)
            kmd_step = step_cls(*attrs)
            steps[step] = kmd_step
            if result:
                try:
                    results[step] = val = self._parse_bytes(result)
                except ValueError as exc:
                    raise ValueError(f'Step {step}, '
                                     f'invalid result bytes: {exc}') from exc
            elif mode == 'generate':
                raise RuntimeError(f'Step {step}, no result bytes provided')
        return steps, results

    def _enumerate_steps_from_log(self, itlog: Iterator[str]) -> \
            Iterator[tuple[str, dict[str, Union[str, int, bytes, bool]],
                           Optional[bytes]]]:
        step = None
        values: dict[str, Union[str, int, bytes, bool]] = {}
        result: Optional[bytes] = None
        for line in itlog:
            smo = re.match(r'^\[(step_\d+)\]$', line)
            if smo:
                if step:
                    yield step, values, result
                    step = None
                    values = {}
                    result = None
                step = smo.group(1)
                continue
            if not step:
                raise ValueError('Parsing error')
            attr, val = [x.strip() for x in line.split('=')]
            if attr == 'output':
                result = val
            else:
                values[attr] = val
        if step:
            yield step, values, result

    def _execute_steps(self) -> dict[str, bytes]:
        results: dict[str, bytes] = {}
        prefix = 'KeyManagerDpeStep'
        for name, step in self._steps.items():
            self._log.info('Executing %s (%s: %s)', name,
                           step.__class__.__name__.removeprefix(prefix),
                           step.dst)
            if isinstance(step, KeyManagerDpeStepInitialize):
                res = self._kmd.initialize(step.dst)
            elif isinstance(step, KeyManagerDpeStepErase):
                res = self._kmd.erase(step.dst)
            elif isinstance(step, KeyManagerDpeStepAdvance):
                res = self._kmd.advance(step.src, step.dst, step.binding)
            elif isinstance(step, KeyManagerDpeStepGenerate):
                res = self._kmd.generate(step.src, step.dst, step.output,
                                         step.salt, step.key_version)
                res = self._retrieve_output(step.dst, res)
                results[name] = res
            else:
                raise ValueError(f'Unknown step type: '
                                 f'{step.__class__.__name__}')
            self._log.debug('Result: %s', res.hex())
        return results

    def _verify_results(self, results: dict[str, bytes]) -> int:
        error = 0
        for name, step in self._steps.items():
            if not isinstance(step, KeyManagerDpeStepGenerate):
                continue
            self._log.info('Verifying result for %s', name)
            exp_result = self._exp_results[name]
            result = results[name]
            if result != exp_result:
                self._log.error('Key mismatch for %s, %s != %s',
                                name, result.hex(), exp_result.hex())
                error += 1
                continue
            self._log.info('Key verified for %s, %s', name, result.hex())
        return error

    def _retrieve_output(self, output: str, key_: bytes) -> bytes:
        if _CRYPTO_EXC:
            raise _CRYPTO_EXC
        output = output.lower()
        self._log.debug('retrieve output %s', output)
        if output == 'aes':
            assert len(key_) >= 32, 'AES Key must be at least 32 bytes long'
            aes = AES.new(key_[:32], AES.MODE_ECB)
            return aes.encrypt(bytes(16))
        if output == 'kmac':
            kmac = KMAC256.new(key=key_[:32], mac_len=256//8)
            kmac.update(bytes(4))
            return kmac.digest()
        if output == 'otbn':
            return key_
        raise ValueError(f'Invalid output type: {output}')
