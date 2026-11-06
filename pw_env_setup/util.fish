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

# This script must be tested on fish 3.6.0

# Note: Colors are unfortunately duplicated in several places; and removing the
# duplication is not easy. Their locations are:
#
#   - bootstrap.sh
#   - pw_cli/color.py
#   - pw_env_setup/py/pw_env_setup/colors.py
#   - pw_env_setup/util.fish
#   - pw_env_setup/util.sh
#
# So please keep them matching then modifying them.
function pw_none
    set_color normal
    echo -e $argv
end

function pw_red
    set_color red
    echo -e $argv
    set_color normal
end

function pw_bold_red
    set_color --bold red
    echo -e $argv
    set_color normal
end

function pw_yellow
    set_color yellow
    echo -e $argv
    set_color normal
end

function pw_bold_yellow
    set_color --bold yellow
    echo -e $argv
    set_color normal
end

function pw_green
    set_color green
    echo -e $argv
    set_color normal
end

function pw_bold_green
    set_color --bold green
    echo -e $argv
    set_color normal
end

function pw_blue
    set_color blue
    echo -e $argv
    set_color normal
end

function pw_cyan
    set_color cyan
    echo -e $argv
    set_color normal
end

function pw_magenta
    set_color magenta
    echo -e $argv
    set_color normal
end

function pw_bold_white
    set_color --bold white
    echo -e $argv
    set_color normal
end

function pw_error
    set_color --bold brred
    echo -e $argv 1>&2
    set_color normal
end

function pw_error_info
    set_color red
    echo -e $argv 1>&2
    set_color normal
end

# Print an error and exit if $is_sourced == 0 (the first argument).
function pw_eval_sourced --argument-names is_sourced bootstrap_path
    if test $is_sourced -eq 0
        set _PW_NAME (basename $bootstrap_path)
        pw_error "Error: Attempting to $_PW_NAME in a subshell"
        pw_error_info "  Since $_PW_NAME modifies your shell's environment"
        pw_error_info "  variables, it must be sourced rather than executed. In"
        pw_error_info "  particular, 'fish $_PW_NAME' will not work since the "
        pw_error_info "  modified environment will get destroyed at the end of the"
        pw_error_info "  script. Instead, source the script's contents in your"
        pw_error_info "  shell:"
        pw_error_info ""
        pw_error_info "    \$ source $_PW_NAME"
        exit 1
    end
end

# Check for spaces in $_PW_ROOT (the first argument).
function pw_check_root --argument-names _PW_ROOT
    if string match --quiet '* *' $_PW_ROOT
        pw_error "Error: The Pigweed path contains spaces"
        pw_error_info "  The path '$_PW_ROOT' contains spaces. "
        pw_error_info "  Pigweed's Python environment currently requires Pigweed to"
        pw_error_info "  be at a path without spaces. Please checkout Pigweed in a"
        pw_error_info "  directory without spaces and retry running bootstrap."
        return -1
    end
end

# Check for and return the environment directory location.
function pw_get_env_root --argument-names script_name
    # PW_ENVIRONMENT_ROOT allows callers to specify where the environment should
    # be installed. bootstrap.sh scripts should not use that variable to store the
    # result of this function. This separation allows scripts to assume
    # PW_ENVIRONMENT_ROOT came from the caller and not from a previous bootstrap
    # possibly from another workspace. PW_ENVIRONMENT_ROOT will be cleared after
    # environment setup completes.
    if set --query PW_ENVIRONMENT_ROOT
        echo $PW_ENVIRONMENT_ROOT
        return
    end

    # Determine project-level root directory.
    if set --query PW_PROJECT_ROOT
        set _PW_ENV_PREFIX $PW_PROJECT_ROOT
    else
        set _PW_ENV_PREFIX $PW_ROOT
    end

    # If <root>/environment exists, use it. Otherwise, if <root>/.environment
    # exists, use it. Finally, use <root>/environment.
    set _PW_DOTENV $_PW_ENV_PREFIX/.environment
    set _PW_ENV $_PW_ENV_PREFIX/environment

    if test -d $_PW_DOTENV
        if test -d $_PW_ENV
            pw_error "Error: both possible environment directories exist."
            pw_error_info "  $_PW_DOTENV"
            pw_error_info "  $_PW_ENV"
            pw_error_info "  If only one of these folders exists it will be used for"
            pw_error_info "  the Pigweed environment. If neither exists"
            pw_error_info "  '<...>/environment' will be used. Since both exist,"
            pw_error_info "  bootstrap doesn't know which to use. Please delete one"
            pw_error_info "  or both and rerun bootstrap."
            exit 1
        end
    end

    if test -d $_PW_ENV
        echo $_PW_ENV
    else if test -d $_PW_DOTENV
        echo $_PW_DOTENV
    else
        # Neither exists. In a git worktree, activate.fish reuses the primary
        # checkout's bootstrapped environment instead of creating a new one;
        # bootstrap.fish always creates a local environment.
        set -l _pw_shared_env
        if contains -- "$script_name" activate.fish activate
            set _pw_shared_env (_pw_shared_env_root "$_PW_ENV_PREFIX")
        end
        if test -n "$_pw_shared_env"
            echo $_pw_shared_env
        else
            echo $_PW_ENV
        end
    end
end

# Prints the primary checkout's environment directory when $_PW_ENV_PREFIX is a
# git worktree that shares one, and nothing otherwise.
function _pw_shared_env_root --argument-names _PW_ENV_PREFIX
    # Linked worktrees and submodules both use a .git file; --git-common-dir
    # resolves to <primary>/.git only for worktrees of a standard checkout.
    if not test -f "$_PW_ENV_PREFIX/.git"
        return
    end
    set -l _pw_common_dir (env GIT_CONFIG_NOSYSTEM=1 GIT_OPTIONAL_LOCKS=0 git -C "$_PW_ENV_PREFIX" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
    if not string match --quiet '*/.git' "$_pw_common_dir"
        return
    end
    set -l _pw_common_root (string replace --regex '/\.git$' '' "$_pw_common_dir")
    for _pw_shared_env in "$_pw_common_root/environment" "$_pw_common_root/.environment"
        if test -f "$_pw_shared_env/activate.fish"
            echo $_pw_shared_env
            return
        end
    end
end


set --export _PW_BANNER "\
 ▒█████▄   █▓  ▄███▒  ▒█    ▒█ ░▓████▒ ░▓████▒ ▒▓████▄
  ▒█░  █░ ░█▒ ██▒ ▀█▒ ▒█░ █ ▒█  ▒█   ▀  ▒█   ▀  ▒█  ▀█▌
  ▒█▄▄▄█░ ░█▒ █▓░ ▄▄░ ▒█░ █ ▒█  ▒███    ▒███    ░█   █▌
  ▒█▀     ░█░ ▓█   █▓ ░█░ █ ▒█  ▒█   ▄  ▒█   ▄  ░█  ▄█▌
  ▒█      ░█░ ░▓███▀   ▒█▓▀▓█░ ░▓████▒ ░▓████▒ ▒▓████▀
"

function _default_pw_banner_func
    pw_magenta $_PW_BANNER
end

set --export _PW_BANNER_FUNC _default_pw_banner_func

function _pw_hello --argument-names _PW_TEXT
    # If $PW_BANNER_FUNC is defined, use that as the banner function
    # instead of the default.
    if set --query PW_BANNER_FUNC
        set _PW_BANNER_FUNC $PW_BANNER_FUNC
    end
    # Display the banner unless PW_ENVSETUP_QUIET or
    # PW_ENVSETUP_NO_BANNER is set.
    if test -z "$PW_ENVSETUP_QUIET" && test -z "$PW_ENVSETUP_NO_BANNER"
        pw_green "\n  WELCOME TO...\n"
        set_color normal
        $_PW_BANNER_FUNC
        set_color normal
        pw_green $_PW_TEXT
    end
end

function pw_deactivate
    # Assume PW_ROOT and PW_PROJECT_ROOT have already been set and we need to
    # preserve their values.
    set _NEW_PW_ROOT $PW_ROOT
    set _NEW_PW_PROJECT_ROOT $PW_PROJECT_ROOT

    # Find deactivate script, run it, and then delete it. This way if the
    # deactivate script is doing something wrong subsequent bootstraps still
    # have a chance to pass.
    set _PW_DEACTIVATE_SH $_PW_ACTUAL_ENVIRONMENT_ROOT/deactivate.fish
    if test -f $_PW_DEACTIVATE_SH
        . $_PW_DEACTIVATE_SH
        rm -f $_PW_DEACTIVATE_SH &>/dev/null
    end

    # If there's a _pw_deactivate function run it. Redirect output to /dev/null
    # in case _pw_deactivate doesn't exist. Remove _pw_deactivate when complete.
    if functions --query _pw_deactivate
        _pw_deactivate >/dev/null 2>/dev/null
        functions -e _pw_deactivate
    end

    # Restore.
    set --export PW_ROOT $_NEW_PW_ROOT
    set --export PW_PROJECT_ROOT $_NEW_PW_PROJECT_ROOT
end

function deactivate
    pw_deactivate
    functions -e pw_deactivate
    functions -e deactivate
    set -e PW_ROOT
    set -e PW_PROJECT_ROOT
    set -e PW_BRANDING_BANNER
    set -e PW_BRANDING_BANNER_COLOR
end

# The next three functions use the following variables.
# * PW_BANNER_FUNC: function to print banner
# * PW_BOOTSTRAP_PYTHON: specific Python interpreter to use for bootstrap
# * PW_ROOT: path to Pigweed root
# * PW_ENVSETUP_QUIET: limit output if "true"
#
# All arguments passed in are passed on to env_setup.py in pw_bootstrap,
# pw_activate takes no arguments, and pw_finalize takes the name of the script
# "bootstrap" or "activate" and the path to the setup script written by
# bootstrap.fish.
function pw_bootstrap
    _pw_hello "  BOOTSTRAP! Bootstrap may take a few minutes; please be patient.\n"

    for cmd in python python3
        if functions --query $cmd
            pw_error "Error: '$cmd' is an alias"
            pw_error_info "The shell has a '$cmd' alias set. This causes many obscure"
            pw_error_info "Python-related issues both in and out of Pigweed. Please"
            pw_error_info "remove the Python alias from your shell init file or at"
            pw_error_info "least run the following command before bootstrapping"
            pw_error_info "Pigweed."
            pw_error_info
            pw_error_info "  functions --erase $cmd"
            pw_error_info
            set _PW_ENV_SETUP_STATUS 1
            return
        end
    end

    # Allow forcing a specific version of Python for testing pursposes.
    if test -n "$PW_BOOTSTRAP_PYTHON"
        set _PW_PYTHON "$PW_BOOTSTRAP_PYTHON"
    else if command -v python3 >/dev/null 2>/dev/null
        set _PW_PYTHON python3
    else if command -v python >/dev/null 2>/dev/null
        set _PW_PYTHON python
    else
        pw_error "Error: No system Python present\n"
        pw_error_info "  Pigweed's bootstrap process requires a local system"
        pw_error_info "  Python. Please install Python on your system, add it to "
        pw_error_info "  your PATH and re-try running bootstrap."
        set _PW_ENV_SETUP_STATUS 1
        return
    end

    if test -n "$_PW_ENV_SETUP"
        $_PW_ENV_SETUP $argv
        set _PW_ENV_SETUP_STATUS $status
    else
        $_PW_PYTHON $PW_ROOT/pw_env_setup/py/pw_env_setup/env_setup.py $argv
        set _PW_ENV_SETUP_STATUS $status
    end

    # Write the directory path at bootstrap time into the directory. This helps
    # us double-check things are still in the same space when calling activate.
    set _PW_ENV_ROOT_TXT $_PW_ACTUAL_ENVIRONMENT_ROOT/env_root.txt
    echo $_PW_ACTUAL_ENVIRONMENT_ROOT >$_PW_ENV_ROOT_TXT 2>/dev/null

    # Create the environment README file. Use quotes to prevent alias expansion.
    cp $PW_ROOT/pw_env_setup/destination.md $_PW_ACTUAL_ENVIRONMENT_ROOT/README.md &>/dev/null
end

function pw_activate_message
    _pw_hello "  ACTIVATOR! This sets your shell environment variables.\n"
    set _PW_ENV_SETUP_STATUS 0
end

function pw_finalize_pre_check --argument-names _PW_NAME _PW_SETUP_SH
    # Check that the environment directory agrees that the path it's at matches
    # where it thinks it should be. If not, bail.
    set _PW_ENV_ROOT_TXT "$_PW_ACTUAL_ENVIRONMENT_ROOT/env_root.txt"
    if test -f $_PW_ENV_ROOT_TXT
        set _PW_PREV_ENV_ROOT (cat $_PW_ENV_ROOT_TXT)
        if test "$_PW_ACTUAL_ENVIRONMENT_ROOT" != "$_PW_PREV_ENV_ROOT"
            pw_error "Error: Environment directory moved"
            pw_error_info "This Pigweed environment was created at"
            pw_error_info
            pw_error_info "    $_PW_PREV_ENV_ROOT"
            pw_error_info
            pw_error_info "But it is now being activated from"
            pw_error_info
            pw_error_info "    $_PW_ACTUAL_ENVIRONMENT_ROOT"
            pw_error_info
            pw_error_info "This is likely because the checkout moved. After moving "
            pw_error_info "the checkout a full '. ./bootstrap.fish' is required."
            pw_error_info
            set _PW_ENV_SETUP_STATUS 1
        end
    end

    if test -n "$_PW_ENV_SETUP_STATUS" && test "$_PW_ENV_SETUP_STATUS" -ne 0
        return
    end

    if not test -f "$_PW_SETUP_SH"
        pw_error "Error during $_PW_NAME--see messages above."
    else
        set -gx _PW_ACTIVATE_ROOT "$PW_ROOT"
        set -gx _PW_ACTIVATE_PROJECT_ROOT "$PW_PROJECT_ROOT"
    end
end

function pw_finalize_post_check --argument-names _PW_NAME _PW_SETUP_SH
    set -l _pw_source_status $status
    set -e _PW_ACTIVATE_ROOT _PW_ACTIVATE_PROJECT_ROOT
    if test "$_pw_source_status" -eq 0
        if test "$_PW_NAME" = bootstrap && test -z "$PW_ENVSETUP_QUIET"
            echo "To reactivate this environment in the future, run this in your "
            echo "terminal:"
            echo
            pw_green "  source ./activate.fish"
            echo
            echo "To deactivate this environment, run this:"
            echo
            pw_green "  deactivate"
            echo
        end
    else
        pw_error "Error during $_PW_NAME--see messages above."
    end
end

function pw_install_post_checkout_hook
    cp $PW_ROOT/pw_env_setup/post-checkout-hook.sh $PW_PROJECT_ROOT/.git/hooks/post-checkout
end

function pw_cleanup
    set -e _PW_BANNER
    set -e _PW_BANNER_FUNC
    set -e PW_BANNER_FUNC
    set -e _PW_ENV_SETUP
    set -e _PW_NAME
    set -e PW_ENVIRONMENT_ROOT
    set -e _PW_PYTHON
    set -e _PW_ENV_ROOT_TXT
    set -e _PW_PREV_ENV_ROOT
    set -e _PW_SETUP_SH
    set -e _PW_DEACTIVATE_SH
    set -e _NEW_PW_ROOT
    set -e _PW_ENV_SETUP_STATUS
    set -e _PW_ENV_PREFIX
    set -e _PW_ENV
    set -e _PW_DOTENV

    functions -e pw_none
    functions -e pw_red
    functions -e pw_bold_red
    functions -e pw_yellow
    functions -e pw_bold_yellow
    functions -e pw_green
    functions -e pw_bold_green
    functions -e pw_blue
    functions -e pw_cyan
    functions -e pw_magenta
    functions -e pw_bold_white
    functions -e pw_eval_sourced
    functions -e pw_check_root
    functions -e pw_get_env_root
    functions -e _pw_shared_env_root
    functions -e _pw_banner
    functions -e pw_bootstrap
    functions -e pw_activate
    functions -e pw_finalize
    functions -e pw_install_post_checkout_hook
    functions -e pw_cleanup
    functions -e _pw_hello
    functions -e pw_error
    functions -e pw_error_info
end
