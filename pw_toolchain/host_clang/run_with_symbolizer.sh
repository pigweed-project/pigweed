#!/bin/bash

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

# Set PATH to include llvm-symbolizer from known toolchain locations in runfiles
#
# This script is intended to be used with Bazel's --run_under flag for tests.
#
# We use TEST_SRCDIR which is set to the absolute path of the runfiles directory.
#
# The symbolizer path depends on the module name and differs based on whether we
# are running in the pigweed tree or from outside it.

if [ -n "$TEST_SRCDIR" ]; then
  PATHS=(
    "$TEST_SRCDIR/pigweed++pw_cxx_toolchain+llvm_toolchain/bin"
    "$TEST_SRCDIR/+pw_cxx_toolchain+llvm_toolchain/bin"
  )

  for p in "${PATHS[@]}"; do
    if [ -x "$p/llvm-symbolizer" ]; then
      export PATH="$p:$PATH"
      break
    fi
  done
fi

exec "$@"
