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
"""Configure C/C++ IDE support for Pigweed projects.

We support C/C++ code analysis via ``clangd``, or other language servers that
are compatible with the ``clangd`` compilation database format.

While clangd can work well out of the box for typical C++ codebases, some work
is required to coax it to work for embedded projects. In particular, Pigweed
projects use multiple toolchains within a distinct environment, and almost
always define multiple targets. This means compilation units are likely have
multiple compile commands and the toolchain executables are unlikely to be in
your path. ``clangd`` is not equipped to deal with this out of the box. We
handle this by:

- Processing the compilation database produced by the build system into
  multiple internally-consistent compilation databases, one for each target
  (where a "target" is a particular build for a particular system using a
  particular toolchain).

- Creating unambiguous paths to toolchain drivers to ensure the right toolchain
  is used and that clangd knows where to find that toolchain's system headers.

- Providing tools for working with several compilation databases that are
  spiritually similar to tools like ``pyenv``, ``rbenv``, etc.

In short, we take the probably-broken compilation database that the build system
generates, process it into several not-broken compilation databases in the
``pw_ide`` working directory, and provide a stable symlink that points to the
selected active target's compliation database. If ``clangd`` is configured to
point at the symlink and is set up with the right paths, you'll get code
intelligence.
"""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import asdict, dataclass, field
import functools
import glob
from hashlib import sha1
from io import TextIOBase
import json
import logging
from pathlib import Path
import platform
import random
import re
import sys
from typing import (
    Any,
    cast,
    Generator,
    Iterator,
    NotRequired,
    TypedDict,
)

from pw_cli.env import pigweed_environment

from pw_ide.exceptions import (
    BadCompDbException,
    InvalidTargetException,
    MissingCompDbException,
    UnresolvablePathException,
)

from pw_ide.settings import PigweedIdeSettings
from pw_ide.symlinks import set_symlink

_LOG = logging.getLogger(__package__)
env = pigweed_environment()

COMPDB_FILE_NAME = 'compile_commands.json'
STABLE_CLANGD_DIR_NAME = '.stable'
_CPP_IDE_FEATURES_DATA_FILE = 'pw_ide_state.json'
_UNSUPPORTED_TOOLCHAIN_EXECUTABLES = ('_pw_invalid', 'python')
_SUPPORTED_WRAPPER_EXECUTABLES = ('ccache',)


@dataclass(frozen=True)
class CppIdeFeaturesTarget:
    """Data pertaining to a C++ code analysis target."""

    name: str
    compdb_file_path: Path
    num_commands: int
    is_enabled: bool = True

    def __str__(self) -> str:
        return self.name

    def serialized(self) -> dict[str, Any]:
        return {
            **asdict(self),
            **{
                'compdb_file_path': str(self.compdb_file_path),
            },
        }

    @classmethod
    def deserialize(cls, **data) -> CppIdeFeaturesTarget:
        return cls(
            **{
                **data,
                **{
                    'compdb_file_path': Path(data['compdb_file_path']),
                },
            }
        )


CppCompilationDatabaseFileHashes = dict[Path, str]
CppCompilationDatabaseFileTargets = dict[Path, list[CppIdeFeaturesTarget]]


@dataclass
class CppIdeFeaturesData:
    """State data about C++ code analysis features."""

    targets: dict[str, CppIdeFeaturesTarget] = field(default_factory=dict)
    current_target: CppIdeFeaturesTarget | None = None
    compdb_hashes: CppCompilationDatabaseFileHashes = field(
        default_factory=dict
    )
    compdb_targets: CppCompilationDatabaseFileTargets = field(
        default_factory=dict
    )

    def serialized(self) -> dict[str, Any]:
        return {
            'current_target': (
                self.current_target.serialized()
                if self.current_target is not None
                else None
            ),
            'targets': {
                name: target_data.serialized()
                for name, target_data in self.targets.items()
            },
            'compdb_hashes': {
                str(path): hash_str
                for path, hash_str in self.compdb_hashes.items()
            },
            'compdb_targets': {
                str(path): [
                    target_data.serialized() for target_data in target_data_list
                ]
                for path, target_data_list in self.compdb_targets.items()
            },
        }

    @classmethod
    def deserialize(cls, **data) -> CppIdeFeaturesData:
        return cls(
            current_target=(
                CppIdeFeaturesTarget.deserialize(**data['current_target'])
                if data['current_target'] is not None
                else None
            ),
            targets={
                name: CppIdeFeaturesTarget.deserialize(**target_data)
                for name, target_data in data['targets'].items()
            },
            compdb_hashes={
                Path(path_str): hash_str
                for path_str, hash_str in data['compdb_hashes'].items()
            },
            compdb_targets={
                Path(path_str): [
                    CppIdeFeaturesTarget.deserialize(**target_data)
                    for target_data in target_data_list
                ]
                for path_str, target_data_list in data['compdb_targets'].items()
            },
        )


class CppIdeFeaturesState:
    """Container for IDE features state data."""

    def __init__(self, pw_ide_settings: PigweedIdeSettings) -> None:
        self.settings = pw_ide_settings

    def __len__(self) -> int:
        return len(self.targets)

    def __getitem__(self, index: str) -> CppIdeFeaturesTarget:
        return self.targets[index]

    def __iter__(self) -> Generator[CppIdeFeaturesTarget, None, None]:
        return (target for target in self.targets.values())

    @property
    def stable_target_link(self) -> Path:
        return self.settings.working_dir / STABLE_CLANGD_DIR_NAME

    @contextmanager
    def _file(self) -> Generator[CppIdeFeaturesData, None, None]:
        """A simple key-value store for state data."""
        file_path = self.settings.working_dir / _CPP_IDE_FEATURES_DATA_FILE

        try:
            with open(file_path) as file:
                data = CppIdeFeaturesData.deserialize(**json.load(file))
        except (FileNotFoundError, json.decoder.JSONDecodeError):
            data = CppIdeFeaturesData()

        yield data

        with open(file_path, 'w') as file:
            json.dump(data.serialized(), file, indent=2)

    @property
    def targets(self) -> dict[str, CppIdeFeaturesTarget]:
        with self._file() as state:
            exclude_predicate = lambda x: (
                x not in self.settings.targets_exclude
                if len(self.settings.targets_exclude) > 0
                else lambda x: True
            )
            include_predicate = lambda x: (
                x in self.settings.targets_include
                if len(self.settings.targets_include) > 0
                else lambda x: True
            )

            return {
                name: target
                for (name, target) in state.targets.items()
                if exclude_predicate(name) and include_predicate(name)
            }

    @targets.setter
    def targets(self, new_targets: dict[str, CppIdeFeaturesTarget]) -> None:
        with self._file() as state:
            state.targets = new_targets

    @property
    def current_target(self) -> CppIdeFeaturesTarget | None:
        with self._file() as state:
            return state.current_target

    @current_target.setter
    def current_target(
        self, new_current_target: str | CppIdeFeaturesTarget | None
    ) -> None:
        with self._file() as state:
            if new_current_target is None:
                state.current_target = None
            else:
                if isinstance(new_current_target, CppIdeFeaturesTarget):
                    name = new_current_target.name
                    new_current_target_inst = new_current_target
                else:
                    name = new_current_target

                    try:
                        new_current_target_inst = state.targets[name]
                    except KeyError:
                        raise InvalidTargetException

                if not new_current_target_inst.compdb_file_path.exists():
                    raise MissingCompDbException

                set_symlink(
                    new_current_target_inst.compdb_file_path.parent,
                    self.stable_target_link,
                )

                state.current_target = state.targets[name]

    @property
    def max_commands_target(self) -> CppIdeFeaturesTarget | None:
        with self._file() as state:
            if len(state.targets) == 0:
                return None

            max_commands_target_name = sorted(
                [
                    (name, target.num_commands)
                    for name, target in state.targets.items()
                ],
                key=lambda x: x[1],
                reverse=True,
            )[0][0]

            return state.targets[max_commands_target_name]

    @property
    def compdb_hashes(self) -> CppCompilationDatabaseFileHashes:
        with self._file() as state:
            return state.compdb_hashes

    @compdb_hashes.setter
    def compdb_hashes(
        self, new_compdb_hashes: CppCompilationDatabaseFileHashes
    ) -> None:
        with self._file() as state:
            state.compdb_hashes = new_compdb_hashes

    @property
    def compdb_targets(self) -> CppCompilationDatabaseFileTargets:
        with self._file() as state:
            return state.compdb_targets

    @compdb_targets.setter
    def compdb_targets(
        self, new_compdb_targets: CppCompilationDatabaseFileTargets
    ) -> None:
        with self._file() as state:
            state.compdb_targets = new_compdb_targets


def path_to_executable(
    exe: str,
    *,
    default_path: Path | None = None,
    path_globs: list[str] | None = None,
    strict: bool = False,
) -> Path | None:
    """Return the path to a compiler executable.

    In a ``clang`` compile command, the executable may or may not include a
    path. For example:

    .. code-block:: none

       /usr/bin/clang      <- includes a path
       ../path/to/my/clang <- includes a path
       clang               <- doesn't include a path

    If it includes a path, then ``clangd`` will have no problem finding the
    driver, so we can simply return the path. If the executable *doesn't*
    include a path, then ``clangd`` will search ``$PATH``, and may not find the
    intended driver unless you actually want the default system toolchain or
    Pigweed paths have been added to ``$PATH``. So this function provides two
    options for resolving those ambiguous paths:

    - Provide a default path, and all executables without a path will be
      re-written with a path within the default path.
    - Provide the a set of globs that will be used to search for the executable,
      which will normally be the query driver globs used with clangd.

    By default, if neither of these options is chosen, or if the executable
    cannot be found within the provided globs, the pathless executable that was
    provided will be returned, and clangd will resort to searching $PATH. If you
    instead pass ``strict=True``, this will raise an exception if an unambiguous
    path cannot be constructed.

    This function only tries to ensure that all executables have a path to
    eliminate ambiguity. A couple of important things to keep in mind:

    - This doesn't guarantee that the path exists or an executable actually
      exists at the path. It only ensures that some path is provided to an
      executable.
    - An executable being present at the indicated path doesn't guarantee that
      it will work flawlessly for clangd code analysis. The clangd
      ``--query-driver`` argument needs to include a path to this executable in
      order for its bundled headers to be resolved correctly.

    This function also filters out invalid or unsupported drivers. For example,
    build systems will sometimes naively include build steps for Python or other
    languages in the compilation database, which are not usable with clangd.
    As a result, this function has four possible end states:

    - It returns a path with an executable that can be used as a ``clangd``
      driver.
    - It returns ``None``, meaning the compile command was invalid.
    - It returns the same string that was provided (as a ``Path``), if a path
      couldn't be resolved and ``strict=False``.
    - It raises an ``UnresolvablePathException`` if the executable cannot be
      placed in an unambiguous path and ``strict=True``.
    """
    maybe_path = Path(exe)

    # We were give an empty string, not a path. Not a valid command.
    if len(maybe_path.parts) == 0:
        _LOG.debug("Invalid executable path. The path was an empty string.")
        return None

    # Determine if the executable name matches unsupported drivers.
    is_supported_driver = True

    for unsupported_executable in _UNSUPPORTED_TOOLCHAIN_EXECUTABLES:
        if unsupported_executable in maybe_path.name:
            is_supported_driver = False

    if not is_supported_driver:
        _LOG.debug(
            "Invalid executable path. This is not a supported driver: %s", exe
        )
        return None

    # Now, ensure the executable has a path.

    # This is either a relative or absolute path -- return it.
    if len(maybe_path.parts) > 1:
        return maybe_path

    # If we got here, there's only one "part", so we assume it's an executable
    # without a path. This logic doesn't work with a path like `./exe` since
    # that also yields only one part. So currently this breaks if you actually
    # have your compiler executable in your root build directory, which is
    # (hopefully) very rare.

    # If we got a default path, use it.
    if default_path is not None:
        return default_path / maybe_path

    # Otherwise, try to find the executable within the query driver globs.
    # Note that unlike the previous paths, this path will only succeed if an
    # executable actually exists somewhere in the query driver globs.
    if path_globs is not None:
        for path_glob in path_globs:
            for path_str in glob.iglob(path_glob):
                path = Path(path_str)
                if path.name == maybe_path.name:
                    return path.absolute()

    if strict:
        raise UnresolvablePathException(
            f'Cannot place {exe} in an unambiguous path!'
        )

    return maybe_path


def command_parts(command: str) -> tuple[str | None, str, list[str]]:
    """Return the executable string and the rest of the command tokens.

    If the command contains a prefixed wrapper like `ccache`, it will be
    extracted separately. So the return value contains:
        (wrapper, compiler executable, all other tokens)
    """
    parts = command.split()
    curr = ''
    wrapper = None

    try:
        curr = parts.pop(0)
    except IndexError:
        return (None, curr, [])

    if curr in _SUPPORTED_WRAPPER_EXECUTABLES:
        wrapper = curr

        while curr := parts.pop(0):
            # This is very `ccache`-centric. It will work for other wrappers
            # that use KEY=VALUE-style options or no options at all, but will
            # not work for other cases.
            if re.fullmatch(r'(.*)=(.*)', curr):
                wrapper = f'{wrapper} {curr}'
            else:
                break

    return (wrapper, curr, parts)


class CppCompileCommandDict(TypedDict):
    file: str
    directory: str
    command: NotRequired[str]
    arguments: NotRequired[list[str]]
    output: NotRequired[str | None]


class CppCompileCommand:
    """A representation of a clang compilation database compile command.

    See: https://clang.llvm.org/docs/JSONCompilationDatabase.html
    """

    def __init__(
        self,
        file: str,
        directory: str,
        command: str | None = None,
        arguments: list[str] | None = None,
        output: str | None = None,
    ) -> None:
        # Per the spec, either one of these two must be present. clangd seems
        # to prefer "arguments" when both are present.
        if command is None and arguments is None:
            raise TypeError(
                'A compile command requires either \'command\' '
                'or \'arguments\'.'
            )

        if command is None:
            raise TypeError(
                'Compile commands without \'command\' ' 'are not supported yet.'
            )

        self._command = command
        self._arguments = arguments
        self._file = file
        self._directory = directory

        _, executable, tokens = command_parts(command)
        self._executable_path = Path(executable)
        self._inferred_output: str | None = None

        try:
            # Find the output argument and grab its value.
            output_flag_idx = tokens.index('-o')
            self._inferred_output = tokens[output_flag_idx + 1]
        except ValueError:
            # No -o found, probably not a C/C++ compile command.
            self._inferred_output = None
        except IndexError:
            # It has an -o but no argument after it.
            raise TypeError(
                'Failed to load compile command with no output argument!'
            )

        self._provided_output = output
        self.target: str | None = None

    @property
    def file(self) -> str:
        return self._file

    @property
    def directory(self) -> str:
        return self._directory

    @property
    def command(self) -> str | None:
        return self._command

    @property
    def arguments(self) -> list[str] | None:
        return self._arguments

    @property
    def output(self) -> str | None:
        # We're ignoring provided output values for now.
        return self._inferred_output

    @property
    def output_path(self) -> Path | None:
        if self.output is None:
            return None

        return Path(self.directory) / Path(self.output)

    @property
    def executable_path(self) -> Path:
        return self._executable_path

    @property
    def executable_name(self) -> str:
        return self.executable_path.name

    @classmethod
    def from_dict(
        cls, compile_command_dict: dict[str, Any]
    ) -> CppCompileCommand:
        return cls(
            # We want to let possible Nones through to raise at runtime.
            file=cast(str, compile_command_dict.get('file')),
            directory=cast(str, compile_command_dict.get('directory')),
            command=compile_command_dict.get('command'),
            arguments=compile_command_dict.get('arguments'),
            output=compile_command_dict.get('output'),
        )

    @classmethod
    def try_from_dict(
        cls, compile_command_dict: dict[str, Any]
    ) -> CppCompileCommand | None:
        try:
            return cls.from_dict(compile_command_dict)
        except TypeError:
            return None

    def process(
        self,
        *,
        default_path: Path | None = None,
        path_globs: list[str] | None = None,
        strict: bool = False,
    ) -> CppCompileCommand | None:
        """Process a compile command.

        At minimum, a compile command from a clang compilation database needs to
        be correlated with its target, and this method returns the target name
        with the compile command. But it also cleans up other things we need for
        reliable code intelligence:

        - Some targets may not be valid C/C++ compile commands. For example,
          some build systems will naively include build steps for Python or for
          linting commands. We want to filter those out.

        - Some compile commands don't provide a path to the compiler executable
          (referred to by clang as the "driver"). In that case, clangd is very
          unlikely to find the executable unless it happens to be in ``$PATH``.
          The ``--query-driver`` argument to ``clangd`` allowlists
          executables/drivers for use its use, but clangd doesn't use it to
          resolve ambiguous paths. We bridge that gap here. Any executable
          without a path will be either placed in the provided default path or
          searched for in the query driver globs and be replaced with a path to
          the executable.
        """
        if self.command is None:
            raise NotImplementedError(
                'Compile commands without \'command\' ' 'are not supported yet.'
            )

        wrapper, executable_str, tokens = command_parts(self.command)
        executable_path = path_to_executable(
            executable_str,
            default_path=default_path,
            path_globs=path_globs,
            strict=strict,
        )

        if executable_path is None:
            _LOG.debug(
                "Compile command rejected due to bad executable path: %s",
                self.command,
            )
            return None

        if self.output is None:
            _LOG.debug(
                "Compile command rejected due to no output property: %s",
                self.command,
            )
            return None

        # TODO(chadnorvell): Some commands include the executable multiple
        # times. It's not clear if that affects clangd.
        new_command = f'{str(executable_path)} {" ".join(tokens)}'

        if wrapper is not None:
            new_command = f'{wrapper} {new_command}'

        return self.__class__(
            file=self.file,
            directory=self.directory,
            command=new_command,
            arguments=None,
            output=self.output,
        )

    def as_dict(self) -> CppCompileCommandDict:
        # TODO(chadnorvell): Support "arguments". The spec requires that a
        # We don't support "arguments" at all right now. When we do, we should
        # preferentially include "arguments" only, and only include "command"
        # when "arguments" is not present.
        if self.command is not None:
            compile_command_dict: CppCompileCommandDict = {
                'command': self.command,
                'file': self.file,
                'directory': self.directory,
                'output': self.output,
            }
        else:
            raise NotImplementedError(
                'Compile commands without \'command\' ' 'are not supported yet.'
            )

        return compile_command_dict


def _path_nearest_parent(path1: Path, path2: Path) -> Path:
    """Get the closest common parent of two paths."""
    # This is the Python < 3.9 version of: if path2.is_relative_to(path1)
    try:
        path2.relative_to(path1)
        return path1
    except ValueError:
        pass

    if path1 == path2:
        return path1

    if len(path1.parts) > len(path2.parts):
        return _path_nearest_parent(path1.parent, path2)

    if len(path1.parts) < len(path2.parts):
        return _path_nearest_parent(path1, path2.parent)

    return _path_nearest_parent(path1.parent, path2.parent)


def _infer_target_pos(target_glob: str) -> list[int]:
    """Infer the position of the target in a compilation unit artifact path."""
    tokens = Path(target_glob).parts
    positions = []

    for pos, token in enumerate(tokens):
        if token == '?':
            positions.append(pos)
        elif token == '*':
            pass
        else:
            raise ValueError(f'Invalid target inference token: {token}')

    return positions


def infer_target(target_glob: str, root: Path, output_path: Path) -> str | None:
    """Infer a target from a compilation unit artifact path.

    See the documentation for ``PigweedIdeSettings.target_inference``."""
    target_pos = _infer_target_pos(target_glob)

    if len(target_pos) == 0:
        return None

    # Depending on the build system and project configuration, the target name
    # may be in the "directory" or the "output" of the compile command. So we
    # need to construct the full path that combines both and use that to search
    # for the target.
    try:
        # The path used for target inference is the path relative to the root
        # dir. If this artifact is a direct child of the root, this just
        # truncates the root off of its path.
        subpath = output_path.relative_to(root)
    except ValueError:
        # If the output path isn't a child path of the root dir, find the
        # closest shared parent dir and use that as the root for truncation.
        common_parent = _path_nearest_parent(root, output_path)
        subpath = output_path.relative_to(common_parent)

    return '_'.join([subpath.parts[pos] for pos in target_pos])


LoadableToCppCompilationDatabase = (
    list[dict[str, Any]] | str | TextIOBase | Path
)


class CppCompilationDatabase:
    """A representation of a clang compilation database.

    See: https://clang.llvm.org/docs/JSONCompilationDatabase.html
    """

    def __init__(
        self,
        root_dir: Path | None = None,
        file_path: Path | None = None,
        source_file_path: Path | None = None,
        target_inference: str | None = None,
    ) -> None:
        self._db: list[CppCompileCommand] = []
        self.file_path: Path | None = file_path
        self.source_file_path: Path | None = source_file_path
        self.source_file_hash: str | None = None

        if target_inference is None:
            self.target_inference = PigweedIdeSettings().target_inference
        else:
            self.target_inference = target_inference

        # Only compilation databases that are loaded will have this, and it
        # contains the root directory of the build that the compilation
        # database is based on. Processed compilation databases will not have
        # a value here.
        self._root_dir = root_dir

    def __len__(self) -> int:
        return len(self._db)

    def __getitem__(self, index: int) -> CppCompileCommand:
        return self._db[index]

    def __iter__(self) -> Generator[CppCompileCommand, None, None]:
        return (compile_command for compile_command in self._db)

    @property
    def file_hash(self) -> str:
        # If this compilation database did not originate from a file, return a
        # hash that is almost certainly not going to match any other hash; these
        # sources are not persistent, so they cannot be compared.
        if self.file_path is None:
            return '%032x' % random.getrandbits(160)

        data = self.file_path.read_text().encode('utf-8')
        return sha1(data).hexdigest()

    def add(self, *commands: CppCompileCommand):
        """Add compile commands to the compilation database."""
        self._db.extend(commands)

    def merge(self, other: CppCompilationDatabase) -> None:
        """Merge values from another database into this one.

        This will not overwrite a compile command that already exists for a
        particular file.
        """
        self_dict = {c.file: c for c in self._db}

        for compile_command in other:
            if compile_command.file not in self_dict:
                self_dict[compile_command.file] = compile_command

        self._db = list(self_dict.values())

    def as_dicts(self) -> list[CppCompileCommandDict]:
        return [compile_command.as_dict() for compile_command in self._db]

    def to_json(self) -> str:
        """Output the compilation database to a JSON string."""

        return json.dumps(self.as_dicts(), indent=2, sort_keys=True)

    def to_file(self, path: Path):
        """Write the compilation database to a JSON file."""
        path.parent.mkdir(parents=True, exist_ok=True)

        with open(path, 'w') as file:
            json.dump(self.as_dicts(), file, indent=2, sort_keys=True)

    @classmethod
    def load(
        cls,
        compdb_to_load: LoadableToCppCompilationDatabase,
        root_dir: Path,
        target_inference: str | None = None,
    ) -> CppCompilationDatabase:
        """Load a compilation database.

        You can provide a JSON file handle or path, a JSON string, or a native
        Python data structure that matches the format (list of dicts).
        """
        db_as_dicts: list[dict[str, Any]]
        file_path = None

        if isinstance(compdb_to_load, list):
            # The provided data is already in the format we want it to be in,
            # probably, and if it isn't we'll find out when we try to
            # instantiate the database.
            db_as_dicts = compdb_to_load
        else:
            if isinstance(compdb_to_load, Path):
                # The provided data is a path to a file, presumably JSON.
                try:
                    file_path = compdb_to_load
                    compdb_data = compdb_to_load.read_text()
                except FileNotFoundError:
                    raise MissingCompDbException()
            elif isinstance(compdb_to_load, TextIOBase):
                # The provided data is a file handle, presumably JSON.
                file_path = Path(compdb_to_load.name)  # type: ignore
                compdb_data = compdb_to_load.read()
            elif isinstance(compdb_to_load, str):
                # The provided data is a a string, presumably JSON.
                compdb_data = compdb_to_load

            db_as_dicts = json.loads(compdb_data)

        compdb = cls(
            root_dir=root_dir,
            file_path=file_path,
            target_inference=target_inference,
        )

        try:
            compdb.add(
                *[
                    compile_command
                    for compile_command_dict in db_as_dicts
                    if (
                        compile_command := CppCompileCommand.try_from_dict(
                            compile_command_dict
                        )
                    )
                    is not None
                ]
            )
        except TypeError:
            # This will arise if db_as_dicts is not actually a list of dicts
            raise BadCompDbException()

        return compdb

    def process(
        self,
        settings: PigweedIdeSettings,
        *,
        default_path: Path | None = None,
        path_globs: list[str] | None = None,
        strict: bool = False,
        always_output_new: bool = False,
    ) -> CppCompilationDatabasesMap | None:
        """Process a ``clangd`` compilation database file.

        Given a clang compilation database that may have commands for multiple
        valid or invalid targets/toolchains, keep only the valid compile
        commands and store them in target-specific compilation databases.

        If this finds that the processed file is functionally identical to the
        input file (meaning that the input file did not require processing to
        be used successfully with ``clangd``), then it will return ``None``,
        indicating that the original file should be used. This behavior can be
        overridden by setting ``always_output_new``, which will ensure that a
        new compilation database is always written to the working directory and
        original compilation databases outside the working directory are never
        made available for code intelligence.
        """
        if self._root_dir is None:
            raise ValueError(
                'Can only process a compilation database that '
                'contains a root build directory, usually '
                'specified when loading the file. Are you '
                'trying to process an already-processed '
                'compilation database?'
            )

        clean_compdbs = CppCompilationDatabasesMap(settings)

        # Do processing, segregate processed commands into separate databases
        # for each target.
        for compile_command in self:
            processed_command = compile_command.process(
                default_path=default_path, path_globs=path_globs, strict=strict
            )

            if (
                processed_command is not None
                and processed_command.output_path is not None
            ):
                target = infer_target(
                    self.target_inference,
                    self._root_dir,
                    processed_command.output_path,
                )

                target = cast(str, target)
                processed_command.target = target
                clean_compdbs[target].add(processed_command)

                if clean_compdbs[target].source_file_path is None:
                    clean_compdbs[target].source_file_path = self.file_path
                    clean_compdbs[target].source_file_hash = self.file_hash

        # TODO(chadnorvell): Handle len(clean_compdbs) == 0

        # Determine if the processed database is functionally identical to the
        # original, unless configured to always output the new databases.
        # The criteria for "functionally identical" are:
        #
        # - The original file only contained commands for a single target
        # - The number of compile commands in the processed database is equal to
        #   that of the original database.
        #
        # This is a little bit crude. For example, it doesn't account for the
        # (rare) edge case of multiple databases having commands for the same
        # target. However, if you know that you have that kind of situation, you
        # should use `always_output_new` and not rely on this.
        if (
            not always_output_new
            and len(clean_compdbs) == 1
            and len(clean_compdbs[0]) == len(self)
        ):
            return None

        return clean_compdbs


class CppCompilationDatabasesMap:
    """Container for a map of target name to compilation database."""

    def __init__(self, settings: PigweedIdeSettings):
        self.settings = settings
        self._dbs: dict[str, CppCompilationDatabase] = dict()

    def __len__(self) -> int:
        return len(self._dbs)

    def _default(self, key: str | int):
        # This is like `defaultdict` except that we can use the provided key
        # (i.e. the target name) in the constructor.
        if isinstance(key, str) and key not in self._dbs:
            file_path = self.settings.working_dir / key / COMPDB_FILE_NAME
            self._dbs[key] = CppCompilationDatabase(file_path=file_path)

    def __getitem__(self, key: str | int) -> CppCompilationDatabase:
        self._default(key)

        # Support list-based indexing...
        if isinstance(key, int):
            return list(self._dbs.values())[key]

        # ... and key-based indexing.
        return self._dbs[key]

    def __setitem__(self, key: str, item: CppCompilationDatabase) -> None:
        self._default(key)
        self._dbs[key] = item

    def __iter__(self) -> Iterator[str]:
        for target, _ in self.items():
            yield target

    @property
    def targets(self) -> list[str]:
        return list(self._dbs.keys())

    def items(
        self,
    ) -> Generator[tuple[str, CppCompilationDatabase], None, None]:
        return ((key, value) for (key, value) in self._dbs.items())

    def _sort_by_commands(self) -> list[str]:
        """Sort targets by the number of compile commands they have."""
        enumerated_targets = sorted(
            [(len(db), target) for target, db in self._dbs.items()],
            key=lambda x: x[0],
            reverse=True,
        )

        return [target for (_, target) in enumerated_targets]

    def _sort_with_target_priority(self, target: str) -> list[str]:
        """Sorted targets, but with the provided target first."""
        sorted_targets = self._sort_by_commands()
        # This will raise a ValueError if the target is not in the list, but
        # we have ensured that that will never happen by the time we get here.
        sorted_targets.remove(target)
        return [target, *sorted_targets]

    def _targets_to_write(self, target: str) -> list[str]:
        """Return the list of targets whose comp. commands should be written.

        Under most conditions, this will return a list with just the provided
        target; essentially it's a no-op. But if ``cascade_targets`` is
        enabled, this returns a list of all targets with the provided target
        at the head of the list.
        """
        if not self.settings.cascade_targets:
            return [target]

        return self._sort_with_target_priority(target)

    def _compdb_to_write(self, target: str) -> CppCompilationDatabase:
        """The compilation database to write to file for this target.

        Under most conditions, this will return the compilation database
        associated with the provided target. But if ``cascade_targets`` is
        enabled, this returns a compilation database with commands from all
        targets, ordered per ``_sort_with_target_priority``.
        """
        targets = self._targets_to_write(target)
        compdb = CppCompilationDatabase()

        for iter_target in targets:
            compdb.add(*self[iter_target])

        return compdb

    def test_write(self) -> None:
        """Test writing to file.

        This will raise an exception if the file is not JSON-serializable."""
        for _, compdb in self.items():
            compdb.to_json()

    def write(self) -> None:
        """Write compilation databases to target-specific JSON files."""
        for target in self:
            path = self.settings.working_dir / target / COMPDB_FILE_NAME
            self._compdb_to_write(target).to_file(path)

    @classmethod
    def merge(
        cls, *db_sets: CppCompilationDatabasesMap
    ) -> CppCompilationDatabasesMap:
        """Merge several sets of processed compilation databases.

        If you process N compilation databases produced by a build system,
        you'll end up with N sets of processed compilation databases,
        containing databases for one or more targets each. This method
        merges them into one set of databases with one database per target.

        The expectation is that the vast majority of the time, each of the
        raw compilation databases that are processed will contain distinct
        targets, meaning that the keys of each ``CppCompilationDatabases``
        object that's merged will be unique to each object, and this operation
        is nothing more than a shallow merge.

        However, this also supports the case where targets may overlap between
        ``CppCompilationDatabases`` objects. In that case, we prioritize
        correctness, ensuring that the resulting compilation databases will
        work correctly with clangd. This means not including duplicate compile
        commands for the same file in the same target's database. The choice
        of which duplicate compile command ends up in the final database is
        unspecified and subject to change. Note also that this method expects
        the ``settings`` value to be the same between all of the provided
        ``CppCompilationDatabases`` objects.
        """
        if len(db_sets) == 0:
            raise ValueError(
                'At least one set of compilation databases is required.'
            )

        # Shortcut for the most common case.
        if len(db_sets) == 1:
            return db_sets[0]

        merged = cls(db_sets[0].settings)

        for dbs in db_sets:
            for target, db in dbs.items():
                merged[target].merge(db)

        return merged


@functools.lru_cache
def find_cipd_installed_exe_path(exe: str) -> Path:
    """Return the path of an executable installed by CIPD.

    Search for the executable in the paths pointed by all the defined
    `PW_<PROJ_NAME>_CIPD_INSTALL_DIR` environment variables.
    """

    if sys.platform.lower() in ("win32", "cygwin"):
        exe += ".exe"

    env_vars = vars(env)

    search_paths: list[str] = []
    for env_var_name, env_var in env_vars.items():
        if re.fullmatch(r"PW_[A-Z_]+_CIPD_INSTALL_DIR", env_var_name):
            search_paths.append(str(Path(env_var) / "bin" / exe))

    if (env_var := env_vars.get('PW_PIGWEED_CIPD_INSTALL_DIR')) is not None:
        search_paths.append(str(Path(env_var) / "bin" / exe))

    path = None
    exception = None
    try:
        path = path_to_executable(
            exe, default_path=None, path_globs=search_paths, strict=True
        )
    except UnresolvablePathException as e:
        exception = e

    if path is None or exception:
        search_paths_str = ":".join(search_paths)
        raise FileNotFoundError(
            f"Not able to find '{exe}' "
            f"among '{search_paths_str}'. Is bootstrap successful?"
        )

    return path


def get_clangd_path(settings: PigweedIdeSettings) -> Path:
    if settings.clangd_alternate_path is not None:
        return settings.clangd_alternate_path

    return find_cipd_installed_exe_path('clangd')


class ClangdSettings:
    """Makes system-specific settings for running ``clangd`` with Pigweed."""

    def __init__(self, settings: PigweedIdeSettings):
        state = CppIdeFeaturesState(settings)

        self.clangd_path = get_clangd_path(settings)

        compile_commands_dir = env.PW_PROJECT_ROOT

        if state.current_target is not None:
            compile_commands_dir = str(state.stable_target_link)

        host_cc_path = find_cipd_installed_exe_path("clang++")

        self.arguments: list[str] = [
            f'--compile-commands-dir={compile_commands_dir}',
            '--background-index',
            '--clang-tidy',
        ]

        query_driver = settings.clangd_query_driver_str(host_cc_path)

        if query_driver is not None:
            self.arguments.append(f'--query-driver={query_driver}')

    def command(self, system: str = platform.system()) -> str:
        """Return the command that runs clangd with Pigweed paths."""

        def make_command(line_continuation: str):
            arguments = f' {line_continuation}\n'.join(
                f'  {arg}' for arg in self.arguments
            )
            return f'\n{self.clangd_path} {line_continuation}\n{arguments}'

        if system.lower() == 'json':
            return '\n' + json.dumps(
                [str(self.clangd_path), *self.arguments], indent=2
            )

        if system.lower() in ['cmd', 'batch']:
            return make_command('`')

        if system.lower() in ['powershell', 'pwsh']:
            return make_command('^')

        if system.lower() == 'windows':
            return (
                f'\nIn PowerShell:\n{make_command("`")}'
                f'\n\nIn Command Prompt:\n{make_command("^")}'
            )

        # Default case for *sh-like shells.
        return make_command('\\')
