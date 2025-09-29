# Copyright (c) 2025 2025 lowRISC contributors.
# SPDX-License-Identifier: Apache2

"""OpenTitan Tops."""

from ot.util.misc import classproperty
from typing import Optional


class OpenTitanTop:
    """OpenTitan supported tops."""

    SHORT_MAP = {
        'darjeeling': 'dj',
        'earlgrey': 'eg',
    }

    @classproperty
    def names(cls) -> list[str]:
        """Supported top names."""
        # pylint: disable=no-self-argument
        return list(cls.SHORT_MAP)

    @classmethod
    def short_name(cls, topname: str) -> Optional[str]:
        """Get the short form of a top."""
        return cls.SHORT_MAP.get(topname.lower())
