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

/**
 * Mocha `--require` hook that redirects `require('vscode')` to the in-process
 * mock, so unit tests can run headless under plain Node:
 *
 *   mocha --ui tdd -r ./out/src/test/register-vscode-mock.js \
 *     'out/src/**\/*.unit.test.js'
 */

import Module from 'module';
import * as path from 'path';

const mockPath = path.join(__dirname, 'vscode-mock.js');

type ResolveFilename = (request: string, ...rest: unknown[]) => string;
const mod = Module as unknown as { _resolveFilename: ResolveFilename };
const originalResolve = mod._resolveFilename;

mod._resolveFilename = function (request: string, ...rest: unknown[]) {
  if (request === 'vscode') return mockPath;
  return originalResolve.call(this, request, ...rest);
};
