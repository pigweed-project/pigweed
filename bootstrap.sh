# Copyright 2020 The Pigweed Authors
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

# This script must be tested on bash, zsh, and dash.

# This must be POSIX compliant to maximize portability. i.e. do NOT use
# `realpath` or `readlink`.
_bootstrap_abspath () {
  # Since this is sourced, use a subshell (...) to ensure the 'cd' command does
  # not change the directory of the current shell context.
  (
    cd "$(dirname "$1")" 2>/dev/null || return 1
    echo "$(pwd)/$(basename "$1")"
  )
}

# Shell: bash.
if test -n "$BASH"; then
  _PW_BOOTSTRAP_PATH="$(_bootstrap_abspath "$BASH_SOURCE")"
# Shell: zsh.
elif test -n "$ZSH_NAME"; then
  _PW_BOOTSTRAP_PATH="$(_bootstrap_abspath "${(%):-%N}")"
# Shell: dash.
elif test ${0##*/} = dash; then
  _PW_BOOTSTRAP_PATH="$(_bootstrap_abspath \
    "$(lsof -p $$ -Fn0 | tail -1 | sed 's#^[^/]*##;')")"
# If everything else fails, try $0. It could work.
else
  _PW_BOOTSTRAP_PATH="$(_bootstrap_abspath "$0")"
fi

# Check if this file is being executed or sourced.
_pw_sourced=0
if [ -n "$SWARMING_BOT_ID" ]; then
  # If set we're running on swarming and don't need this check.
  _pw_sourced=1
elif [ -n "$ZSH_EVAL_CONTEXT" ]; then
  case $ZSH_EVAL_CONTEXT in *:file) _pw_sourced=1;; esac
elif [ -n "$KSH_VERSION" ]; then
  [ "$(cd $(dirname -- $0) && pwd -P)/$(basename -- $0)" != \
    "$(cd $(dirname -- ${.sh.file}) && pwd -P)/$(basename -- ${.sh.file})" ] \
    && _pw_sourced=1
elif [ -n "$BASH_VERSION" ]; then
  (return 0 2>/dev/null) && _pw_sourced=1
else  # All other shells: examine $0 for known shell binary filenames
  # Detects `sh` and `dash`; add additional shell filenames as needed.
  case ${0##*/} in sh|dash) _pw_sourced=1;; esac
fi

# Downstream projects need to set something other than PW_ROOT here, like
# YOUR_PROJECT_ROOT. Please also set PW_PROJECT_ROOT and PW_ROOT before
# invoking pw_bootstrap or pw_activate.
######### BEGIN PROJECT-SPECIFIC CODE #########
PW_ROOT="$(dirname "$_PW_BOOTSTRAP_PATH")"

# Please also set PW_PROJECT_ROOT to YOUR_PROJECT_ROOT.
PW_PROJECT_ROOT="$PW_ROOT"

# Downstream projects may wish to set PW_BANNER_FUNC to a function that prints
# an ASCII art banner here.
########## END PROJECT-SPECIFIC CODE ##########
export PW_ROOT
export PW_PROJECT_ROOT
if [ -n "$PW_BANNER_FUNC" ]; then
  export PW_BANNER_FUNC
fi

. "$PW_ROOT/pw_env_setup/util.sh"

# Check environment properties
pw_deactivate
pw_eval_sourced "$_pw_sourced" "$_PW_BOOTSTRAP_PATH"
if ! pw_check_root "$PW_ROOT"; then
  return
fi

_PW_ACTUAL_ENVIRONMENT_ROOT="$(pw_get_env_root)"
export _PW_ACTUAL_ENVIRONMENT_ROOT
SETUP_SH="$_PW_ACTUAL_ENVIRONMENT_ROOT/activate.sh"

# Run full bootstrap when invoked as bootstrap, or env file is missing/empty.
if [ "$(basename "$_PW_BOOTSTRAP_PATH")" = "bootstrap.sh" ] || \
  [ ! -f "$SETUP_SH" ] || \
  [ ! -s "$SETUP_SH" ]; then
######### BEGIN PROJECT-SPECIFIC CODE #########
  pw_bootstrap --shell-file "$SETUP_SH" --install-dir "$_PW_ACTUAL_ENVIRONMENT_ROOT"
########## END PROJECT-SPECIFIC CODE ##########
  pw_finalize bootstrap "$SETUP_SH"
else
  pw_activate
  pw_finalize activate "$SETUP_SH"
fi

if [ "$_PW_ENV_SETUP_STATUS" -eq 0 ]; then
# This is where any additional checks about the environment should go.
######### BEGIN PROJECT-SPECIFIC CODE #########
  echo -n
########## END PROJECT-SPECIFIC CODE ##########
fi

unset _pw_sourced
unset _PW_BOOTSTRAP_PATH
unset SETUP_SH
unset _bootstrap_abspath

if [[ "$TERM" != dumb ]]; then
  # Shell completion: bash.
  if test -n "$BASH"; then
    . "$PW_ROOT/pw_cli/py/pw_cli/shell_completion/pw.bash"
    . "$PW_ROOT/pw_cli/py/pw_cli/shell_completion/pw_build.bash"
  # Shell completion: zsh.
  elif test -n "$ZSH_NAME"; then
    . "$PW_ROOT/pw_cli/py/pw_cli/shell_completion/pw.zsh"
    . "$PW_ROOT/pw_cli/py/pw_cli/shell_completion/pw_build.zsh"
  fi
fi

pw_cleanup

# Only write the setting when missing: writing takes .git/config.lock, which
# contends across concurrent activations (parallel shells, worktrees) and
# fails in read-only checkouts.
if GIT_OPTIONAL_LOCKS=0 git -C "$PW_ROOT" rev-parse --is-inside-work-tree > /dev/null 2>&1; then
  if [ "$(GIT_OPTIONAL_LOCKS=0 git -C "$PW_ROOT" config --get blame.ignoreRevsFile 2>/dev/null)" != ".git-blame-ignore-revs" ]; then
    git -C "$PW_ROOT" config blame.ignoreRevsFile .git-blame-ignore-revs 2>/dev/null || true
  fi
fi
