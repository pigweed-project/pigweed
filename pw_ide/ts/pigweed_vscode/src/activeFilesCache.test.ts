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
import * as path from 'path';
import * as fs from 'fs';
import { spawnSync } from 'child_process';
import { ClangdActiveFilesCache } from './clangd/activeFilesCache';
import { workingDir } from './settings/vscode';
import { clangdPath } from './clangd/bazel';

suite('ClangdActiveFilesCache Tests', () => {
  const workspaceRoot = workingDir.get();
  const clangdFilePath = path.join(workspaceRoot, '.clangd');
  const testDir = path.join(workspaceRoot, 'path_with+');
  const testFile = path.join(testDir, 'file.cc');

  setup(() => {
    // Clean up before each test
    if (fs.existsSync(clangdFilePath)) {
      fs.unlinkSync(clangdFilePath);
    }
    if (fs.existsSync(testFile)) {
      fs.unlinkSync(testFile);
    }
    if (fs.existsSync(testDir)) {
      fs.rmdirSync(testDir);
    }
  });

  teardown(() => {
    // Clean up after each test
    if (fs.existsSync(clangdFilePath)) {
      fs.unlinkSync(clangdFilePath);
    }
    if (fs.existsSync(testFile)) {
      fs.unlinkSync(testFile);
    }
    if (fs.existsSync(testDir)) {
      fs.rmdirSync(testDir);
    }
  });

  test('writeToSettings escapes + in paths', async () => {
    const mockRefreshManager = {
      on: () => {
        /* mock */
      },
    };

    const cache = new ClangdActiveFilesCache(mockRefreshManager as any);
    const mockMap = new Map<string, any>();
    mockMap.set('path_with+/file.cc', {});
    cache.activeFiles['mock-target'] = mockMap;

    await cache.writeToSettings('mock-target');

    assert.ok(fs.existsSync(clangdFilePath), '.clangd file should be created');
    const content = fs.readFileSync(clangdFilePath, 'utf-8');

    // Check for escaped path. It might be single or double escaped in YAML output.
    const hasSingleEscape = content.includes('path_with\\+/file.cc');
    const hasDoubleEscape = content.includes('path_with\\\\+/file.cc');

    assert.ok(
      hasSingleEscape || hasDoubleEscape,
      `Content should contain escaped path. Found: ${content}`,
    );
  });

  test('writeToSettings converts backslashes to forward slashes', async () => {
    const mockRefreshManager = {
      on: () => {
        /* mock */
      },
    };

    const cache = new ClangdActiveFilesCache(mockRefreshManager as any);
    const mockMap = new Map<string, any>();
    mockMap.set('path_with\\file.cc', {});
    cache.activeFiles['mock-target'] = mockMap;

    await cache.writeToSettings('mock-target');

    assert.ok(fs.existsSync(clangdFilePath), '.clangd file should be created');
    const content = fs.readFileSync(clangdFilePath, 'utf-8');

    assert.ok(
      content.includes('path_with/file.cc'),
      `Content should contain forward slashes. Found: ${content}`,
    );
  });

  test('Clangd parses escaped paths without error', async () => {
    const mockRefreshManager = {
      on: () => {
        /* mock */
      },
    };

    const cache = new ClangdActiveFilesCache(mockRefreshManager as any);
    const mockMap = new Map<string, any>();
    mockMap.set('path_with+/file.cc', {});
    cache.activeFiles['mock-target'] = mockMap;

    await cache.writeToSettings('mock-target');

    // Create the test file so clangd can check it
    fs.mkdirSync(testDir, { recursive: true });
    fs.writeFileSync(testFile, 'void foo() {}');

    const cPath = clangdPath();
    if (!cPath) {
      console.warn(
        'Skipping Clangd execution test because clangd path is not found.',
      );
      return;
    }

    const result = spawnSync(cPath, ['--check=' + testFile], {
      cwd: workspaceRoot,
      encoding: 'utf-8',
    });

    if (result.error) {
      assert.fail(`Failed to run clangd: ${result.error.message}`);
    }

    assert.ok(
      result.stderr,
      `result.stderr is undefined. result.stdout: ${result.stdout}`,
    );

    assert.ok(
      !result.stderr.includes('repetition-operator operand invalid'),
      `Clangd reported regex error: ${result.stderr}`,
    );
  });
});
