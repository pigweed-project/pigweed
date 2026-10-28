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
"""Bazel rules for TypeScript libraries and Node-based unit tests."""

load("@aspect_rules_esbuild//esbuild:defs.bzl", "esbuild")
load("@aspect_rules_js//js:defs.bzl", "js_library", "js_run_binary", "js_test")
load("@bazel_skylib//rules:write_file.bzl", "write_file")
load("//pw_build:compatibility.bzl", "incompatible_with_mcu")

def _pw_ts_typecheck(name, srcs, src_lib, deps, target_compatible_with, testonly = False):
    ts_srcs = [
        s
        for s in srcs
        if s.endswith(".ts") and not s.startswith("//") and not s.startswith(":")
    ]
    if not ts_srcs:
        return None

    pkg = native.package_name()
    rel_root = "/".join([".."] * len(pkg.split("/"))) if pkg else "."
    tsconfig_file = "_%s_tsconfig.json" % name
    tsbuildinfo_out = "_%s.tsbuildinfo" % name
    tsconfig_bin_path = ("%s/%s" % (pkg, tsconfig_file)) if pkg else tsconfig_file

    tsconfig_json = """{
  "compilerOptions": {
    "allowJs": true,
    "baseUrl": "%s",
    "esModuleInterop": true,
    "experimentalDecorators": true,
    "incremental": true,
    "lib": ["dom", "dom.iterable", "esnext"],
    "module": "esnext",
    "moduleResolution": "node",
    "noEmit": true,
    "paths": {
      "pigweedjs/pw_build/*": ["./pw_build/ts/*"],
      "pigweedjs/pw_*": ["./pw_*/ts"],
      "pigweedjs/protos/*": ["./pw_web/dist/protos/*"],
      "pigweedjs/types/*": ["./pw_web/ts/types/*"],
      "*": ["./pw_web/node_modules/*"]
    },
    "resolveJsonModule": true,
    "skipLibCheck": true,
    "target": "es2020",
    "tsBuildInfoFile": "./%s",
    "typeRoots": ["%s/pw_web/node_modules/@types"]
  },
  "files": %s
}""" % (
        rel_root,
        tsbuildinfo_out,
        rel_root,
        json.encode(["./" + s for s in ts_srcs]),
    )

    write_file(
        name = "_%s_write_tsconfig" % name,
        out = tsconfig_file,
        content = [tsconfig_json],
        tags = ["manual"],
        testonly = testonly,
    )

    typecheck_name = "_%s_typecheck" % name
    js_run_binary(
        name = typecheck_name,
        srcs = [
            src_lib,
            tsconfig_file,
            "//pw_web:node_modules",
        ] + deps,
        outs = [tsbuildinfo_out],
        args = [
            "-p",
            tsconfig_bin_path,
        ],
        include_types = True,
        tags = ["manual"],
        target_compatible_with = target_compatible_with,
        testonly = testonly,
        tool = "//pw_web:tsc_bin",
    )
    return ":%s" % typecheck_name

def pw_ts_library(
        name,
        srcs = [],
        deps = [],
        data = [],
        visibility = None,
        tags = [],
        testonly = False,
        **kwargs):
    """Defines a TypeScript library with compile-time typechecking.

    Args:
        name: Target name.
        srcs: TypeScript source files and assets.
        deps: Dependencies (other pw_ts_library or js_library targets).
        data: Runtime data dependencies.
        visibility: Target visibility.
        tags: Bazel tags.
        testonly: Whether this library is test-only.
        **kwargs: Additional arguments forwarded to js_library.
    """
    target_compatible_with = kwargs.pop(
        "target_compatible_with",
        incompatible_with_mcu(),
    )
    raw_lib_name = "_%s_srcs" % name
    js_library(
        name = raw_lib_name,
        srcs = srcs,
        data = data,
        deps = deps,
        tags = ["manual"],
        target_compatible_with = target_compatible_with,
        testonly = testonly,
    )

    typecheck_target = _pw_ts_typecheck(
        name = name,
        srcs = srcs,
        src_lib = ":%s" % raw_lib_name,
        deps = deps,
        target_compatible_with = target_compatible_with,
        testonly = testonly,
    )

    js_library(
        name = name,
        srcs = [typecheck_target] if typecheck_target else [],
        data = data,
        deps = [":%s" % raw_lib_name] + deps,
        tags = tags,
        target_compatible_with = target_compatible_with,
        testonly = testonly,
        visibility = visibility,
        **kwargs
    )

def pw_ts_test(
        name,
        srcs,
        deps = [],
        data = [],
        visibility = None,
        tags = [],
        size = None,
        timeout = None,
        **kwargs):
    """Defines a TypeScript unit test suite executed in Node.js.

    Args:
        name: Test target name.
        srcs: TypeScript test source files.
        deps: Dependencies (pw_ts_library or js_library targets).
        data: Runtime data dependencies (such as C++ server binaries).
        visibility: Target visibility.
        tags: Bazel tags.
        size: Bazel test size.
        timeout: Bazel test timeout.
        **kwargs: Additional arguments forwarded to js_test.
    """
    target_compatible_with = kwargs.pop(
        "target_compatible_with",
        incompatible_with_mcu(),
    )
    all_deps = deps + ["//pw_build/ts:jest_shim"]
    raw_lib_name = "_%s_srcs" % name
    js_library(
        name = raw_lib_name,
        srcs = srcs,
        deps = all_deps,
        tags = ["manual"],
        target_compatible_with = target_compatible_with,
        testonly = True,
    )

    typecheck_target = _pw_ts_typecheck(
        name = name,
        srcs = srcs,
        src_lib = ":%s" % raw_lib_name,
        deps = all_deps,
        target_compatible_with = target_compatible_with,
        testonly = True,
    )

    runner_file = "_%s_runner.ts" % name
    runner_lines = ['import "pigweedjs/pw_build/jest_shim";']
    for src in srcs:
        if src.endswith(".ts"):
            runner_lines.append('import "./%s";' % src[:-3])

    write_file(
        name = "_%s_write_runner" % name,
        out = runner_file,
        content = runner_lines,
        tags = ["manual"],
        testonly = True,
    )

    bundle_name = "_%s_bundle" % name
    esbuild(
        name = bundle_name,
        srcs = [typecheck_target] if typecheck_target else [],
        deps = [
            ":%s" % raw_lib_name,
            "//pw_web:node_modules",
        ] + all_deps,
        entry_point = runner_file,
        format = "cjs",
        platform = "node",
        sourcemap = "inline",
        tags = ["manual"],
        target = ["es2020"],
        target_compatible_with = target_compatible_with,
        testonly = True,
        tsconfig = "//:tsconfig",
    )

    js_test(
        name = name,
        data = data + [":%s" % bundle_name],
        entry_point = ":%s.js" % bundle_name,
        size = size,
        tags = tags,
        target_compatible_with = target_compatible_with,
        timeout = timeout,
        visibility = visibility,
        **kwargs
    )
