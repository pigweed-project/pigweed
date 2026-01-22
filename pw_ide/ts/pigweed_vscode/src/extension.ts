// Copyright 2023 The Pigweed Authors
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
import { ExtensionContext } from 'vscode';

import {
  configureBazelSettings,
  interactivelySetBazeliskPath,
  setBazelRecommendedSettings,
  shouldSupportBazel,
  updateVendoredBazelisk,
  updateVendoredBuildifier,
} from './bazel';

import {
  availableTargets,
  ClangdActiveFilesCache,
  disableInactiveFileCodeIntelligence,
  enableInactiveFileCodeIntelligence,
  getTarget,
  initBazelClangdPath,
  setCompileCommandsTarget,
  setTargetWithClangd,
} from './clangd';

import { getSettingsData, syncSettingsSharedToProject } from './configParsing';
import { Disposer } from './disposables';
import { didInit, linkRefreshManagerToEvents } from './events';
import { checkExtensions } from './extensionManagement';
import { InactiveFileDecorationProvider } from './inactiveFileDecoration';
import logger, { output } from './logging';
import { fileBug, launchTroubleshootingLink } from './links';

import {
  getPigweedProjectRoot,
  isBazelProject,
  isBootstrapProject,
} from './project';
import { OK, RefreshManager } from './refreshManager';
import { settings, workingDir } from './settings/vscode';
import {
  ClangdFileWatcher,
  SettingsFileWatcher,
} from './settings/settingsWatcher';

import {
  InactiveVisibilityStatusBarItem,
  TargetStatusBarItem,
} from './statusBar';

import {
  launchBootstrapTerminal,
  launchTerminal,
  patchBazeliskIntoTerminalPath,
} from './terminal';
import {
  WebviewProvider,
  executeRefreshCompileCommandsManually,
} from './webviewProvider';

import { commandRegisterer, VscCommandCallback } from './utils';
import { shouldSupportGn } from './gn';
import { shouldSupportCmake } from './cmake';
import { CompileCommandsWatcher } from './clangd/compileCommandsWatcher';
import { existsSync, statSync } from 'node:fs';
import {
  createBazelInterceptorFile,
  deleteBazelInterceptorFile,
} from './clangd/compileCommandsUtils';
import { checkClangdVersion } from './clangd/extensionChecker';
import { handleInactiveFileCodeIntelligenceEnabled } from './clangd/activeFilesCache';
import * as path from 'path';
import { getPreconfiguredTargets } from './bazelQuery';
import { manageBazelInterceptor } from './interceptorLogic';

interface CommandEntry {
  name: string;
  callback: VscCommandCallback;
}

const disposer = new Disposer();

type ProjectType = 'bazel' | 'bootstrap' | 'both';

async function registerCommands(
  projectType: ProjectType,
  context: ExtensionContext,
  refreshManager: RefreshManager<any>,
  clangdActiveFilesCache: ClangdActiveFilesCache,
): Promise<void> {
  const commands: CommandEntry[] = [
    {
      name: 'pigweed.open-output-panel',
      callback: output.show,
    },
    {
      name: 'pigweed.file-bug',
      callback: fileBug,
    },
    {
      name: 'pigweed.check-extensions',
      callback: checkExtensions,
    },
    {
      name: 'pigweed.sync-settings',
      callback: async () =>
        syncSettingsSharedToProject(await getSettingsData(), true),
    },
    {
      name: 'pigweed.disable-inactive-file-code-intelligence',
      callback: () =>
        disableInactiveFileCodeIntelligence(clangdActiveFilesCache),
    },
    {
      name: 'pigweed.enable-inactive-file-code-intelligence',
      callback: () =>
        enableInactiveFileCodeIntelligence(clangdActiveFilesCache),
    },
    {
      name: 'pigweed.select-target',
      callback: () => setCompileCommandsTarget(clangdActiveFilesCache),
    },
    {
      name: 'pigweed.launch-terminal',
      callback: launchTerminal,
    },
    {
      name: 'pigweed.bootstrap-terminal',
      callback: launchBootstrapTerminal,
    },
    {
      name: 'pigweed.set-bazel-recommended-settings',
      callback: setBazelRecommendedSettings,
    },
    {
      name: 'pigweed.set-bazelisk-path',
      callback: interactivelySetBazeliskPath,
    },
    {
      name: 'pigweed.refresh-compile-commands',
      callback: async () => {
        const lastBuildCmd = settings.bazelCompileCommandsManualBuildCommand();
        if (lastBuildCmd) {
          await executeRefreshCompileCommandsManually(lastBuildCmd);
        } else {
          vscode.window.showInformationMessage(
            'No manual build command found. Please set and run once from the Pigweed sidebar.',
          );
        }
      },
    },
    {
      name: 'pigweed.activate-bazelisk-in-terminal',
      callback: patchBazeliskIntoTerminalPath,
    },
  ];

  const registerCommand = commandRegisterer(context);
  commands.forEach((c) => registerCommand(c.name, c.callback));
}

async function initAsBazelProject(refreshManager: RefreshManager<any>) {
  // Do stuff that we want to do on load.
  await updateVendoredBazelisk();
  await updateVendoredBuildifier();
  await initBazelClangdPath();
  await configureBazelSettings();
  linkRefreshManagerToEvents(refreshManager);

  if (settings.activateBazeliskInNewTerminals()) {
    vscode.window.onDidOpenTerminal(
      patchBazeliskIntoTerminalPath,
      disposer.disposables,
    );
  }
}

/**
 * Get the project type and configuration details on startup.
 *
 * This is a little long and over-complex, but essentially it does just a few
 * things:
 *   - Do I know where the Pigweed project root is?
 *   - Do I know if this is a Bazel or bootstrap project?
 *   - If I don't know any of that, ask the user to tell me and save the
 *     selection to settings.
 *   - If the user needs help, route them to the right place.
 */
async function configureProject(
  context: vscode.ExtensionContext,
  refreshManager: RefreshManager<any>,
  clangdActiveFilesCache: ClangdActiveFilesCache,
  forceProjectType?: 'bazel' | 'bootstrap' | undefined,
): Promise<ProjectType | undefined> {
  const projectTypes: ProjectType[] = [];

  // If we're missing a piece of information, we can ask the user to manually
  // provide it. If they do, we should re-run this flow, and that intent is
  // signaled by setting this var.
  let tryAgain = false;
  let tryAgainProjectType: 'bazel' | 'bootstrap' | undefined;

  const projectRoot = await getPigweedProjectRoot(settings, workingDir);

  if (projectRoot) {
    output.appendLine(`The Pigweed project root is ${projectRoot}`);
    let foundProjectBuildFile = false;

    if (isBazelProject(projectRoot) || forceProjectType === 'bazel') {
      output.appendLine('This is a Bazel project');
      projectTypes.push('bazel');
      foundProjectBuildFile = true;
    }

    if (isBootstrapProject(projectRoot) || forceProjectType === 'bootstrap') {
      output.appendLine(
        `This is ${foundProjectBuildFile ? 'also ' : ' '}bootstrap project`,
      );

      projectTypes.push('bootstrap');
      foundProjectBuildFile = true;
    }

    if (!foundProjectBuildFile) {
      vscode.window
        .showErrorMessage(
          "I couldn't automatically determine what type of Pigweed project " +
            'this is. Choose one of these project types, or get more help.',
          'Bazel',
          'Bootstrap',
          'Get Help',
        )
        .then((selection) => {
          switch (selection) {
            case 'Bazel': {
              tryAgainProjectType = 'bazel';
              vscode.window.showInformationMessage(
                'Configured as a Pigweed Bazel project',
              );
              tryAgain = true;
              break;
            }
            case 'Bootstrap': {
              tryAgainProjectType = 'bootstrap';
              vscode.window.showInformationMessage(
                'Configured as a Pigweed Bootstrap project',
              );
              tryAgain = true;
              break;
            }
            case 'Get Help': {
              launchTroubleshootingLink('project-type');
              break;
            }
          }
        });
    }
  } else {
    // If we can't find the root, we silently fail and let the webview show the disabled state.
    return undefined;
  }

  // This should only re-run if something has materially changed, e.g., the user
  // provided a piece of information we needed.
  if (tryAgain) {
    await configureProject(
      context,
      refreshManager,
      clangdActiveFilesCache,
      tryAgainProjectType,
    );
  }

  const projectType =
    projectTypes.length === 2
      ? 'both'
      : projectTypes.length === 1
        ? projectTypes[0]
        : undefined;

  if (projectType) {
    vscode.commands.executeCommand(
      'setContext',
      'pigweed.projectType',
      projectType,
    );
  }

  return projectType;
}

function buildSystemStatusReason(
  settingsValue: boolean | 'auto',
  activeState: boolean,
): string {
  switch (settingsValue) {
    case true:
      return 'Enabled in settings.';
    case false:
      return 'Disabled in settings.';
    case 'auto':
      return `Build files ${activeState ? '' : "don't "}exist in project.`;
  }
}

export async function activate(context: vscode.ExtensionContext) {
  // Perform the Clangd version check when the extension activates
  const clangdOK = await checkClangdVersion();

  if (!clangdOK) {
    logger.warn(
      'Pigweed extension is unable to function fully due to Clangd version requirements.',
    );
    return;
  }

  logger.info('Extension loaded');
  logger.info('');

  const preconfiguredTargets = await getPreconfiguredTargets(workingDir.get());
  const isPreconfigured = preconfiguredTargets.length > 0;

  const useBazel = await shouldSupportBazel();
  const useCmake = await shouldSupportCmake();
  const useGn = await shouldSupportGn();

  logger.info(`Bazel support is ${useBazel ? 'ACTIVE' : 'DISABLED'}`);
  logger.info(
    `↳ Reason: ${buildSystemStatusReason(
      settings.supportBazelTargets(),
      useBazel,
    )}`,
  );
  logger.info('');

  logger.info(`CMake support is ${useCmake ? 'ACTIVE' : 'DISABLED'}`);
  logger.info(
    `↳ Reason: ${buildSystemStatusReason(
      settings.supportCmakeTargets(),
      useCmake,
    )}`,
  );
  logger.info('');

  logger.info(`GN support is ${useGn ? 'ACTIVE' : 'DISABLED'}`);
  logger.info(
    `↳ Reason: ${buildSystemStatusReason(settings.supportGnTargets(), useGn)}`,
  );
  logger.info('');

  // Marshall all of our components and dependencies.
  const refreshManager = disposer.add(RefreshManager.create({ logger }));

  refreshManager.on(async () => {
    const target = getTarget();
    const allTargets = await availableTargets();

    if (target && allTargets.some((t) => t.name === target.name)) {
      // If we have a target and it's still in the list of available targets,
      // just re-run the setup for it. This will fix any bad settings.
      await setTargetWithClangd(target, clangdActiveFilesCache.writeToSettings);
    } else if (allTargets.length > 0) {
      // If there's no target, or the old one is gone, pick the biggest file as
      // the new target.
      const fileSizes = await Promise.all(
        allTargets.map(async (t) => {
          if (existsSync(t.path)) {
            return statSync(t.path).size;
          } else {
            return 0;
          }
        }),
      );
      const biggestFileIndex = fileSizes.indexOf(Math.max(...fileSizes));
      const biggestFileTarget = allTargets[biggestFileIndex];
      logger.info('Auto-picked biggest target: ' + biggestFileTarget.name);
      await setTargetWithClangd(
        biggestFileTarget,
        clangdActiveFilesCache.writeToSettings,
      );
    }
    return OK;
  }, 'didRefresh');

  const { clangdActiveFilesCache } = disposer.addMany({
    clangdActiveFilesCache: new ClangdActiveFilesCache(refreshManager),
    inactiveVisibilityStatusBarItem: new InactiveVisibilityStatusBarItem(),
    settingsFileWatcher: new SettingsFileWatcher(),
    targetStatusBarItem: new TargetStatusBarItem(),
  });

  const provider = new WebviewProvider(
    context.extensionUri,
    clangdActiveFilesCache,
  );

  refreshManager.on(async () => {
    await provider.refresh();
    return OK;
  }, 'didRefresh');

  context.subscriptions.push(
    vscode.window.registerWebviewViewProvider(
      WebviewProvider.viewType,
      provider,
    ),
  );

  disposer.add(new ClangdFileWatcher(clangdActiveFilesCache));
  disposer.add(new InactiveFileDecorationProvider(clangdActiveFilesCache));

  // Determine the project type and configuration parameters.
  const projectType = await configureProject(
    context,
    refreshManager,
    clangdActiveFilesCache,
  );

  if (projectType === undefined) {
    provider.setExtensionDisabled(true);
    return;
  }

  if (projectType === 'bazel' || projectType === 'both') {
    disposer.add(
      new CompileCommandsWatcher(refreshManager, clangdActiveFilesCache),
    );

    // Don't do Bazel init if the user has explicitly disabled Bazel support.
    if (useBazel) {
      await initAsBazelProject(refreshManager);
    }

    await manageBazelInterceptor(
      settings.disableBazelInterceptor(),
      isPreconfigured,
      createBazelInterceptorFile,
    );

    registerCommands(
      projectType,
      context,
      refreshManager,
      clangdActiveFilesCache,
    );
  } else {
    registerCommands(
      projectType,
      context,
      refreshManager,
      clangdActiveFilesCache,
    );
  }

  if (settings.enforceExtensionRecommendations()) {
    logger.info('Project is configured to enforce extension recommendations');
    await checkExtensions();
  }

  logger.info('Extension initialization complete');
  didInit.fire();
  refreshManager.refresh();
}

export async function deactivate() {
  // Clean up the Bazel interceptor script
  deleteBazelInterceptorFile();

  // Clean up the .clangd file to a default state
  const rootClangdPath = path.join(workingDir.get(), '.clangd');
  const sharedClangdPath = path.join(workingDir.get(), '.clangd.shared');
  handleInactiveFileCodeIntelligenceEnabled(rootClangdPath, sharedClangdPath);

  disposer.dispose();
}
