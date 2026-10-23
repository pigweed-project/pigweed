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
"""Test runner wrapper that configures warnings from pyproject.toml."""

import os
from pathlib import Path
import runpy
import sys
import warnings

try:
    import tomllib  # type: ignore
except ImportError:
    try:
        import tomli as tomllib  # type: ignore
    except ImportError:
        tomllib = None  # type: ignore


def _load_pyproject_filterwarnings(pyproject_path: Path) -> list[str] | None:
    if tomllib is None or not pyproject_path.is_file():
        return None
    data = tomllib.loads(pyproject_path.read_text(encoding='utf-8'))
    filters = (
        data.get('tool', {})
        .get('pytest', {})
        .get('ini_options', {})
        .get('filterwarnings')
    )
    if filters is None:
        return None
    if not isinstance(filters, list):
        raise ValueError(
            f'Expected filterwarnings in {pyproject_path} to be a list, '
            f'got {type(filters).__name__}'
        )
    return [str(item) for item in filters]


def _find_filterwarnings(main_script: Path) -> list[str]:
    candidates: list[Path] = []
    for start_path in (
        Path(os.path.abspath(main_script)).parent,
        main_script.resolve().parent,
        Path.cwd().resolve(),
    ):
        for parent in (start_path, *start_path.parents):
            candidate = parent / 'pyproject.toml'
            if candidate not in candidates:
                candidates.append(candidate)

    for candidate in candidates:
        filters = _load_pyproject_filterwarnings(candidate)
        if filters is not None:
            return filters

    return []


def _apply_warning_filter(filter_spec: str) -> None:
    parts = [s.strip() for s in filter_spec.split(':')]
    if len(parts) > 5:
        raise ValueError(
            f'Invalid warning filter specification (too many fields): '
            f'{filter_spec!r}'
        )
    while len(parts) < 5:
        parts.append('')

    action_str, message, category_str, module, lineno_str = parts
    get_action = getattr(warnings, '_getaction', None)
    get_category = getattr(warnings, '_getcategory', None)
    action = (
        get_action(action_str)
        if callable(get_action)
        else (action_str or 'default')
    )
    category = (
        get_category(category_str)
        if callable(get_category)
        else (
            DeprecationWarning
            if category_str == 'DeprecationWarning'
            else Warning
        )
    )
    lineno = int(lineno_str) if lineno_str else 0

    warnings.filterwarnings(
        action,  # type: ignore[arg-type]
        message=message,
        category=category,
        module=module,
        lineno=lineno,
    )


def _configure_test_warnings(main_script: Path) -> None:
    filters = _find_filterwarnings(main_script)
    if not filters:
        return

    for filter_spec in filters:
        _apply_warning_filter(filter_spec)

    # Prevent unittest.main() from resetting warning filters when
    # sys.warnoptions is empty. Do not append raw pytest filter_spec strings to
    # sys.warnoptions, as multiprocessing passes sys.warnoptions to child
    # Python processes via -W flags (which do not support regexes).
    if not sys.warnoptions:
        sys.warnoptions.append('default')


def main() -> None:
    main_script_str = os.environ.pop('PW_PY_TEST_MAIN', None)
    if main_script_str:
        main_script = Path(os.path.abspath(main_script_str))
    elif len(sys.argv) > 1:
        main_script = Path(os.path.abspath(sys.argv.pop(1)))
    else:
        raise RuntimeError(
            'No test script specified via PW_PY_TEST_MAIN environment '
            'variable or command-line argument'
        )

    _configure_test_warnings(main_script)

    sys.argv[0] = str(main_script)
    script_dir = str(main_script.parent)
    runner_dirs = {
        str(Path(os.path.abspath(__file__)).parent),
        str(Path(__file__).resolve().parent),
    }
    sys.path[:] = [p for p in sys.path if p not in runner_dirs]
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)

    runpy.run_path(str(main_script), run_name='__main__')


if __name__ == '__main__':
    main()
