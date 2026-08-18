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
"""pw module template loading and files.

This directory should contain files ending in '.jinja'.

All files need to be added to:
  1. //pw_module/py/BUILD.gn as inputs
  2. //pw_module/py/setup.cfg under package_data

For jinja syntax help see: https://jinja.palletsprojects.com/en/3.1.x/templates/
"""

from importlib.resources import files
from itertools import chain
import logging

from jinja2 import Environment, DictLoader, make_logging_undefined
from jinja2.environment import Template

TEMPLATE_PATH = files(__package__)

# Dict containing file names and their contents.
JINJA_TEMPLATES = {
    t.relative_to(TEMPLATE_PATH).as_posix(): t.read_text()  # type: ignore
    for t in chain(
        files(__package__).iterdir(),
        files(__package__ + '.helpers').iterdir(),
        files(__package__ + '.python').iterdir(),
    )
    if t.suffix == '.jinja'  # type: ignore
}

JINJA_ENV = Environment(
    # Load templates automatically from pw_console/templates
    loader=DictLoader(JINJA_TEMPLATES),
    extensions=[
        # https://jinja.palletsprojects.com/en/3.1.x/extensions/#expression-statement
        'jinja2.ext.do',
    ],
    # Raise errors if variables are undefined in templates
    undefined=make_logging_undefined(
        logger=logging.getLogger(__package__),
    ),
    # Trim whitespace in templates
    trim_blocks=True,
    lstrip_blocks=True,
)


def get_template(file_name: str) -> Template:
    """Return the Template defined by a given file_name.

    Raises:
      jinja2.exceptions.TemplateNotFound when the template file name can't be
          found.
    """
    return JINJA_ENV.get_template(file_name)
