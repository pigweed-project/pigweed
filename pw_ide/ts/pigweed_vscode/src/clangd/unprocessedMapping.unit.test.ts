// Copyright 2024 The Pigweed Authors
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
import * as fs_p from 'fs/promises';
import * as os from 'os';
import * as path from 'path';

import {
  loadUnprocessedMapping,
  saveUnprocessedMapping,
} from './unprocessedMapping';
import { availableTargets, CDB_FILE_DIR, CDB_FILE_NAME } from './paths';
import { workingDir } from '../settings/vscode';

const TEST_DIR = path.join(os.tmpdir(), '__pw_ide_test');
const CDB_DIR = path.join(TEST_DIR, CDB_FILE_DIR);

suite('unprocessedMapping', () => {
  let origWorkingDir: string;

  setup(async () => {
    origWorkingDir = workingDir.get();
    await fs_p.mkdir(CDB_DIR, { recursive: true });

    await fs_p.mkdir(path.join(CDB_DIR, 'target1'));
    await fs_p.mkdir(path.join(CDB_DIR, 'target2'));
    await fs_p.mkdir(path.join(TEST_DIR, 'build', 'target3'), {
      recursive: true,
    });

    await fs_p.writeFile(path.join(CDB_DIR, 'target1', CDB_FILE_NAME), '{}');
    await fs_p.writeFile(path.join(CDB_DIR, 'target2', CDB_FILE_NAME), '{}');
    await fs_p.writeFile(
      path.join(TEST_DIR, 'build', 'target3', CDB_FILE_NAME),
      '{}',
    );
  });

  teardown(async () => {
    // Restore the shared working dir so later suites don't inherit TEST_DIR
    // after it has been deleted.
    workingDir.set(origWorkingDir);
    await fs_p.rm(TEST_DIR, { recursive: true, force: true });
  });

  test('round trip', async () => {
    workingDir.set(TEST_DIR);

    const unprocessedCompDbs: [string, string][] = [
      ['target3', 'build/target3/compile_commands.json'],
    ];

    await saveUnprocessedMapping(unprocessedCompDbs);
    const result = await loadUnprocessedMapping();
    assert.equal(result['target3'], 'build/target3/compile_commands.json');

    const allTargets = await availableTargets();

    assert.equal(allTargets.length, 3);

    const target1 = allTargets.find((t) => t.name === 'target1');
    const target2 = allTargets.find((t) => t.name === 'target2');
    const target3 = allTargets.find((t) => t.name === 'target3');

    assert.equal(target1?.path, path.join(CDB_DIR, 'target1', CDB_FILE_NAME));
    assert.equal(target2?.path, path.join(CDB_DIR, 'target2', CDB_FILE_NAME));
    assert.equal(
      target3?.path,
      path.join(TEST_DIR, 'build', 'target3', CDB_FILE_NAME),
    );
  });
});

suite('loadUnprocessedMapping', () => {
  test('returns empty object when file does not exist', async () => {
    const result = await loadUnprocessedMapping();
    assert.deepStrictEqual(result, {});
  });
});
