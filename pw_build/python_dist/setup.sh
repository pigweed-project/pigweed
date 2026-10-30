#!/bin/bash

# Copyright 2021 The Pigweed Authors
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

set -o xtrace -o errexit -o nounset

SRC="${BASH_SOURCE[0]}"
DIR="$(python3 -c "import os; print(os.path.dirname(os.path.abspath(os.path.realpath(\"$SRC\"))))")"
VENV="${DIR}/python-venv"
PY_TO_TEST="python3"
CONSTRAINTS_PATH="${DIR}/constraints.txt"
EXTRA_REQUIREMENT_PATH="${DIR}/python_wheels/requirements.txt"

if [ ! -z "${1-}" ]; then
  VENV="${1-}"
  PY_TO_TEST="${VENV}/bin/python"
fi

CONSTRAINTS_ARG=""
if [ -f "${CONSTRAINTS_PATH}" ]; then
    CONSTRAINTS_ARG="-c ""${CONSTRAINTS_PATH}"
fi

EXTRA_REQUIREMENT_ARG=""
if [ -f "${EXTRA_REQUIREMENT_PATH}" ]; then
    EXTRA_REQUIREMENT_ARG="-r ""${EXTRA_REQUIREMENT_PATH}"
fi

PY_MAJOR_VERSION=$(${PY_TO_TEST} -c "import sys; print(sys.version_info[0])")
PY_MINOR_VERSION=$(${PY_TO_TEST} -c "import sys; print(sys.version_info[1])")

if [ ${PY_MAJOR_VERSION} -ne 3 ] || [ ${PY_MINOR_VERSION} -lt 11 ]
then
    echo "ERROR: This Python distributable requires Python 3.11 or newer."
    exit 1
fi

if [ ! -d "${VENV}" ]
then
    ${PY_TO_TEST} -m venv "${VENV}"
fi

# Note: pip install --require-hashes will be triggered if any hashes are present
# in the requirement.txt file.
"${VENV}/bin/python" -m pip install \
  --find-links="${DIR}/python_wheels" \
  -r ${DIR}/requirements.txt ${EXTRA_REQUIREMENT_ARG} ${CONSTRAINTS_ARG}

exit 0
