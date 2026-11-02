# Copyright 2022 The Pigweed Authors
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
"""Configure Visual Studio Code (VSC) for Pigweed projects.

VSC recognizes three sources of configurable settings:

1. Project settings, stored in {project root}/.vscode/settings.json
2. Workspace settings, stored in (workspace root)/.vscode/settings.json;
   a workspace is a collection of projects/repositories that are worked on
   together in a single VSC instance
3. The user's personal settings, which are stored somewhere in the user's home
   directory, and are applied to all instances of VSC

This provides three levels of settings cascading:

    Workspace <- Project <- User

... where the arrow indicates the ability to override.

Out of these three, only project settings are useful to Pigweed projects. User
settings are essentially global and outside the scope of Pigweed. Workspaces are
seldom used and don't work well with the Pigweed directory structure.

Nonetheless, we want a three-tiered settings structure for Pigweed projects too:

A. Default settings provided by Pigweed, configuring VSC to use IDE features
B. Project-level overrides that downstream projects may define
C. User-level overrides that individual users may define

We accomplish all of that with only the project settings described in #1 above.

Default settings are defined in this module. Project settings can be defined in
.vscode/pw_project_settings.json and should be checked into the repository. User
settings can be defined in .vscode/pw_user_settings.json and should not be
checked into the repository. None of these settings have any effect until they
are merged into VSC's settings (.vscode/settings.json) via the functions in this
module. Those resulting settings are system-specific and should also not be
checked into the repository.

We provide the same structure to both tasks and extensions as well. Defaults
are provided by Pigweed, can be augmented or overridden at the project level
with .vscode/pw_project_tasks.json and .vscode/pw_project_extensions.json,
can be augmented or overridden by an individual developer with
.vscode/pw_user_tasks.json and .vscode/pw_user.extensions.json, and none of
this takes effect until they are merged into VSC's active settings files
(.vscode/tasks.json and .vscode/extensions.json) by running the appropriate
command.
"""

from __future__ import annotations

from collections import OrderedDict
from enum import Enum
from pathlib import Path
from typing import Any, Callable

from pw_cli.env import pigweed_environment

from pw_ide.cpp import ClangdSettings, CppIdeFeaturesState

from pw_ide.editors import (
    EditorSettingsDict,
    EditorSettingsManager,
    EditorSettingsTypesWithDefaults,
    Json5FileFormat,
)

from pw_ide.python import PythonPaths
from pw_ide.settings import PigweedIdeSettings

env = pigweed_environment()


def _local_clangd_settings(ide_settings: PigweedIdeSettings) -> dict[str, Any]:
    """VSC settings for running clangd with Pigweed paths."""

    try:
        clangd_settings = ClangdSettings(ide_settings)

        return {
            'clangd.path': str(clangd_settings.clangd_path),
            'clangd.arguments': clangd_settings.arguments,
        }
    except FileNotFoundError:
        return {}


def _local_python_settings() -> dict[str, Any]:
    """VSC settings for finding the Python virtualenv."""
    paths = PythonPaths()
    return {
        'python.defaultInterpreterPath': str(paths.interpreter),
    }


# The order is preserved despite starting with a plain dict because in Python
# 3.6+, plain dicts are actually ordered as an implementation detail. This could
# break on interpreters other than CPython, or if the implementation changes in
# the future. However, for now, this is much more readable than the more robust
# alternative of instantiating with a list of tuples.
_DEFAULT_SETTINGS: EditorSettingsDict = OrderedDict(
    {
        "cmake.format.allowOptionalArgumentIndentation": True,
        "editor.detectIndentation": False,
        "editor.rulers": [80],
        "editor.tabSize": 2,
        "files.associations": OrderedDict({"*.inc": "cpp"}),
        "files.exclude": OrderedDict(
            {
                "**/*.egg-info": True,
                "**/.mypy_cache": True,
                "**/__pycache__": True,
                ".cache": True,
                ".cipd": True,
                ".environment": True,
                ".presubmit": True,
                ".pw_ide": True,
                ".pw_ide.user.yaml": True,
                "bazel-bin": True,
                "bazel-out": True,
                "bazel-pigweed": True,
                "bazel-testlogs": True,
                "environment": True,
                "node_modules": True,
                "out": True,
            }
        ),
        "files.insertFinalNewline": True,
        "files.trimTrailingWhitespace": True,
        "search.useGlobalIgnoreFiles": True,
        "npm.autoDetect": "off",
        "C_Cpp.intelliSenseEngine": "disabled",
        "[cpp]": OrderedDict(
            {"editor.defaultFormatter": "llvm-vs-code-extensions.vscode-clangd"}
        ),
        "python.analysis.diagnosticSeverityOverrides": OrderedDict(
            # Due to our project structure, the linter spuriously thinks we're
            # shadowing system modules any time we import them. This disables
            # that check.
            {"reportShadowedImports": "none"}
        ),
        # The "strict" mode is much more strict than what we currently enforce.
        "python.analysis.typeCheckingMode": "basic",
        # Restrict the search for Python files to the locations we expect to
        # have Python files. This minimizes the time & RAM the LSP takes to
        # parse the project.
        "python.analysis.include": ["pw_*/py/**/*"],
        "python.terminal.activateEnvironment": False,
        "python.testing.unittestEnabled": True,
        "[python]": OrderedDict({"editor.tabSize": 4}),
        "typescript.tsc.autoDetect": "off",
        "[gn]": OrderedDict({"editor.defaultFormatter": "msedge-dev.gnls"}),
        "[proto3]": OrderedDict(
            {"editor.defaultFormatter": "zxh404.vscode-proto3"}
        ),
        "[restructuredtext]": OrderedDict({"editor.tabSize": 3}),
    }
)

_DEFAULT_TASKS: EditorSettingsDict = OrderedDict(
    {
        "version": "2.0.0",
        "tasks": [
            {
                "type": "process",
                "label": "Pigweed: Format",
                "command": "${config:python.defaultInterpreterPath}",
                "args": [
                    "-m",
                    "pw_ide.activate",
                    "-x 'pw format --fix'",
                ],
                "presentation": {
                    "focus": True,
                },
                "problemMatcher": [],
            },
            {
                "type": "process",
                "label": "Pigweed: Presubmit",
                "command": "${config:python.defaultInterpreterPath}",
                "args": [
                    "-m",
                    "pw_ide.activate",
                    "-x 'pw presubmit'",
                ],
                "presentation": {
                    "focus": True,
                },
                "problemMatcher": [],
            },
            {
                "label": "Pigweed: Set Python Virtual Environment",
                "command": "${command:python.setInterpreter}",
                "problemMatcher": [],
            },
            {
                "label": "Pigweed: Restart Python Language Server",
                "command": "${command:python.analysis.restartLanguageServer}",
                "problemMatcher": [],
            },
            {
                "label": "Pigweed: Restart C++ Language Server",
                "command": "${command:clangd.restart}",
                "problemMatcher": [],
            },
            {
                "type": "process",
                "label": "Pigweed: Sync IDE",
                "command": "${config:python.defaultInterpreterPath}",
                "args": [
                    "-m",
                    "pw_ide.activate",
                    "-x 'pw ide sync'",
                ],
                "presentation": {
                    "focus": True,
                },
                "problemMatcher": [],
            },
            {
                "type": "process",
                "label": "Pigweed: Current C++ Target Toolchain",
                "command": "${config:python.defaultInterpreterPath}",
                "args": [
                    "-m",
                    "pw_ide.activate",
                    "-x 'pw ide cpp'",
                ],
                "presentation": {
                    "focus": True,
                },
                "problemMatcher": [],
            },
            {
                "type": "process",
                "label": "Pigweed: List C++ Target Toolchains",
                "command": "${config:python.defaultInterpreterPath}",
                "args": [
                    "-m",
                    "pw_ide.activate",
                    "-x 'pw ide cpp --list'",
                ],
                "presentation": {
                    "focus": True,
                },
                "problemMatcher": [],
            },
            {
                "type": "process",
                "label": (
                    "Pigweed: Change C++ Target Toolchain "
                    "without LSP restart"
                ),
                "command": "${config:python.defaultInterpreterPath}",
                "args": [
                    "-m",
                    "pw_ide.activate",
                    "-x 'pw ide cpp --set ${input:availableTargetToolchains}'",
                ],
                "presentation": {
                    "focus": True,
                },
                "problemMatcher": [],
            },
            {
                "label": "Pigweed: Set C++ Target Toolchain",
                "dependsOrder": "sequence",
                "dependsOn": [
                    "Pigweed: Change C++ Target Toolchain without LSP restart",
                    "Pigweed: Restart C++ Language Server",
                ],
                "presentation": {
                    "focus": True,
                },
                "problemMatcher": [],
            },
        ],
    }
)

_DEFAULT_EXTENSIONS: EditorSettingsDict = OrderedDict(
    {
        "recommendations": [
            "llvm-vs-code-extensions.vscode-clangd",
            "ms-python.mypy-type-checker",
            "ms-python.python",
            "ms-python.pylint",
            "npclaudiu.vscode-gn",
            "msedge-dev.gnls",
            "zxh404.vscode-proto3",
            "josetr.cmake-language-support-vscode",
        ],
        "unwantedRecommendations": [
            "ms-vscode.cpptools",
            "persidskiy.vscode-gnformat",
            "lextudio.restructuredtext",
        ],
    }
)

_DEFAULT_LAUNCH: EditorSettingsDict = OrderedDict(
    {
        "version": "0.2.0",
        "configurations": [],
    }
)


def _default_settings(
    pw_ide_settings: PigweedIdeSettings,
) -> EditorSettingsDict:
    return OrderedDict(
        {
            **_DEFAULT_SETTINGS,
            **_local_clangd_settings(pw_ide_settings),
            **_local_python_settings(),
        }
    )


def _default_settings_no_side_effects(
    _pw_ide_settings: PigweedIdeSettings,
) -> EditorSettingsDict:
    return OrderedDict(
        {
            **_DEFAULT_SETTINGS,
        }
    )


def _default_tasks(
    pw_ide_settings: PigweedIdeSettings,
    state: CppIdeFeaturesState | None = None,
) -> EditorSettingsDict:
    if state is None:
        state = CppIdeFeaturesState(pw_ide_settings)

    inputs = [
        {
            "type": "pickString",
            "id": "availableTargetToolchains",
            "description": "Available target toolchains",
            "options": sorted(list(state.targets)),
        }
    ]

    return OrderedDict(**_DEFAULT_TASKS, inputs=inputs)


def _default_extensions(
    _pw_ide_settings: PigweedIdeSettings,
) -> EditorSettingsDict:
    return _DEFAULT_EXTENSIONS


def _default_launch(
    _pw_ide_settings: PigweedIdeSettings,
) -> EditorSettingsDict:
    return _DEFAULT_LAUNCH


_DEFAULT_SETTINGS_PATH: Callable[[PigweedIdeSettings], Path] = (
    lambda settings: settings.workspace_root / '.vscode'
)


class VscSettingsType(Enum):
    """Visual Studio Code settings files.

    VSC supports editor settings (``settings.json``), recommended
    extensions (``extensions.json``), tasks (``tasks.json``), and
    launch/debug configurations (``launch.json``).
    """

    SETTINGS = 'settings'
    TASKS = 'tasks'
    EXTENSIONS = 'extensions'
    LAUNCH = 'launch'

    @classmethod
    def all(cls) -> list[VscSettingsType]:
        return list(cls)


class VscSettingsManager(EditorSettingsManager[VscSettingsType]):
    """Manages all settings for Visual Studio Code."""

    default_settings_dir: Callable[[PigweedIdeSettings], Path] = (
        _DEFAULT_SETTINGS_PATH
    )
    file_format = Json5FileFormat()

    types_with_defaults: EditorSettingsTypesWithDefaults = {
        VscSettingsType.SETTINGS: _default_settings,
        VscSettingsType.TASKS: _default_tasks,
        VscSettingsType.EXTENSIONS: _default_extensions,
        VscSettingsType.LAUNCH: _default_launch,
    }


class VscSettingsManagerNoSideEffects(EditorSettingsManager[VscSettingsType]):
    """This is like VscSettingsManager, but optimized for unit testing."""

    default_settings_dir: Callable[[PigweedIdeSettings], Path] = (
        _DEFAULT_SETTINGS_PATH
    )
    file_format = Json5FileFormat()

    types_with_defaults: EditorSettingsTypesWithDefaults = {
        VscSettingsType.SETTINGS: _default_settings_no_side_effects,
        VscSettingsType.TASKS: _default_tasks,
        VscSettingsType.EXTENSIONS: _default_extensions,
        VscSettingsType.LAUNCH: _default_launch,
    }
