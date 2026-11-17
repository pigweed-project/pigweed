# Copyright 2025 The Pigweed Authors
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
"""Tests for the workflow validator."""

import re
import unittest

from google.protobuf import text_format

from pw_build.workflows.build_driver import BuildDriver
from pw_build.workflows.private import validator
from pw_build.proto import build_driver_pb2, workflows_pb2

# Registers BazelDriverOptions so mix-ins can merge driver options. In `./pw`,
# importing the Bazel build driver does this.
from pw_build.proto import (  # pylint: disable=unused-import
    pigweed_build_driver_pb2,
)


class NopBuildDriver(BuildDriver):
    def generate_action_sequence(
        self, job: build_driver_pb2.JobRequest
    ) -> build_driver_pb2.JobResponse:
        return build_driver_pb2.JobResponse()


class ValidatorTest(unittest.TestCase):
    """Tests for the Validator class."""

    def test_check_fragment_has_unique_name(self):
        """Test that duplicate names are detected."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        workflow_suite.builds.add(name='same-name')
        workflow_suite.tools.add(name='same-name')
        with self.assertRaises(validator.ValidationError):
            val = validator.Validator(workflow_suite, {})
            val.check_fragment_has_unique_name(workflow_suite.builds[0])

    def test_check_fragment_has_name(self):
        """Test that all fragments are named."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        workflow_suite.tools.add()
        with self.assertRaises(validator.ValidationError):
            val = validator.Validator(workflow_suite, {})
            val.check_fragment_has_name(workflow_suite.tools[0])

    def test_check_config_references_real_build_type(self):
        """Test that configs reference real build types."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        config = workflow_suite.configs.add(
            name='test-config', build_type='bazel'
        )
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_config_references_real_build_type(config)

    def test_check_build_references_real_config(self):
        """Test that builds reference real configs."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        build = workflow_suite.builds.add(
            name='test-build', use_config='missing-config'
        )
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_build_references_real_config(build)

    def test_check_build_references_real_output(self):
        """Test that builds reference real outputs."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        build = workflow_suite.builds.add(name='test-build')
        build.use_output.append('missing-output')
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_build_references_real_output(build)

    def test_check_build_has_config(self):
        """Test that builds have a config."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        build = workflow_suite.builds.add(name='test-build')
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_build_has_config(build)

    def test_check_build_has_targets(self):
        """Test that builds have targets."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        build = workflow_suite.builds.add(name='test-build')
        build.build_config.name = 'test-config'
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_build_has_targets(build)

    def test_check_tool_references_real_config(self):
        """Test that tools reference real configs."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        tool = workflow_suite.tools.add(
            name='test-tool', use_config='missing-config'
        )
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_tool_references_real_config(tool)

    def test_check_tool_references_real_output(self):
        """Test that tools reference real configs."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        tool = workflow_suite.tools.add(name='test-tool')
        tool.use_output.append('missing-output')
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_tool_references_real_output(tool)

    def test_check_tool_has_config(self):
        """Test that tools have a config."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        tool = workflow_suite.tools.add(name='test-tool')
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_tool_has_config(tool)

    def test_check_tool_has_target(self):
        """Test that tools have a target."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        tool = workflow_suite.tools.add(name='test-tool')
        tool.build_config.name = 'test-config'
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_tool_has_target(tool)

    def test_check_group_builds_exist(self):
        """Test that group builds exist."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        group = workflow_suite.groups.add(name='test-group')
        group.builds.append('missing-build')
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_group_builds_exist(group)

    def test_check_group_analyzers_exist(self):
        """Test that group analyzers exist."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        group = workflow_suite.groups.add(name='test-group')
        group.analyzers.append('missing-analyzer')
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_group_analyzers_exist(group)

    def test_check_group_groups_exist(self):
        """Test that group groups exist."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        group = workflow_suite.groups.add(name='test-group')
        group.groups.append('missing-group')
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_group_groups_exist(group)

    def test_check_group_groups_are_groups(self):
        """Test that group groups are TaskGroups."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        group = workflow_suite.groups.add(name='test-group')
        group.groups.append('not-a-group')
        workflow_suite.builds.add(name='not-a-group')
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_group_groups_exist(group)

    def test_check_group_has_no_cycles(self):
        """Test that group cycles are detected."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        group1 = workflow_suite.groups.add(name='group1')
        group2 = workflow_suite.groups.add(name='group2')
        group1.groups.append('group2')
        group2.groups.append('group1')
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_group_has_no_cycles(group1)

    def test_check_general_tools_do_not_set_rerun_shortcut(self):
        """Test that tools missing analyzer_safe_args do not set shortcut."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        general_tool = workflow_suite.tools.add(
            name='test-tool', rerun_shortcut='test-tool-alt'
        )
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_tool_rerun_shortcut_if_hybrid(general_tool)

    def test_check_analyzers_do_not_set_rerun_shortcut(self):
        """Test analyzers do not use rerun_shortcut."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        analyzer = workflow_suite.tools.add(
            name='test-tool',
            type=workflows_pb2.Tool.Type.ANALYZER,
            rerun_shortcut='test-tool-alt',
        )
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_tool_rerun_shortcut_if_hybrid(analyzer)

    def test_check_group_analyzers_are_tools(self):
        """Test that group analyzers are tools."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        group = workflow_suite.groups.add(name='test-group')
        group.analyzers.append('not-a-tool')
        workflow_suite.builds.add(name='not-a-tool')
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_group_analyzers_exist(group)

    def test_check_group_analyzers_are_analyzers(self):
        """Test that group analyzers are of type ANALYZER."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        group = workflow_suite.groups.add(name='test-group')
        group.analyzers.append('not-an-analyzer')
        tool = workflow_suite.tools.add(name='not-an-analyzer')
        tool.type = workflows_pb2.Tool.Type.GENERAL
        val = validator.Validator(workflow_suite, {})
        with self.assertRaises(validator.ValidationError):
            val.check_group_analyzers_exist(group)

    @staticmethod
    def test_validate_valid_config():
        """Test that a valid config passes validation."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        workflow_suite.configs.add(name='test-config', build_type='bazel')
        build = workflow_suite.builds.add(
            name='test-build', use_config='test-config'
        )
        build.targets.append('//:test')
        tool = workflow_suite.tools.add(
            name='test-tool', use_config='test-config'
        )
        tool.target = '//:test_tool'
        group = workflow_suite.groups.add(name='test-group')
        group.builds.append('test-build')
        group.analyzers.append('test-tool')
        tool.type = workflows_pb2.Tool.Type.ANALYZER
        val = validator.Validator(workflow_suite, {'bazel': NopBuildDriver()})
        val.validate()

    def test_validate_invalid_config(self):
        """Test that an invalid config fails validation."""
        workflow_suite = workflows_pb2.WorkflowSuite()
        workflow_suite.builds.add(name='same-name')
        workflow_suite.tools.add(name='same-name')
        with self.assertRaises(validator.ValidationError):
            val = validator.Validator(workflow_suite, {})
            val.validate()


# A valid suite that uses mix-ins. Each test case below adds to it.
# `value` holds serialized BazelDriverOptions: b'\x12\x03sub' is
# `project_working_subdir: "sub"` and b'\x08\x01' is `no_test: true`.
_VALID_SUITE = r'''
configs {
  name: 'base'
  build_type: 'bazel'
  driver_options {
    type_url: 'pw.build.proto.BazelDriverOptions'
    value: '\x12\x03sub'
  }
}
configs { name: 'plain' build_type: 'bazel' }
configs { name: 'm1' mixin: true args: '-1' }
configs { name: 'm2' mixin: true build_type: 'bazel' args: '-2' }
configs {
  name: 'no_test'
  mixin: true
  build_type: 'bazel'
  driver_options {
    type_url: 'pw.build.proto.BazelDriverOptions'
    value: '\x08\x01'
  }
}
configs {
  name: 'other_options'
  mixin: true
  build_type: 'bazel'
  driver_options { type_url: 'pw.test.OtherOptions' value: '\x08\x01' }
}
builds {
  name: 'build'
  use_config: 'base'
  use_config_mixins: ['m1', 'm2']
  targets: '//...'
}
tools {
  name: 'tool'
  use_config: 'base'
  use_config_mixins: 'm2'
  target: '//:tool'
}
'''


def _build(name: str, fields: str) -> str:
    return f"builds {{ name: '{name}' {fields} targets: '//...' }}"


def _tool(name: str, fields: str) -> str:
    return f"tools {{ name: '{name}' {fields} target: '//:t' }}"


def _validate(extra: str = '') -> None:
    """Validates _VALID_SUITE with ``extra`` text-format fields appended."""
    suite = text_format.Parse(
        _VALID_SUITE + extra, workflows_pb2.WorkflowSuite()
    )
    drivers: dict[str, BuildDriver] = {
        'bazel': NopBuildDriver(),
        'other': NopBuildDriver(),
    }
    validator.Validator(suite, drivers).validate()


class ValidatorMixinTest(unittest.TestCase):
    """Tests for mix-in validation rules."""

    def test_valid(self):
        """Valid mix-in setups pass validation."""
        cases = {
            'base suite': '',
            'same driver_options type': _build(
                'b', "use_config: 'base' use_config_mixins: 'no_test'"
            ),
            'driver_options type with no fields': (
                "configs { name: 'empty' mixin: true build_type: 'bazel' "
                "driver_options { type_url: "
                "'pw.build.proto.BazelDriverOptions' } }"
                + _build('b', "use_config: 'base' use_config_mixins: 'empty'")
            ),
            'inline base': _build(
                'b',
                "build_config { name: 'inline' build_type: 'bazel' } "
                "use_config_mixins: 'm2'",
            ),
            'same combination twice': _build(
                'b', "use_config: 'base' use_config_mixins: ['m1', 'm2']"
            ),
            # Only config names matter for collisions, not build names.
            'build named like a merged config': _build(
                'base+m2', "use_config: 'plain'"
            ),
            'plus in an unmixed name': (
                "configs { name: 'a+b' build_type: 'bazel' }"
                + _build('b', "use_config: 'a+b'")
            ),
        }
        for case, extra in cases.items():
            with self.subTest(case):
                _validate(extra)

    def test_invalid(self):
        """Each mix-in rule rejects a config that breaks it."""
        cases = {
            'missing mixin': (
                _build('bad', "use_config: 'base' use_config_mixins: 'nope'"),
                'Build `bad` references missing mixin `nope`',
            ),
            'tool lists a non-mixin': (
                _tool('bad', "use_config: 'base' use_config_mixins: 'plain'"),
                'Tool `bad` lists `plain` in `use_config_mixins`, but `plain` '
                'is not a mixin',
            ),
            'listed twice': (
                _build(
                    'bad', "use_config: 'base' use_config_mixins: ['m1', 'm1']"
                ),
                'Build `bad` lists mixin `m1` more than once',
            ),
            'mixin as use_config': (
                _build('bad', "use_config: 'm2'"),
                'Build `bad` cannot use mixin `m2`',
            ),
            'mixin as use_config with mixins': (
                _tool('bad', "use_config: 'm1' use_config_mixins: 'm2'"),
                'Tool `bad` cannot use mixin `m1`',
            ),
            'inline mixin': (
                _build(
                    'bad',
                    "build_config { name: 'i' build_type: 'bazel' "
                    "mixin: true }",
                ),
                'Build `bad` has an inline `build_config` marked as a mixin',
            ),
            'build_type mismatch': (
                "configs { name: 'm_other' mixin: true build_type: 'other' }"
                + _build(
                    'bad', "use_config: 'base' use_config_mixins: 'm_other'"
                ),
                'build_type `other` does not match base build_type `bazel`',
            ),
            'driver_options type mismatch with base': (
                _build(
                    'bad',
                    "use_config: 'base' use_config_mixins: 'other_options'",
                ),
                'driver_options type `pw.test.OtherOptions` does not match',
            ),
            'driver_options type mismatch with no fields': (
                "configs { name: 'empty_other' mixin: true build_type: 'bazel' "
                "driver_options { type_url: 'pw.test.OtherOptions' } }"
                + _build(
                    'bad', "use_config: 'base' use_config_mixins: 'empty_other'"
                ),
                'driver_options type `pw.test.OtherOptions` does not match',
            ),
            'driver_options type mismatch between mixins': (
                _build(
                    'bad',
                    "use_config: 'plain' "
                    "use_config_mixins: ['no_test', 'other_options']",
                ),
                'driver_options type `pw.test.OtherOptions` does not match',
            ),
            'mixin with driver_options but no build_type': (
                "configs { name: 'm_no_type' mixin: true "
                "driver_options { type_url: 'pw.test.OtherOptions' } }",
                'Mixin `m_no_type` sets `driver_options`, so it must also set '
                '`build_type`',
            ),
            'mixin with unknown build_type': (
                "configs { name: 'm_unknown' mixin: true build_type: 'nope' }",
                'unknown build type',
            ),
            'merged name taken by a shared config': (
                "configs { name: 'base+m1+m2' build_type: 'bazel' }",
                '`base+m1+m2` collides with an existing build config name',
            ),
            'merged name taken by an inline config': (
                _build(
                    'i', "build_config { name: 'base+m2' build_type: 'bazel' }"
                ),
                'Tool `tool`: merged config name `base+m2` collides',
            ),
            'different combinations, two builds': (
                "configs { name: 'base+m1' build_type: 'bazel' }"
                + _build(
                    'other', "use_config: 'base+m1' use_config_mixins: 'm2'"
                ),
                '`base+m1+m2` collides: `build` and `other`',
            ),
            'different combinations, build and tool': (
                "configs { name: 'base+m1' build_type: 'bazel' }"
                + _tool(
                    'other', "use_config: 'base+m1' use_config_mixins: 'm2'"
                ),
                '`base+m1+m2` collides: `build` and `other`',
            ),
        }
        for case, (extra, message) in cases.items():
            with (
                self.subTest(case),
                self.assertRaisesRegex(
                    validator.ValidationError, re.escape(message)
                ),
            ):
                _validate(extra)


if __name__ == '__main__':
    unittest.main()
