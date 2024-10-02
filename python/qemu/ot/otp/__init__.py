# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""One-Time Programmable controller."""

from .descriptor import OTPPartitionDesc, OTPRegisterDef  # noqa: F401
from .image import OtpImage  # noqa: F401
from .map import OtpMap  # noqa: F401
from .partition import OtpLifecycleExtension, OtpPartition  # noqa: F401
