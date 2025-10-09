# Copyright (c) 2024-2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Miscellaneous helpers.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from io import BytesIO
from os.path import abspath, dirname, exists, isdir, isfile, join as joinpath
from subprocess import check_output
from sys import stdout
from textwrap import dedent, indent
from typing import Any, Iterable, Optional, TextIO, Union
import re


_TRUE_BOOLEANS = ['on', 'high', 'true', 'enable', 'enabled', 'yes', '1']
"""String values evaluated as true boolean values"""

_FALSE_BOOLEANS = ['off', 'low', 'false', 'disable', 'disabled', 'no', '0']
"""String values evaluated as false boolean values"""


class classproperty(property):
    """Getter property decorator for a class"""
    # pylint: disable=invalid-name
    def __get__(self, obj: Any, objtype=None) -> Any:
        return super().__get__(objtype)


class HexInt(int):
    """Simple wrapper to always represent an integer in hexadecimal format."""

    def __repr__(self) -> str:
        return f'0x{self:x}'

    @staticmethod
    def parse(val: Optional[Union[str, int]], base: Optional[int] = None,
              accept_int: bool = False) \
            -> Optional['HexInt']:
        """Simple helper to support hexadecimal integer in argument parser."""
        if val is None:
            return None
        if accept_int and isinstance(val, int):
            return HexInt(val)
        if base is not None:
            return HexInt(int(val, base))
        return HexInt(int(val, val.startswith('0x') and 16 or 10))

    @staticmethod
    def xparse(value: Union[None, int, str]) -> Optional['HexInt']:
        """Parse a value and convert it into an integer value if possible.

        Input value may be:
        - None
        - a string with an integer coded as a decimal value
        - a string with an integer coded as a hexadecimal value
        - a integral value
        - a integral value with a unit specifier (kilo or mega)

        :param value: input value to convert to an integer
        :return: the value as an integer
        :raise ValueError: if the input value cannot be converted into an int
        """
        if value is None:
            return None
        if isinstance(value, int):
            return HexInt(value)
        imo = re.match(r'^\s*(\d+)\s*(?:([KMkm]i?)?B?)?\s*$', value)
        if imo:
            mult = {'K': (1000),
                    'KI': (1 << 10),
                    'M': (1000 * 1000),
                    'MI': (1 << 20)}
            value = int(imo.group(1))
            if imo.group(2):
                value *= mult[imo.group(2).upper()]
            return value
        return HexInt(int(value.strip(), value.startswith('0x') and 16 or 10))


class EasyDict(dict):
    """Dictionary whose members can be accessed as instance members
    """

    def __init__(self, dictionary=None, **kwargs):
        if dictionary is not None:
            self.update(dictionary)
        self.update(kwargs)

    def __getattr__(self, name):
        try:
            return self.__getitem__(name)
        except KeyError as exc:
            raise AttributeError(f"'{self.__class__.__name__}' object has no "
                                 f"attribute '{name}'") from exc

    def __setattr__(self, name, value):
        self.__setitem__(name, value)

    def __dir__(self) -> Iterable[Any]:
        items = set(super().__dir__())
        items.update(set(self))
        yield from sorted(items)


def flatten(lst: list) -> list:
    """Flatten a list."""
    return [item for sublist in lst for item in sublist]


def group(lst, count):
    """Group a list into consecutive count-tuples. Incomplete tuples are
    discarded.

    `group([0,3,4,10,2,3], 2) => [(0,3), (4,10), (2,3)]`

    From: http://aspn.activestate.com/ASPN/Cookbook/Python/Recipe/303060
    """
    return list(zip(*[lst[i::count] for i in range(count)]))


def dump_buffer(buffer: Union[bytes, bytearray, BytesIO], addr: int = 0,
                file: Optional[TextIO] = None) -> None:
    """Dump a binary buffer, same format as hexdump -C."""
    if isinstance(buffer, BytesIO):
        buffer = buffer.getbuffer()
    size = len(buffer)
    if not file:
        file = stdout
    for pos in range(0, size, 16):
        chunks = buffer[pos:pos+8], buffer[pos+8:pos+16]
        buf = '  '.join(' '.join(f'{x:02x}' for x in c) for c in chunks)
        if len(buf) < 48:
            buf = f'{buf}{" " * (48 - len(buf))}'
        chunk = buffer[pos:pos+16]
        text = ''.join(chr(c) if 0x20 <= c < 0x7f else '.' for c in chunk)
        if len(text) < 16:
            text = f'{text}{" " * (16-len(text))}'
        print(f'{addr+pos:08x}  {buf}  |{text}|', file=file)


def round_up(value: int, rnd: int) -> int:
    """Round up a integer value."""
    return (value + rnd - 1) & -rnd


def camel_to_snake_case(camel: str) -> str:
    """Convert CamelString string into snake_case lower string."""
    pattern = r'(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])'
    return re.sub(pattern, '_', camel).lower()


def camel_to_snake_uppercase(name: str) -> str:
    """Extended version of camel_to_snake_case to convert device/parameter
       names.
    """
    pattern = r'(?<=[\d])(?=[A-Z]{2,})'
    return re.sub(pattern, '_', camel_to_snake_case(name).upper())


def to_bool(value, permissive=True, prohibit_int=False):
    """Parse a string and convert it into a boolean value if possible.

       Input value may be:
       - a string with an integer value, if `prohibit_int` is not set
       - a boolean value
       - a string with a common boolean definition

       :param value: the value to parse and convert
       :type value: str or int or bool
       :param bool permissive: default to the False value if parsing fails
       :param bool prohibit_int: prohibit an integral type as the input value
       :rtype: bool
       :raise ValueError: if the input value cannot be converted into an bool
    """
    if value is None:
        return False
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        if not prohibit_int:
            if permissive:
                return bool(value)
            if value in (0, 1):
                return bool(value)
        raise ValueError(f"Invalid boolean value: '{value}'")
    if value.lower() in _TRUE_BOOLEANS:
        return True
    if permissive or (value.lower() in _FALSE_BOOLEANS):
        return False
    raise ValueError(f"Invalid boolean value: '{value}")


def alphanum_key(text: str) -> list[Union[int, str]]:
    """Alphanumerical sorting key.

       :param text: the text to generate a sorting key from
       :return: a list of alternating str and integer values
    """
    return [int(t) if t.isdigit() else t for t in re.split(r'(\d+)', text)]


def redent(text: str, spc: int = 0, strip_end: bool = False) -> str:
    """Utility function to re-indent code string.

        :param text: the text to re-indent
        :param spc: the number of leading empty space chars to prefix lines
        :param strip_end: whether to strip trailing whitespace and newline
    """
    text = dedent(text.lstrip('\n'))
    text = indent(text, ' ' * spc)
    if strip_end:
        text = text.rstrip(' ').rstrip('\n')
    return text


def retrieve_git_version(path: str, max_tag_dist: int = 100) \
        -> Optional[str]:
    """Return a Git identifier whenever possible.

        :param path: the configuration file or directory to track the repository
                     version identifier. Note that the returned Git identifier
                     is not the commit version of this file / directory, but the
                     one of the repo top-level directory.
        :param max_tag_dist: maximum distance (in commit number) to the closest
                             tag. If distance is greater, only emit the commit
                             identifier
        :return: Git version of the top-level directory
    """
    cfgdir: Optional[str] = None
    path = abspath(path)
    if isfile(path):
        cfgdir = dirname(path)
    elif isdir(path):
        cfgdir = path
    else:
        return None
    while cfgdir and isdir(cfgdir):
        gitdir = joinpath(cfgdir, '.git')
        if exists(gitdir):  # either a dir or a file for worktree
            break
        pardir = dirname(cfgdir)
        if pardir == cfgdir:
            # reach top of tree hierarchy without any detected .git directory
            return None
        cfgdir = pardir
    else:
        return None
    assert cfgdir is not None
    try:
        descstr = check_output(['git', 'describe', '--long', '--dirty'],
                               text=True, cwd=cfgdir).strip()
        gmo = re.match(r'^(?P<tag>.*)-(?P<dist>\d+)-g(?P<commit>[0-9a-f]+)'
                       r'(?:-(?P<dirty>dirty))?$', descstr)
        if not gmo:
            raise ValueError('Unknown Git describe format')
        distance = int(gmo.group('dist'))
        dirty = gmo.group('dirty')
        if distance == 0:
            return '-'.join(filter(None, (gmo.group('tag'), dirty)))
        if distance <= max_tag_dist:
            return descstr
        return '-'.join(filter(None, (gmo.group('commit'), dirty)))
    except (OSError, ValueError):
        pass
    try:
        change = check_output(['git', 'status', '--porcelain'],
                              text=True, cwd=cfgdir).strip()
        descstr = check_output(['git', 'rev-parse', '--short', 'HEAD'],
                               text=True, cwd=cfgdir).strip()
        if len(change) > 1:
            descstr = f'{descstr}-dirty'
        return descstr
    except OSError:
        pass
    return None
