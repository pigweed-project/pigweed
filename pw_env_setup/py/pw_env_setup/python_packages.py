#!/usr/bin/env python

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
"""Save list of installed packages and versions."""

import argparse
import importlib.metadata
import itertools
import json
from pathlib import Path
import re
import sys
from typing import Iterator

from packaging.requirements import Requirement
from packaging.utils import canonicalize_name
from packaging.version import Version


def _as_requirement(dist: importlib.metadata.Distribution) -> Requirement:
    """Convert a Distribution into a pinned Requirement."""
    return Requirement(
        f'{canonicalize_name(dist.name)}=={Version(dist.version)}'
    )


def _strip_extras_and_markers(req: Requirement) -> Requirement:
    """Return a Requirement with only the canonicalized name and specifier."""
    return Requirement(f'{canonicalize_name(req.name)}{req.specifier}')


def _is_dist_info(dist: importlib.metadata.Distribution) -> bool:
    """Return True if the distribution is a non-editable .dist-info install."""
    path = getattr(dist, '_path', None)
    if path is not None and not str(path).endswith('.dist-info'):
        return False
    direct_url = dist.read_text('direct_url.json')
    if direct_url:
        if (json.loads(direct_url).get('dir_info') or {}).get('editable'):
            return False
    return True


def _working_set() -> dict[str, importlib.metadata.Distribution]:
    """Return the first Distribution per canonicalized package name."""
    dists: dict[str, importlib.metadata.Distribution] = {}
    for dist in importlib.metadata.distributions():
        dists.setdefault(canonicalize_name(dist.name), dist)
    return dists


def _installed_packages() -> Iterator[str]:
    """Run pip python_packages and write to out."""
    installed_packages = list(
        _as_requirement(pkg)
        for pkg in _working_set().values()
        # Non-editable packages only
        if _is_dist_info(pkg)
        # This will skip packages with local versions.
        #   For example text after a plus sign: 1.2.3+dev456
        and not Version(pkg.version).local
        # These are always installed by default in:
        #   pw_env_setup/py/pw_env_setup/virtualenv_setup/install.py
        and canonicalize_name(pkg.name) not in ['pip', 'setuptools', 'wheel']
    )
    for req in sorted(installed_packages, key=lambda pkg: pkg.name.lower()):
        yield str(req)


def ls(output_file: Path | None) -> int:  # pylint: disable=invalid-name
    """Run pip python_packages and write to output_file."""
    actual_requirements = frozenset(
        Requirement(line) for line in _installed_packages()
    )
    missing_requirements = set()

    # If updating an existing file, load the existing requirements to find lines
    # that are missing in the active environment.
    if output_file:
        expected_requirements = set(
            Requirement(line) for line in _load_requirements_lines(output_file)
        )
        missing_requirements = expected_requirements - actual_requirements

    new_requirements: list[Requirement] = list(actual_requirements)

    working_set = _working_set()
    for requirement in missing_requirements:
        # Preserve this requirement if it has a marker that doesn't apply to
        # the current environment. For example a line with ;python_version <
        # "3.9" if running on Python 3.9 or higher will be saved.
        if requirement.marker and not requirement.marker.evaluate():
            new_requirements.append(requirement)
            continue

        # If this package is in the active environment then it has a marker
        # in the existing file that should be preserved.
        found_package = working_set.get(canonicalize_name(requirement.name))
        found_req = _as_requirement(found_package) if found_package else None
        # If the package version doesn't match, save the new version.
        if (
            found_package
            and found_req in new_requirements
            and requirement.specifier.contains(
                found_package.version, prereleases=True
            )
        ):
            # Delete the old line with no marker.
            new_requirements.remove(found_req)
            # Add the existing requirement line that includes the marker.
            new_requirements.append(requirement)

    out = output_file.open('w', encoding='utf-8') if output_file else sys.stdout

    for package in sorted(new_requirements, key=lambda pkg: pkg.name.lower()):
        print(package, file=out)

    if output_file:
        out.close()
    return 0


class UpdateRequiredError(Exception):
    pass


def _stderr(*args, **kwargs):
    return print(*args, file=sys.stderr, **kwargs)


def _load_requirements_lines(
    *req_files: Path,
    follow_constraints: bool = False,
) -> Iterator[str]:
    """Yield normalized requirement lines from one or more requirement files."""
    for req_file in req_files:
        for line in req_file.read_text(encoding='utf-8').splitlines():
            # Strip comments.
            line = re.sub(r'(^|\s+)#.*$', '', line)
            # Remove surrounding whitespace and trailing backslashes.
            line = line.strip().rstrip('\\').strip()
            # Other constraint files.
            if line.startswith('-c'):
                if follow_constraints:
                    constraint_path = req_file.parent / line[2:].strip()
                    if constraint_path.is_file():
                        yield from _load_requirements_lines(
                            constraint_path,
                            follow_constraints=True,
                        )
                continue
            # Ignore blank lines and hash lines.
            if not line or line.startswith('--hash='):
                continue
            yield line


def _matches(
    req_name: str, req_version: str, req_set: frozenset[Requirement]
) -> bool:
    """Return True if (req_name, req_version) matches any req in req_set."""
    canon = canonicalize_name(req_name)
    v = Version(req_version)
    for r in req_set:
        if canonicalize_name(r.name) == canon:
            if not r.specifier or r.specifier.contains(v, prereleases=True):
                return True
    return False


def _get_ignored_requirements(
    expected_requirements: frozenset[Requirement],
    ignored_lines: set[str],
) -> tuple[frozenset[Requirement], set[Requirement]]:
    """Return direct and transitive ignored requirements."""
    ignored_raw_requirements = [
        req
        for line in ignored_lines
        if not (req := Requirement(line)).marker or req.marker.evaluate()
    ]
    ignored_requirements = frozenset(
        _strip_extras_and_markers(req) for req in ignored_raw_requirements
    )
    ignored_extras: dict[str, set[str]] = {}
    for req in ignored_raw_requirements:
        ignored_extras.setdefault(canonicalize_name(req.name), {''}).update(
            req.extras
        )

    working_set = _working_set()
    ignored_distributions = list(
        distribution
        for distribution in working_set.values()
        if _matches(
            distribution.name, distribution.version, ignored_requirements
        )
    )
    expected_distributions = list(
        distribution
        for distribution in working_set.values()
        if _matches(
            distribution.name, distribution.version, expected_requirements
        )
    )

    def get_requirements(
        dist_info: importlib.metadata.Distribution,
    ) -> Iterator[importlib.metadata.Distribution]:
        """Return requirement that are not in expected_distributions."""
        extras = ignored_extras.get(canonicalize_name(dist_info.name), {''})
        for req_str in dist_info.requires or []:
            req = Requirement(req_str)
            if req.marker and not any(
                req.marker.evaluate({'extra': extra}) for extra in extras
            ):
                continue
            req_dist_info = working_set.get(canonicalize_name(req.name))
            if not req_dist_info or not req.specifier.contains(
                req_dist_info.version, prereleases=True
            ):
                continue
            if req_dist_info in expected_distributions:
                continue
            yield req_dist_info

    visited: set[str] = {
        canonicalize_name(dist.name) for dist in ignored_distributions
    }

    def expand_requirements(
        reqs: list[importlib.metadata.Distribution],
    ) -> Iterator[list[importlib.metadata.Distribution]]:
        """Recursively expand requirements."""
        for dist_info in reqs:
            deps = []
            for dep in get_requirements(dist_info):
                canon = canonicalize_name(dep.name)
                if canon not in visited:
                    visited.add(canon)
                    deps.append(dep)
            if deps:
                yield deps
                yield from expand_requirements(deps)

    ignored_transitive_deps = set(
        _as_requirement(dist)
        for dist in itertools.chain.from_iterable(
            expand_requirements(ignored_distributions)
        )
    )
    return ignored_requirements, ignored_transitive_deps


def diff(
    expected: Path, ignore_requirements_file: list[Path] | None = None
) -> int:
    """Report on differences between installed and expected versions."""
    actual_lines = set(_installed_packages())
    expected_lines = set(
        _load_requirements_lines(expected, follow_constraints=True)
    )
    ignored_lines = set()
    if ignore_requirements_file:
        ignored_lines = set(
            _load_requirements_lines(
                *ignore_requirements_file, follow_constraints=True
            )
        )

    if actual_lines == expected_lines:
        _stderr('package versions are identical')
        return 0

    actual_requirements = frozenset(Requirement(line) for line in actual_lines)
    expected_requirements = frozenset(
        _strip_extras_and_markers(req)
        for line in expected_lines
        if not (req := Requirement(line)).marker or req.marker.evaluate()
    )
    ignored_requirements, ignored_transitive_deps = _get_ignored_requirements(
        expected_requirements, ignored_lines
    )

    removed_requirements = expected_requirements - actual_requirements
    added_requirements = actual_requirements - expected_requirements

    removed_packages: dict[Requirement, str] = {}
    updated_packages: dict[Requirement, str] = {}
    new_packages: dict[Requirement, str] = {}
    reformatted_packages: dict[Requirement, str] = {}

    actual_requirements_by_req = {req: req for req in actual_requirements}
    working_set = _working_set()

    for line in expected_lines:
        # pip-tools adds a space before requirement specifier semicolons. E.g.
        #   tomli==2.0.1 ; python_version < "3.11"
        # instead of:
        #   tomli==2.0.1; python_version < "3.11"
        # Remove that extra space before checking anything else.
        line = re.sub(r'\s+;', ';', line)

        requirement = Requirement(line)

        # Check for lines that need reformatting
        # This will catch lines that use underscores instead of dashes in the
        # name of missing spaces after specifiers.

        # Match this requirement with the original one found in
        # actual_requirements.
        #
        # Note the requirement variable may equal its counterpart in
        # actual_requirements due to the __hash__ and __eq__ on the Requirement
        # class checking their normalized name. Since we are looking for
        # formatting mismatches here we need to retrieve the requirement
        # instance from the actual_requirements set.
        matching_found_requirement = actual_requirements_by_req.get(requirement)

        # If the actual requirement line doesn't match.
        if (
            matching_found_requirement
            and str(matching_found_requirement) != line
        ):
            reformatted_packages[matching_found_requirement] = line

        # If this requirement isn't in the active enviroment and the line
        # doesn't match the repr: flag for reformatting.
        if not matching_found_requirement and (
            str(requirement) != line
            or requirement.name != canonicalize_name(requirement.name)
        ):
            requirement.name = canonicalize_name(requirement.name)
            reformatted_packages[requirement] = line

        # If a requirement specifier is used and it doesn't apply, skip this
        # line. See details for requirement specifiers at:
        # https://pip.pypa.io/en/stable/reference/requirement-specifiers/#requirement-specifiers
        if requirement.marker and not requirement.marker.evaluate():
            continue

        # Try to find this requirement in the current environment.
        found_package = working_set.get(canonicalize_name(requirement.name))

        # If this requirement isn't in the environment, it was removed.
        if not found_package:
            removed_packages[requirement] = line
            continue

        found_requirement = _as_requirement(found_package)
        # If found_package is set, the version doesn't match so it was updated.
        if requirement.specifier != found_requirement.specifier:
            updated_packages[found_requirement] = line

    # Check for new packages
    for requirement in added_requirements - removed_requirements:
        if requirement in updated_packages:
            continue
        found_package = working_set.get(canonicalize_name(requirement.name))
        if (
            (
                found_package
                and _matches(
                    found_package.name,
                    found_package.version,
                    ignored_requirements,
                )
            )
            or requirement in ignored_requirements
            or requirement in ignored_transitive_deps
        ):
            continue

        new_packages[requirement] = str(requirement)

    # Print status messages to stderr

    if reformatted_packages:
        _stderr('Requirements that need reformatting:')
        for requirement, line in reformatted_packages.items():
            _stderr(f'  "{line}"')
            _stderr('  should be:')
            _stderr(f'  "{str(requirement)}"')

    if updated_packages:
        _stderr('Updated packages')
        for requirement, line in updated_packages.items():
            _stderr(f'  {str(requirement)} (from {line})')

    if removed_packages:
        _stderr('Removed packages')
        for requirement in removed_packages:
            _stderr(f'  {requirement}')

    if new_packages:
        _stderr('New packages')
        for requirement in new_packages:
            _stderr(f'  {requirement}')

    if updated_packages or new_packages:
        _stderr("Package versions don't match!")
        _stderr(
            f"""
Please do the following:

* purge your environment directory
  * Linux/Mac: 'rm -rf "$_PW_ACTUAL_ENVIRONMENT_ROOT"'
  * Windows: 'rmdir /S %_PW_ACTUAL_ENVIRONMENT_ROOT%'
* bootstrap
  * Linux/Mac: '. ./bootstrap.sh'
  * Windows: 'bootstrap.bat'
* update the constraint file
  * 'pw python-packages list {expected.name}'
"""
        )
        return -1

    return 0


def parse(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        prog="python -m pw_env_setup.python_packages"
    )
    subparsers = parser.add_subparsers(dest='cmd')

    list_parser = subparsers.add_parser(
        'list', aliases=('ls',), help='List installed package versions.'
    )
    list_parser.add_argument('output_file', type=Path, nargs='?')

    diff_parser = subparsers.add_parser(
        'diff',
        help='Show differences between expected and actual package versions.',
    )
    diff_parser.add_argument('expected', type=Path)
    diff_parser.add_argument(
        '--ignore-requirements-file', type=Path, action='append'
    )

    return parser.parse_args(argv)


def main() -> int:
    args = vars(parse())
    cmd = args.pop('cmd')
    if cmd == 'diff':
        return diff(**args)
    if cmd in ('list', 'ls'):
        return ls(**args)
    return -1


if __name__ == '__main__':
    sys.exit(main())
