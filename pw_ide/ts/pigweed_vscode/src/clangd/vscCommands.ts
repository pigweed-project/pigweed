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

import * as vscode from 'vscode';
import { cpus } from 'os';
import { ClangdActiveFilesCache } from './activeFilesCache';
import { clangdPath as bazelClangdPath } from './bazel';
import {
  availableTargets,
  getTarget,
  baseSetTarget,
  Target,
  CDB_FILE_DIR,
  CDB_FILE_NAME,
} from './paths';

import { didChangeClangdConfig, didChangeTarget } from '../events';

import { launchTroubleshootingLink } from '../links';
import logger from '../logging';
import { OK, RefreshCallbackResult, RefreshManager } from '../refreshManager';
import {
  settingFor,
  settings,
  stringSettingFor,
  workingDir,
} from '../settings/vscode';
import { processCompDbs } from './compileCommandsUtils';
import {
  getTargetType,
  loadProcessedMapping,
  ProcessedTargetMapping,
  saveProcessedMapping,
} from './processedMapping';
import { saveUnprocessedMapping } from './unprocessedMapping';

export function getClangdArgs(targetDir: string, cores: number): string[] {
  return [
    `--compile-commands-dir=${targetDir}`,
    '--query-driver=/*',
    '--header-insertion=never',
    '--background-index',
    '-j=' + Math.max(1, Math.round(cores / 4)),
  ];
}

export async function setTargetWithClangd(
  target: Target | undefined,
  settingsFileWriter: (target: string) => Promise<void>,
): Promise<void> {
  target = target ?? getTarget();
  if (!target) return;

  if (!(await availableTargets()).map((t) => t.name).includes(target.name)) {
    throw new Error(`Target not among available targets: ${target}`);
  }

  const targetType = await getTargetType(target.name);
  let clangdPath: string;

  if (targetType === 'bazel') {
    const clangdPathForBazel = bazelClangdPath();

    if (!clangdPathForBazel) {
      vscode.window.showErrorMessage(
        'A Bazel target was selected, but a clangd binary could not be found ' +
          'among the Bazel binaries.',
      );

      return;
    }

    clangdPath = clangdPathForBazel;
  } else {
    const clangdPathForBootstrap = settings.clangdAlternatePath();

    if (!clangdPathForBootstrap) {
      vscode.window.showErrorMessage(
        'Set this config value to the path to clangd: ' +
          'pigweed.clangdAlternatePath',
      );

      return;
    }

    clangdPath = clangdPathForBootstrap;
  }

  await baseSetTarget(target);
  didChangeTarget.fire(target.name);

  const { update: updatePath } = stringSettingFor('path', 'clangd');
  const { update: updateArgs } = settingFor<string[]>('arguments', 'clangd');
  const cores = cpus().length;

  // These updates all happen asynchronously, and we want to make sure they're
  // all done before we trigger a clangd restart.
  await Promise.all([
    updatePath(clangdPath),
    updateArgs(getClangdArgs(target.dir, cores)),
    settingsFileWriter(target.name),
  ]);
  // Restart the clangd server so it picks up the new setting.
  await restartClangd();
}

/** Show a checkmark next to the item if it's the current setting. */
function markIfActive(active: boolean): vscode.ThemeIcon | undefined {
  return active ? new vscode.ThemeIcon('check') : undefined;
}

export async function setCompileCommandsTarget(
  activeFilesCache: ClangdActiveFilesCache,
): Promise<void> {
  const currentTarget = getTarget();
  const targets = await availableTargets();
  const targetNameMap = Object.fromEntries(
    targets.map((target) => [target.displayName, target]),
  );

  const targetEntries = targets.map((target) => ({
    label: target.displayName,
    iconPath: markIfActive(target === currentTarget),
  }));

  if (targetEntries.length === 0) {
    vscode.window
      .showErrorMessage("Couldn't find any targets!", 'Get Help')
      .then((selection) => {
        switch (selection) {
          case 'Get Help': {
            launchTroubleshootingLink('bazel-no-targets');
            break;
          }
        }
      });

    return;
  }

  vscode.window
    .showQuickPick(targetEntries, {
      title: 'Select a target',
      canPickMany: false,
    })
    .then(async (selection) => {
      if (!selection) return;
      const { label: targetName } = selection;
      await setTargetWithClangd(
        targetNameMap[targetName],
        activeFilesCache.writeToSettings,
      );
    });
}

async function refreshNonBazelCompileCommandsInternal(): Promise<RefreshCallbackResult> {
  const { processedCompDbs, unprocessedCompDbs } = await processCompDbs();

  const currentProcessedMapping = await loadProcessedMapping();

  const updatedProcessedMapping: ProcessedTargetMapping = Object.fromEntries(
    processedCompDbs.map((targetName) => [targetName, 'bootstrap']),
  );

  const newProcessedMapping: ProcessedTargetMapping = {
    ...currentProcessedMapping,
    ...updatedProcessedMapping,
  };

  const writePromises = [
    processedCompDbs.writeAll(workingDir.get(), CDB_FILE_DIR, CDB_FILE_NAME),
    saveProcessedMapping(newProcessedMapping),
  ];

  if (unprocessedCompDbs.length > 0) {
    writePromises.push(saveUnprocessedMapping(unprocessedCompDbs));
  }

  await Promise.all(writePromises);
  return OK;
}

export async function refreshNonBazelCompileCommands(
  refreshManager: RefreshManager<any>,
) {
  refreshManager.onOnce(refreshNonBazelCompileCommandsInternal, 'refreshing');
  refreshManager.refresh();
}

export async function refreshCompileCommandsAndSetTarget(
  refresh: () => void,
  refreshManager: RefreshManager<any>,
  activeFilesCache: ClangdActiveFilesCache,
) {
  refresh();
  await refreshManager.waitFor('didRefresh');
  await setCompileCommandsTarget(activeFilesCache);
}

export async function disableInactiveFileCodeIntelligence(
  activeFilesCache: ClangdActiveFilesCache,
) {
  logger.info('Disabling inactive file code intelligence');
  await settings.disableInactiveFileCodeIntelligence(true);
  didChangeClangdConfig.fire();
  await activeFilesCache.writeToSettings(settings.codeAnalysisTarget());
  await restartClangd();
}

export async function enableInactiveFileCodeIntelligence(
  activeFilesCache: ClangdActiveFilesCache,
) {
  logger.info('Enabling inactive file code intelligence');
  await settings.disableInactiveFileCodeIntelligence(false);
  didChangeClangdConfig.fire();
  await activeFilesCache.writeToSettings();
  await restartClangd();
}

export async function restartClangd(): Promise<void> {
  logger.warn(`Restarting clangd.`);
  await vscode.commands.executeCommand('clangd.restart');
}
