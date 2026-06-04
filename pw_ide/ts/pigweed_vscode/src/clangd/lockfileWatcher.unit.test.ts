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
import { LockfileWatcher } from './lockfileWatcher';
import { WebviewProvider } from '../webviewProvider';
import { LockManager } from './lock';

suite('LockfileWatcher', () => {
  let mockWebviewProvider: any;
  let mockFs: any;
  let mockCheckPidAlive: any;
  let refreshCalled = false;

  setup(() => {
    refreshCalled = false;
    mockWebviewProvider = {
      refresh: () => {
        refreshCalled = true;
      },
    };

    mockFs = {
      existsSync: () => false,
      readFileSync: () => '',
      unlinkSync: () => {
        /* no-op */
      },
    };

    mockCheckPidAlive = () => true;
  });

  test('pollLockfile detects creation and triggers refresh', () => {
    let exists = false;
    mockFs.existsSync = () => exists;

    const lockManager = new LockManager(mockFs, mockCheckPidAlive);
    const watcher = new LockfileWatcher(
      mockWebviewProvider as WebviewProvider,
      lockManager,
    );

    // Initial check (false)
    assert.strictEqual(refreshCalled, false);

    // Simulate file creation
    exists = true;
    (watcher as any).pollLockfile();

    assert.strictEqual(refreshCalled, true);

    watcher.dispose();
  });

  test('pollLockfile detects deletion and triggers refresh', () => {
    let exists = true;
    mockFs.existsSync = () => exists;

    const lockManager = new LockManager(mockFs, mockCheckPidAlive);
    const watcher = new LockfileWatcher(
      mockWebviewProvider as WebviewProvider,
      lockManager,
    );

    // Initial check (true)
    assert.strictEqual(refreshCalled, true);
    refreshCalled = false; // Reset

    // Simulate file deletion
    exists = false;
    (watcher as any).pollLockfile();

    assert.strictEqual(refreshCalled, true);

    watcher.dispose();
  });
  test('isLocked returns true if lockfile exists and PID is alive', () => {
    mockFs.existsSync = () => true;
    mockFs.readFileSync = () => '12345';
    mockCheckPidAlive = () => true;

    const lockManager = new LockManager(mockFs, mockCheckPidAlive);
    assert.strictEqual(lockManager.isLocked(), true);
  });

  test('isLocked returns false if lockfile exists but PID is dead', () => {
    mockFs.existsSync = () => true;
    mockFs.readFileSync = () => '12345';
    mockCheckPidAlive = () => false;

    const lockManager = new LockManager(mockFs, mockCheckPidAlive);
    assert.strictEqual(lockManager.isLocked(), false);
  });

  test('isLocked returns false if lockfile does not exist', () => {
    mockFs.existsSync = () => false;

    const lockManager = new LockManager(mockFs, mockCheckPidAlive);
    assert.strictEqual(lockManager.isLocked(), false);
  });
});
