// Copyright 2025 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

use std::collections::{BTreeMap, BTreeSet};

use cargo_toml::Manifest;
use handlebars::Handlebars;
use serde::Serialize;

use crate::Config;

#[derive(Serialize)]
struct Alias {
    pub name: String,
    // Using BTreeSet and BTreeMap here for a stable output order.
    pub target_compatible_with: BTreeSet<String>,
    pub actual: BTreeMap<String, String>,
}

// Create a wrapper struct so that we expose `aliases` as a single element
// in the template's global namespace.
#[derive(Serialize)]
struct TemplateData<'a> {
    aliases: &'a BTreeMap<String, Alias>,
}

pub fn generate(config: &Config) {
    // Using BTreeMap here for a stable output order.
    let mut aliases: BTreeMap<String, Alias> = BTreeMap::new();

    for manifest_path in &config.manifests {
        let manifest = Manifest::from_path(manifest_path).expect("manifest parses");
        let package_name = &manifest.package().name;
        let Some(mapping) = config.bazel_aliases.get(package_name) else {
            panic!("No mapping for {package_name} in config.");
        };

        for dep_name in manifest.dependencies.keys() {
            let alias = aliases.entry(dep_name.clone()).or_insert(Alias {
                name: dep_name.clone(),
                target_compatible_with: BTreeSet::new(),
                actual: BTreeMap::new(),
            });
            alias
                .target_compatible_with
                .insert(mapping.constraint.clone());

            let dep_path = format!("{}:{dep_name}", mapping.path);
            alias.actual.insert(mapping.constraint.clone(), dep_path);
        }
    }

    for (alias_name, repos) in &config.extra_targets {
        let alias = aliases.entry(alias_name.clone()).or_insert(Alias {
            name: alias_name.clone(),
            target_compatible_with: BTreeSet::new(),
            actual: BTreeMap::new(),
        });

        for repo in repos {
            let mapping = config
                .bazel_aliases
                .get(repo)
                .expect("Repo listed in extra_targets exists in bazel_aliases");
            alias
                .target_compatible_with
                .insert(mapping.constraint.clone());

            let dep_path = format!("{}:{alias_name}", mapping.path);
            alias.actual.insert(mapping.constraint.clone(), dep_path);
        }
    }

    let template = r#"
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
"""Utility to generate crate aliases"""

# THIS FILE IS AUTO-GENERATED. DO NOT EDIT MANUALLY!!!
# See https://pigweed.dev/third_party/crates_io/ for information on updating.

{{#each aliases}}
alias(
    name = "{{this.name}}",
    target_compatible_with = select({
        {{#each this.target_compatible_with}}
        "{{this}}": [],
        {{/each}}
        "//conditions:default": ["@platforms//:incompatible"],
    }),
    actual = select({
        {{#each this.actual}}
        "{{@key}}": "{{this}}",
        {{/each}}
    }),
    visibility = ["//visibility:public"],
)

{{/each}}
"#
    .trim();

    let output = Handlebars::new()
        .render_template(template, &TemplateData { aliases: &aliases })
        .expect("Template renders");

    print!("{}", output);
}
