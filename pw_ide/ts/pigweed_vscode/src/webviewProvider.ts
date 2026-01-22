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

import path from 'path';
import fs from 'fs';
import * as vscode from 'vscode';
import { checkExtensionsAndGetStatus } from './extensionManagement';
import { restartClangd, setTargetWithClangd } from './clangd';
import { initBazelClangdPath } from './clangd/bazel';
import logging, { output } from './logging';
import { getSettingsData } from './configParsing';
import getCipdReport from './clangd/report';
import { availableTargets } from './clangd/paths';
import {
  ClangdActiveFilesCache,
  parseForSourceFiles,
} from './clangd/activeFilesCache';
import { existsSync } from 'fs';
import {
  createBazelInterceptorFile,
  deleteBazelInterceptorFile,
  getBazelInterceptorPath,
} from './clangd/compileCommandsUtils';
import { LoggerUI } from './clangd/compileCommandsGeneratorUI';
import { spawn } from 'child_process';

import { getReliableBazelExecutable } from './bazel';
import { settings, workingDir } from './settings/vscode';
import { CDB_FILE_DIR, LAST_BAZEL_COMMAND_FILE_NAME } from './clangd/paths';

function saveLastBazelCommand(
  cwd: string,
  bazelCmd: string,
  logger?: LoggerUI,
) {
  try {
    const compileCommandsDir = path.join(cwd, CDB_FILE_DIR);
    if (!fs.existsSync(compileCommandsDir)) {
      fs.mkdirSync(compileCommandsDir, { recursive: true });
    }
    const filePath = path.join(
      compileCommandsDir,
      LAST_BAZEL_COMMAND_FILE_NAME,
    );
    fs.writeFileSync(filePath, bazelCmd, 'utf-8');
    logger?.addStdout(
      `Saved last bazel command to ${CDB_FILE_DIR}/${LAST_BAZEL_COMMAND_FILE_NAME}.`,
    );
  } catch (e: unknown) {
    logger?.addStderr('Failed to save last bazel command: ' + String(e));
  }
}

function spawnAsync(
  command: string,
  args: string[],
  cwd: string,
  logger?: LoggerUI,
  bazelBinary?: string,
): Promise<number> {
  return new Promise((resolve) => {
    bazelBinary = getReliableBazelExecutable();
    logger?.addStdout(`Running command: ${command} ${args.join(' ')}\n`);
    const env = bazelBinary
      ? {
          ...process.env,
          PATH: `${path.dirname(bazelBinary)}:${process.env?.PATH || ''}`,
          BAZELISK_SKIP_WRAPPER: '1',
          BAZEL_REAL: bazelBinary,
        }
      : process.env;
    const child = spawn(command, args, { cwd, env });

    child.stdout.on('data', (data) => logger?.addStdout(data.toString()));
    child.stderr.on('data', (data) => logger?.addStderr(data.toString()));
    child.on('close', (code) => resolve(code ?? -1));
  });
}

export async function generateAspectCompileCommands(
  bazelBinary: string,
  buildCmd: string,
  cwd: string,
  logger?: LoggerUI,
) {
  // Aspect-based generator
  logger?.addStdout('Building with compile commands aspect...\n');
  const exitCode = await spawnAsync(
    bazelBinary,
    [
      'run',
      '@pigweed//pw_ide/bazel:update_compile_commands',
      '--',
      '--',
      'build',
      buildCmd,
    ],
    cwd,
    logger,
    bazelBinary,
  );

  if (exitCode !== 0) {
    logger?.finishWithError(
      `❌ Updating compile commands failed with exit code ${exitCode}.`,
    );
    return;
  }

  logger?.finish('✅ Compile commands generated successfully.');
  saveLastBazelCommand(cwd, `build ${buildCmd}`, logger);
}

export async function executeRefreshCompileCommandsManually(buildCmd: string) {
  const bazelBinary = getReliableBazelExecutable();
  const cwd = workingDir.get();
  logging.info(`Generating compile commands for ${buildCmd}`);

  if (buildCmd.trim() === '' || !bazelBinary) {
    vscode.window.showWarningMessage(
      'Build command is empty or Bazel binary not found.',
    );
    return;
  }

  output.show();
  const logger = new LoggerUI(logging);
  await settings.bazelCompileCommandsManualBuildCommand(buildCmd);

  generateAspectCompileCommands(bazelBinary, buildCmd, cwd, logger);
}

export class WebviewProvider implements vscode.WebviewViewProvider {
  public static readonly viewType = 'pigweed.webview';

  private _view?: vscode.WebviewView;
  private _isExtensionDisabled = false;

  constructor(
    private readonly _extensionUri: vscode.Uri,
    private readonly _activeFilesCache: ClangdActiveFilesCache,
  ) {}

  public async refresh() {
    if (this._view) {
      await this.sendCipdReport();
    }
  }

  public setExtensionDisabled(disabled: boolean) {
    this._isExtensionDisabled = disabled;
    this._view?.webview.postMessage({
      type: 'extensionDisabled',
      data: disabled,
    });
  }

  public resolveWebviewView(
    webviewView: vscode.WebviewView,
    // eslint-disable-next-line @typescript-eslint/no-unused-vars
    _context: vscode.WebviewViewResolveContext,
    // eslint-disable-next-line @typescript-eslint/no-unused-vars
    _token: vscode.CancellationToken,
  ) {
    this._view = webviewView;

    // Send the disabled state immediately upon resolution
    if (this._isExtensionDisabled) {
      webviewView.webview.postMessage({
        type: 'extensionDisabled',
        data: this._isExtensionDisabled,
      });
    }

    webviewView.webview.options = {
      // Allow scripts in the webview
      enableScripts: true,

      localResourceRoots: [this._extensionUri],
    };

    webviewView.webview.html = this._getHtmlForWebview(webviewView.webview);

    webviewView.webview.onDidReceiveMessage(async (data) => {
      switch (data.type) {
        case 'getExtensionData': {
          const report = await checkExtensionsAndGetStatus();
          logging.info('getExtensionData reported: ' + JSON.stringify(report));
          this._view?.webview.postMessage({
            type: 'extensionData',
            data: report,
          });
          this._view?.webview.postMessage({
            type: 'extensionDisabled',
            data: this._isExtensionDisabled,
          });
          break;
        }
        case 'getCipdReport': {
          await this.sendCipdReport();
          break;
        }
        case 'refreshCompileCommands': {
          vscode.commands.executeCommand('pigweed.refresh-compile-commands');
          break;
        }
        case 'restartClangd': {
          await restartClangd();
          break;
        }
        case 'retryClangdPath': {
          output.show();
          await initBazelClangdPath();
          await this.sendCipdReport();
          break;
        }
        case 'openExtension': {
          const extensionId = data.data;
          await vscode.commands.executeCommand('extension.open', extensionId);
          break;
        }
        case 'dumpLogs': {
          const workspaceFolders = vscode.workspace.workspaceFolders;
          if (!workspaceFolders) return {};
          const workspaceFolder = workspaceFolders[0];

          const logsFilePath = vscode.Uri.joinPath(
            workspaceFolder.uri,
            'pigweed-vscode-logs.txt',
          );

          const logs = logging.logs;
          let output = '';
          const settings = await getSettingsData();
          output +=
            'SETTINGS\n========\n' + JSON.stringify(settings, null, 2) + '\n\n';
          output += 'LOGS\n====\n';
          for (const log of logs) {
            output += `[${log.timestamp.toISOString()}] [${log.level.toUpperCase()}] ${
              log.message
            }\n`;
          }
          await vscode.workspace.fs.writeFile(
            logsFilePath,
            Buffer.from(output),
          );
          vscode.window.showInformationMessage(
            'Logs dumped to ' + logsFilePath.fsPath,
          );
          break;
        }
        case 'enableBazelBuildInterceptor': {
          logging.info('Enabling bazel build interceptor');
          await createBazelInterceptorFile();
          settings.disableBazelInterceptor(false);
          vscode.window.showInformationMessage(
            'Bazel build interceptor enabled',
          );
          await this.sendCipdReport();
          break;
        }
        case 'disableBazelBuildInterceptor': {
          logging.info('Disabling bazel build interceptor');
          deleteBazelInterceptorFile();
          settings.disableBazelInterceptor(true);
          vscode.window.showInformationMessage(
            'Bazel build interceptor disabled',
          );
          await this.sendCipdReport();
          break;
        }

        case 'refreshCompileCommandsManually': {
          const buildCmd = data.data;
          await executeRefreshCompileCommandsManually(buildCmd);
          break;
        }
        case 'runPreconfiguredTarget': {
          const target = data.data;
          const bazelBinary = getReliableBazelExecutable();
          if (!bazelBinary) {
            vscode.window.showErrorMessage('Bazel binary not found.');
            break;
          }
          output.show();
          const logger = new LoggerUI(logging);
          const cwd = workingDir.get();

          logging.info(`Running ${bazelBinary} run ${target}`);

          // We use `bazel run` to execute the target.
          // The target is expected to be a `pw_compile_commands_generator` target.
          const exitCode = await spawnAsync(
            bazelBinary,
            ['run', target],
            cwd,
            logger,
            bazelBinary,
          );
          if (exitCode !== 0) {
            logger?.finishWithError(
              `❌ Updating compile commands failed with exit code ${exitCode}.`,
            );
            return;
          }

          logger?.finish('✅ Compile commands generated successfully.');
          break;
        }
        case 'openDocs': {
          vscode.env.openExternal(
            vscode.Uri.parse('https://pigweed.dev/pw_ide/guide/vscode/'),
          );
          break;
        }
        case 'fileBug': {
          vscode.env.openExternal(
            vscode.Uri.parse('https://issues.pigweed.dev/issues?q=status:open'),
          );
          break;
        }
        case 'selectTarget': {
          const targetName = data.data;
          const targets = await availableTargets();
          const target = targets.find((t) => t.name === targetName);

          if (target) {
            await setTargetWithClangd(
              target,
              this._activeFilesCache.writeToSettings,
            );
          }

          await this.sendCipdReport();
          break;
        }
      }
    });
  }

  private async sendCipdReport() {
    let report = await getCipdReport();
    const pathForBazelBuildInterceptor = getBazelInterceptorPath();
    if (!pathForBazelBuildInterceptor) return;
    const bazelInterceptorExists = existsSync(pathForBazelBuildInterceptor);

    const targets = await availableTargets();
    const lastBuildPlatformCount = targets.length;

    let activeFileCount = 0;
    if (report.targetSelected) {
      const selectedTarget = targets.find(
        (t) => t.name === report.targetSelected,
      );
      if (selectedTarget) {
        const files = await parseForSourceFiles(selectedTarget);
        activeFileCount = files.size;
      }
    }

    report = {
      ...report,
      isBazelInterceptorEnabled: bazelInterceptorExists,
      lastBuildPlatformCount,
      activeFileCount,
      availableTargets: targets.map((t) => ({
        name: t.name,
        displayName: t.displayName,
      })),
    };
    logging.info('getCipdReport reported: ' + JSON.stringify(report));
    this._view?.webview.postMessage({
      type: 'cipdReport',
      data: report,
    });
  }

  private _getHtmlForWebview(webview: vscode.Webview) {
    // Get the local path to main script run in the webview, then convert it to a uri we can use in the webview.
    const scriptUri = webview.asWebviewUri(
      vscode.Uri.joinPath(this._extensionUri, 'dist', 'webview.js'),
    );

    // Use a nonce to only allow a specific script to be run.
    const nonce = getNonce();

    return `<!DOCTYPE html>
			<html lang="en">
			<head>
				<meta charset="UTF-8">
				<meta name="viewport" content="width=device-width, initial-scale=1.0">
				<title>Pigweed</title>
			</head>
			<body>
				<app-root>
                </app-root>

				<script nonce="${nonce}" src="${scriptUri}"></script>
			</body>
			</html>`;
  }
}

function getNonce() {
  let text = '';
  const possible =
    'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
  for (let i = 0; i < 32; i++) {
    text += possible.charAt(Math.floor(Math.random() * possible.length));
  }
  return text;
}
