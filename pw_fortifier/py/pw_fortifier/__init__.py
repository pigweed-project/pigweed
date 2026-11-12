# Copyright 2026 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""pw_fortifier package."""

from pkgutil import extend_path  # type: ignore
import sys
import threading
from typing import Any

# Allows importing modules from both the source tree and the generated output
# tree under a single pw_fortifier namespace.
__path__ = extend_path(__path__, __name__)  # type: ignore


def patch_pyfakefs_thread_safety() -> None:
    """Makes pyfakefs FakeOsModule.use_original thread-local.

    In Python 3.13+, pyfakefs patches linecache.checkcache and
    linecache.updatecache with use_original_os(), which sets a global
    FakeOsModule.use_original = True. When IsolatedAsyncioTestCase runs with
    debug=True on the main event loop thread, traceback extraction invokes
    linecache concurrently with AsyncPath worker threads, causing worker
    threads to bypass the fake filesystem and call real os functions.

    See: https://github.com/pytest-dev/pyfakefs/issues/1317
    """
    fake_os: Any = sys.modules.get('pyfakefs.fake_os')
    if fake_os is None:
        return

    if fake_os.FakeOsModule.__name__ == '_ThreadLocalFakeOsModule':
        return

    tls = threading.local()
    orig_cls: Any = fake_os.FakeOsModule

    class _UseOriginalDescriptor:
        def __init__(self, local_state: threading.local) -> None:
            self._tls = local_state

        def __get__(self, _obj: Any, _objtype: Any = None) -> bool:
            return getattr(self._tls, 'use_original', False)

        def __set__(self, _obj: Any, val: bool) -> None:
            self._tls.use_original = val

    class _FakeOsMeta(type):
        _orig_cls: Any = orig_cls
        _tls: threading.local = tls

        @property
        def use_original(cls) -> bool:
            return getattr(cls._tls, 'use_original', False)

        @use_original.setter
        def use_original(cls, val: bool) -> None:
            cls._tls.use_original = val

        def __instancecheck__(cls, instance: Any) -> bool:
            return isinstance(instance, cls._orig_cls)

    descriptor = _UseOriginalDescriptor(tls)
    orig_cls.use_original = descriptor

    class _ThreadLocalFakeOsModule(orig_cls, metaclass=_FakeOsMeta):
        use_original = descriptor

    fake_os.FakeOsModule = _ThreadLocalFakeOsModule


patch_pyfakefs_thread_safety()
