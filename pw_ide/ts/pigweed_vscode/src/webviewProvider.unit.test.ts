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
import * as path from 'path';
import * as fs from 'fs';
import { getReliableBazelExecutable } from './bazel';
import { generateAspectCompileCommands } from './webviewProvider';
import { workingDir } from './settings/vscode';
import { glob } from 'glob';
import { rimraf } from 'rimraf';

suite('webviewProvider Test Suite', () => {
  if (process.platform !== 'win32') {
    test('generateAspectCompileCommands creates compile_commands.json', async () => {
      const workspaceRoot = workingDir.get();
      const bazelBinary = getReliableBazelExecutable() || 'bazel';
      const buildCmd = 'pw_rpc';
      const compileCommandsDir = path.join(workspaceRoot, '.compile_commands');

      rimraf.sync(compileCommandsDir);

      await generateAspectCompileCommands(bazelBinary, buildCmd, workspaceRoot);

      const files = await glob('**/compile_commands.json', {
        cwd: compileCommandsDir,
      });
      assert.ok(files.length > 0, 'compile_commands.json should exist');

      const hasRpcCallCc = files.some((file) => {
        const compileCommandsPath = path.join(compileCommandsDir, file);
        const compileCommandsContent = fs.readFileSync(
          compileCommandsPath,
          'utf-8',
        );
        const compileCommands = JSON.parse(compileCommandsContent);
        return compileCommands.some(
          (command: { file: string }) => command.file === 'pw_rpc/call.cc',
        );
      });

      assert.ok(hasRpcCallCc, 'should have entry for pw_rpc/call.cc');
    });
  }
});
