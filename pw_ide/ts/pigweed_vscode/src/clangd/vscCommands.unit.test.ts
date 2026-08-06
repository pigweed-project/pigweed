// Copyright 2026 The Pigweed Authors
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
import { getClangdArgs, setTargetWithRust } from './vscCommands';
import { Target, CDB_FILE_DIRS, availableTargets } from './paths';
import { workingDir } from '../settings/vscode';

test('getClangdArgs returns correct arguments', () => {
  const targetDir = '/path/to/target';
  const cores = 4;
  const args = getClangdArgs(targetDir, cores);

  assert.deepStrictEqual(args, [
    `--compile-commands-dir=${targetDir}`,
    '--query-driver=/*',
    '--header-insertion=never',
    '--background-index',
    '-j=1', // Math.max(1, Math.round(4 / 4)) = 1
  ]);
});

test('getClangdArgs handles different core counts', () => {
  const targetDir = '/path/to/target';

  // Test with 8 cores -> -j=2
  const args8 = getClangdArgs(targetDir, 8);
  assert.ok(args8.includes('-j=2'));

  // Test with 1 core -> -j=1
  const args1 = getClangdArgs(targetDir, 1);
  assert.ok(args1.includes('-j=1'));
});

test('getClangdArgs uses /* for query-driver glob', () => {
  const targetDir = '/path/to/target';
  const cores = 4;
  const args = getClangdArgs(targetDir, cores);
  assert.ok(args.includes('--query-driver=/*'));
});

test('setTargetWithRust does nothing when target is undefined', async () => {
  await assert.doesNotReject(async () => {
    await setTargetWithRust(undefined);
  });
});

test('setTargetWithRust throws when target is not available', async () => {
  const fakeTarget = new Target('nonexistent_target', '/path/to/nonexistent');
  await assert.rejects(async () => {
    await setTargetWithRust(fakeTarget);
  }, /Target not among available targets/);
});

test('setTargetWithRust configures rust-project.json symlink for valid target', async () => {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pw_ide_rust_test_'));
  const origWorkingDir = workingDir.get();
  workingDir.set(tmpDir);

  try {
    const cdbDir = path.join(tmpDir, CDB_FILE_DIRS[0]);
    fs.mkdirSync(cdbDir, { recursive: true });

    const rustTargetDir = path.join(cdbDir, 'my_rust_target');
    fs.mkdirSync(rustTargetDir);
    fs.writeFileSync(
      path.join(rustTargetDir, 'rust-project.json'),
      '{"sysroot_src":""}',
    );
    fs.writeFileSync(
      path.join(rustTargetDir, 'ide_config.json'),
      JSON.stringify({
        rust_analyzer_check_override_command: ['cargo', 'check'],
      }),
    );

    const target = (await availableTargets()).find(
      (t) => t.name === 'my_rust_target',
    );
    assert.ok(target);

    await setTargetWithRust(target);

    const rootRustProject = path.join(tmpDir, 'rust-project.json');
    assert.strictEqual(fs.existsSync(rootRustProject), true);
  } finally {
    workingDir.set(origWorkingDir);
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
});
