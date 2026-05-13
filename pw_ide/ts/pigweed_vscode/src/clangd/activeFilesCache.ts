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

import * as fs from 'fs';
import * as fs_p from 'fs/promises';
import * as path from 'path';

import { Uri } from 'vscode';

import { createHash } from 'crypto';
import * as yaml from 'js-yaml';

import { availableTargets, Target } from './paths';

import { Disposable } from '../disposables';
import { didInit, didUpdateActiveFilesCache } from '../events';
import logger from '../logging';
import { OK, RefreshCallback, RefreshManager } from '../refreshManager';
import { settings, workingDir } from '../settings/vscode';
import { CompileCommand, CompilationDatabase } from './parser';
import { glob } from 'glob';

function isNotInExcludedDirs(
  excludedDirs: string[],
  filePath: string,
): boolean {
  for (const excludedDir of excludedDirs) {
    const relative = path.relative(excludedDir, filePath);

    if (relative && !relative.startsWith('..') && !path.isAbsolute(relative)) {
      return false;
    }
  }

  return true;
}

/** Parse a compilation database and get the source files in the build. */
export async function parseForSourceFiles(
  target: Target,
): Promise<Map<string, CompileCommand>> {
  const compDb = await CompilationDatabase.fromFile(target.path);
  const files = new Map<string, CompileCommand>();
  if (!compDb) return files;

  const _workingDir = workingDir.get();

  const excludedDirs = [
    ...(await glob(path.join(workingDir.get(), 'bazel-*'))),
    path.join(_workingDir, 'external'),
    path.join(_workingDir, 'environment'),
  ];

  for (const command of compDb.db) {
    if (isNotInExcludedDirs(excludedDirs, command.sourceFilePath)) {
      try {
        const realPath = await fs_p.realpath(command.sourceFilePath);
        const relativeRealPath = path.relative(_workingDir, realPath);
        files.set(relativeRealPath, command);
      } catch (error) {
        // Ignore errors, for example if the file doesn't exist
      }
    }
  }

  return files;
}

// See: https://clangd.llvm.org/config#files
const clangdSettingsDisableFiles = (paths: string[]) => ({
  If: {
    PathExclude: paths,
  },
  Diagnostics: {
    Suppress: '*',
  },
});

export type FileStatus = 'ACTIVE' | 'INACTIVE' | 'ORPHANED' | 'INVALID';

export class ClangdActiveFilesCache extends Disposable {
  activeFiles: Record<string, Map<string, CompileCommand>> = {};

  constructor(refreshManager: RefreshManager<any>) {
    super();
    refreshManager.on(this.refresh, 'didRefresh');
    this.disposables.push(didInit.event(this.refresh));
  }

  /** Get the active files for a particular target. */
  getForTarget = async (
    target: string,
  ): Promise<Map<string, CompileCommand>> => {
    if (!Object.keys(this.activeFiles).includes(target)) {
      return new Map();
    }

    return this.activeFiles[target];
  };

  /** Get all the targets that include the provided file. */
  targetsForFile = (fileName: string): string[] =>
    Object.entries(this.activeFiles)
      .map(([target, files]) => (files.has(fileName) ? target : undefined))
      .filter((it) => it !== undefined);

  fileStatus = async (
    projectRoot: string,
    uri: Uri,
    target?: string,
  ): Promise<{
    status: FileStatus;
    targets: string[];
    error?: Error;
  }> => {
    let fileName = path.relative(projectRoot, uri.fsPath);
    try {
      const realPath = await fs_p.realpath(uri.fsPath);
      fileName = path.relative(projectRoot, realPath);
    } catch (error) {
      // Ignore errors, for example if the file doesn't exist
    }

    const activeFiles = target ? await this.getForTarget(target) : new Map();
    const targets = this.targetsForFile(fileName);
    const command = activeFiles.get(fileName);

    if (command?.error) {
      return { status: 'INVALID', targets, error: command.error };
    }

    const status: FileStatus =
      // prettier-ignore
      activeFiles.has(fileName) ? 'ACTIVE' :
      targets.length === 0 ? 'ORPHANED' : 'INACTIVE';

    return {
      status,
      targets,
    };
  };

  refresh: RefreshCallback = async () => {
    logger.info('Refreshing active files cache');
    const targets = await availableTargets();

    const targetSourceFiles = await Promise.all(
      targets.map(
        async (target) =>
          [target.name, await parseForSourceFiles(target)] as const,
      ),
    );

    this.activeFiles = Object.fromEntries(targetSourceFiles);
    logger.info('Finished refreshing active files cache');
    didUpdateActiveFilesCache.fire();
    return OK;
  };

  writeToSettings = async (target?: string) => {
    const settingsPath = path.join(workingDir.get(), '.clangd');
    const sharedSettingsPath = path.join(workingDir.get(), '.clangd.shared');

    // If the setting to disable code intelligence for files not in the build
    // of this target is disabled, then we need to:
    // 1. *Not* add configuration to disable clangd for any files
    // 2. *Remove* any prior such configuration that may have existed
    if (!settings.disableInactiveFileCodeIntelligence()) {
      handleInactiveFileCodeIntelligenceEnabled(
        settingsPath,
        sharedSettingsPath,
      );

      return;
    }

    if (!target) return;

    // Create clangd settings that disable code intelligence for all files
    // except those that are in the build for the specified target.
    const activeFilesForTarget = [
      ...((await this.getForTarget(target)).keys() ?? []),
    ];
    const escapedPaths = activeFilesForTarget.map((p) =>
      p.replace(/\\/g, '/').replace(/\+/g, '\\+'),
    );
    let data = yaml.dump(clangdSettingsDisableFiles(escapedPaths));

    // If there are other clangd settings for the project, append this fragment
    // to the end of those settings.
    if (fs.existsSync(sharedSettingsPath)) {
      const sharedSettingsData = (
        await fs_p.readFile(sharedSettingsPath)
      ).toString();
      data = `${sharedSettingsData}\n---\n${data}`;
    }

    await fs_p.writeFile(settingsPath, data, { flag: 'w+' });

    logger.info(
      `Updated .clangd to exclude files not in the build for: ${target}`,
    );
  };
}

/**
 * Handle the case where inactive file code intelligence is enabled.
 *
 * When this setting is enabled, we don't want to disable clangd for any files.
 * That's easy enough, but we also need to revert any configuration we created
 * while the setting was disabled (in other words, while we were disabling
 * clangd for certain files). This handles that and ends up at one of two
 * outcomes:
 *
 * - If there's a `.clangd.shared` file, that will become `.clangd`
 * - If there's not, `.clangd` will be removed
 *
 * Note: We use sync ops here to be able to use this in extension's deactivate()
 */
export function handleInactiveFileCodeIntelligenceEnabled(
  settingsPath: string,
  sharedSettingsPath: string,
) {
  if (fs.existsSync(sharedSettingsPath)) {
    if (!fs.existsSync(settingsPath)) {
      // If there's a shared settings file, but no active settings file, copy
      // the shared settings file to make an active settings file.
      // await fs_p.copyFile(sharedSettingsPath, settingsPath);
      fs.copyFileSync(sharedSettingsPath, settingsPath);
    } else {
      // If both shared settings and active settings are present, check if they
      // are identical. If so, no action is required. Otherwise, copy the shared
      // settings file over the active settings file.
      const settingsHash = createHash('md5').update(
        fs.readFileSync(settingsPath),
      );
      const sharedSettingsHash = createHash('md5').update(
        fs.readFileSync(sharedSettingsPath),
      );

      if (settingsHash !== sharedSettingsHash) {
        fs.copyFileSync(sharedSettingsPath, settingsPath);
      }
    }
  } else if (fs.existsSync(settingsPath)) {
    // If there's no shared settings file, then we just need to remove the
    // active settings file if it's present.
    fs.unlinkSync(settingsPath);
  }
}
