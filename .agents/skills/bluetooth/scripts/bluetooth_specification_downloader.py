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

"""Downloads and formats Bluetooth core specification documents."""

import os
import re
import sys
from typing import Optional
from urllib.request import urlopen, Request
from urllib.error import URLError
from urllib.parse import urlparse
from bs4 import BeautifulSoup, Tag

# Root project directory for specification cache
SPEC_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "specifications",
)

# Hardcoded list of official Bluetooth Core Specification URLs
CORE_SPEC_URLS = [
    (
        "https://www.bluetooth.com/wp-content/uploads/Files/Specification/"
        "HTML/Core-62/out/en/host-controller-interface/"
        "host-controller-interface-functional-specification.html"
    ),
    (
        "https://www.bluetooth.com/wp-content/uploads/Files/Specification/"
        "HTML/Core-62/out/en/host/"
        "logical-link-control-and-adaptation-protocol-specification.html"
    ),
    (
        "https://www.bluetooth.com/wp-content/uploads/Files/Specification/"
        "HTML/Core-62/out/en/host/attribute-protocol--att-.html"
    ),
    (
        "https://www.bluetooth.com/wp-content/uploads/Files/Specification/"
        "HTML/Core-62/out/en/host/generic-access-profile.html"
    ),
    (
        "https://www.bluetooth.com/wp-content/uploads/Files/Specification/"
        "HTML/Core-62/out/en/host/generic-attribute-profile--gatt-.html"
    ),
    (
        "https://www.bluetooth.com/wp-content/uploads/Files/Specification/"
        "HTML/Core-62/out/en/host/security-manager-specification.html"
    ),
    (
        "https://www.bluetooth.com/wp-content/uploads/Files/Specification/"
        "HTML/Core-62/out/en/br-edr-controller/"
        "link-manager-protocol-specification.html"
    ),
    (
        "https://www.bluetooth.com/wp-content/uploads/Files/Specification/"
        "HTML/Core-62/out/en/low-energy-controller/"
        "link-layer-specification.html"
    ),
]


def fetch_html(url: str) -> Optional[str]:
    """Downloads HTML content from the given URL."""
    print(f"Downloading: {url}...")
    headers = {'User-Agent': 'Mozilla/5.0'}
    try:
        req = Request(url, headers=headers)
        with urlopen(req, timeout=30) as response:
            return response.read().decode('utf-8')
    except URLError as e:
        print(f"Error downloading {url}: {e}")
        return None


def extract_metadata_header(soup: BeautifulSoup, url: str) -> str:
    """Extracts the document title and formats an initial metadata header."""
    meta_header = f"Source URL: {url}\n"
    title_div = soup.find('div', class_='titlepage')

    if title_div:
        title_element = title_div.find(class_='title')
        raw_title_source = title_element if title_element else title_div
        raw_title = raw_title_source.get_text(separator=' ', strip=True)
        # Remove internal SIG codenames/revisions like 'vAtlanta r00'
        clean_title = re.sub(r'\bv[A-Za-z]+\s+r\d{2,}\b', '', raw_title)
        # Clean up any double spaces left behind
        clean_title = re.sub(r'\s{2,}', ' ', clean_title).strip()

        meta_header += clean_title + "\n"
        meta_header += "=" * max(10, len(clean_title)) + "\n\n"

    return meta_header


def stitch_headers(content_root: Tag) -> None:
    """Combines section numbers and titles into single Markdown headers."""
    for header in content_root.find_all(['h1', 'h2', 'h3', 'h4', 'h5', 'h6']):
        num = header.find("span", class_="formal-number")
        title = header.find("span", class_="formal-title")

        if num and title:
            # Prefix with Markdown header hashes based on heading level
            level = int(header.name[1])
            hashes = "#" * level
            stitched = (
                f"{hashes} {num.get_text().strip()}. {title.get_text().strip()}"
            )
            header.clear()
            header.append(stitched)


def convert_table_to_markdown(table: Tag) -> str:
    """Converts a BeautifulSoup table element into a Markdown table string."""
    rows = []
    max_cols = 0
    for tr in table.find_all('tr'):
        cells = []
        for cell in tr.find_all(['th', 'td']):
            # Clean up cell text: remove extra newlines/spaces and escape pipes
            text = (
                cell.get_text(separator=' ', strip=True)
                .replace('|', '\\|')
                .replace('\n', ' ')
            )
            cells.append(text)

        max_cols = max(max_cols, len(cells))
        if cells:
            rows.append(f"| {' | '.join(cells)} |")

    if not rows:
        return ""

    # Create the separator row
    if max_cols > 0:
        separator = f"|{'---|' * max_cols}"
        # Insert after header if possible
        if len(rows) > 1:
            rows.insert(1, separator)
        else:
            rows.append(separator)

    return '\n\n' + '\n'.join(rows) + '\n\n'


def replace_tables_with_markdown(
    content_root: Tag, soup: BeautifulSoup
) -> None:
    """Finds all tables in the HTML and replaces them with Markdown text."""
    for table in content_root.find_all('table'):
        md_table = convert_table_to_markdown(table)
        table.replace_with(soup.new_string(md_table))


def extract_clean_text(meta_header: str, content_root: Tag) -> str:
    """Extracts text content and cleans up excessive newlines."""
    text_content = content_root.get_text(separator='\n', strip=True)
    # Remove internal SIG codenames/revisions like 'vAtlanta r00'
    # (on its own line)
    text_content = re.sub(r'(?im)^v[a-z]+\s+r\d{2,}$\n?', '', text_content)
    full_text = meta_header + text_content
    return re.sub(r'\n{3,}', '\n\n', full_text)


def process_spec(url: str) -> None:
    """
    Coordinates the process of downloading, parsing, and saving a specification.
    """
    sys.setrecursionlimit(10000)

    # Determine filenames
    parsed_url = urlparse(url)
    page_name = os.path.basename(parsed_url.path).replace('.html', '')
    raw_path = os.path.join(SPEC_DIR, f"{page_name}.html")
    pretty_path = os.path.join(SPEC_DIR, f"{page_name}_pretty.html")
    md_path = os.path.join(SPEC_DIR, f"{page_name}.md")

    # Ensure output directory exists
    os.makedirs(SPEC_DIR, exist_ok=True)

    if (
        os.path.exists(md_path)
        and os.path.exists(raw_path)
        and os.path.exists(pretty_path)
    ):
        print(f"Skipping cached spec: {page_name}")
        return

    html_content = fetch_html(url)
    if not html_content:
        return
    # Save raw HTML
    with open(raw_path, 'w', encoding='utf-8') as f:
        f.write(html_content)

    soup = BeautifulSoup(html_content, 'html.parser')

    # Save prettified HTML
    with open(pretty_path, 'w', encoding='utf-8') as f:
        f.write(soup.prettify())

    meta_header = extract_metadata_header(soup, url)

    main_article = soup.find('article', class_='topic')
    content_root = main_article if main_article else soup

    stitch_headers(content_root)
    replace_tables_with_markdown(content_root, soup)
    text_content = extract_clean_text(meta_header, content_root)

    # Save Markdown
    with open(md_path, 'w', encoding='utf-8') as f:
        f.write(text_content)
    print(f"Generated enhanced Markdown: {md_path}")


def generate_index() -> None:
    """Scans all .md files and generates a global index.md."""
    print("\nGenerating Section Index...")
    index_entries = []

    # Regex to find lines starting with section numbers (prefixed with hashes)
    section_pattern = re.compile(r'^(#+)\s+(\d+(\.\d+)*\.)\s+(.+)$')

    for filename in sorted(os.listdir(SPEC_DIR)):
        if filename.endswith(".md") and filename != "index.md":
            filepath = os.path.join(SPEC_DIR, filename)
            spec_name = ""

            with open(filepath, 'r', encoding='utf-8') as f:
                for line_num, line in enumerate(f, 1):
                    stripped = line.strip()
                    # The second line is the spec title
                    if line_num == 2 and not spec_name:
                        spec_name = stripped

                    match = section_pattern.match(stripped)
                    if match:
                        section_num = match.group(2)
                        title = match.group(4)
                        index_entries.append(
                            f"| {section_num} | {title} | {spec_name} | "
                            f"{filename}:{line_num} |"
                        )

    index_path = os.path.join(SPEC_DIR, "index.md")
    with open(index_path, 'w', encoding='utf-8') as f:
        f.write("# Bluetooth Core Specification Index\n\n")
        f.write("| Section | Title | Specification | File:Line |\n")
        f.write("|---|---|---|---|\n")

        # Remove duplicates while keeping order
        seen = set()
        for entry in index_entries:
            if entry not in seen:
                f.write(entry + "\n")
                seen.add(entry)

    print(f"Index created: {index_path}")


if __name__ == '__main__':
    for spec_url in CORE_SPEC_URLS:
        process_spec(spec_url)

    generate_index()
    print("\nAll core specifications synchronized and indexed successfully.")
