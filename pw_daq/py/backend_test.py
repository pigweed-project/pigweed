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
"""Unit tests for pw_daq backend registry and YAML configuration."""

# pylint: disable=missing-function-docstring,protected-access

from __future__ import annotations

import copy
import importlib.metadata
import os
from pathlib import Path
import tempfile
from typing import Any
import unittest
from unittest import mock

from pw_cli import plugins
from pw_config_loader.yaml_config_loader_mixin import Stage
from pw_daq import (
    backend_registry as backend_registry_mod,
    config as config_mod,
)
from pw_daq.backend import DaqBackend
from pw_daq.backend_registry import (
    BUILTIN_BACKENDS,
    ENTRY_POINT_GROUP,
    BackendRegistry,
    _builtin_backends,
    _validate_backend_plugin,
)
from pw_daq.backends.mock_daq import mock_backend as mock_backend_mod
from pw_daq.backends.mock_daq.mock_backend import MockBackend
from pw_daq.common import DaqError
from pw_daq.config import (
    _CONFIG_ENV_VAR,
    DaqConfig,
    _deep_merge,
    _ensure_project_root_env,
)
import yaml


class FakeBackend(MockBackend):
    """Test backend subclass used for registration and instantiation tests."""

    def __init__(self, label: str = "default", **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self.label = label

    def name(self) -> str:
        return "fake"


def fake_backend_factory(label: str = "from_factory") -> DaqBackend:
    """Factory function returning a FakeBackend instance."""
    return FakeBackend(label=label)


NOT_CALLABLE_TARGET = "not_a_callable_object"


class BackendRegistryTest(unittest.TestCase):
    """Tests for BackendRegistry and _validate_backend_plugin."""

    def test_validate_backend_plugin(self) -> None:
        valid_plugin = plugins.Plugin("fake", FakeBackend)
        _validate_backend_plugin(valid_plugin)

        factory_plugin = plugins.Plugin("factory", fake_backend_factory)
        _validate_backend_plugin(factory_plugin)

        invalid_plugin = plugins.Plugin("bad", 12345)
        with self.assertRaises(plugins.Error):
            _validate_backend_plugin(invalid_plugin)

        non_backend_cls_plugin = plugins.Plugin("bad_cls", dict)
        with self.assertRaises(plugins.Error):
            _validate_backend_plugin(non_backend_cls_plugin)

    def test_builtin_backends_preregistered_by_default(self) -> None:
        self.assertEqual(list(BUILTIN_BACKENDS.keys()), ["mock"])
        registry = BackendRegistry(discover_entry_points=False)
        self.assertEqual(list(registry.backend_names()), ["mock"])
        self.assertEqual(len(registry), 1)
        self.assertIn("mock", registry)
        self.assertIn("MOCK", registry)
        self.assertIsInstance(registry.create_backend("mock", {}), MockBackend)

        # Duplicate programmatic built-in registration raises DaqError.
        fresh_registry = BackendRegistry(discover_entry_points=False)
        with self.assertRaises(DaqError):
            fresh_registry.register("MOCK", FakeBackend)

        # Sourced registration overrides a pre-registered built-in.
        override_reg = BackendRegistry(discover_entry_points=False)
        override_reg.register_by_name(
            "mock",
            __name__,
            "FakeBackend",
            source=Path("<test_override>"),
        )
        self.assertIs(override_reg["mock"].target, FakeBackend)

    def test_builtin_backends_module_follows_enclosing_package(self) -> None:
        self.assertTrue(backend_registry_mod.__package__)
        self.assertTrue(
            BUILTIN_BACKENDS["mock"]["module"].startswith(
                f"{backend_registry_mod.__package__}."
            )
        )
        self.assertEqual(
            BUILTIN_BACKENDS["mock"]["module"],
            mock_backend_mod.__name__,
        )
        registry = BackendRegistry(discover_entry_points=False)
        self.assertIs(registry["mock"].target, MockBackend)

        prefixed = _builtin_backends("parent_pkg.pw_daq")
        self.assertEqual(
            prefixed["mock"]["module"],
            "parent_pkg.pw_daq.backends.mock_daq.mock_backend",
        )
        self.assertEqual(
            _builtin_backends(None)["mock"]["module"],
            "pw_daq.backends.mock_daq.mock_backend",
        )
        self.assertEqual(
            _builtin_backends("")["mock"]["module"],
            "pw_daq.backends.mock_daq.mock_backend",
        )

    def test_independent_registry_instances_and_case_normalization(
        self,
    ) -> None:
        reg_a = BackendRegistry(
            register_builtins=False, discover_entry_points=False
        )
        reg_b = BackendRegistry(
            register_builtins=False,
            discover_entry_points=False,
            extra_backends={"Extra_One": fake_backend_factory},
        )
        self.assertIsInstance(reg_a, plugins.Registry)
        self.assertEqual(list(reg_a.backend_names()), [])
        self.assertEqual(list(reg_b.backend_names()), ["extra_one"])
        self.assertIs(reg_b["EXTRA_ONE"].target, fake_backend_factory)

        plugin = reg_a.register("My_Custom", FakeBackend)
        self.assertIsNotNone(plugin)
        self.assertIn("my_custom", reg_a)
        self.assertIn("MY_CUSTOM", reg_a)
        self.assertNotIn(12345, reg_a)
        self.assertNotIn("my_custom", reg_b)
        self.assertIs(reg_a["MY_CUSTOM"].target, FakeBackend)

        # Duplicate built-in registration raises DaqError.
        with self.assertRaises(DaqError):
            reg_a.register("MY_CUSTOM", fake_backend_factory)

        # Non-callable target and non-DaqBackend class raise DaqError.
        with self.assertRaises(DaqError):
            bad_target: Any = "not_callable"
            reg_a.register("bad_target", bad_target)

        with self.assertRaises(DaqError):
            bad_cls: Any = dict
            reg_a.register("bad_cls", bad_cls)

    def test_register_by_name_and_source_override(self) -> None:
        registry = BackendRegistry(discover_entry_points=False)
        src1 = Path("<source_1>")
        src2 = Path("<source_2>")

        # Built-in registration (source=None) can be overridden by a sourced
        # registration, while subsequent sourced registrations keep the first.
        builtin = registry.register_by_name("ByName", __name__, "MockBackend")
        self.assertIsNotNone(builtin)
        self.assertIs(registry["byname"].target, MockBackend)

        plugin = registry.register_by_name(
            "BYNAME", __name__, "FakeBackend", source=src1
        )
        self.assertIsNotNone(plugin)
        self.assertIn("byname", registry)
        self.assertIs(registry["byname"].target, FakeBackend)
        self.assertEqual(registry["byname"].source, src1)

        ignored = registry.register_by_name(
            "byname", __name__, "fake_backend_factory", source=src2
        )
        self.assertIsNone(ignored)
        self.assertIs(registry["byname"].target, FakeBackend)
        self.assertEqual(registry["byname"].source, src1)

        # Duplicate source=None registration for a built-in raises DaqError.
        fresh = BackendRegistry(discover_entry_points=False)
        with self.assertRaises(DaqError):
            fresh.register_by_name("MOCK", __name__, "FakeBackend")

        # Missing module or non-callable attribute raises DaqError.
        with self.assertRaises(DaqError):
            registry.register_by_name(
                "missing_mod", "non_existent_mod_xyz", "FakeBackend"
            )

        with self.assertRaises(DaqError):
            registry.register_by_name(
                "not_callable", __name__, "NOT_CALLABLE_TARGET"
            )

    def test_entry_point_discovery_and_lazy_loading(self) -> None:
        ep_fake = mock.create_autospec(
            importlib.metadata.EntryPoint, instance=True
        )
        ep_fake.name = "EP_Custom"
        ep_fake.value = f"{__name__}:FakeBackend"
        ep_fake.group = ENTRY_POINT_GROUP
        ep_fake.load.return_value = FakeBackend

        ep_dup = mock.create_autospec(
            importlib.metadata.EntryPoint, instance=True
        )
        ep_dup.name = "ep_custom"
        ep_dup.value = f"{__name__}:fake_backend_factory"
        ep_dup.group = ENTRY_POINT_GROUP
        ep_dup.load.return_value = fake_backend_factory

        ep_shadow_builtin = mock.create_autospec(
            importlib.metadata.EntryPoint, instance=True
        )
        ep_shadow_builtin.name = "mock"
        ep_shadow_builtin.value = f"{__name__}:FakeBackend"
        ep_shadow_builtin.group = ENTRY_POINT_GROUP
        ep_shadow_builtin.load.return_value = FakeBackend

        ep_broken = mock.create_autospec(
            importlib.metadata.EntryPoint, instance=True
        )
        ep_broken.name = "broken_ep"
        ep_broken.value = "missing_pkg.mod:BrokenBackend"
        ep_broken.group = ENTRY_POINT_GROUP
        ep_broken.load.side_effect = ImportError("No module named missing_pkg")

        ep_invalid = mock.create_autospec(
            importlib.metadata.EntryPoint, instance=True
        )
        ep_invalid.name = "invalid_ep"
        ep_invalid.value = "builtins:dict"
        ep_invalid.group = ENTRY_POINT_GROUP
        ep_invalid.load.return_value = dict

        with mock.patch(
            "importlib.metadata.entry_points",
            return_value=[
                ep_fake,
                ep_dup,
                ep_shadow_builtin,
                ep_broken,
                ep_invalid,
            ],
        ) as mock_eps:
            registry = BackendRegistry()
            mock_eps.assert_called_once_with(group=ENTRY_POINT_GROUP)

            # Discovery is lazy: no entry points are loaded on init, lookup, or
            # iteration.
            ep_fake.load.assert_not_called()
            ep_broken.load.assert_not_called()
            self.assertIn("ep_custom", registry)
            self.assertIn("EP_CUSTOM", registry)
            self.assertIn("broken_ep", registry)
            self.assertEqual(
                sorted(registry.backend_names()),
                ["broken_ep", "ep_custom", "invalid_ep", "mock"],
            )
            self.assertEqual(len(registry), 4)
            ep_fake.load.assert_not_called()
            ep_broken.load.assert_not_called()

            # Built-in backend is not shadowed by an entry point of the same
            # name.
            self.assertIsInstance(
                registry.create_backend("mock", {}), MockBackend
            )
            ep_shadow_builtin.load.assert_not_called()

            # Accessing 'EP_Custom' loads only that entry point lazily.
            inst = registry.create_backend(
                "EP_Custom", {"device": "ep_sim"}, label="ep_label"
            )
            self.assertIsInstance(inst, FakeBackend)
            assert isinstance(inst, FakeBackend)
            self.assertEqual(inst.label, "ep_label")
            ep_fake.load.assert_called_once_with()
            ep_dup.load.assert_not_called()
            ep_broken.load.assert_not_called()

            # Broken or invalid entry points raise DaqError consistently when
            # accessed without affecting other backends.
            for _ in range(2):
                with self.assertRaises(DaqError):
                    registry.create_backend("broken_ep", {})
                with self.assertRaises(DaqError):
                    registry.create_backend("invalid_ep", {})

            # Explicit extra_backends take precedence over unloaded entry
            # points of the same name.
            override_ep_reg = BackendRegistry(
                extra_backends={"ep_custom": fake_backend_factory}
            )
            self.assertIs(
                override_ep_reg["ep_custom"].target, fake_backend_factory
            )

            # discover_entry_points=False skips entry-point discovery.
            no_ep_reg = BackendRegistry(discover_entry_points=False)
            self.assertNotIn("ep_custom", no_ep_reg)

    def test_backend_names_and_create_backend(self) -> None:
        registry = BackendRegistry(
            register_builtins=False, discover_entry_points=False
        )
        self.assertEqual(list(registry.backend_names()), [])

        with self.assertRaises(DaqError) as empty_err:
            registry.create_backend("unknown", {})
        self.assertIn("Available backends: none", str(empty_err.exception))

        registry.register("mock", MockBackend)
        registry.register("zeta", FakeBackend)
        registry.register("alpha", fake_backend_factory)
        self.assertEqual(
            sorted(registry.backend_names()),
            ["alpha", "mock", "zeta"],
        )

        # Case-insensitive lookup, kwargs forwarding, and configure() call.
        dev_cfg = {
            "backend": "zeta",
            "device": "sim42",
            "dev": {"type": "mock"},
            "capabilities": {
                "blocks": ["digital0"],
                "resources": ["dio0"],
                "mappings": {"p0": "dio0"},
                "config": {
                    "digital0": {"attributes": {"pin": "p0"}},
                },
            },
        }
        zeta_inst = registry.create_backend(
            "ZETA", dev_cfg, label="custom_label", log_capacity=8
        )
        self.assertIsInstance(zeta_inst, FakeBackend)
        assert isinstance(zeta_inst, FakeBackend)
        self.assertEqual(zeta_inst.label, "custom_label")
        self.assertEqual(zeta_inst.log_capacity, 8)
        self.assertEqual(zeta_inst.device, "sim42")
        self.assertEqual(zeta_inst.config, dev_cfg)

        alpha_inst = registry.create_backend(
            "Alpha", {"device": "alpha_sim"}, label="alpha_label"
        )
        self.assertIsInstance(alpha_inst, FakeBackend)
        assert isinstance(alpha_inst, FakeBackend)
        self.assertEqual(alpha_inst.label, "alpha_label")
        self.assertEqual(alpha_inst.device, "alpha_sim")

        # Unknown backend lists available backends.
        with self.assertRaises(DaqError) as missing_err:
            registry.create_backend("missing", {})
        self.assertIn("mock", str(missing_err.exception))

        # Factory returning a non-DaqBackend instance raises DaqError.
        bad_factory: Any = lambda: "not_a_backend"
        registry.register("bad_return", bad_factory)
        with self.assertRaises(DaqError):
            registry.create_backend("bad_return", {})


class DaqConfigTest(unittest.TestCase):
    """Tests for DaqConfig loading, error handling, and validation."""

    def test_ensure_project_root_env(self) -> None:
        with mock.patch.dict(
            os.environ,
            {"BUILD_WORKSPACE_DIRECTORY": "/ws/root"},
            clear=True,
        ):
            _ensure_project_root_env()
            self.assertEqual(os.environ["PW_PROJECT_ROOT"], "/ws/root")

        with mock.patch.dict(
            os.environ,
            {"PW_PROJECT_ROOT": "/existing", "PW_ROOT": "/other"},
            clear=True,
        ):
            _ensure_project_root_env()
            self.assertEqual(os.environ["PW_PROJECT_ROOT"], "/existing")

    def test_deep_merge_and_handle_overloaded_value(self) -> None:
        base: dict[str, Any] = {
            "nested": {
                "keep": "from_base",
                "override": "old",
                "sub": {"a": 1, "b": [1, 2]},
            },
            "blocks": ["digital0", "digital1"],
            "mappings": {"p0": "gp14", "p1": "gp21"},
            "mixed_bool_int": [1, False],
            "dict_to_scalar": {"x": 1},
            "scalar_to_dict": "plain",
            "list_to_dict": ["a", "b"],
        }
        override: dict[str, Any] = {
            "nested": {
                "override": "new",
                "added": 42,
                "sub": {"b": [2, 3], "c": True},
            },
            "blocks": ["digital1", "uart0"],
            "mappings": {"p1": "gp21", "p2": "gp4"},
            "mixed_bool_int": [True, 0, 1],
            "dict_to_scalar": "replaced",
            "scalar_to_dict": {"now": "dict"},
            "list_to_dict": {"now": "mapping"},
            "new_key": "value",
        }
        merged = _deep_merge(base, override)

        # Input dictionaries are not mutated by _deep_merge.
        self.assertEqual(base["nested"]["override"], "old")
        self.assertEqual(base["blocks"], ["digital0", "digital1"])

        self.assertEqual(
            merged,
            {
                "nested": {
                    "keep": "from_base",
                    "override": "new",
                    "added": 42,
                    "sub": {"a": 1, "b": [1, 2, 3], "c": True},
                },
                "blocks": ["digital0", "digital1", "uart0"],
                "mappings": {"p0": "gp14", "p1": "gp21", "p2": "gp4"},
                "mixed_bool_int": [1, False, True, 0],
                "dict_to_scalar": "replaced",
                "scalar_to_dict": {"now": "dict"},
                "list_to_dict": {"now": "mapping"},
                "new_key": "value",
            },
        )

        # Top-level list merge deduplicates while preserving order.
        self.assertEqual(
            _deep_merge(["a", "b", "a"], ["b", "c"]),
            ["a", "b", "c"],
        )

        # Incompatible types raise TypeError if passed directly to _deep_merge.
        bad_arg: Any = ["not_a_mapping"]
        with self.assertRaises(TypeError):
            _deep_merge(base, bad_arg)

        # handle_overloaded_value dispatches mappings, lists, and scalars.
        daq_config = DaqConfig(
            project_file=None,
            project_user_file=None,
            user_file=None,
            environment_var=None,
        )
        self.assertEqual(
            daq_config.handle_overloaded_value(
                "devices",
                Stage.USER_PROJECT_FILE,
                {"pico2": {"backend": "mock"}},
                {"pico2": {"dev": {"type": "serial"}}},
            ),
            {"pico2": {"backend": "mock", "dev": {"type": "serial"}}},
        )
        self.assertEqual(
            daq_config.handle_overloaded_value(
                "custom_list",
                Stage.USER_FILE,
                ["digital0", "digital1"],
                ["digital1", "uart0"],
            ),
            ["digital0", "digital1", "uart0"],
        )
        self.assertEqual(
            daq_config.handle_overloaded_value(
                "config_title",
                Stage.USER_FILE,
                "pw_daq",
                "pw_daq",
            ),
            "pw_daq",
        )
        self.assertEqual(
            daq_config.handle_overloaded_value(
                "capabilities",
                Stage.USER_FILE,
                "legacy_scalar",
                {"blocks": ["digital0"]},
            ),
            {"blocks": ["digital0"]},
        )

    def test_layered_deep_merge_across_config_stages(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            tmp = Path(tempdir)
            proj_dir = tmp / "project"
            home_dir = tmp / "home"
            proj_dir.mkdir()
            home_dir.mkdir()

            # Project config defines board-level device properties without
            # user-specific 'dev' connection settings or custom 'backends'.
            proj_cfg = {
                "config_title": "pw_daq",
                "options": {},
                "devices": {
                    "pico2": {
                        "backend": "mock",
                        "token_db": "/proj/rp2350_dhalsim",
                        "capabilities": {
                            "blocks": ["digital0", "digital1"],
                            "mappings": {"p0": "gp14"},
                            "config": {
                                "digital0": {
                                    "attributes": {"pin": "p0"},
                                },
                            },
                        },
                    },
                },
            }
            # Project user config supplies 'default_device', 'dev' connection
            # details, and extends capabilities.
            proj_user_cfg = {
                "config_title": "pw_daq",
                "options": {"default_device": "pico2"},
                "devices": {
                    "pico2": {
                        "dev": {
                            "type": "serial",
                            "path": "/dev/cu.usbmodem1113301",
                            "baudrate": 115200,
                        },
                        "capabilities": {
                            "blocks": ["digital1", "uart0"],
                            "mappings": {"p0": "gp14", "p1": "gp21"},
                            "config": {
                                "uart0": {
                                    "attributes": {"txpin": "p2"},
                                },
                            },
                        },
                    },
                },
            }
            # Global user config overrides leaf device fields without wiping
            # sibling settings in 'devices.pico2'.
            user_cfg = {
                "config_title": "pw_daq",
                "devices": {
                    "pico2": {
                        "dev": {
                            "baudrate": 230400,
                        },
                    },
                },
            }
            (proj_dir / ".pw_daq.yaml").write_text(
                yaml.safe_dump(proj_cfg), encoding="utf-8"
            )
            (proj_dir / ".pw_daq.user.yaml").write_text(
                yaml.safe_dump(proj_user_cfg), encoding="utf-8"
            )
            (home_dir / ".pw_daq.yaml").write_text(
                yaml.safe_dump(user_cfg), encoding="utf-8"
            )

            with mock.patch.dict(
                os.environ,
                {
                    "PW_PROJECT_ROOT": str(proj_dir),
                    "HOME": str(home_dir),
                    "USERPROFILE": str(home_dir),
                },
                clear=True,
            ):
                daq_config = DaqConfig()

            daq_config.validate()
            self.assertEqual(daq_config.options, {"default_device": "pico2"})
            self.assertEqual(
                daq_config.get_device_config("pico2"),
                {
                    "backend": "mock",
                    "token_db": "/proj/rp2350_dhalsim",
                    "dev": {
                        "type": "serial",
                        "path": "/dev/cu.usbmodem1113301",
                        "baudrate": 230400,
                    },
                    "capabilities": {
                        "blocks": ["digital0", "digital1", "uart0"],
                        "mappings": {"p0": "gp14", "p1": "gp21"},
                        "config": {
                            "digital0": {
                                "attributes": {"pin": "p0"},
                            },
                            "uart0": {
                                "attributes": {"txpin": "p2"},
                            },
                        },
                    },
                },
            )

    def test_layered_default_configs_and_env_var(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            tmp = Path(tempdir)
            proj_dir = tmp / "project"
            home_dir = tmp / "home"
            proj_dir.mkdir()
            home_dir.mkdir()

            proj_cfg = {
                "config_title": "pw_daq",
                "devices": {
                    "sim0": {
                        "backend": "mock",
                        "dev": {"type": "mock"},
                        "capabilities": {},
                    }
                },
            }
            proj_user_cfg = {
                "config_title": "pw_daq",
                "options": {"default_device": "sim0"},
                "devices": {
                    "sim1": {
                        "backend": "user_custom",
                        "dev": {"type": "mock"},
                        "capabilities": {},
                    }
                },
            }
            user_cfg = {
                "config_title": "pw_daq",
                "options": {"default_device": "sim1"},
            }
            (proj_dir / ".pw_daq.yaml").write_text(
                yaml.safe_dump(proj_cfg), encoding="utf-8"
            )
            (proj_dir / ".pw_daq.user.yaml").write_text(
                yaml.safe_dump(proj_user_cfg), encoding="utf-8"
            )
            (home_dir / ".pw_daq.yaml").write_text(
                yaml.safe_dump(user_cfg), encoding="utf-8"
            )

            with mock.patch.dict(
                os.environ,
                {
                    "PW_PROJECT_ROOT": str(proj_dir),
                    "HOME": str(home_dir),
                    "USERPROFILE": str(home_dir),
                },
                clear=True,
            ):
                daq_config = DaqConfig()

            custom_reg = BackendRegistry(
                extra_backends={"user_custom": FakeBackend}
            )
            daq_config.validate(registry=custom_reg)
            self.assertEqual(daq_config.options, {"default_device": "sim1"})
            self.assertIn("sim0", daq_config.devices)
            self.assertIn("sim1", daq_config.devices)
            self.assertEqual(
                daq_config.get_device_config("sim0", registry=custom_reg)[
                    "backend"
                ],
                "mock",
            )
            self.assertEqual(
                daq_config.get_device_config("sim1", registry=custom_reg)[
                    "backend"
                ],
                "user_custom",
            )

            # PW_DAQ_CONFIG replaces the layered default files on top of
            # default_config.
            env_file = tmp / "env_config.yaml"
            env_file.write_text(
                yaml.safe_dump(
                    {
                        "config_title": "pw_daq",
                        "options": {"default_device": "env_dev"},
                        "devices": {
                            "env_dev": {
                                "backend": "mock",
                                "dev": {"type": "mock"},
                                "capabilities": {},
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )
            with mock.patch.dict(
                os.environ,
                {
                    "PW_PROJECT_ROOT": str(proj_dir),
                    "HOME": str(home_dir),
                    "USERPROFILE": str(home_dir),
                    _CONFIG_ENV_VAR: str(env_file),
                },
                clear=True,
            ):
                env_config = DaqConfig()

            env_config.validate()
            self.assertEqual(env_config.options, {"default_device": "env_dev"})
            self.assertEqual(list(env_config.devices.keys()), ["env_dev"])

    def test_all_config_sources_reject_backends_section(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            tmp = Path(tempdir)
            proj_dir = tmp / "project"
            home_dir = tmp / "home"
            proj_dir.mkdir()
            home_dir.mkdir()

            disallowed_backends_payload = {
                "config_title": "pw_daq",
                "backends": {
                    "untrusted": {
                        "module": "untrusted_pkg.evil",
                        "class": "EvilBackend",
                    }
                },
                "devices": {
                    "evil_dev": {
                        "backend": "mock",
                        "capabilities": {},
                    }
                },
            }
            for label, target_file in (
                ("project", proj_dir / ".pw_daq.yaml"),
                ("project_user", proj_dir / ".pw_daq.user.yaml"),
                ("home", home_dir / ".pw_daq.yaml"),
            ):
                target_file.write_text(
                    yaml.safe_dump(disallowed_backends_payload),
                    encoding="utf-8",
                )
                try:
                    with (
                        self.subTest(source=label),
                        mock.patch.dict(
                            os.environ,
                            {
                                "PW_PROJECT_ROOT": str(proj_dir),
                                "HOME": str(home_dir),
                                "USERPROFILE": str(home_dir),
                            },
                            clear=True,
                        ),
                    ):
                        cfg = DaqConfig()
                        with self.assertRaises(DaqError):
                            cfg.validate()
                finally:
                    target_file.unlink()

            explicit_path = tmp / "explicit_with_backends.yaml"
            explicit_path.write_text(
                yaml.safe_dump(disallowed_backends_payload), encoding="utf-8"
            )
            explicit_cfg = DaqConfig(config_file=explicit_path)
            with self.assertRaises(DaqError):
                explicit_cfg.validate()

    def test_missing_home_directory_logs_warning_without_raising(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            proj_dir = Path(tempdir)
            with (
                mock.patch.dict(
                    os.environ,
                    {"PW_PROJECT_ROOT": str(proj_dir)},
                    clear=True,
                ),
                mock.patch.object(
                    Path,
                    "expanduser",
                    side_effect=RuntimeError(
                        "Could not determine home directory."
                    ),
                ),
                self.assertLogs(
                    config_mod.__name__, level="WARNING"
                ) as warn_logs,
            ):
                cfg = DaqConfig()
                self.assertEqual(cfg.devices, {})
                self.assertTrue(
                    any(
                        "Could not determine home directory" in msg
                        for msg in warn_logs.output
                    )
                )
                with self.assertRaises(DaqError):
                    DaqConfig(config_file="~/explicit.yaml")

    def test_config_file_exclusively_overrides_default_files(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            tmp = Path(tempdir)
            proj_dir = tmp / "project"
            home_dir = tmp / "home"
            proj_dir.mkdir()
            home_dir.mkdir()

            (proj_dir / ".pw_daq.yaml").write_text(
                yaml.safe_dump(
                    {
                        "config_title": "pw_daq",
                        "options": {"default_device": "proj_dev"},
                        "devices": {
                            "proj_dev": {
                                "backend": "mock",
                                "dev": {"type": "mock"},
                                "capabilities": {},
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )
            explicit_cfg = tmp / "explicit.yaml"
            explicit_cfg.write_text(
                yaml.safe_dump(
                    {
                        "config_title": "pw_daq",
                        "options": {"default_device": "explicit_dev"},
                        "devices": {
                            "explicit_dev": {
                                "backend": "mock",
                                "dev": {"type": "mock"},
                                "capabilities": {},
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )

            with mock.patch.dict(
                os.environ,
                {
                    "PW_PROJECT_ROOT": str(proj_dir),
                    "HOME": str(home_dir),
                    "USERPROFILE": str(home_dir),
                },
                clear=True,
            ):
                daq_config = DaqConfig(config_file=explicit_cfg)

            daq_config.validate()
            self.assertEqual(
                daq_config.options, {"default_device": "explicit_dev"}
            )
            self.assertEqual(list(daq_config.devices.keys()), ["explicit_dev"])

            # Non-existent or empty explicit config_file raises DaqError.
            with self.assertRaises(DaqError):
                DaqConfig(config_file=tmp / "does_not_exist.yaml")
            with self.assertRaises(DaqError):
                DaqConfig(config_file="   ")

            # Relative config_file resolved against BUILD_WORKING_DIRECTORY.
            with mock.patch.dict(
                os.environ,
                {
                    "BUILD_WORKING_DIRECTORY": str(tmp),
                    "HOME": str(home_dir),
                    "USERPROFILE": str(home_dir),
                },
                clear=True,
            ):
                rel_cfg = DaqConfig(config_file="explicit.yaml")
            self.assertIn("explicit_dev", rel_cfg.devices)

    def test_missing_and_malformed_default_files_log_without_raising(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            tmp = Path(tempdir)
            proj_dir = tmp / "project"
            home_dir = tmp / "home"
            proj_dir.mkdir()
            home_dir.mkdir()

            # Non-existent default files log INFO messages.
            with (
                mock.patch.dict(
                    os.environ,
                    {
                        "PW_PROJECT_ROOT": str(proj_dir),
                        "HOME": str(home_dir),
                        "USERPROFILE": str(home_dir),
                    },
                    clear=True,
                ),
                self.assertLogs(config_mod.__name__, level="INFO") as info_logs,
            ):
                cfg = DaqConfig()
            cfg.validate()
            self.assertTrue(
                any(
                    "Configuration file not found, skipping" in msg
                    for msg in info_logs.output
                )
            )

            # Valid project config preserved when later default files are
            # malformed, empty, comment-only, missing config_title, or
            # non-mapping.
            (proj_dir / ".pw_daq.yaml").write_text(
                yaml.safe_dump(
                    {
                        "config_title": "pw_daq",
                        "options": {"default_device": "proj_dev"},
                        "devices": {
                            "proj_dev": {
                                "backend": "mock",
                                "capabilities": {},
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )
            bad_cases: list[tuple[str, bytes]] = [
                ("empty", b"   \n"),
                ("comment_only", b"# comment only\n"),
                ("bad_yaml", b"{not: valid: yaml"),
                ("bad_utf8", b"\xff\xfe\x00\x00"),
                ("missing_title", b"options:\n  default_device: foo\n"),
                ("non_mapping", b"[1, 2, 3]\n"),
            ]
            for case_name, raw_bytes in bad_cases:
                with self.subTest(case=case_name):
                    (home_dir / ".pw_daq.yaml").write_bytes(raw_bytes)
                    with (
                        mock.patch.dict(
                            os.environ,
                            {
                                "PW_PROJECT_ROOT": str(proj_dir),
                                "HOME": str(home_dir),
                                "USERPROFILE": str(home_dir),
                            },
                            clear=True,
                        ),
                        self.assertLogs(
                            config_mod.__name__, level="WARNING"
                        ) as warn_logs,
                    ):
                        loaded = DaqConfig()
                    loaded.validate()
                    self.assertIn("proj_dev", loaded.devices)
                    self.assertEqual(
                        loaded.options, {"default_device": "proj_dev"}
                    )
                    self.assertTrue(len(warn_logs.output) >= 1)

            # Remove broken home file so explicit override tests start from a
            # valid default project configuration.
            (home_dir / ".pw_daq.yaml").unlink()

            # Explicit overrides (PW_DAQ_CONFIG and config_file) raise DaqError
            # on missing, empty, comment-only, unreadable, or malformed files
            # instead of falling back to .pw_daq.yaml.
            missing_explicit = tmp / "missing_explicit.yaml"
            with (
                mock.patch.dict(
                    os.environ,
                    {
                        "PW_PROJECT_ROOT": str(proj_dir),
                        "HOME": str(home_dir),
                        "USERPROFILE": str(home_dir),
                        "PW_DAQ_CONFIG": str(missing_explicit),
                    },
                    clear=True,
                ),
                self.assertRaises(DaqError),
            ):
                DaqConfig()

            for case_name, raw_bytes in bad_cases:
                bad_explicit = tmp / f"explicit_{case_name}.yaml"
                bad_explicit.write_bytes(raw_bytes)
                with self.subTest(explicit_config_file=case_name):
                    with (
                        mock.patch.dict(
                            os.environ,
                            {
                                "PW_PROJECT_ROOT": str(proj_dir),
                                "HOME": str(home_dir),
                                "USERPROFILE": str(home_dir),
                            },
                            clear=True,
                        ),
                        self.assertRaises(DaqError),
                    ):
                        DaqConfig(config_file=bad_explicit)

                with self.subTest(pw_daq_config_env=case_name):
                    with (
                        mock.patch.dict(
                            os.environ,
                            {
                                "PW_PROJECT_ROOT": str(proj_dir),
                                "HOME": str(home_dir),
                                "USERPROFILE": str(home_dir),
                                "PW_DAQ_CONFIG": str(bad_explicit),
                            },
                            clear=True,
                        ),
                        self.assertRaises(DaqError),
                    ):
                        DaqConfig()

    def test_empty_yaml_section_headers_preserve_existing_mappings(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            tmp = Path(tempdir)
            proj_dir = tmp / "project"
            home_dir = tmp / "home"
            proj_dir.mkdir()
            home_dir.mkdir()

            # Base layer defines options and devices; overriding layer has
            # bare 'options:' and 'devices:' keys that parse to None in YAML.
            (proj_dir / ".pw_daq.yaml").write_text(
                yaml.safe_dump(
                    {
                        "config_title": "pw_daq",
                        "options": {"default_device": "base_dev"},
                        "devices": {
                            "base_dev": {
                                "backend": "mock",
                                "capabilities": {"blocks": ["digital0"]},
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )
            (proj_dir / ".pw_daq.user.yaml").write_text(
                "config_title: pw_daq\n"
                "options:\n"
                "  # comment-only options block\n"
                "devices:\n"
                "  base_dev:\n"
                "    capabilities:\n",
                encoding="utf-8",
            )

            with mock.patch.dict(
                os.environ,
                {
                    "PW_PROJECT_ROOT": str(proj_dir),
                    "HOME": str(home_dir),
                    "USERPROFILE": str(home_dir),
                },
                clear=True,
            ):
                loaded = DaqConfig()

            loaded.validate()
            self.assertEqual(loaded.options, {"default_device": "base_dev"})
            self.assertEqual(
                loaded.devices["base_dev"]["capabilities"],
                {"blocks": ["digital0"]},
            )

            # Single explicit file with bare 'options:' and 'devices:' preserves
            # empty dictionary defaults ({}) and validates cleanly.
            bare_cfg = tmp / "bare_sections.yaml"
            bare_cfg.write_text(
                "config_title: pw_daq\noptions:\ndevices:\n",
                encoding="utf-8",
            )
            explicit_bare = DaqConfig(config_file=bare_cfg)
            explicit_bare.validate()
            self.assertEqual(explicit_bare.options, {})
            self.assertEqual(explicit_bare.devices, {})

    def test_validate_and_get_device_config(self) -> None:
        daq_config = DaqConfig(
            project_file=None,
            project_user_file=None,
            user_file=None,
            environment_var=None,
        )
        daq_config.devices = {
            "pico2": {
                "backend": "mock",
                "dev": {
                    "type": "serial",
                    "path": "/dev/cu.usbmodem1113301",
                    "baudrate": 115200,
                },
                "capabilities": {
                    "blocks": ["digital0"],
                    "resources": ["gp14"],
                    "mappings": {"p0": "gp14"},
                    "config": {
                        "digital0": {
                            "attributes": {"pin": "p0"},
                        },
                    },
                },
            }
        }
        daq_config.options = {"default_device": "pico2"}
        daq_config.validate()
        self.assertEqual(
            daq_config.get_device_config("pico2")["backend"], "mock"
        )

        # Backend lookup in get_device_config() / validate() is
        # case-insensitive, matching BackendRegistry.create_backend().
        daq_config.devices["pico2"]["backend"] = "MOCK"
        daq_config.validate()
        self.assertEqual(
            daq_config.get_device_config("pico2")["backend"], "MOCK"
        )
        daq_config.devices["pico2"]["backend"] = "mock"

        # String file paths for capabilities are rejected; inline mapping is
        # required.
        saved_pico2 = copy.deepcopy(daq_config.devices["pico2"])
        daq_config.devices["pico2"]["capabilities"] = "capabilities.json"
        with self.assertRaises(DaqError):
            daq_config.validate()
        daq_config.devices["pico2"] = saved_pico2

        # 'dev' is optional in the schema so project-level .pw_daq.yaml device
        # definitions validate without user-specific connection paths.
        no_dev_cfg = dict(saved_pico2)
        no_dev_cfg.pop("dev")
        daq_config.devices = {"pico2": no_dev_cfg}
        daq_config.validate()
        daq_config.devices = {"pico2": saved_pico2}

        # Empty options (no default_device) is valid.
        daq_config.options = {}
        daq_config.validate()

        # Disallowed option keys (e.g. verbose, config_file, device) fail.
        for bad_opt in (
            {"verbose": 1},
            {"config_file": "other.yaml"},
            {"device": "pico2"},
        ):
            with self.subTest(bad_opt=bad_opt):
                daq_config.options = bad_opt
                with self.assertRaises(DaqError):
                    daq_config.validate()
        daq_config.options = {}

        # default_device pointing to unknown device or device with unknown
        # backend fails validation.
        daq_config.options = {"default_device": "unknown_dev"}
        with self.assertRaises(DaqError):
            daq_config.validate()

        daq_config.devices["broken_dev"] = {
            "backend": "missing_backend",
            "dev": {"type": "mock"},
            "capabilities": {},
        }
        daq_config.options = {"default_device": "broken_dev"}
        with self.assertRaises(DaqError):
            daq_config.validate()
        daq_config.options = {}
        del daq_config.devices["broken_dev"]

        # Missing required device fields (backend, capabilities) fail.
        saved = dict(daq_config.devices["pico2"])
        for missing_field in ("backend", "capabilities"):
            copy_dict = dict(saved)
            copy_dict.pop(missing_field)
            daq_config.devices = {"pico2": copy_dict}
            with self.subTest(missing_field=missing_field):
                with self.assertRaises(DaqError):
                    daq_config.validate()
        daq_config.devices = {"pico2": saved}

        # reset_config restores empty defaults.
        daq_config.reset_config()
        self.assertEqual(daq_config.options, {})
        self.assertEqual(daq_config.devices, {})


if __name__ == "__main__":
    unittest.main()
