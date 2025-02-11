#!/usr/bin/env python3

# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Tiny tool to convert time absolute log time into relative log time.
   It enables comparing different runs time-wise with log files produced
   with OT scripts using the --log-time option.

   timelog.py < pyot.log > pyot-rel.log

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from time import mktime, strptime
from typing import Optional
import sys


def main():
    """Main routine."""
    ts_base: Optional[float] = None
    for line in sys.stdin:
        line = line.strip()
        ts_str, msg = line.split(' ', 1)
        ts_hms, ts_ms = ts_str.split('.', 1)
        ts_f = mktime(strptime(ts_hms, '%H:%M:%S'))
        # for some reason, strptime does not parse %f millisecond string
        ts_f += float(f'.{ts_ms}')
        if ts_base is None:
            ts_base = ts_f
        ts_rel = ts_f - ts_base
        ts_srel = f'{ts_rel:.3f}'
        print(f'{ts_srel:<8s} {msg}')


if __name__ == '__main__':
    main()
