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
"""Tests for pw_env_setup.virtualenv_setup.install."""

# pylint: disable=protected-access

import importlib
import io
import os
import subprocess
import tempfile
import unittest
from unittest import mock

# pw_env_setup.virtualenv_setup re-exports install.install(), which shadows the
# install module attribute, so import the module explicitly.
install = importlib.import_module('pw_env_setup.virtualenv_setup.install')

_TIMEOUT_OUTPUT = (
    'pip._vendor.urllib3.exceptions.ReadTimeoutError: '
    "HTTPSConnectionPool(host='files.pythonhosted.org', port=443): "
    'Read timed out.\n'
)
_RESOLUTION_OUTPUT = 'ERROR: ResolutionImpossible: conflicting dependencies\n'


def _fake_check_call(outputs):
    """Returns a check_call fake that emits outputs, failing if non-None."""
    remaining = list(outputs)

    def check_call(args, **kwargs):
        output = remaining.pop(0)
        if output is None:
            return 0
        kwargs['stdout'].write(output)
        raise subprocess.CalledProcessError(1, args)

    return check_call


def _fake_call(outputs):
    """Returns a subprocess.call fake that emits outputs, failing if set."""
    remaining = list(outputs)

    def call(unused_args, **kwargs):
        output = remaining.pop(0)
        if output is None:
            return 0
        kwargs['stdout'].write(output)
        return 1

    return call


@mock.patch.object(install.time, 'sleep')
class CheckCallTest(unittest.TestCase):
    """Tests for install._check_call."""

    def test_retries_transient_network_error(self, unused_sleep) -> None:
        out = io.StringIO()
        with mock.patch.object(
            install.subprocess,
            'check_call',
            side_effect=_fake_check_call([_TIMEOUT_OUTPUT, None]),
        ) as check_call:
            install._check_call(
                ['pip', 'install'], retry_on_network_errors=True, stdout=out
            )
        self.assertEqual(check_call.call_count, 2)
        self.assertIn('ReadTimeoutError', out.getvalue())
        self.assertIn('retrying', out.getvalue())

    def test_gives_up_after_max_attempts(self, unused_sleep) -> None:
        out = io.StringIO()
        attempts = install._NETWORK_COMMAND_ATTEMPTS
        with mock.patch.object(
            install.subprocess,
            'check_call',
            side_effect=_fake_check_call([_TIMEOUT_OUTPUT] * attempts),
        ) as check_call:
            with self.assertRaises(subprocess.CalledProcessError):
                install._check_call(
                    ['pip', 'install'], retry_on_network_errors=True, stdout=out
                )
        self.assertEqual(check_call.call_count, attempts)

    def test_does_not_retry_non_network_error(self, unused_sleep) -> None:
        out = io.StringIO()
        with mock.patch.object(
            install.subprocess,
            'check_call',
            side_effect=_fake_check_call([_RESOLUTION_OUTPUT]),
        ) as check_call:
            with self.assertRaises(subprocess.CalledProcessError):
                install._check_call(
                    ['pip', 'install'], retry_on_network_errors=True, stdout=out
                )
        self.assertEqual(check_call.call_count, 1)
        self.assertIn('ResolutionImpossible', out.getvalue())

    def test_does_not_retry_by_default(self, unused_sleep) -> None:
        out = io.StringIO()
        with mock.patch.object(
            install.subprocess,
            'check_call',
            side_effect=_fake_check_call([_TIMEOUT_OUTPUT]),
        ) as check_call:
            with self.assertRaises(subprocess.CalledProcessError):
                install._check_call(['pip', 'install'], stdout=out)
        self.assertEqual(check_call.call_count, 1)


@mock.patch.object(install.time, 'sleep')
class CheckCallLoggedTest(unittest.TestCase):
    """Tests for install._check_call_logged."""

    def setUp(self) -> None:
        self._tempdir = tempfile.TemporaryDirectory()
        self.log_path = os.path.join(self._tempdir.name, 'ninja.log')

    def tearDown(self) -> None:
        self._tempdir.cleanup()

    def test_retries_transient_network_error(self, unused_sleep) -> None:
        with mock.patch.object(
            install.subprocess,
            'call',
            side_effect=_fake_call([_TIMEOUT_OUTPUT, None]),
        ) as call:
            install._check_call_logged(
                ['ninja'], self.log_path, retry_on_network_errors=True
            )
        self.assertEqual(call.call_count, 2)
        with open(self.log_path) as ins:
            log = ins.read()
        self.assertIn("['ninja']", log)
        self.assertIn('ReadTimeoutError', log)
        self.assertIn('retrying', log)

    def test_raises_with_full_log(self, unused_sleep) -> None:
        attempts = install._NETWORK_COMMAND_ATTEMPTS
        with mock.patch.object(
            install.subprocess,
            'call',
            side_effect=_fake_call([_TIMEOUT_OUTPUT] * attempts),
        ) as call:
            with self.assertRaises(subprocess.CalledProcessError) as ctx:
                install._check_call_logged(
                    ['ninja'], self.log_path, retry_on_network_errors=True
                )
        self.assertEqual(call.call_count, attempts)
        self.assertEqual(
            ctx.exception.output.count('ReadTimeoutError'), attempts
        )

    def test_does_not_retry_non_network_error(self, unused_sleep) -> None:
        with mock.patch.object(
            install.subprocess,
            'call',
            side_effect=_fake_call([_RESOLUTION_OUTPUT]),
        ) as call:
            with self.assertRaises(subprocess.CalledProcessError) as ctx:
                install._check_call_logged(
                    ['ninja'], self.log_path, retry_on_network_errors=True
                )
        self.assertEqual(call.call_count, 1)
        self.assertIn('ResolutionImpossible', ctx.exception.output)

    def test_only_inspects_latest_attempt(self, unused_sleep) -> None:
        # A network error from an earlier attempt must not cause a later,
        # unrelated failure to be retried.
        with mock.patch.object(
            install.subprocess,
            'call',
            side_effect=_fake_call([_TIMEOUT_OUTPUT, _RESOLUTION_OUTPUT]),
        ) as call:
            with self.assertRaises(subprocess.CalledProcessError):
                install._check_call_logged(
                    ['ninja'], self.log_path, retry_on_network_errors=True
                )
        self.assertEqual(call.call_count, 2)


class PipNetworkEnvTest(unittest.TestCase):
    """Tests for install._pip_network_env."""

    def test_sets_default_timeout(self) -> None:
        with mock.patch.dict(os.environ, clear=True):
            env = install._pip_network_env()
        self.assertEqual(
            env['PIP_DEFAULT_TIMEOUT'], install._PIP_DEFAULT_TIMEOUT_SECONDS
        )

    def test_respects_existing_timeout(self) -> None:
        with mock.patch.dict(os.environ, {'PIP_DEFAULT_TIMEOUT': '5'}):
            env = install._pip_network_env()
        self.assertEqual(env['PIP_DEFAULT_TIMEOUT'], '5')


if __name__ == '__main__':
    unittest.main()
