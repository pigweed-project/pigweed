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
"""Sphinx directives for Pigweed SEEDs"""

import json
import os

import docutils
from docutils import nodes

# pylint: disable=consider-using-from-import
import docutils.parsers.rst.directives as directives  # type: ignore
import docutils.statemachine

# pylint: enable=consider-using-from-import
from sphinx.application import Sphinx
from sphinx.util.docutils import SphinxDirective
from sphinx_design.cards import CardDirective


# pylint: disable=import-error
try:
    import jsonschema  # type: ignore

    jsonschema_enabled = True
except ModuleNotFoundError:
    jsonschema_enabled = False
# pylint: enable=import-error


# Get the SEED metadata and schema. The location of these files changes
# depending on whether we're building with GN or Bazel.
try:  # Bazel
    from python.runfiles import runfiles  # type: ignore

    script_dir = os.path.dirname(os.path.abspath(__file__))
    metadata_path = f"{script_dir}/../seed/seed_metadata.json"
    schema_path = f"{script_dir}/../seed/seed_metadata_schema.json"
except ImportError:  # GN
    metadata_path = f'{os.environ["PW_ROOT"]}/seed/seed_metadata.json'
    schema_path = f'{os.environ["PW_ROOT"]}/seed/seed_metadata_schema.json'
with open(metadata_path, 'r') as f:
    metadata = json.load(f)
with open(schema_path, 'r') as f:
    schema = json.load(f)
# Make sure the metadata matches its schema. Raise an uncaught exception
# if not.
if jsonschema_enabled:
    jsonschema.validate(metadata, schema)


def map_status_to_badge_style(status: str) -> str:
    """Return the badge style for a given status."""
    mapping = {
        'draft': 'bdg-primary-line',
        'intent_approved': 'bdg-info',
        'open_for_comments': 'bdg-primary',
        'last_call': 'bdg-warning',
        'accepted': 'bdg-success',
        'rejected': 'bdg-danger',
        'deprecated': 'bdg-secondary',
        'superseded': 'bdg-info',
        'on_hold': 'bdg-secondary-line',
        'meta': 'bdg-success-line',
    }
    badge = mapping[parse_status(status)]
    return f':{badge}:`{status}`'


def status_choice(arg) -> str:
    return directives.choice(
        arg,
        (
            'draft',
            'intent_approved',
            'open_for_comments',
            'last_call',
            'accepted',
            'rejected',
            'on_hold',
            'meta',
        ),
    )


def parse_status(arg) -> str:
    """Support variations on the status choices.

    For example, you can use capital letters and spaces.
    """

    return status_choice('_'.join([token.lower() for token in arg.split(' ')]))


def status_badge(seed_status: str, badge_status) -> str:
    """Given a SEED status, return the status badge for rendering."""

    return (
        ':bdg-primary:'
        if seed_status == badge_status
        else ':bdg-secondary-line:'
    )


def cl_link(cl_num):
    return (
        f'`pwrev.dev/{cl_num} '
        '<https://pigweed-review.googlesource.com/c/pigweed/pigweed/+/'
        f'{cl_num}>`_'
    )


class PigweedSeedDirective(SphinxDirective):
    """Directive registering & rendering SEED metadata."""

    required_arguments = 0
    final_argument_whitespace = True
    has_content = True
    option_spec = {
        'number': directives.positive_int,
        'name': directives.unchanged_required,
        'status': parse_status,
        'proposal_date': directives.unchanged_required,
        'cl': directives.positive_int_list,
        'authors': directives.unchanged_required,
        'facilitator': directives.unchanged_required,
    }

    def _try_get_option(self, option: str):
        """Try to get an option by name and raise on failure."""

        try:
            return self.options[option]
        except KeyError:
            raise self.error(f' :{option}: option is required')

    def run(self) -> list[nodes.Node]:
        seed_number = '{:04d}'.format(self._try_get_option('number'))
        seed_name = self._try_get_option('name')
        status = self._try_get_option('status')
        proposal_date = self._try_get_option('proposal_date')
        cl_nums = self._try_get_option('cl')
        authors = self._try_get_option('authors')
        facilitator = self._try_get_option('facilitator')

        title = (
            f':fas:`seedling` SEED-{seed_number}: :ref:'
            f'`{seed_name}<seed-{seed_number}>`\n'
        )

        authors_heading = 'Authors' if len(authors.split(',')) > 1 else 'Author'

        self.content = docutils.statemachine.StringList(
            [
                ':octicon:`comment-discussion` Status:',
                f'{status_badge(status, "open_for_comments")}'
                '`Open for Comments`',
                ':octicon:`chevron-right`',
                f'{status_badge(status, "intent_approved")}'
                '`Intent Approved`',
                ':octicon:`chevron-right`',
                f'{status_badge(status, "last_call")}`Last Call`',
                ':octicon:`chevron-right`',
                f'{status_badge(status, "accepted")}`Accepted`',
                ':octicon:`kebab-horizontal`',
                f'{status_badge(status, "rejected")}`Rejected`',
                '\n',
                f':octicon:`calendar` Proposal Date: {proposal_date}',
                '\n',
                ':octicon:`code-review` CL: ',
                ', '.join([cl_link(cl_num) for cl_num in cl_nums]),
                '\n',
                f':octicon:`person` {authors_heading}: {authors}',
                '\n',
                f':octicon:`person` Facilitator: {facilitator}',
            ]
        )

        card = CardDirective.create_card(
            inst=self,
            arguments=[title],
            options={},
        )

        return [card]


def generate_seed_index(app: Sphinx, docname: str, source: list[str]) -> None:
    """Auto-generates content for //seed/0000.rst.

    The SEED index table and toctree are generated by modifying the
    reStructuredText of //seed/0000.rst in-place, before Sphinx converts the
    reST into HTML.
    """
    if docname != 'seed/0000':
        return
    # Build the SEED index table.
    source[0] += '.. csv-table::\n'
    source[
        0
    ] += '   :header: "Number","Title","Status","Authors","Facilitator"\n\n'
    for number, seed in metadata.items():
        cl = seed['cl']
        title = seed['title']
        title = f'`{title} <https://pwrev.dev/{cl}>`__'
        if seed['status'].lower() == 'accepted':
            title = f':ref:`seed-{number}`'
        authors = ', '.join(seed['authors'])
        facilitator = seed['facilitator']
        status = map_status_to_badge_style(seed['status'])
        source[
            0
        ] += f'   "{number}","{title}","{status}","{authors}","{facilitator}"\n'
    source[0] += '\n\n'
    # Build the toctree for //seed/0000.rst.
    source[0] += '.. toctree::\n'
    source[0] += '   :hidden:\n\n'
    for number in metadata:
        path = f'{app.srcdir}/seed/{number}.rst'
        if os.path.exists(path):
            # Link to the SEED content if it exists.
            source[0] += f'   {number}\n'
        else:
            # Otherwise link to the change associated to the SEED.
            cl = metadata[number]['cl']
            title = metadata[number]['title']
            source[0] += f'   {number}: {title} <https://pwrev.dev/{cl}>\n'


def setup(app: Sphinx) -> dict[str, bool]:
    app.add_directive('seed', PigweedSeedDirective)
    app.connect('source-read', generate_seed_index)
    return {
        'parallel_read_safe': True,
        'parallel_write_safe': True,
    }
