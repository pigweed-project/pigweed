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
"""Serializes an Environment into a shell file."""

import inspect


class _BaseShellVisitor:
    def __init__(self, *args, **kwargs):
        pathsep = kwargs.pop('pathsep', ':')
        super().__init__(*args, **kwargs)
        self._pathsep = pathsep
        self._outs = None

    def _write_shell_header(self):
        # Defines _pw_remove_path to remove entries from PATH-like variables in
        # pure POSIX shell rather than spawning sed per entry.
        self._outs.write(
            inspect.cleandoc(
                '''
                # Usage: _pw_remove_path VARIABLE ENTRY
                _pw_remove_path() {{
                  if [ -n "${{ZSH_VERSION:-}}" ]; then
                    emulate -L sh
                  fi
                  eval "_pw_val=\\${{$1-}}"
                  # Entries are matched literally; nothing to do if absent.
                  case "{pathsep}$_pw_val{pathsep}" in
                    *"{pathsep}$2{pathsep}"*) ;;
                    *)
                      unset _pw_val
                      return 0
                      ;;
                  esac
                  # Split on the separator with globbing disabled, then rebuild
                  # the value without the entry. Restore IFS and -f afterward.
                  _pw_old_ifs="${{IFS-__PW_UNSET__}}"
                  case "$-" in
                    *f*) _pw_had_f=1 ;;
                    *) _pw_had_f=0; set -f ;;
                  esac
                  IFS="{pathsep}"
                  _pw_new=""
                  _pw_first=1
                  for _pw_ent in $_pw_val; do
                    if [ "$_pw_ent" != "$2" ]; then
                      if [ "$_pw_first" -eq 1 ]; then
                        _pw_new="$_pw_ent"
                        _pw_first=0
                      else
                        _pw_new="$_pw_new{pathsep}$_pw_ent"
                      fi
                    fi
                  done
                  if [ "$_pw_old_ifs" = "__PW_UNSET__" ]; then
                    unset IFS
                  else
                    IFS="$_pw_old_ifs"
                  fi
                  if [ "$_pw_had_f" -eq 0 ]; then
                    set +f
                  fi
                  eval "$1=\\"\\$_pw_new\\""
                  export "$1"
                  unset _pw_val _pw_old_ifs _pw_had_f _pw_new _pw_first _pw_ent
                }}
                '''.format(
                    pathsep=self._pathsep
                )
            )
            + '\n\n'
        )

    def _write_shell_footer(self):
        self._outs.write('\nunset -f _pw_remove_path\n')

    def serialize(self, env, outs):
        try:
            self._outs = outs
            self._write_shell_header()
            env.accept(self)
            self._write_shell_footer()
        finally:
            self._outs = None

    def _remove_value_from_path(
        self, variable, value
    ):  # pylint: disable=no-self-use
        return '_pw_remove_path {variable} "{value}"\n'.format(
            variable=variable, value=value
        )

    def visit_hash(self, hash):  # pylint: disable=redefined-builtin
        del hash
        self._outs.write(
            inspect.cleandoc(
                '''
        # This should detect bash and zsh, which have a hash command that must
        # be called to get it to forget past commands. Without forgetting past
        # commands the $PATH changes we made may not be respected.
        if [ -n "${BASH:-}" -o -n "${ZSH_VERSION:-}" ] ; then
            hash -r\n
        fi
        '''
            )
        )


class ShellVisitor(_BaseShellVisitor):
    """Serializes an Environment into a bash-like shell file."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._replacements = ()

    def serialize(self, env, outs):
        """Write a shell file based on the given environment.

        Args:
            env (environment.Environment): Environment variables to use.
            outs (file): Shell file to write.
        """
        try:
            self._replacements = tuple(
                (key, env.get(key) if value is None else value)
                for key, value in env.replacements
            )
            super().serialize(env, outs)
        finally:
            self._replacements = ()

    def _apply_replacements(self, action):
        if not action.replace:
            return action.value
        value = action.value
        for var, replacement in self._replacements:
            if var != action.name:
                value = value.replace(replacement, '${}'.format(var))
        return value

    def visit_set(self, set):  # pylint: disable=redefined-builtin
        value = self._apply_replacements(set)
        if set.override_var:
            self._outs.write(
                '{name}="${{{override}:-{value}}}"\n'
                'unset {override}\n'
                'export {name}\n'.format(
                    name=set.name, override=set.override_var, value=value
                )
            )
        else:
            self._outs.write(
                '{name}="{value}"\nexport {name}\n'.format(
                    name=set.name, value=value
                )
            )

    def visit_clear(self, clear):
        self._outs.write('unset {name}\n'.format(**vars(clear)))

    def visit_remove(self, remove):
        value = self._apply_replacements(remove)
        self._outs.write(
            '# Remove \n#   {value}\n# from {name} before adding it '
            'back.\n'.format(value=remove.value, name=remove.name)
        )
        self._outs.write(self._remove_value_from_path(remove.name, value))

    def _join(self, *args):
        if len(args) == 1 and isinstance(args[0], (list, tuple)):
            args = args[0]
        return self._pathsep.join(args)

    def visit_prepend(self, prepend):
        value = self._apply_replacements(prepend)
        value = self._join(value, '${}'.format(prepend.name))
        self._outs.write(
            '{name}="{value}"\nexport {name}\n'.format(
                name=prepend.name, value=value
            )
        )

    def visit_append(self, append):
        value = self._apply_replacements(append)
        value = self._join('${}'.format(append.name), value)
        self._outs.write(
            '{name}="{value}"\nexport {name}\n'.format(
                name=append.name, value=value
            )
        )

    def visit_echo(self, echo):
        # TODO(mohrr) use shlex.quote().
        self._outs.write('if [ -z "${PW_ENVSETUP_QUIET:-}" ]; then\n')
        if echo.newline:
            self._outs.write('  echo "{}"\n'.format(echo.value))
        else:
            self._outs.write('  echo -n "{}"\n'.format(echo.value))
        self._outs.write('fi\n')

    def visit_comment(self, comment):
        for line in comment.value.splitlines():
            self._outs.write('# {}\n'.format(line))

    def visit_command(self, command):
        # TODO(mohrr) use shlex.quote here?
        self._outs.write('{}\n'.format(' '.join(command.command)))
        if not command.exit_on_error:
            return

        # Assume failing command produced relevant output.
        self._outs.write('if [ "$?" -ne 0 ]; then\n  return 1\nfi\n')

    def visit_doctor(self, doctor):
        self._outs.write('if [ -z "${PW_ACTIVATE_SKIP_CHECKS:-}" ]; then\n')
        self.visit_command(doctor)
        self._outs.write('else\n')
        self.visit_echo(doctor.skip_echo)
        self._outs.write('fi\n')

    def visit_blank_line(self, blank_line):
        del blank_line
        self._outs.write('\n')

    def visit_function(self, function):
        self._outs.write(
            '{name}() {{\n{body}\n}}\n'.format(
                name=function.name, body=function.body
            )
        )


class DeactivateShellVisitor(_BaseShellVisitor):
    """Removes values from a bash-like shell environment."""

    def __init__(self, *args, **kwargs):
        pathsep = kwargs.pop('pathsep', ':')
        super().__init__(*args, **kwargs)
        self._pathsep = pathsep

    def visit_set(self, set):  # pylint: disable=redefined-builtin
        if set.deactivate:
            self._outs.write('unset {name}\n'.format(name=set.name))

    def visit_clear(self, clear):
        pass  # Not relevant.

    def visit_remove(self, remove):
        pass  # Not relevant.

    def visit_prepend(self, prepend):
        self._outs.write(
            self._remove_value_from_path(prepend.name, prepend.value)
        )

    def visit_append(self, append):
        self._outs.write(
            self._remove_value_from_path(append.name, append.value)
        )

    def visit_echo(self, echo):
        pass  # Not relevant.

    def visit_comment(self, comment):
        pass  # Not relevant.

    def visit_command(self, command):
        pass  # Not relevant.

    def visit_doctor(self, doctor):
        pass  # Not relevant.

    def visit_blank_line(self, blank_line):
        pass  # Not relevant.

    def visit_function(self, function):
        pass  # Not relevant.


class FishShellVisitor(ShellVisitor):
    """Serializes an Environment into a fish shell file."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._pathsep = ' '

    def _write_shell_header(self):
        pass

    def _write_shell_footer(self):
        pass

    def _remove_value_from_path(self, variable, value):
        return 'set PATH (string match -v {value} ${variable})\n'.format(
            variable=variable, value=value
        )

    def visit_set(self, set):  # pylint: disable=redefined-builtin
        value = self._apply_replacements(set)
        if set.override_var:
            self._outs.write(
                'set -x {name} {value}\n'
                'if set -q {override}; and test -n "${override}"\n'
                '  set -x {name} ${override}\n'
                '  set -e {override}\n'
                'end\n'.format(
                    name=set.name, override=set.override_var, value=value
                )
            )
        else:
            self._outs.write(
                'set -x {name} {value}\n'.format(name=set.name, value=value)
            )

    def visit_clear(self, clear):
        self._outs.write('set -e {name}\n'.format(**vars(clear)))

    def visit_remove(self, remove):
        value = self._apply_replacements(remove)
        self._remove_value_from_path(remove.name, value)

    def visit_prepend(self, prepend):
        value = self._apply_replacements(prepend)
        self._outs.write(
            'set -x --prepend {name} {value}\n'.format(
                name=prepend.name, value=value
            )
        )

    def visit_append(self, append):
        value = self._apply_replacements(append)
        self._outs.write(
            'set -x --append {name} {value}\n'.format(
                name=append.name, value=value
            )
        )

    def visit_echo(self, echo):
        self._outs.write('if not set -q PW_ENVSETUP_QUIET\n')
        if echo.newline:
            self._outs.write('  echo "{}"\n'.format(echo.value))
        else:
            self._outs.write('  echo -n "{}"\n'.format(echo.value))
        self._outs.write('end\n')

    def visit_hash(self, hash):  # pylint: disable=redefined-builtin
        del hash

    def visit_function(self, function):
        self._outs.write(
            'function {name}\n{body}\nend\n'.format(
                name=function.name, body=function.body
            )
        )

    def visit_command(self, command):
        self._outs.write('{}\n'.format(' '.join(command.command)))
        if not command.exit_on_error:
            return

        # Assume failing command produced relevant output.
        self._outs.write('if test $status -ne 0\n  return 1\nend\n')

    def visit_doctor(self, doctor):
        self._outs.write('if not set -q PW_ACTIVATE_SKIP_CHECKS\n')
        self.visit_command(doctor)
        self._outs.write('else\n')
        self.visit_echo(doctor.skip_echo)
        self._outs.write('end\n')


class DeactivateFishShellVisitor(FishShellVisitor):
    """Removes values from a fish shell environment."""

    def serialize(self, env, outs):
        _BaseShellVisitor.serialize(self, env, outs)

    def visit_set(self, set):  # pylint: disable=redefined-builtin
        if set.deactivate:
            self._outs.write('set -e {name}\n'.format(name=set.name))

    def visit_clear(self, clear):
        pass  # Not relevant.

    def visit_remove(self, remove):
        pass  # Not relevant.

    def visit_prepend(self, prepend):
        self._outs.write(
            self._remove_value_from_path(prepend.name, prepend.value)
        )

    def visit_append(self, append):
        self._outs.write(
            self._remove_value_from_path(append.name, append.value)
        )

    def visit_echo(self, echo):
        pass  # Not relevant.

    def visit_comment(self, comment):
        pass  # Not relevant.

    def visit_command(self, command):
        pass  # Not relevant.

    def visit_doctor(self, doctor):
        pass  # Not relevant.

    def visit_blank_line(self, blank_line):
        pass  # Not relevant.

    def visit_function(self, function):
        pass  # Not relevant.
