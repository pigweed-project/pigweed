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
"""Sphinx extension to unify Sphinx, Doxygen, and Rustdoc UIs."""

from typing import Any

from sphinx.application import Sphinx

from .header import postprocess


def _on_build_finished(app: Sphinx, exception: Exception | None) -> None:
    if exception is not None or app.builder.format != "html":
        return

    postprocess(app, exception)


def setup(app: Sphinx) -> dict[str, Any]:
    app.connect("build-finished", _on_build_finished)
    return {
        "version": "0.1",
        "parallel_read_safe": True,
        "parallel_write_safe": True,
    }
