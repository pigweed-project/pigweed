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
"""Common utilities for pigweed.dev documentation extensions."""

import re


def format_href(href: str) -> str:
    """Formats relative or pw:// links into canonical site URLs."""
    if not href:
        return ""
    if href.startswith("http://") or href.startswith("https://"):
        url = href
    elif href.startswith("pw://"):
        url = "https://pigweed.dev/" + href[5:]
    elif href.startswith("/"):
        url = "https://pigweed.dev" + href
    else:
        url = "https://pigweed.dev/" + href

    if url in ("https://pigweed.dev", "https://pigweed.dev/"):
        url = "https://pigweed.dev/index.html"

    m = re.match(r"^(https://pigweed\.dev/rustdoc/[^/]+)/?$", url)
    if m:
        url = f"{m.group(1)}/index.html"

    return url
