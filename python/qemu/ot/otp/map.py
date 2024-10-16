# Copyright (c) 2023-2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""OTP map.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from logging import getLogger
from typing import Any, Iterator, Optional, TextIO

try:
    # try to load HJSON if available
    from hjson import load as hjload
except ImportError:
    hjload = None

from ot.util.misc import round_up


class OtpMap:
    """OTP configuration.

       Assume partition file does not contain any error or missing information,
       it should have been validated by OT tools first.
    """
    BLOCK_SIZE = 8

    HARDENED_BOOLEANS = {
        0x739: True,
        0x1d4: False
    }

    MUBI8_BOOLEANS = {
        0x96: False,
        0x69: True,
        0x00: None
    }

    def __init__(self):
        self._log = getLogger('otp.map')
        self._map: dict = {}
        self._otp_size = 0
        self._partitions: list[OtpPartition] = []

    def load(self, hfp: TextIO) -> None:
        """Parse a HJSON configuration file, typically otp_ctrl_mmap.hjson
        """
        if hjload is None:
            raise ImportError('HJSON module is required')
        self._map = hjload(hfp, object_pairs_hook=dict)
        otp = self._map['otp']
        self._otp_size = int(otp['width']) * int(otp['depth'])
        self._generate_partitions()
        self._compute_locations()

    @property
    def partitions(self) -> dict[str, Any]:
        """Return the partitions (in any)"""
        return {p['name']: p for p in self._map.get('partitions', [])}

    @classmethod
    def part_offset(cls, part: dict[str, Any]) -> int:
        """Get the offset of a partition."""
        # expect a KeyError if missing
        return int(part['offset'])

    def enumerate_partitions(self) -> Iterator['OtpPartition']:
        """Enumerate the partitions in their address order."""
        return iter(self._partitions)

    def _generate_partitions(self) -> None:
        parts = self._map.get('partitions', [])
        have_offset = all('offset' in p for p in parts)
        if not have_offset:
            # either all or no partition should have an offset definition
            if any('offset' in p for p in parts):
                raise RuntimeError('Incoherent offset use in partitions')
        if have_offset:
            # if offset are defined, first create a shallow copy of the
            # partition in sorted order
            parts = list(sorted(parts, key=OtpMap.part_offset))
        self._partitions = []
        for part in parts:
            # shallow copy of the partition
            part = dict(part)
            name = part['name']
            # remove the name from the dict
            del part['name']
            desc = part.get('desc', '').replace('\n', ' ')
            # remove description from partition
            if desc:
                del part['desc']
            # remove descriptions from items
            items = {}
            for item in part.get('items', []):
                assert isinstance(item, dict)
                # shallow copy
                item = dict(item)
                if 'desc' in item:
                    del item['desc']
                # assume name & size are always defined for each item
                item_name = item['name']
                del item['name']
                item_size = int(item['size'])
                item['size'] = item_size
                assert item_name not in items
                items[item_name] = item
            part['items'] = items
            items_size = sum(it['size'] for it in items.values())
            # some partitions define their overall size, most don't
            # if the size is defined, it takes precedence over the sum of its
            # items
            part_size = int(part.get('size', '0'))
            has_digest = any(part.get(f'{k}w_digest') for k in 'sh')
            if has_digest:
                items_size += OtpPartition.DIGEST_SIZE
            if part_size:
                assert items_size <= part_size
            else:
                part_size = round_up(items_size, self.BLOCK_SIZE)
            # update the partition with is actual size
            part['size'] = part_size
            # special ugly case as configuration file defines is_keymgr per item
            # but RTL defines it per partition for some reason
            kmm = self._check_keymgr_materials(name, part['items'])
            if kmm:
                part[kmm[0]] = kmm[1]
            prefix = name.title().replace('_', '')
            partname = f'{prefix}Part'
            newpart = type(partname, (OtpPartition,),
                           {'name': name, '__doc__': desc})
            self._partitions.append(newpart(part))

    def _check_keymgr_materials(self, partname: str, items: dict[str, dict]) \
            -> Optional[tuple[str, bool]]:
        """Check partition for key manager material fields."""
        kms: dict[str, bool] = {}
        kmprefix = 'iskeymgr'
        for props in items.values():
            for prop, value in props.items():
                if prop.startswith(kmprefix):
                    kind = prop[len(kmprefix):]
                    if kind not in kms:
                        kms[kind] = set()
                    kms[kind].add(value)
        kind_count = len(kms)
        if not kind_count:
            return None
        if kind_count > 1:
            raise ValueError(f'Incoherent key manager material definition in '
                             f'{partname} partition')
        kind = set(kms).pop()
        enable = any(kms[kind])
        return f'{kmprefix}{kind}', enable

    def _compute_locations(self) -> None:
        """Update partitions with their location within the OTP map."""
        absorb_parts = [p for p in self._partitions
                        if getattr(p, 'absorb', False)]
        total_size = sum(p.size for p in self._partitions)
        rem_size = self._otp_size - total_size
        rem_blocks = rem_size // self.BLOCK_SIZE
        absorb_count = len(absorb_parts)
        blk_per_part = rem_blocks // absorb_count
        extra_blocks = rem_blocks % absorb_count
        self._log.info("%d bytes (%d blocks) to absorb into %d partition%s",
                       rem_size, rem_blocks, absorb_count,
                       's' if absorb_count > 1 else '')
        for part in absorb_parts:
            psize = part.size
            part.size += self.BLOCK_SIZE * blk_per_part
            if extra_blocks:
                part.size += self.BLOCK_SIZE
                extra_blocks -= 1
            self._log.info('Partition %s size augmented from %u to %u',
                           part.name, psize, part.size)
        for part in self._partitions:
            part_offset = 0
            for part in self._partitions:
                if part.sw_digest or part.hw_digest:
                    digest_offset = part_offset + part.size - 8
                else:
                    digest_offset = None
                setattr(part, 'offset', part_offset)
                setattr(part, 'digest_offset', digest_offset)
                part_offset += part.size
        assert part_offset == self._otp_size, "Unexpected partition offset"


# imported here to avoid Python circular dependency issue
# pylint: disable=cyclic-import
# pylint: disable=wrong-import-position
from .partition import OtpPartition  # noqa: E402
