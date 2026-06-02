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

import { existsSync, readFileSync } from 'fs';
import * as path from 'path';
import { getReliableBazelExecutable } from '../bazel';
import { getTarget, CDB_FILE_DIR, LAST_BAZEL_COMMAND_FILE_NAME } from './paths';
import { clangdPath } from './bazel';
import { settings, workingDir } from '../settings/vscode';

import { getPreconfiguredTargets } from '../bazelQuery';

interface CipdReport {
  clangdPath?: string;
  bazelPath?: string;
  targetSelected?: string;
  isCompileCommandsGenerated?: boolean;
  compileCommandsPath?: string;
  bazelCompileCommandsManualBuildCommand?: string;
  bazelCompileCommandsLastBuildCommand?: string;
  preconfiguredTargets?: { label: string; displayName?: string }[];
  [key: string]: any;
}

export default async function getCipdReport() {
  const report: CipdReport = {};

  // Check if clangd is found
  const clangdCmd = clangdPath();
  report['clangdPath'] = clangdCmd;

  // Check if bazel is found
  const bazelCmd = getReliableBazelExecutable();
  report['bazelPath'] = bazelCmd;

  // Check if target is selected
  const target = getTarget();
  report['targetSelected'] = target?.name;

  // Check if compile_commands exists
  report['isCompileCommandsGenerated'] = target
    ? existsSync(target.path)
    : false;
  report['compileCommandsPath'] = target?.path;

  report['bazelCompileCommandsManualBuildCommand'] =
    settings.bazelCompileCommandsManualBuildCommand() || '';

  const lastCommandPath = path.join(
    workingDir.get(),
    CDB_FILE_DIR,
    LAST_BAZEL_COMMAND_FILE_NAME,
  );
  if (existsSync(lastCommandPath)) {
    report['bazelCompileCommandsLastBuildCommand'] = readFileSync(
      lastCommandPath,
      'utf-8',
    ).trim();
  } else {
    report['bazelCompileCommandsLastBuildCommand'] = '';
  }

  // Check for preconfigured compile commands
  report['preconfiguredTargets'] = await getPreconfiguredTargets(
    workingDir.get(),
  );

  return report;
}
