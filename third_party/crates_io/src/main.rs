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

use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::PathBuf;

use clap::Parser;
use serde::Deserialize;

mod aliases;

#[derive(Debug, Parser)]
struct Args {
    #[arg(long, required(true))]
    config: PathBuf,
}

#[derive(Deserialize)]
pub struct Config {
    manifests: HashSet<String>,
    bazel_aliases: HashMap<String, Mapping>,
    #[serde(default)]
    extra_targets: HashMap<String, Vec<String>>,
}

#[derive(Deserialize)]
struct Mapping {
    constraint: String,
    path: String,
}

fn main() {
    let args = Args::parse();
    let config_str = fs::read_to_string(args.config).expect("config file exists");
    let config: Config = toml::from_str(&config_str).expect("config file parses");
    aliases::generate(&config);
}
