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
"""Bazel rule for parallel pigweed.dev site assembly."""

load("//pw_build:python.bzl", "pw_py_binary")

_SPHINX_SERVE_SRC = Label("@sphinxdocs//sphinxdocs/private:sphinx_server.py")

def _assemble_site_impl(ctx):
    sphinx_html = ctx.attr.sphinx[OutputGroupInfo].html.to_list()[0]
    conf = [
        f
        for f in ctx.files.sphinx_sources
        if f.short_path.endswith("/_sources/conf.py")
    ][0]

    out_dir = ctx.actions.declare_directory(ctx.label.name + "/_build/html")
    args = ctx.actions.args()
    args.add("--sphinx-html", sphinx_html.path)
    args.add("--doxygen-html", ctx.file.doxygen.path)
    args.add("--rustdoc-html", ctx.file.rustdoc.path)
    args.add("--sphinx-srcdir", conf.dirname)
    args.add("--out-dir", out_dir.path)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = depset(
            direct = [sphinx_html, ctx.file.doxygen, ctx.file.rustdoc],
            transitive = [ctx.attr.sphinx_sources[DefaultInfo].files],
        ),
        outputs = [out_dir],
        mnemonic = "DocsAssembleSite",
        progress_message = "Assembling pigweed.dev site %{output}",
    )
    return [
        DefaultInfo(
            files = depset([out_dir]),
            runfiles = ctx.runfiles(files = [out_dir]),
        ),
        OutputGroupInfo(html = depset([out_dir])),
    ]

_assemble_site = rule(
    implementation = _assemble_site_impl,
    attrs = {
        "doxygen": attr.label(allow_single_file = True, mandatory = True),
        "rustdoc": attr.label(allow_single_file = True, mandatory = True),
        "sphinx": attr.label(mandatory = True, providers = [OutputGroupInfo]),
        "sphinx_sources": attr.label(allow_files = True, mandatory = True),
        "_tool": attr.label(
            cfg = "exec",
            default = Label("//docs/common:assemble"),
            executable = True,
        ),
    },
)

def assemble_site(name, **kwargs):
    _assemble_site(name = name, **kwargs)
    pw_py_binary(
        name = name + ".serve",
        srcs = [_SPHINX_SERVE_SRC],
        args = ["$(rlocationpath :{})".format(name)],
        data = [":" + name],
        main = _SPHINX_SERVE_SRC,
        tags = ["manual", "nomypy", "nopylint"],
        target_compatible_with = kwargs.get("target_compatible_with"),
        visibility = kwargs.get("visibility"),
        deps = [Label("@rules_python//python/runfiles")],
    )
