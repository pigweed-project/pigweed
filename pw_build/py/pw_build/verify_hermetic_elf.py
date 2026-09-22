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
"""Verifies hermetic library constraints on an input ELF object file."""

import argparse
import fnmatch
from pathlib import Path
import re
import sys
from elftools.elf.elffile import ELFFile  # type: ignore
from elftools.elf.sections import SymbolTableSection  # type: ignore


def _is_undefined_symbol(symbol) -> bool:
    """Returns True if the symbol is undefined and non-empty."""
    return symbol.entry["st_shndx"] == "SHN_UNDEF" and bool(symbol.name)


def _parse_args() -> argparse.Namespace:
    """Parses command-line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--elf-file",
        type=Path,
        required=True,
        help="Input ELF object file",
    )
    parser.add_argument(
        "--stamp-file",
        type=Path,
        required=True,
        help="Output stamp file",
    )
    parser.add_argument(
        "--allow-init",
        action="store_true",
        default=False,
        help="Allow SHT_INIT_ARRAY sections (global constructors)",
    )
    parser.add_argument(
        "--allow-fini",
        action="store_true",
        default=False,
        help="Allow SHT_FINI_ARRAY sections (global destructors)",
    )
    parser.add_argument(
        "--allowed-symbol",
        action="append",
        type=lambda s: re.compile(fnmatch.translate(s)),
        dest="allowed_sym_patterns",
        default=[],
        help="Allowed undefined symbol name or pattern (supports fnmatch)",
    )
    parser.add_argument(
        "--label",
        default="",
        help="Bazel target label for error messages",
    )
    return parser.parse_args()


def main() -> None:
    """Verifies hermetic library constraints on the target ELF object file."""
    args = _parse_args()
    errors = []

    with open(args.elf_file, "rb") as f:
        elf = ELFFile(f)
        undef_syms = set()

        for section in elf.iter_sections():
            sh_type = section.header["sh_type"]
            if not args.allow_init and sh_type == "SHT_INIT_ARRAY":
                errors.append(
                    f"Disallowed SHT_INIT_ARRAY section '{section.name}'"
                    " (global constructor)"
                )
            if not args.allow_fini and sh_type == "SHT_FINI_ARRAY":
                errors.append(
                    f"Disallowed SHT_FINI_ARRAY section '{section.name}'"
                    " (global destructor)"
                )
            if isinstance(section, SymbolTableSection):
                for symbol in section.iter_symbols():
                    if _is_undefined_symbol(symbol):
                        undef_syms.add(symbol.name)

        def is_allowed_symbol(sym: str) -> bool:
            return any(pat.match(sym) for pat in args.allowed_sym_patterns)

        forbidden_undefs = {
            sym for sym in undef_syms if not is_allowed_symbol(sym)
        }
        if forbidden_undefs:
            formatted = "\n    ".join(sorted(forbidden_undefs))
            errors.append(f"Forbidden undefined symbols:\n    {formatted}")

    if errors:
        target_str = f" for {args.label}" if args.label else ""
        print(
            f"ERROR: Failed hermetic verification{target_str}:", file=sys.stderr
        )
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        sys.exit(1)

    args.stamp_file.touch()


if __name__ == "__main__":
    main()
