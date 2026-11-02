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
"""A rule that generates a hardcoded C++ source and header file."""

load("@rules_cc//cc:cc_library.bzl", "cc_library")

def cpp_gen(name, **kwargs):
    genrule_name = name + "_gen"

    native.genrule(
        name = genrule_name,
        outs = [name + ".cc", name + ".h"],
        cmd = "sh $(execpath :generate_cpp.sh) \"$(execpath :" + name + ".cc)\" \"$(execpath :" + name + ".h)\"",
        tools = [":generate_cpp.sh"],
    )

    cc_library(
        name = name,
        srcs = [name + ".cc"],
        hdrs = [name + ".h"],
        includes = ["."],
        **kwargs
    )
