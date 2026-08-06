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

import * as assert from 'assert';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import {
  Target,
  CDB_FILE_DIRS,
  CDB_FILE_NAME,
  availableTargets,
} from './paths';
import { workingDir } from '../settings/vscode';

test('should use base name for targets in canonical directory', () => {
  const target = new Target('host_clang');

  assert.equal(target.name, 'host_clang');
  assert.equal(target.displayName, 'host_clang');
  assert.equal(path.dirname(target.dir), CDB_FILE_DIRS[0]);
  assert.equal(
    target.path,
    path.join(CDB_FILE_DIRS[0], 'host_clang', CDB_FILE_NAME),
  );
});

test('should append directory name for targets in non-canonical directory', () => {
  const target = new Target(
    'host_clang',
    path.join(CDB_FILE_DIRS[1], 'host_clang'),
  );

  assert.equal(target.name, 'host_clang');
  assert.equal(target.displayName, 'host_clang (.pw_ide)');
  assert.equal(path.dirname(target.dir), CDB_FILE_DIRS[1]);
  assert.equal(
    target.path,
    path.join(CDB_FILE_DIRS[1], 'host_clang', CDB_FILE_NAME),
  );
});

test('should store hasCpp and hasRust properties correctly', () => {
  const targetCppOnly = new Target(
    'host_clang',
    path.join(CDB_FILE_DIRS[0], 'host_clang'),
    'host_clang',
    undefined,
    true,
    false,
  );
  assert.strictEqual(targetCppOnly.hasCpp, true);
  assert.strictEqual(targetCppOnly.hasRust, false);

  const targetRustOnly = new Target(
    'host_rust',
    path.join(CDB_FILE_DIRS[0], 'host_rust'),
    'host_rust',
    undefined,
    false,
    true,
  );
  assert.strictEqual(targetRustOnly.hasCpp, false);
  assert.strictEqual(targetRustOnly.hasRust, true);

  const targetBoth = new Target(
    'host_both',
    path.join(CDB_FILE_DIRS[0], 'host_both'),
    'host_both',
    undefined,
    true,
    true,
  );
  assert.strictEqual(targetBoth.hasCpp, true);
  assert.strictEqual(targetBoth.hasRust, true);
});

test('availableTargets detects cpp and rust target flags', async () => {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pw_ide_test_'));
  const origWorkingDir = workingDir.get();
  workingDir.set(tmpDir);

  try {
    const cdbDir = path.join(tmpDir, CDB_FILE_DIRS[0]);
    fs.mkdirSync(cdbDir, { recursive: true });

    const cppTargetDir = path.join(cdbDir, 'cpp_only');
    fs.mkdirSync(cppTargetDir);
    fs.writeFileSync(path.join(cppTargetDir, 'compile_commands.json'), '[]');

    const rustTargetDir = path.join(cdbDir, 'rust_only');
    fs.mkdirSync(rustTargetDir);
    fs.writeFileSync(path.join(rustTargetDir, 'rust-project.json'), '{}');

    const bothTargetDir = path.join(cdbDir, 'both_target');
    fs.mkdirSync(bothTargetDir);
    fs.writeFileSync(path.join(bothTargetDir, 'compile_commands.json'), '[]');
    fs.writeFileSync(path.join(bothTargetDir, 'rust-project.json'), '{}');

    const neitherTargetDir = path.join(cdbDir, 'neither_target');
    fs.mkdirSync(neitherTargetDir);

    const targets = await availableTargets();
    const targetsByName = new Map(targets.map((t) => [t.name, t]));

    assert.strictEqual(targetsByName.has('cpp_only'), true);
    assert.strictEqual(targetsByName.get('cpp_only')?.hasCpp, true);
    assert.strictEqual(targetsByName.get('cpp_only')?.hasRust, false);

    assert.strictEqual(targetsByName.has('rust_only'), true);
    assert.strictEqual(targetsByName.get('rust_only')?.hasCpp, false);
    assert.strictEqual(targetsByName.get('rust_only')?.hasRust, true);

    assert.strictEqual(targetsByName.has('both_target'), true);
    assert.strictEqual(targetsByName.get('both_target')?.hasCpp, true);
    assert.strictEqual(targetsByName.get('both_target')?.hasRust, true);

    assert.strictEqual(targetsByName.has('neither_target'), false);
  } finally {
    workingDir.set(origWorkingDir);
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
});
