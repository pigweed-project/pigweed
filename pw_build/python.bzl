# Copyright 2024 The Pigweed Authors
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
"""Pigweed python build environment for bazel."""

load("@rules_python//python:defs.bzl", "py_binary", "py_library", "py_test")
load("//pw_build:compatibility.bzl", "incompatible_with_mcu")

def pw_py_test(**kwargs):
    """Wrapper for py_test providing some defaults.

    Specifically, this wrapper,

    * Defaults to setting `target_compatible_with` to
      `incompatible_with_mcu()`.
    * Wraps the test entry point with `pw_py_test_runner.py` and includes
      `pyproject.toml` in `data` so `[tool.pytest.ini_options].filterwarnings`
      is applied before the test runs.

    Args:
      **kwargs: Passed to py_test.
    """

    # Python tests are always host only, but allow a user to override
    # the default value.
    if kwargs.get("target_compatible_with") == None:
        kwargs["target_compatible_with"] = incompatible_with_mcu()

    extra_data = []
    for item in native.glob(["pyproject.toml"], allow_empty = True):
        item_label = native.package_relative_label(item)
        if item_label not in extra_data:
            extra_data = extra_data + [item_label]

    if extra_data:
        srcs = kwargs.get("srcs", [])
        orig_main = kwargs.get("main")
        if orig_main == None and type(srcs) == "list":
            if len(srcs) == 1:
                orig_main = srcs[0]
            else:
                name_py = kwargs.get("name", "") + ".py"
                for src in srcs:
                    src_str = str(src)
                    if (
                        src_str == name_py or
                        src_str.endswith("/" + name_py) or
                        src_str.endswith(":" + name_py)
                    ):
                        orig_main = src
                        break

        if orig_main != None and type(orig_main) in ("string", "Label"):
            runner = Label("//pw_build/py:pw_build/pw_py_test_runner.py")
            kwargs["main"] = runner
            if type(srcs) == "list":
                if runner not in srcs:
                    kwargs["srcs"] = srcs + [runner]
            else:
                kwargs["srcs"] = srcs + [runner]

            orig_env = kwargs.get("env")
            main_env = {"PW_PY_TEST_MAIN": "$(rootpath " + str(orig_main) + ")"}
            if orig_env == None:
                kwargs["env"] = main_env
            elif type(orig_env) == "dict":
                env = dict(orig_env)
                env.update(main_env)
                kwargs["env"] = env
            else:
                kwargs["env"] = orig_env | main_env

        data = kwargs.get("data", [])
        if type(data) == "list":
            for item in extra_data:
                if item not in data:
                    data = data + [item]
            kwargs["data"] = data
        else:
            kwargs["data"] = data + extra_data

    py_test(**kwargs)

def pw_py_binary(**kwargs):
    """Wrapper for py_binary providing some defaults.

    Specifically, this wrapper,

    * Defaults to setting `target_compatible_with` to
      `incompatible_with_mcu()`.

    Args:
      **kwargs: Passed to py_binary.
    """

    # Python binaries are always host only, but allow a user to override the
    # default value.
    if kwargs.get("target_compatible_with") == None:
        kwargs["target_compatible_with"] = incompatible_with_mcu()

    py_binary(**kwargs)

def pw_py_library(**kwargs):
    """Wrapper for py_library providing some defaults.

    Specifically, this wrapper,

    * Defaults to setting `target_compatible_with` to
      `incompatible_with_mcu()`.

    Args:
      **kwargs: Passed to py_library.
    """

    # Python binaries are always host only, but allow a user to override the
    # default value.
    if kwargs.get("target_compatible_with") == None:
        kwargs["target_compatible_with"] = incompatible_with_mcu()

    py_library(**kwargs)
