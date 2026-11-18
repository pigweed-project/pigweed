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
"""Tooling to generate C++ constants from a yaml sensor definition."""

import argparse
import importlib.resources
import re
import sys
import typing
from collections.abc import Sequence
from dataclasses import dataclass, fields, is_dataclass
from typing import Any
import types

import jinja2
import yaml


def kid_from_name(name: str) -> str:
    """Generate a const style ID name from a given name string.

    Example:
      If name is "sample_rate", the ID would be kSampleRate

    Args:
      name: the name to convert to an ID

    Returns:
      C++ style 'k' prefixed camel cased ID

    """
    return "k" + "".join(ele.title() for ele in re.split(r"[\s_\-\,]+", name))


@dataclass
class UnitsSpec:
    """Typing for the Units definition dictionary."""

    name: str
    symbol: str


@dataclass
class AttributeSpec:
    """Typing for the Attribute definition dictionary."""

    name: str
    description: str


@dataclass
class ChannelSpec:
    """Typing for the Channel definition dictionary."""

    name: str
    description: str
    units: str


@dataclass
class TriggerSpec:
    """Typing for the Trigger definition dictionary."""

    name: str
    description: str


@dataclass
class SensorAttributeSpec:
    """Typing for the SensorAttribute definition dictionary."""

    channel: str | None
    trigger: str | None
    attribute: str
    units: str


@dataclass
class CompatibleSpec:
    """Typing for the Compatible dictionary."""

    org: str | None
    part: str


@dataclass
class SensorSpec:
    """Typing for the Sensor definition dictionary."""

    description: str
    compatible: CompatibleSpec
    supported_buses: list[str]
    attributes: list[SensorAttributeSpec]
    channels: dict[str, list[ChannelSpec]]
    triggers: list[Any]
    extras: dict[str, Any]


@dataclass
class Args:
    """CLI arguments"""

    package: Sequence[str]
    language: str
    zephyr: bool


@dataclass
class InputSpec:
    """Typing for the InputData spec dictionary"""

    units: dict[str, UnitsSpec]
    attributes: dict[str, AttributeSpec]
    channels: dict[str, ChannelSpec]
    triggers: dict[str, TriggerSpec]
    sensors: dict[str, SensorSpec]


def is_list_type(t: Any) -> bool:  # noqa: ANN401
    """
    Checks if the given type `t` is a list.

    Args:
        t: The type to check.

    Returns:
        True if `t` is a list type, False otherwise.

    """
    origin = typing.get_origin(t)
    return origin is list or (origin is list and typing.get_args(t) == ())


def is_primitive(value: Any) -> bool:  # noqa: ANN401
    """Checks if the given value is of a primitive type.

    Args:
        value: The value to check.

    Returns:
        True if the value is of a primitive type, False otherwise.

    """
    return isinstance(value, int | float | complex | str | bool)


def is_union(t: Any) -> bool:  # noqa: ANN401
    """Check if the given type is a union

    Args:
        t: The type to check.

    Returns:
        True if `t` is a union type, False otherwise.

    """
    return (
        typing.get_origin(t) is typing.Union
        or typing.get_origin(t) is types.UnionType
    )


def create_dataclass_from_dict(
    cls: Any,
    data: Any,
    indent: int = 0,  # noqa: ANN401
) -> Any:  # noqa: ANN401
    """Recursively creates a dataclass instance from a nested dictionary."""
    field_values: dict[str, Any] = {}

    if is_list_type(cls):
        result = []
        for item in data:
            result.append(  # noqa: PERF401
                create_dataclass_from_dict(
                    typing.get_args(cls)[0], item, indent + 2
                )
            )
        return result

    if is_primitive(data):
        return data

    for field in fields(cls):
        field_value = data.get(field.name)
        if field_value is None:
            field_value = data.get(field.name.replace("_", "-"))

        if (
            is_union(field.type)
            and type(None) in typing.get_args(field.type)
            and field_value is None
        ):
            # We have an optional field and no value, skip it
            field_values[field.name] = None
            continue

        assert field_value is not None

        # We need to check if the field is a List, dictionary, or another
        # dataclass. If it is, recurse.
        if is_list_type(field.type):
            item_type = typing.get_args(field.type)[0]
            field_value = [
                create_dataclass_from_dict(item_type, item, indent + 2)
                for item in field_value
            ]
        elif dict in getattr(field.type, "__mro__", []):
            # We might not have types specified in the dataclass
            value_types = typing.get_args(field.type)
            if len(value_types) != 0:
                value_type = value_types[1]
                field_value = {
                    key: create_dataclass_from_dict(value_type, val, indent + 2)
                    for key, val in field_value.items()
                }
        elif is_dataclass(field.type):
            field_value = create_dataclass_from_dict(
                field.type, field_value, indent + 2
            )

        field_values[field.name] = field_value

    return cls(**field_values)


def main() -> None:
    """Main entry point

    This function will:
    - Get CLI flags
    - Read YAML from stdin
    - Find all attribute, channel, trigger, and unit definitions
    - Print header
    """
    args = get_args()
    yaml_input = yaml.safe_load(sys.stdin)
    spec: InputSpec = create_dataclass_from_dict(InputSpec, yaml_input)

    jinja_templates = {
        resource.name: resource.read_text()
        for resource in importlib.resources.files(
            "pw_sensor.templates"
        ).iterdir()
        if resource.is_file() and resource.name.endswith('.jinja')
    }
    environment = jinja2.Environment(
        loader=jinja2.DictLoader(jinja_templates),
        autoescape=True,
        # Trim whitespace in templates
        trim_blocks=True,
        lstrip_blocks=True,
    )
    environment.globals["kid_from_name"] = kid_from_name  # type: ignore[index]

    if args.language == "cpp":
        template = environment.get_template("cpp_constants.jinja")
        out = template.render(
            {
                "spec": spec,
                "package_name": "::".join(args.package),
            }
        )
    else:
        error = f"Invalid language selected: '{args.language}'"
        raise ValueError(error)

    sys.stdout.write(out)


def validate_package_arg(value: str) -> str:
    """
    Validate that the package argument is a valid string

    Args:
      value: The package name

    Returns:
      The same value after being validated.

    """
    if value is None or value == "":
        return value
    if not re.match(r"[a-zA-Z_$][\w$]*(\.[a-zA-Z_$][\w$]*)*", value):
        raise argparse.ArgumentError(
            argument=None,
            message=f"Invalid string {value}. Must use alphanumeric values "
            "separated by dots.",
        )
    return value


def get_args() -> Args:
    """
    Get CLI arguments

    Returns:
      Typed arguments class instance

    """
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--package",
        "-pkg",
        default="",
        type=validate_package_arg,
        help="Output package name separated by '.', example: com.google",
    )
    parser.add_argument(
        "--language",
        type=str,
        choices=["cpp"],
        default="cpp",
    )
    parser.add_argument(
        "--zephyr",
        action="store_true",
    )
    args = parser.parse_args()
    return Args(
        package=args.package.split("."),
        language=args.language,
        zephyr=args.zephyr,
    )


if __name__ == "__main__":
    main()
