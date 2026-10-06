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

# TODO: b/531679151 - Remove this file and switch MODULE.bazel back to
# `@rules_fuchsia//fuchsia:products.bzl` once the upstream Fuchsia
# `fuchsia_remote_product_bundle` rule is updated to stop passing the removed
# `product_version` field to `FuchsiaProductBundleInfo`.

"""Workaround for @rules_fuchsia//fuchsia:products.bzl passing removed product_version field."""

def _fuchsia_products_repository_impl(ctx):
    product_bundles_metadata = json.decode(ctx.read(ctx.attr.metadata_file))
    ctx.file("defs.bzl", """
load(
    "@rules_fuchsia//fuchsia/private/workflows:fuchsia_product_bundle_tasks.bzl",
    "fuchsia_product_bundle_tasks",
    "product_bundles_help_executable",
)
load("@rules_fuchsia//fuchsia/private/workflows:providers.bzl", "FuchsiaProductBundleInfo")

def _fuchsia_remote_product_bundle_impl(ctx):
    return [
        DefaultInfo(
            executable = product_bundles_help_executable(ctx, is_remote = True),
        ),
        FuchsiaProductBundleInfo(
            is_remote = True,
            product_bundle = ctx.attr.transfer_url,
            product_bundle_name = ctx.attr.product_bundle_name,
        ),
    ]

_fuchsia_remote_product_bundle = rule(
    implementation = _fuchsia_remote_product_bundle_impl,
    attrs = {
        "transfer_url": attr.string(mandatory = True),
        "product_bundle_name": attr.string(mandatory = True),
    },
    executable = True,
)

def fuchsia_remote_product_bundle(*, name, transfer_url, product_bundle_name = None, **kwargs):
    product_bundle_name = product_bundle_name or name
    _fuchsia_remote_product_bundle(
        name = name,
        transfer_url = transfer_url,
        product_bundle_name = product_bundle_name,
        **kwargs
    )
    fuchsia_product_bundle_tasks(
        name = name + "_tasks",
        product_bundle = name,
        is_remote = True,
        **kwargs
    )
""")
    ctx.file("BUILD.bazel", """# DO NOT MODIFY.
load(":defs.bzl", "fuchsia_remote_product_bundle")

""" + "\n\n".join([
        """
fuchsia_remote_product_bundle(
    name = "%s",
    transfer_url = "%s",
    visibility = ["//visibility:public"],
)
""".strip() % (pb["name"], pb["transfer_manifest_url"])
        for pb in product_bundles_metadata
    ]))

fuchsia_products_repository = repository_rule(
    implementation = _fuchsia_products_repository_impl,
    attrs = {
        "metadata_file": attr.label(
            allow_single_file = True,
        ),
    },
)
