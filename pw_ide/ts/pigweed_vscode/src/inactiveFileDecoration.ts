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

import {
  EventEmitter,
  FileDecoration,
  FileDecorationProvider,
  ThemeColor,
  Uri,
  TextEditor,
  TextEditorDecorationType,
  DecorationOptions,
  Range,
  window,
} from 'vscode';

// Ensure FileStatus is imported as a value (enum) and not just a type
import { ClangdActiveFilesCache, FileStatus, CDB_FILE_DIR } from './clangd';
import { availableTargets } from './clangd/paths';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { Disposable } from './disposables';

export const fsUtils = {
  realpathSync: (pathStr: string) => fs.realpathSync(pathStr),
};

import {
  didChangeClangdConfig,
  didChangeTarget,
  didUpdateActiveFilesCache,
} from './events';

import logger from './logging';
import { getPigweedProjectRoot } from './project';
import { settings, workingDir } from './settings/vscode';

type DecorationsMap = Map<string, FileDecoration | undefined>;

/** Event emitted when the file decorations have changed. */
const didChangeFileDecorations = new EventEmitter<Uri[]>();

const makeDecoration = (
  target?: string,
): Record<FileStatus, FileDecoration | undefined> => ({
  ACTIVE: {
    badge: '✓',
    tooltip: `This file is built in the ${
      target ? `"${target}"` : 'current'
    } target.`,
  },
  INACTIVE: {
    badge: '𝓲',
    color: new ThemeColor('disabledForeground'),
    tooltip: `This file is not built in the ${
      target ? `"${target}"` : 'current'
    } target. Code intelligence will not work.`,
  },
  ORPHANED: {
    badge: '𝓲',
    color: new ThemeColor('disabledForeground'),
    tooltip: `This file is not built in the ${
      target ? `"${target}"` : 'current'
    } target. Code intelligence will not work.`,
  },
  INVALID: {
    badge: '⚠️',
    color: new ThemeColor('list.warningForeground'),
    tooltip: 'This file has an invalid compile command.',
  },
});

// Define the banner decoration type
const bannerDecorationType = window.createTextEditorDecorationType({
  isWholeLine: true,
  // We will define the specific 'before' content dynamically using DecorationOptions
});

export class InactiveFileDecorationProvider
  extends Disposable
  implements FileDecorationProvider
{
  private decorations: DecorationsMap;
  private activeFilesCache: ClangdActiveFilesCache;
  private bannerDecorationType: TextEditorDecorationType;
  private isOutdated = false;

  constructor(activeFilesCache: ClangdActiveFilesCache) {
    super();
    this.activeFilesCache = activeFilesCache;
    this.decorations = new Map();
    this.bannerDecorationType = bannerDecorationType; // Use the shared instance

    this.disposables.push(
      ...[
        window.registerFileDecorationProvider(this),
        // Listen to events that might change file status
        // didChangeTarget emits string | undefined, which matches handleStatusChange signature
        didChangeTarget.event(this.handleStatusChange),
        // didUpdateActiveFilesCache emits void, use lambda to call handler without args
        didUpdateActiveFilesCache.event(() => this.handleStatusChange()),
        // Listen to editor changes to update banner
        window.onDidChangeActiveTextEditor((editor) =>
          this.updateEditorBanner(editor),
        ),
        window.onDidChangeTextEditorSelection((event) =>
          this.updateEditorBanner(event.textEditor),
        ),

        // Also dispose the banner decoration type when the provider is disposed
        this.bannerDecorationType,
      ],
    );

    // Conditionally listen to clangd config changes
    if (!settings.hideInactiveFileIndicators()) {
      // didChangeClangdConfig emits void, use lambda to call handler without args
      this.disposables.push(
        didChangeClangdConfig.event(() => this.handleStatusChange()),
      );
    }

    // Initial update for file decorations and the currently active editor banner
    this.update().then(() => {
      this.updateEditorBanner(window.activeTextEditor);
    });
  }

  // When this event is triggered, VSC is notified that the files included in
  // the event have had their decorations changed.
  readonly onDidChangeFileDecorations = didChangeFileDecorations.event;

  // This is called internally by VSC to provide the decoration for a file
  // whenever it's asked for.
  provideFileDecoration(uri: Uri) {
    // Return decoration only if the feature is enabled
    if (settings.hideInactiveFileIndicators()) {
      return undefined;
    }
    return this.decorations.get(uri.toString());
  }

  private async checkOutdated() {
    const projectRoot = await getPigweedProjectRoot(settings, workingDir);
    if (!projectRoot) return;

    const timeFile = path.join(
      projectRoot,
      CDB_FILE_DIR,
      'pw_lastGenerationTime.txt',
    );

    if (fs.existsSync(timeFile)) {
      try {
        const content = fs.readFileSync(timeFile, 'utf8');
        const timestamp = parseInt(content.trim(), 10);
        const now = Math.floor(Date.now() / 1000);
        // Older than 1 week (604800 seconds)
        if (now - timestamp > 604800) {
          this.isOutdated = true;
          return;
        }
      } catch (e) {
        logger.error(`Failed to read timestamp file: ${e}`);
      }
    }
    this.isOutdated = false;
  }

  /** Provide updated file decorations for the current target's active files. */
  async refreshDecorations(providedTarget?: string): Promise<DecorationsMap> {
    const newDecorations: DecorationsMap = new Map();
    const target = providedTarget ?? settings.codeAnalysisTarget();

    // Don't calculate decorations if feature is disabled
    if (settings.hideInactiveFileIndicators()) {
      return newDecorations;
    }

    const projectRoot = await getPigweedProjectRoot(settings, workingDir);

    if (!projectRoot) {
      return newDecorations;
    }

    const workspaceFiles = await vscode.workspace.findFiles(
      '**/*.{c,cc,cpp,h,hh,hpp,hxx}', // include
      '**/{.*,bazel*,external}/**', // exclude
    );

    const allAvailableTargets = await availableTargets();
    const matchedTarget = allAvailableTargets.find((t) => t.name === target);
    const targetDisplayName = matchedTarget?.displayName || target;

    // Create the static part of the decoration map once
    const decorationMap = makeDecoration(targetDisplayName);

    for (const uri of workspaceFiles) {
      if (isGeneratedOrExternalFile(uri.fsPath, projectRoot)) {
        continue;
      }
      // Fetch status only if the feature is enabled
      const { status } = await this.activeFilesCache.fileStatus(
        projectRoot,
        uri,
        target,
      );

      const decoration = decorationMap[status];
      newDecorations.set(uri.toString(), decoration);
    }

    return newDecorations;
  }

  /** Update file decorations per the current target's active files. */
  update = async (target?: string) => {
    logger.info('Updating inactive file indicators');
    await this.checkOutdated();
    const newDecorations = await this.refreshDecorations(target);

    // Determine which URIs need to be refreshed in the UI
    const updatedUris = new Set<string>();
    const previousUris = new Set(this.decorations.keys());

    for (const [uriStr, newDecor] of newDecorations.entries()) {
      const oldDecor = this.decorations.get(uriStr);
      // Update if decoration changed or if it's a new entry
      if (oldDecor !== newDecor) {
        updatedUris.add(uriStr);
      }
    }
    // Also update URIs that were decorated before but are not anymore
    for (const uriStr of previousUris) {
      if (!newDecorations.has(uriStr)) {
        updatedUris.add(uriStr);
      }
    }

    // Update internal map for provideFileDecoration
    this.decorations = newDecorations;

    // Firing this event notifies VSC that decorations for these specific files have changed.
    // Only fire if there are changes or if the feature was toggled (to clear old ones).
    if (updatedUris.size > 0) {
      // Doing a bulk .fire doesn't seem to always reflect change in UI for some reason.
      for (const uriStr of updatedUris) {
        didChangeFileDecorations.fire([Uri.parse(uriStr, true)]);
      }
    }

    // After updating file decorations, also update the banner in the active editor,
    // as its status might have changed.
    await this.updateEditorBanner(window.activeTextEditor);
  };

  /** Handles events that require both file decorations and editor banner updates. */
  private handleStatusChange = async (target?: string | undefined) => {
    // The target parameter might come from didChangeTarget event (string | undefined)
    // or be undefined if called from a void event listener.
    // `update` already handles an optional target.
    await this.update(target);
  };

  /** Update the banner decoration for the given text editor based on file status. */
  private async updateEditorBanner(editor: TextEditor | undefined) {
    if (!editor) {
      return; // No active editor
    }

    const uri = editor.document.uri;
    // Only apply banners to files potentially relevant to C/C++ analysis
    if (!/\.(c|cc|cpp|h|hh|hpp|hxx)$/.test(uri.fsPath)) {
      editor.setDecorations(this.bannerDecorationType, []);
      return;
    }

    // Don't show banner if feature is disabled
    if (settings.hideInactiveFileIndicators()) {
      editor.setDecorations(this.bannerDecorationType, []);
      return;
    }

    const projectRoot = await getPigweedProjectRoot(settings, workingDir);
    if (!projectRoot) {
      editor.setDecorations(this.bannerDecorationType, []); // Clear banner if no project root
      return;
    }

    // Do not show banners on generated or external files (e.g., under bazel-out, external, environment)
    if (isGeneratedOrExternalFile(uri.fsPath, projectRoot)) {
      editor.setDecorations(this.bannerDecorationType, []);
      return;
    }

    const target = settings.codeAnalysisTarget();

    const { status, targets, error } = await this.activeFilesCache.fileStatus(
      projectRoot,
      uri,
      target,
    );

    const decorationsArray: DecorationOptions[] = [];

    if (status === 'INVALID' && error) {
      const message = `⚠️ Invalid compile command: ${error.message}`;
      const hoverMessage = `This file has an invalid entry in the compile_commands.json file for the active target "${target}". Please check the file for errors.`;
      const range = new Range(0, 0, 0, 0); // Position at the very top
      decorationsArray.push({
        range,
        hoverMessage,
        renderOptions: {
          before: {
            contentText: message,
            color: new ThemeColor('editorError.foreground'),
            backgroundColor: new ThemeColor('editorPane.background'),
            border: `1px solid`,
            borderColor: new ThemeColor('editorError.foreground'),
            textDecoration:
              ';padding: 2px 5px; margin: 2px 0; display: block;position: absolute;',
          },
        },
      });
    } else if (
      status === 'ACTIVE' &&
      this.isOutdated &&
      editor.selection.active.line >= 5
    ) {
      const message = `⚠️ Compile commands may be outdated. Regenerate for accurate results.`;
      const hoverMessage = `The compile commands file was last updated more than 1 week ago.`;

      const range = new Range(0, 0, 0, 0);
      decorationsArray.push({
        range,
        hoverMessage,
        renderOptions: {
          before: {
            contentText: message,
            color: new ThemeColor('editorWarning.foreground'),
            backgroundColor: new ThemeColor('editorPane.background'),
            border: `1px solid`,
            borderColor: new ThemeColor('editorWarning.foreground'),
            textDecoration:
              ';padding: 2px 5px; margin: 2px 0; display: block;position: absolute;',
          },
        },
      });
    } else if (
      (status === 'INACTIVE' || status === 'ORPHANED') &&
      editor.selection.active.line >= 5
    ) {
      const allAvailableTargets = await availableTargets();
      const displayTargets = targets.map((t) => {
        const matched = allAvailableTargets.find((at) => at.name === t);
        return matched?.displayName || t;
      });

      const maxTargetsToShow = 3;
      const truncatedTargets = displayTargets.slice(0, maxTargetsToShow);
      const andMore = displayTargets.length > maxTargetsToShow ? '...' : '';

      const message =
        status === 'INACTIVE' && targets.length > 0
          ? `𝓲 Not in active target. Switch to: ${truncatedTargets
              .map((t: string) => `"${t}"`)
              .join(', ')}${andMore}`
          : `𝓲 Generate compile commands to see code intelligence for this file`;

      const hoverMessage =
        status === 'INACTIVE' && targets.length > 0
          ? `This file is not in the active "${target}" target, so code intelligence may be incorrect. To fix this, switch to a target that builds this file. This file is included in the following targets: ${displayTargets
              .map((t: string) => `"${t}"`)
              .join(', ')}.`
          : `This file is not included in the compilation for any known target. Clangd features like completion, diagnostics, and navigation will not be available.`;

      // Define the banner content and style dynamically
      const range = new Range(0, 0, 0, 0); // Position at the very top
      decorationsArray.push({
        range,
        hoverMessage,
        renderOptions: {
          before: {
            contentText: message,
            // Apply styling similar to the user's example
            color: new ThemeColor('editorWarning.foreground'),
            backgroundColor: new ThemeColor('editorPane.background'),
            // Reference ThemeColor id directly for border
            border: `1px solid`,
            borderColor: new ThemeColor('editorWarning.foreground'),
            // Use CSS for padding and positioning
            textDecoration:
              ';padding: 2px 5px; margin: 2px 0; display: block;position: absolute;',
          },
        },
      });
    }

    // Apply the decoration (or clear it if decorationsArray is empty)
    editor.setDecorations(this.bannerDecorationType, decorationsArray);
  }
}

/**
 * Returns true if the file path is located under a generated or external directory
 * (e.g., bazel-out, external, or environment).
 */
export function isGeneratedOrExternalFile(
  filePath: string,
  projectRoot?: string,
): boolean {
  if (/[/\\\\](bazel-[^/\\\\]+|external|environment)[/\\\\]/.test(filePath)) {
    return true;
  }
  if (projectRoot) {
    try {
      const resolvedPath = fsUtils.realpathSync(filePath);
      const resolvedRoot = fsUtils.realpathSync(projectRoot);
      if (!resolvedPath.startsWith(resolvedRoot)) {
        return true;
      }
    } catch (e) {
      if (!filePath.startsWith(projectRoot)) {
        return true;
      }
    }
  }
  return false;
}
