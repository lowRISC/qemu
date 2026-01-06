# Copyright (c) 2024, Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""QMP synchronous wrapper.
"""

from asyncio import new_event_loop, run_coroutine_threadsafe, set_event_loop
from functools import wraps
from inspect import stack, getargvalues  # noqa: F401
from logging import getLogger
from threading import Thread

from qmp.protocol import Runstate
from qmp.qmp_client import QMPClient


class QMPWrapper:
    """Small wrapper around QMPClient to call async QEMU API from a synchronous
       context.
    """

    def __init__(self, target: tuple[str, int]):
        self._qmp = QMPClient('opentitan')
        self._target = target
        self._log = getLogger('qmp')
        self._loop = new_event_loop()
        self._thread = Thread(target=self._run_event_loop, args=(self._loop,),
                              daemon=True)
        self._thread.start()

    def _run_event_loop(self, loop):
        set_event_loop(loop)
        loop.run_forever()

    def reboot(self) -> None:
        """Reboot the guest."""
        self._call_async()

    @property
    def loop(self):
        """Return the event loop."""
        return self._loop

    @staticmethod
    def sync(func):
        """Wrap the function into its asynchronous version."""
        @wraps(func)
        def wrapper(self, *args, **kwargs):
            func_name = func.__name__
            async_fn = getattr(self, f'_{func_name}', None)
            if not async_fn or not callable(async_fn):
                raise RuntimeError(f"Unable to locate asynchrounous function "
                                   f"'{func_name}'")
            # pylint: disable=not-callable
            future = run_coroutine_threadsafe(async_fn(*args, **kwargs),
                                              self.loop)
            return future.result()
        return wrapper

    def _call_async(self) -> None:
        caller = stack()[1]
        frame = caller.frame
        func_name = frame.f_code.co_name
        argvalues = getargvalues(frame)
        argnames = argvalues[0][1:]  # discard self
        argvals = argvalues[-1]
        kwargs = {k: argvals[k] for k in argnames}
        async_fn = getattr(self, f'_{func_name}', None)
        if not async_fn or not callable(async_fn):
            raise RuntimeError(f"Unable to locate asynchrounous function "
                               f"'{func_name}'")
        # pylint: disable=not-callable
        future = run_coroutine_threadsafe(async_fn(**kwargs), self._loop)
        future.result()

    async def _reboot(self):
        """Reset the VM."""
        await self._reconnect()
        self._log.info('Resetting')
        await self._qmp.execute('system_reset')

    async def _reconnect(self):
        if self._qmp.runstate != Runstate.RUNNING:
            self._log.info('Connecting to %s:%d', *self._target)
            await self._qmp.connect(self._target)
