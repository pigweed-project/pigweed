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

import * as child_process from 'child_process';
import * as path from 'path';
import * as vscode from 'vscode';

import { getReliableBazelExecutable } from '../bazel';
import logger from '../logging';
import { getPigweedProjectRoot } from '../project';
import { settings, stringSettingFor, workingDir } from '../settings/vscode';
import { existsSync } from 'fs';

const createClangdSymlinkTarget =
  '@pigweed//pw_toolchain/host_clang:copy_clangd' as const;

let cachedExecRoot: string | undefined;
let cachedClangdPath: string | undefined;

function getBazelExecRoot(): string | undefined {
  if (cachedExecRoot) {
    return cachedExecRoot;
  }
  const cmd = getReliableBazelExecutable();
  if (!cmd) return;
  const args = ['info', 'execution_root'];
  logger.info(`Running ${cmd} ${args.join(' ')}`);
  const result = child_process.spawnSync(cmd, args, { cwd: workingDir.get() });
  cachedExecRoot = result.stdout.toString().trim();
  return cachedExecRoot;
}

export const clangdPath = () => {
  if (cachedClangdPath && existsSync(cachedClangdPath)) {
    return cachedClangdPath;
  }

  const cmd = getReliableBazelExecutable();
  if (!cmd) return;
  const args = ['cquery', createClangdSymlinkTarget, '--output=files'];
  logger.info(`Running ${cmd} ${args.join(' ')}`);
  const result = child_process.spawnSync(cmd, args, { cwd: workingDir.get() });
  const relativeClangdPath = result.stdout.toString().trim().split('\n').pop()!;
  logger.info('Relative clangd path resolves to ' + relativeClangdPath);

  if (!relativeClangdPath) {
    logger.error('Could not determine relative clangd path from cquery.');
    return undefined;
  }

  const execRoot = getBazelExecRoot();
  if (!execRoot) {
    logger.error('Could not determine bazel execution root.');
    return;
  }

  const absoluteClangdPath = path.join(execRoot, relativeClangdPath);

  if (existsSync(absoluteClangdPath)) {
    cachedClangdPath = absoluteClangdPath;
    return absoluteClangdPath;
  }

  // Fallback for when the path might be relative to the workspace,
  // which can happen with the bazel-out symlink.
  const workspaceRelativePath = path.join(workingDir.get(), relativeClangdPath);
  if (existsSync(workspaceRelativePath)) {
    cachedClangdPath = workspaceRelativePath;
    return workspaceRelativePath;
  }

  logger.error(
    `Could not find clangd at ${absoluteClangdPath} or in the workspace.`,
  );
  return undefined;
};

/** Create the `clangd` symlink and add it to settings. */
export async function initBazelClangdPath(): Promise<boolean> {
  const existingClangdPath = clangdPath();
  logger.info('Ensuring presence of stable clangd symlink');
  const cwd = (await getPigweedProjectRoot(settings, workingDir)) as string;
  const cmd = getReliableBazelExecutable();

  if (!cmd) {
    logger.error("Couldn't find a Bazel or Bazelisk executable");
    return false;
  }

  const args = ['build', createClangdSymlinkTarget];
  const spawnedProcess = child_process.spawn(cmd, args, { cwd });

  const success = await new Promise<boolean>((resolve) => {
    spawnedProcess.on('spawn', () => {
      logger.info(`Running ${cmd} ${args.join(' ')}`);
    });

    spawnedProcess.stdout.on('data', (data) => logger.info(data.toString()));
    spawnedProcess.stderr.on('data', (data) => logger.info(data.toString()));

    spawnedProcess.on('error', (err) => {
      const { name, message } = err;
      logger.error(`[${name}] while creating clangd symlink: ${message}`);
      resolve(false);
    });

    spawnedProcess.on('exit', (code) => {
      if (code === 0) {
        logger.info('Finished ensuring presence of stable clangd symlink');
        resolve(true);
      } else {
        const message =
          'Failed to ensure presence of stable clangd symlink ' +
          `(error code: ${code})`;

        logger.error(message);
        resolve(false);
      }
    });
  });

  if (!success) return false;

  const { update: updatePath } = stringSettingFor('path', 'clangd');
  await updatePath(clangdPath());
  return true;
}
