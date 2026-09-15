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
"""Demo freshness scanner for repository freshness policies."""

import argparse
import asyncio
from pathlib import Path
import sys

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.bazel_cipd import BazelCipdAnalyzer
from pw_fortifier.bazel_dep import BazelDepAnalyzer
from pw_fortifier.bazel_maven import BazelMavenAnalyzer
from pw_fortifier.cargo import CargoAnalyzer
from pw_fortifier.cipd_setup import CipdSetupAnalyzer
from pw_fortifier.copybara import CopybaraAnalyzer
from pw_fortifier.demo_issue_tracker import DemoIssueTracker
from pw_fortifier.freshness_scanner import FreshnessScanner
from pw_fortifier.git_utils import get_git_repo_url
from pw_fortifier.go_mod import GoModAnalyzer
from pw_fortifier.npm import NpmAnalyzer
from pw_fortifier.pip import PipAnalyzer


class DemoFreshnessScanner(FreshnessScanner):
    """Demo freshness scanner with local issue tracker and analyzers."""

    def __init__(self, name: str) -> None:
        """Initializes DemoFreshnessScanner and registers all analyzers.

        Args:
            name: Program name string.
        """
        super().__init__(name)
        self.repo_url = get_git_repo_url()
        self.register(BazelCipdAnalyzer())
        self.register(BazelDepAnalyzer())
        self.register(BazelMavenAnalyzer())
        self.register(CargoAnalyzer())
        self.register(CipdSetupAnalyzer())
        self.register(CopybaraAnalyzer())
        self.register(GoModAnalyzer())
        self.register(NpmAnalyzer())
        self.register(PipAnalyzer())

    def _parse_args(self, *cli_args) -> argparse.Namespace:
        """Parses arguments and ensures uploads are disabled for demos.

        Args:
            *cli_args: Command line arguments.

        Returns:
            Parsed arguments namespace with allow_uploads set to False.
        """
        args = super()._parse_args(*cli_args)
        args.allow_uploads = False
        return args

    def _instantiate(self, args: argparse.Namespace) -> None:
        """Instantiates DemoIssueTracker and pipeline stages.

        Args:
            args: Command line arguments namespace.
        """
        self.issue_tracker = DemoIssueTracker(AsyncPath(args.working_dir))
        super()._instantiate(args)


def main() -> None:
    """Main CLI entry point for DemoFreshnessScanner."""
    prog_name = Path(sys.argv[0]).stem
    scanner = DemoFreshnessScanner(prog_name)
    asyncio.run(scanner.run(*sys.argv[1:]))


if __name__ == '__main__':
    main()
