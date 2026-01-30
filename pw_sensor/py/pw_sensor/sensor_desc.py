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
"""CLI to describe a yaml sensor definition."""

import argparse
from collections.abc import Sequence
from dataclasses import dataclass
import importlib.resources
import logging
from pathlib import Path
import subprocess
import sys

import jsonschema  # type: ignore
import jsonschema.exceptions  # type: ignore
from pw_sensor.validator import Validator
import yaml

logging.basicConfig(level=logging.DEBUG)
_LOG = logging.getLogger("sensor-describe")

_OUTPUT_SCHEMA = yaml.safe_load(
    importlib.resources.files("pw_sensor")
    .joinpath("resolved_schema.json")
    .read_text()
)


@dataclass
class Args:
    """Strongly typed wrapper around the arguments provided"""

    include_paths: Sequence[Path]
    descriptor_paths: Sequence[Path]
    generator_command: str | None
    output_file: Path | None
    log_level: int = logging.WARNING


def get_args() -> Args:
    """
    Setup the argument parser, parse the args, and return a dataclass with the
    arguments.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--include-path",
        "-I",
        action="append",
        type=lambda p: Path(p).resolve(),
        required=True,
        help="Directories in which to search for dependency files",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="count",
        default=0,
        help="Increase verbosity level (can be used multiple times)",
    )
    parser.add_argument(
        "--generator",
        "-g",
        type=str,
        help="Generator command to run along with any flags. Data will be "
        "passed into the generator as YAML through stdin",
    )
    parser.add_argument(
        "-o",
        dest="output",
        type=Path,
        help="Write output to file instead of standard out",
    )
    parser.add_argument(
        "descriptors",
        nargs="*",
        type=lambda p: Path(p).resolve(),
        help="One or more files to validate",
    )

    args = parser.parse_args()
    if args.verbose == 0:
        log_level = logging.WARNING
    elif args.verbose == 1:
        log_level = logging.INFO
    else:
        log_level = logging.DEBUG
    return Args(
        include_paths=args.include_path,
        descriptor_paths=args.descriptors,
        generator_command=args.generator,
        output_file=args.output,
        log_level=log_level,
    )


def main() -> None:
    """
    Main entry point to the CLI. After parsing the arguments for the below
    parameters, the utility will validate the descriptor files and pass the
    output to the generator:
    - include paths
    - verbosity
    - generator
    - descriptor files
    """
    args = get_args()
    _LOG.setLevel(level=args.log_level)

    validator = Validator(
        include_paths=args.include_paths, log_level=args.log_level
    )
    superset: dict = {
        "attributes": {},
        "channels": {},
        "triggers": {},
        "units": {},
        "sensors": {},
    }
    for descriptor_file in args.descriptor_paths:
        _LOG.info("Loading '%s'", descriptor_file)
        if not descriptor_file.is_file():
            raise RuntimeError(f"'{descriptor_file}' is not a file")
        with open(descriptor_file, mode="r", encoding="utf-8") as stream:
            content = yaml.safe_load(stream=stream)
            _LOG.debug("Validating:\n%s", yaml.safe_dump(content, indent=2))
            content = validator.validate(content)
            _LOG.debug("Result:\n%s", yaml.safe_dump(content, indent=2))
        # Add sensor
        for sensor_id, values in content["sensors"].items():
            assert superset["sensors"].get(sensor_id) is None
            superset["sensors"][sensor_id] = values
        # Add channels
        for chan_id, chan_spec in content["channels"].items():
            conflict = superset["channels"].get(chan_id)
            assert conflict is None or conflict == chan_spec
            superset["channels"][chan_id] = chan_spec
        # Add attributes
        for attr_id, attr_spec in content["attributes"].items():
            conflict = superset["attributes"].get(attr_id)
            assert conflict is None or conflict == attr_spec
            superset["attributes"][attr_id] = attr_spec
        # Add triggers
        for trig_id, trig_spec in content["triggers"].items():
            conflict = superset["triggers"].get(trig_id)
            assert conflict is None or conflict == trig_spec
            superset["triggers"][trig_id] = trig_spec
        # Add units
        for units_id, units_spec in content["units"].items():
            conflict = superset["units"].get(units_id)
            assert conflict is None or conflict == units_spec
            superset["units"][units_id] = units_spec

    _LOG.debug("Final descriptor:\n%s", yaml.safe_dump(superset, indent=2))
    _LOG.info("Validating...")
    try:
        jsonschema.validate(instance=superset, schema=_OUTPUT_SCHEMA)
    except jsonschema.exceptions.ValidationError as e:
        raise RuntimeError(
            "ERROR: Malformed merged output:\n"
            f"{yaml.safe_dump(superset, indent=2)}"
        ) from e
    content_string = yaml.safe_dump(superset)

    if args.generator_command:
        cmd = args.generator_command.split(sep=" ")
        _LOG.info("Running generator %s", cmd)

        with subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ) as process:
            assert process.stdin is not None
            process.stdin.write(content_string.encode("utf-8"))
            out, err = process.communicate()

        if out:
            if args.output_file:
                args.output_file.parent.mkdir(parents=True, exist_ok=True)
                with open(args.output_file, mode="w", encoding="utf-8") as o:
                    o.write(out.decode("utf-8"))
            else:
                print(out.decode("utf-8"))
        if err:
            _LOG.error(err.decode("utf-8"))
        if process.returncode != 0:
            sys.exit(-1)
    else:
        print(content_string)


if __name__ == '__main__':
    main()
