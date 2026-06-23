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
"""Binary to scan packages and output their freshness as CSV."""

import argparse
import csv
import os
import sys
from datetime import date

from pw_fortifier.cargo_scanner import CargoScanner
from pw_fortifier.go_mod_scanner import GoModScanner
from pw_fortifier.package_scanner import (
    PackageScannerRegistry,
    TIER0_ON_DEVICE,
    TIER1_TOOLCHAIN,
    TIER2_DEVHOST,
    TIER3_UPSTREAM,
)


def main() -> None:
    """Scans packages and outputs freshness as CSV."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '-r',
        '--root',
        default=os.getenv('PW_PROJECT_ROOT', os.getcwd()),
        help='Root directory to scan (default: current directory)',
    )
    parser.add_argument(
        '-o',
        '--output',
        help='Output CSV file (default: stdout)',
    )
    args = parser.parse_args()

    registry = PackageScannerRegistry(args.root)
    for d in ('out', 'environment', '.environment', '.git'):
        registry.prune(d)

    # Register all leaf concrete scanners
    registry.register(CargoScanner())
    registry.register(GoModScanner())

    out_file = sys.stdout
    if args.output:
        out_file = open(args.output, 'w', newline='', encoding='utf-8')

    try:
        writer = csv.writer(out_file)
        writer.writerow(
            [
                'package',
                'tier',
                'tier_desc',
                'type',
                'freshness',
                'freshness_desc',
                'current_ver',
                'current_ts',
                'earliest_ver',
                'earliest_ts',
                'earliest_age',
                'owner',
                'source',
            ]
        )

        for res in registry.scan_all():
            if res.current.version == res.earliest.version:
                freshness = 100
            else:
                freshness = -(
                    res.earliest.timestamp - res.current.timestamp
                ).days

            if freshness == 100:
                freshness_desc = 'Fresh'
            elif freshness >= 0:
                freshness_desc = f'{freshness} days left'
            else:
                freshness_desc = f'{abs(freshness)} days overdue'

            if res.tier == TIER0_ON_DEVICE:
                tier_desc = 'on device'
            elif res.tier == TIER1_TOOLCHAIN:
                tier_desc = 'used in build'
            elif res.tier == TIER2_DEVHOST:
                tier_desc = 'dev tool'
            elif res.tier == TIER3_UPSTREAM:
                tier_desc = 'upstream-only'
            else:
                tier_desc = 'unknown'

            writer.writerow(
                [
                    res.package,
                    res.tier,
                    tier_desc,
                    res.pkg_type,
                    freshness,
                    freshness_desc,
                    res.current.version,
                    res.current.timestamp.isoformat(),
                    res.earliest.version,
                    res.earliest.timestamp.isoformat(),
                    (date.today() - res.earliest.timestamp).days,
                    res.owner or '',
                    res.source,
                ]
            )
    finally:
        if out_file is not sys.stdout:
            out_file.close()


if __name__ == '__main__':
    main()
