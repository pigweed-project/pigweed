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
"""Merging of mix-in build configs for Workflows configurations."""

from collections.abc import Sequence

from google.protobuf import descriptor_pool, message_factory
from pw_build.proto import workflows_pb2


def driver_options_type_name(config: workflows_pb2.BuildConfig) -> str:
    """Returns the bare message type name of a config's driver options."""
    return config.driver_options.type_url.split('/')[-1]


def _merge_driver_options(
    result: workflows_pb2.BuildConfig,
    mixin: workflows_pb2.BuildConfig,
) -> None:
    """Merges a mix-in's driver options into ``result`` in place.

    Driver options count as set when their type is set, even if every field
    has its default value (which leaves ``value`` empty).
    """
    if not mixin.driver_options.type_url:
        return
    if not result.driver_options.type_url:
        result.driver_options.CopyFrom(mixin.driver_options)
        return

    type_name = driver_options_type_name(result)
    mixin_type_name = driver_options_type_name(mixin)
    if type_name != mixin_type_name:
        raise ValueError(
            f'Cannot apply mixin `{mixin.name}` to `{result.name}`: '
            f'driver_options type `{mixin_type_name}` does not match '
            f'`{type_name}`'
        )
    if not result.driver_options.value:
        result.driver_options.value = mixin.driver_options.value
        return
    try:
        descriptor = descriptor_pool.Default().FindMessageTypeByName(type_name)
    except KeyError as e:
        raise ValueError(
            f'Cannot apply mixin `{mixin.name}` to `{result.name}`: '
            f'driver_options type `{type_name}` is not registered'
        ) from e
    message_class = message_factory.GetMessageClass(descriptor)
    merged = message_class()
    merged.ParseFromString(result.driver_options.value)
    mixin_options = message_class()
    mixin_options.ParseFromString(mixin.driver_options.value)
    merged.MergeFrom(mixin_options)
    result.driver_options.value = merged.SerializeToString()


def merge_build_configs(
    base: workflows_pb2.BuildConfig,
    mixins: Sequence[workflows_pb2.BuildConfig],
) -> workflows_pb2.BuildConfig:
    """Applies mix-in build configs on top of a base build config.

    The Validator calls this for every build and tool that lists mix-ins, so
    the ValueErrors raised here are reported when a configuration is loaded.

    Mix-ins are applied in order to a copy of ``base``:

    * ``args`` are appended.
    * ``env`` values override the accumulated values key by key.
    * ``driver_options`` are unpacked and merged with Protobuf ``MergeFrom``
      semantics.
    * ``build_type``, if set on a mix-in, must match the base.

    Args:
        base: The base build configuration. Not modified.
        mixins: Mix-in build configurations, applied in order. Not modified.

    Returns:
        A new BuildConfig named ``<base>+<mixin1>+<mixin2>...``.

    Raises:
        ValueError: A mix-in's ``build_type`` or ``driver_options`` type is
            incompatible with the base.
    """
    result = workflows_pb2.BuildConfig()
    result.CopyFrom(base)
    result.mixin = False
    for mixin in mixins:
        if mixin.build_type and mixin.build_type != base.build_type:
            raise ValueError(
                f'Cannot apply mixin `{mixin.name}` to `{base.name}`: '
                f'build_type `{mixin.build_type}` does not match base '
                f'build_type `{base.build_type}`'
            )
        result.args.extend(mixin.args)
        for key, value in mixin.env.items():
            result.env[key] = value
        _merge_driver_options(result, mixin)
    result.name = '+'.join([base.name, *(mixin.name for mixin in mixins)])
    return result
