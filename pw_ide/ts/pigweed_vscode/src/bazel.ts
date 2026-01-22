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

import { getNativeBinary as getBazeliskBinary } from '@bazel/bazelisk';
import node_modules from 'node_modules-path';

import logger from './logging';

import {
  bazel_executable,
  buildifier_executable,
  settings,
  ConfigAccessor,
  bazel_codelens,
  workingDir,
} from './settings/vscode';
import { getPigweedProjectRoot, isBazelProject } from './project';
import { existsSync } from 'fs';

/** Should we try to generate compile commands for Bazel targets? */
export async function shouldSupportBazel(): Promise<boolean> {
  const projectRoot = await getPigweedProjectRoot(settings, workingDir);
  if (!projectRoot) return false;
  const settingValue = settings.supportBazelTargets();

  if (settingValue !== undefined && settingValue !== 'auto') {
    return settingValue;
  }

  return isBazelProject(projectRoot!);
}

/**
 * Is there a path to the given tool configured in VS Code settings?
 *
 * @param name The name of the tool
 * @param configAccessor A config accessor for the setting
 * @return Whether there is a path in settings that matches
 */
function hasConfiguredPathTo(
  name: string,
  configAccessor: ConfigAccessor<string>,
): boolean {
  const exe = configAccessor.get();
  return exe
    ? path.basename(exe).toLowerCase().includes(name.toLowerCase())
    : false;
}

/**
 * Find all system paths to the tool.
 * @param name The name of the tool
 * @return List of paths
 */
function findPathsTo(name: string): string[] {
  // TODO: https://pwbug.dev/351883170 - This only works on Unix-ish OSes.
  try {
    const stdout = child_process
      .execSync(`which -a ${name.toLowerCase()}`)
      .toString();
    // Parse the output into a list of paths, removing any duplicates/blanks.
    return [...new Set(stdout.split('\n'))].filter((item) => item.length > 0);
  } catch (err: unknown) {
    // If the above finds nothing, it returns a non-zero exit code.
    return [];
  }
}

export function vendoredBazeliskPath(): string | undefined {
  const result = getBazeliskBinary();

  // If there isn't a binary for this platform, the function appears to return
  // Promise<1>... strange. Either way, if it's not a string, then we don't
  // have a path.
  if (typeof result !== 'string') return undefined;

  return path.resolve(
    node_modules()!,
    '@bazel',
    'bazelisk',
    path.basename(result),
  );
}

/**
 * Get a path to Bazel no matter what.
 *
 * The difference between this and `bazel_executable.get()` is that this will
 * return the vendored Bazelisk as a last resort, whereas the former only
 * returns whatever path has been configured.
 */
export const getReliableBazelExecutable = () =>
  bazel_executable.get() ?? vendoredBazeliskPath();

function vendoredBuildifierPath(): string | undefined {
  const result = getBazeliskBinary();

  // If there isn't a binary for this platform, the function appears to return
  // Promise<1>... strange. Either way, if it's not a string, then we don't
  // have a path.
  if (typeof result !== 'string') return undefined;

  // Unlike the @bazel/bazelisk package, @bazel/buildifer doesn't export any
  // code. The logic is exactly the same, but with a different name.
  const binaryName = path.basename(result).replace('bazelisk', 'buildifier');

  return path.resolve(node_modules()!, '@bazel', 'buildifier', binaryName);
}

const VENDORED_LABEL = 'Use the version built in to the Pigweed extension';

/** Callback called when a tool path is selected from the dropdown. */
function onSetPathSelection(
  item: { label: string; picked?: boolean } | undefined,
  configAccessor: ConfigAccessor<string>,
  vendoredPath: string,
) {
  if (item && !item.picked) {
    if (item.label === VENDORED_LABEL) {
      configAccessor.update(vendoredPath);
    } else {
      configAccessor.update(item.label);
    }
  }
}

/** Show a checkmark next to the item if it's the current setting. */
function markIfActive(active: boolean): vscode.ThemeIcon | undefined {
  return active ? new vscode.ThemeIcon('check') : undefined;
}

/**
 * Let the user select a path to the given tool, or use the vendored version.
 *
 * @param name The name of the tool
 * @param configAccessor A config accessor for the setting
 * @param vendoredPath Path to the vendored version of the tool
 */
export async function interactivelySetToolPath(
  name: string,
  configAccessor: ConfigAccessor<string>,
  vendoredPath: string | undefined,
) {
  const systemPaths = findPathsTo(name);

  const vendoredItem = vendoredPath
    ? [
        {
          label: VENDORED_LABEL,
          iconPath: markIfActive(configAccessor.get() === vendoredPath),
        },
      ]
    : [];

  const items = [
    ...vendoredItem,
    {
      label: 'Paths found on your system',
      kind: vscode.QuickPickItemKind.Separator,
    },
    ...systemPaths.map((item) => ({
      label: item,
      iconPath: markIfActive(configAccessor.get() === item),
    })),
  ];

  if (items.length === 0) {
    vscode.window.showWarningMessage(
      `${name} couldn't be found on your system, and we don't have a ` +
        'vendored version compatible with your system. See the Bazelisk ' +
        'documentation for installation instructions.',
    );

    return;
  }

  vscode.window
    .showQuickPick(items, {
      title: `Select a ${name} path`,
      canPickMany: false,
    })
    .then((item) => onSetPathSelection(item, configAccessor, vendoredPath!));
}

export const interactivelySetBazeliskPath = () =>
  interactivelySetToolPath(
    'Bazelisk',
    bazel_executable,
    vendoredBazeliskPath(),
  );

export async function setBazelRecommendedSettings() {
  if (!settings.preserveBazelPath()) {
    await bazel_executable.update(vendoredBazeliskPath());
  }

  await buildifier_executable.update(vendoredBuildifierPath());
  await bazel_codelens.update(true);
}

export async function configureBazelSettings() {
  if (settings.disableBazelSettingsRecommendations()) return;

  logger.info('Auto-configuring Pigweed Bazel settings.');
  await setBazelRecommendedSettings();
  await settings.disableBazelSettingsRecommendations(true);
}

export async function updateVendoredBazelisk() {
  const bazeliskPath = bazel_executable.get();
  const isUsingVendoredBazelisk = !!bazeliskPath?.match(/pigweed\.pigweed-.*/);

  if (isUsingVendoredBazelisk && !settings.preserveBazelPath()) {
    logger.info('Updating Bazelisk path for current extension version');
    await bazel_executable.update(vendoredBazeliskPath());
  } else if (!existsSync(bazeliskPath!) && !settings.preserveBazelPath()) {
    await bazel_executable.update(vendoredBazeliskPath());
  }
}

export async function updateVendoredBuildifier() {
  const buildifierPath = buildifier_executable.get();
  const isUsingVendoredBuildifier =
    !!buildifierPath?.match(/pigweed\.pigweed-.*/);

  if (isUsingVendoredBuildifier) {
    logger.info('Updating Buildifier path for current extension version');
    await buildifier_executable.update(vendoredBuildifierPath());
  } else if (!existsSync(buildifierPath!)) {
    await buildifier_executable.update(vendoredBuildifierPath());
  }
}
