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
"""Internal Bazel helpers for downloading CIPD packages."""

load("@bazel_tools//tools/build_defs/repo:utils.bzl", "patch")
load(
    ":cipd_repository_list_templates.bzl",
    "CIPD_INIT_BZL_TEMPLATE",
    "CIPD_REPOSITORY_TEMPLATE",
)

_CIPD_HOST = "https://chrome-infra-packages.appspot.com"
_CIPD_CACHE_DIR_ENV_VAR = "CIPD_CACHE_DIR"

# NOTE: the ~ is for convenience here, it is manually expanded inside
# get_cipd_cache_dir().
_DEFAULT_CIPD_CACHE_DIR = "~/.cipd-cache-dir"

def platform_normalized(rctx):
    """Normalizes the platform to match CIPDs naming system.

    Args:
        rctx: Repository context.

    Returns:
        str: Normalized string.
    """

    # Chained if else used because Bazel's rctx.os.name is not stable
    # between different versions of windows i.e. windows 10 vs windows
    # server.
    if "windows" in rctx.os.name:
        return "windows"
    elif "linux" == rctx.os.name:
        return "linux"
    elif "mac os x" == rctx.os.name:
        return "mac"
    else:
        fail("Could not normalize os:", rctx.os.name)

def arch_normalized(rctx):
    """Normalizes the architecture string to match CIPDs naming system.

    Args:
        rctx: Repository context.

    Returns:
        str: Normalized architecture.
    """

    if "arm" in rctx.os.name or "aarch" in rctx.os.arch:
        return "arm64"

    return "amd64"

def get_client_cipd_version(rctx):
    """Gets the CIPD client version from the config file.

    Args:
        rctx: Repository context.

    Returns:
        str: The CIPD client version tag to use.
    """
    return rctx.read(rctx.attr._cipd_version_file).strip()

def _platform(rctx):
    """Generates a normalized platform string from the host OS/architecture.

    Args:
        rctx: Repository context.
    """
    return "{}-{}".format(platform_normalized(rctx), arch_normalized(rctx))

def get_client_cipd_digest(rctx):
    """Gets the CIPD client digest from the digest file.

    Args:
        rctx: Repository context.

    Returns:
        str: The CIPD client digest.
    """
    platform = _platform(rctx)
    digest_file = rctx.read(rctx.attr._cipd_digest_file)
    digest_lines = [
        digest
        for digest in digest_file.splitlines()
        # Remove comments from version file
        if not digest.startswith("#") and digest
    ]

    for line in digest_lines:
        (digest_platform, digest_type, digest) = \
            [element for element in line.split(" ") if element]
        if digest_platform == platform:
            if digest_type != "sha256":
                fail("Bazel only supports sha256 type digests.")
            return digest
    fail("Could not find CIPD digest that matches this platform.")

def get_cipd_cache_dir(rctx):
    """Returns the CIPD cache directory.

    This may return None if the CIPD cache directory could not be determined.

    Args:
        rctx: Repository context.

    Returns:
        str: The CIPD cache directory, or None if undetermined.
    """
    cipd_cache_dir = rctx.getenv(_CIPD_CACHE_DIR_ENV_VAR, None)
    if cipd_cache_dir != None:
        return cipd_cache_dir

    if "windows" in rctx.os.name:
        user_home = rctx.getenv("USERPROFILE")
    else:
        user_home = rctx.getenv("HOME")

    # If we can't locate $HOME, don't cache.
    if user_home == None:
        return None

    return _DEFAULT_CIPD_CACHE_DIR.replace("~", user_home)

def cipd_client_impl(rctx):
    """Initializes the CIPD client repository.

    Args:
        rctx: Repository context.
    """
    platform = _platform(rctx)
    path = "/client?platform={}&version={}".format(
        platform,
        get_client_cipd_version(rctx),
    )
    rctx.download(
        output = "cipd",
        url = _CIPD_HOST + path,
        sha256 = get_client_cipd_digest(rctx),
        executable = True,
    )
    rctx.file("BUILD", "exports_files([\"cipd\"], visibility = [\"//visibility:public\"])")

def cipd_repository_base(rctx, packages):
    """Populates the base contents of a CIPD repository.

    Args:
        rctx: Repository context.
        packages: List of package paths.
    """
    cipd_path = rctx.path(rctx.attr._cipd_client)
    ensure_path = rctx.name + ".ensure"

    if rctx.attr.tag_by_os:
        os = platform_normalized(rctx)
        if not os in rctx.attr.tag_by_os:
            fail("cipd_repository specifies tags, but no tag for current OS '{}'".format(os))
        tag = rctx.attr.tag_by_os[os]
    else:
        tag = rctx.attr.tag

    rctx.template(
        ensure_path,
        Label(str(Label("//pw_env_setup/bazel/cipd_setup:ensure.tpl"))),
        {
            "%{data}": "\n".join(["%s\t%s" % (pkg, tag) for pkg in packages]),
        },
    )

    cache_args = []
    cipd_cache_dir = get_cipd_cache_dir(rctx)
    if cipd_cache_dir:
        cache_args = [
            "-cache-dir",
            cipd_cache_dir,
        ]

    result = rctx.execute(
        [
            cipd_path,
            "ensure",
            "-root",
            ".",
            "-ensure-file",
            ensure_path,
        ] + cache_args,
    )

    if result.return_code != 0:
        fail("Failed to fetch CIPD repository `{}`:\n{}".format(rctx.name, result.stderr))

def _cipd_repository_impl(rctx, packages):
    """Generates an external repository from a CIPD package.

    Args:
        rctx: Repository context.
        packages: List of package paths.
    """
    cipd_repository_base(rctx, packages)
    patch(rctx)

    # Allow the BUILD file to be overriden in the generated repository.
    # If unspecified, default to a BUILD file that exposes all of its files.
    if rctx.attr.build_file:
        rctx.file("BUILD", rctx.read(rctx.attr.build_file))
    elif not rctx.path("BUILD").exists and not rctx.path("BUILD.bazel").exists:
        rctx.file("BUILD", """
exports_files(
    glob(["**"]),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "all",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
  """)

def cipd_repository_impl(rctx):
    """Generates an external repository from a CIPD package.

    Args:
        rctx: Repository context.
    """
    _cipd_repository_impl(rctx, [rctx.attr.path])

def cipd_composite_repository_impl(rctx):
    """Generates an external repository from list of CIPD packages.

    Args:
        rctx: Repository context.
    """
    _cipd_repository_impl(rctx, rctx.attr.packages)

def _cipd_path_to_repository_name(path, platform):
    """ Converts a cipd path to a repository name

    Args:
        path: The cipd path.
        platform: The cipd platform name.

    Example:
        print(_cipd_path_to_repository_name(
            "infra/3pp/tools/cpython3/windows-amd64",
            "linux-amd64"
        ))
        >> cipd_infra_3pp_tools_cpython3_windows_amd64
    """
    return "cipd_" + \
           path.replace("/", "_") \
               .replace("${platform}", platform) \
               .replace("-", "_")

def _cipd_dep_to_cipd_repositories_str(dep, indent):
    """ Converts a CIPD dependency to a CIPD repositories string

    Args:
        dep: The CIPD dependency.
        indent: The indentation to use.
    """
    return "\n".join([CIPD_REPOSITORY_TEMPLATE.format(
        name = _cipd_path_to_repository_name(dep["path"], platform),
        path = dep["path"].replace("${platform}", platform),
        tag = dep["tags"][0],
        indent = indent,
    ) for platform in dep["platforms"]])

def cipd_deps_impl(repository_ctx):
    """ Generates a CIPD dependencies file """
    pigweed_deps = json.decode(
        repository_ctx.read(repository_ctx.attr._pigweed_packages_json),
    )["packages"]
    repository_ctx.file("BUILD", "exports_files(glob([\"**/*\"]), visibility = [\"//visibility:public\"])\n")

    repository_ctx.file("cipd_init.bzl", CIPD_INIT_BZL_TEMPLATE.format(
        cipd_deps = "\n".join([
            _cipd_dep_to_cipd_repositories_str(dep, indent = "    ")
            for dep in pigweed_deps
        ]),
    ))
