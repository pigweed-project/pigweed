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
 * A lightweight, in-process stand-in for the `vscode` module.
 *
 * This lets unit tests run under plain Node + Mocha (no VS Code / Electron, no
 * display server). Load it via `register-vscode-mock.js`, which redirects
 * `require('vscode')` here.
 *
 * Fidelity goals:
 *  - Configuration is stateful and honors the defaults declared in this
 *    extension's package.json, like real VS Code does.
 *  - `workspace.fs` and `workspace.findFiles` hit the real file system.
 *  - The workspace root is a fresh temp directory by default, so tests never
 *    write into a developer's checkout. Override with PW_IDE_TEST_WORKSPACE.
 *
 * Anything UI-related (terminals, status bar, decorations, webviews) is a
 * no-op that returns a disposable object of the right shape.
 */

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { glob } from 'glob';

/* eslint-disable @typescript-eslint/no-explicit-any */

// ---------------------------------------------------------------------------
// Core types
// ---------------------------------------------------------------------------

/** Shared no-op used for UI methods and default callbacks. */
const noop = (): void => {
  /* intentionally empty */
};

export class Disposable {
  constructor(private readonly callOnDispose: () => unknown = noop) {}

  static from(...disposables: { dispose(): unknown }[]): Disposable {
    return new Disposable(() => disposables.forEach((d) => d.dispose()));
  }

  dispose(): void {
    this.callOnDispose();
  }
}

export type Event<T> = (
  listener: (e: T) => unknown,
  thisArgs?: unknown,
  disposables?: Disposable[],
) => Disposable;

export class EventEmitter<T> {
  private listeners = new Set<(e: T) => unknown>();

  readonly event: Event<T> = (listener, thisArgs, disposables) => {
    const bound = thisArgs ? listener.bind(thisArgs) : listener;
    this.listeners.add(bound);
    const d = new Disposable(() => this.listeners.delete(bound));
    disposables?.push(d);
    return d;
  };

  fire(data: T): void {
    for (const l of [...this.listeners]) l(data);
  }

  dispose(): void {
    this.listeners.clear();
  }
}

/** Returns an `Event` that never fires. */
const noopEvent: Event<any> = () => new Disposable();

export class Uri {
  private constructor(
    readonly scheme: string,
    readonly authority: string,
    readonly path: string,
    readonly query = '',
    readonly fragment = '',
  ) {}

  get fsPath(): string {
    return this.scheme === 'file' ? path.normalize(this.path) : this.path;
  }

  static file(fsPath: string): Uri {
    return new Uri('file', '', fsPath.replace(/\\/g, '/'));
  }

  static parse(value: string): Uri {
    const m =
      /^([a-zA-Z][\w+.-]*):(?:\/\/([^/?#]*))?([^?#]*)(?:\?([^#]*))?(?:#(.*))?$/.exec(
        value,
      );
    if (!m) return Uri.file(value);
    return new Uri(m[1], m[2] ?? '', m[3] ?? '', m[4] ?? '', m[5] ?? '');
  }

  static joinPath(base: Uri, ...segments: string[]): Uri {
    return base.with({ path: path.posix.join(base.path, ...segments) });
  }

  with(change: {
    scheme?: string;
    authority?: string;
    path?: string;
    query?: string;
    fragment?: string;
  }): Uri {
    return new Uri(
      change.scheme ?? this.scheme,
      change.authority ?? this.authority,
      change.path ?? this.path,
      change.query ?? this.query,
      change.fragment ?? this.fragment,
    );
  }

  toString(): string {
    const q = this.query ? `?${this.query}` : '';
    const f = this.fragment ? `#${this.fragment}` : '';
    return `${this.scheme}://${this.authority}${this.path}${q}${f}`;
  }

  toJSON(): unknown {
    return { scheme: this.scheme, path: this.path, fsPath: this.fsPath };
  }
}

export class Position {
  constructor(
    readonly line: number,
    readonly character: number,
  ) {}
}

export class Range {
  readonly start: Position;
  readonly end: Position;

  constructor(start: Position, end: Position);
  constructor(
    startLine: number,
    startChar: number,
    endLine: number,
    endChar: number,
  );
  constructor(
    a: Position | number,
    b: Position | number,
    c?: number,
    d?: number,
  ) {
    if (typeof a === 'number') {
      this.start = new Position(a, b as number);
      this.end = new Position(c ?? a, d ?? (b as number));
    } else {
      this.start = a;
      this.end = b as Position;
    }
  }
}

export class ThemeColor {
  constructor(readonly id: string) {}
}

export class ThemeIcon {
  static readonly File = new ThemeIcon('file');
  static readonly Folder = new ThemeIcon('folder');
  constructor(
    readonly id: string,
    readonly color?: ThemeColor,
  ) {}
}

export class FileDecoration {
  constructor(
    public badge?: string,
    public tooltip?: string,
    public color?: ThemeColor,
  ) {}
  propagate?: boolean;
}

export class RelativePattern {
  readonly baseUri: Uri;
  readonly base: string;

  constructor(
    base: string | Uri | { uri: Uri },
    readonly pattern: string,
  ) {
    if (typeof base === 'string') this.baseUri = Uri.file(base);
    else if (base instanceof Uri) this.baseUri = base;
    else this.baseUri = base.uri;
    this.base = this.baseUri.fsPath;
  }
}

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

export enum ConfigurationTarget {
  Global = 1,
  Workspace = 2,
  WorkspaceFolder = 3,
}

export enum StatusBarAlignment {
  Left = 1,
  Right = 2,
}

export enum ProgressLocation {
  SourceControl = 1,
  Window = 10,
  Notification = 15,
}

export enum QuickPickItemKind {
  Separator = -1,
  Default = 0,
}

export enum FileType {
  Unknown = 0,
  File = 1,
  Directory = 2,
  SymbolicLink = 64,
}

export enum ViewColumn {
  Active = -1,
  Beside = -2,
  One = 1,
}

export enum TreeItemCollapsibleState {
  None = 0,
  Collapsed = 1,
  Expanded = 2,
}

export enum ExtensionMode {
  Production = 1,
  Development = 2,
  Test = 3,
}

export enum DecorationRangeBehavior {
  OpenOpen = 0,
  ClosedClosed = 1,
  OpenClosed = 2,
  ClosedOpen = 3,
}

// ---------------------------------------------------------------------------
// Workspace root
// ---------------------------------------------------------------------------

function initWorkspaceRoot(): string {
  const fromEnv = process.env.PW_IDE_TEST_WORKSPACE;
  if (fromEnv) return path.resolve(fromEnv);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'pw_ide_mock_ws_'));
  process.on('exit', () => {
    try {
      fs.rmSync(dir, { recursive: true, force: true });
    } catch {
      /* best effort */
    }
  });
  return dir;
}

const workspaceRoot = initWorkspaceRoot();

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** Defaults declared in package.json `contributes.configuration`. */
function loadDeclaredDefaults(): Map<string, unknown> {
  const defaults = new Map<string, unknown>();
  // Compiled location is out/src/test/, so package.json is three levels up.
  const candidates = [
    path.resolve(__dirname, '..', '..', '..', 'package.json'),
    path.resolve(process.cwd(), 'package.json'),
  ];
  for (const p of candidates) {
    try {
      const pkg = JSON.parse(fs.readFileSync(p, 'utf-8'));
      const props = pkg?.contributes?.configuration?.properties ?? {};
      for (const [key, schema] of Object.entries<any>(props)) {
        if (schema && 'default' in schema) defaults.set(key, schema.default);
      }
      return defaults;
    } catch {
      /* try next */
    }
  }
  return defaults;
}

const declaredDefaults = loadDeclaredDefaults();
const configStore = new Map<string, unknown>();
const onDidChangeConfigurationEmitter = new EventEmitter<{
  affectsConfiguration(section: string): boolean;
}>();

function fullKey(category: string | undefined, section: string): string {
  return category ? `${category}.${section}` : section;
}

function getConfiguration(category?: string) {
  return {
    get<T>(section: string, defaultValue?: T): T | undefined {
      const key = fullKey(category, section);
      if (configStore.has(key)) return configStore.get(key) as T;
      if (declaredDefaults.has(key)) return declaredDefaults.get(key) as T;
      return defaultValue;
    },
    has(section: string): boolean {
      const key = fullKey(category, section);
      return configStore.has(key) || declaredDefaults.has(key);
    },
    inspect<T>(section: string) {
      const key = fullKey(category, section);
      return {
        key,
        defaultValue: declaredDefaults.get(key) as T | undefined,
        globalValue: undefined as T | undefined,
        workspaceValue: configStore.get(key) as T | undefined,
        workspaceFolderValue: undefined as T | undefined,
      };
    },
    update(section: string, value: unknown): Promise<void> {
      const key = fullKey(category, section);
      if (value === undefined) configStore.delete(key);
      else configStore.set(key, value);
      onDidChangeConfigurationEmitter.fire({
        affectsConfiguration: (s: string) =>
          key === s || key.startsWith(`${s}.`),
      });
      return Promise.resolve();
    },
  };
}

// ---------------------------------------------------------------------------
// workspace
// ---------------------------------------------------------------------------

const workspaceFs = {
  async readFile(uri: Uri): Promise<Uint8Array> {
    return fs.promises.readFile(uri.fsPath);
  },
  async writeFile(uri: Uri, content: Uint8Array): Promise<void> {
    await fs.promises.mkdir(path.dirname(uri.fsPath), { recursive: true });
    await fs.promises.writeFile(uri.fsPath, content);
  },
  async stat(uri: Uri) {
    const s = await fs.promises.lstat(uri.fsPath);
    let type = s.isDirectory() ? FileType.Directory : FileType.File;
    if (s.isSymbolicLink()) type |= FileType.SymbolicLink;
    return { type, ctime: s.ctimeMs, mtime: s.mtimeMs, size: s.size };
  },
  async readDirectory(uri: Uri): Promise<[string, FileType][]> {
    const entries = await fs.promises.readdir(uri.fsPath, {
      withFileTypes: true,
    });
    return entries.map((e) => [
      e.name,
      e.isDirectory() ? FileType.Directory : FileType.File,
    ]);
  },
  async createDirectory(uri: Uri): Promise<void> {
    await fs.promises.mkdir(uri.fsPath, { recursive: true });
  },
  async delete(uri: Uri, options?: { recursive?: boolean }): Promise<void> {
    await fs.promises.rm(uri.fsPath, {
      recursive: options?.recursive ?? false,
      force: true,
    });
  },
  async rename(source: Uri, target: Uri): Promise<void> {
    await fs.promises.rename(source.fsPath, target.fsPath);
  },
  async copy(source: Uri, target: Uri): Promise<void> {
    await fs.promises.cp(source.fsPath, target.fsPath, { recursive: true });
  },
};

async function findFiles(
  include: string | RelativePattern,
  exclude?: string | RelativePattern | null,
  maxResults?: number,
): Promise<Uri[]> {
  const cwd = typeof include === 'string' ? workspaceRoot : include.base;
  const pattern = typeof include === 'string' ? include : include.pattern;
  const ignore =
    exclude == null
      ? undefined
      : typeof exclude === 'string'
        ? exclude
        : exclude.pattern;
  const matches = await glob(pattern, {
    cwd,
    ignore,
    absolute: true,
    nodir: true,
  });
  const uris = matches.sort().map((m) => Uri.file(m));
  return maxResults === undefined ? uris : uris.slice(0, maxResults);
}

function createFileSystemWatcher() {
  return {
    ignoreCreateEvents: false,
    ignoreChangeEvents: false,
    ignoreDeleteEvents: false,
    onDidCreate: noopEvent,
    onDidChange: noopEvent,
    onDidDelete: noopEvent,
    dispose: noop,
  };
}

export const workspace = {
  get workspaceFolders() {
    return [
      {
        uri: Uri.file(workspaceRoot),
        name: path.basename(workspaceRoot),
        index: 0,
      },
    ];
  },
  get rootPath() {
    return workspaceRoot;
  },
  workspaceFile: undefined as Uri | undefined,
  name: path.basename(workspaceRoot),
  getConfiguration,
  onDidChangeConfiguration: onDidChangeConfigurationEmitter.event,
  fs: workspaceFs,
  findFiles,
  createFileSystemWatcher,
  getWorkspaceFolder: () => workspace.workspaceFolders[0],
  asRelativePath: (p: string | Uri) =>
    path.relative(workspaceRoot, typeof p === 'string' ? p : p.fsPath),
  openTextDocument: () => Promise.reject(new Error('Not supported in mock')),
  onDidOpenTextDocument: noopEvent,
  onDidCloseTextDocument: noopEvent,
  onDidSaveTextDocument: noopEvent,
  onDidChangeTextDocument: noopEvent,
  onDidChangeWorkspaceFolders: noopEvent,
};

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------

function createOutputChannel(name: string) {
  return {
    name,
    logLevel: 0,
    onDidChangeLogLevel: noopEvent,
    append: noop,
    appendLine: noop,
    replace: noop,
    clear: noop,
    show: noop,
    hide: noop,
    dispose: noop,
    trace: noop,
    debug: noop,
    info: noop,
    warn: noop,
    error: noop,
  };
}

function createStatusBarItem() {
  return {
    text: '',
    tooltip: undefined as string | undefined,
    command: undefined as unknown,
    color: undefined as unknown,
    backgroundColor: undefined as unknown,
    alignment: StatusBarAlignment.Left,
    priority: undefined as number | undefined,
    show: noop,
    hide: noop,
    dispose: noop,
  };
}

function createTerminal(options?: { name?: string } | string) {
  const name = typeof options === 'string' ? options : (options?.name ?? '');
  return {
    name,
    processId: Promise.resolve(undefined),
    exitStatus: undefined,
    sendText: noop,
    show: noop,
    hide: noop,
    dispose: noop,
  };
}

async function withProgress<R>(
  _options: unknown,
  task: (
    progress: { report(value: unknown): void },
    token: {
      isCancellationRequested: boolean;
      onCancellationRequested: Event<void>;
    },
  ) => Thenable<R>,
): Promise<R> {
  return task(
    { report: noop },
    { isCancellationRequested: false, onCancellationRequested: noopEvent },
  );
}

export const window = {
  activeTextEditor: undefined as unknown,
  visibleTextEditors: [] as unknown[],
  activeTerminal: undefined as unknown,
  terminals: [] as unknown[],
  showInformationMessage: () => Promise.resolve(undefined),
  showWarningMessage: () => Promise.resolve(undefined),
  showErrorMessage: () => Promise.resolve(undefined),
  showQuickPick: () => Promise.resolve(undefined),
  showInputBox: () => Promise.resolve(undefined),
  showOpenDialog: () => Promise.resolve(undefined),
  showTextDocument: () => Promise.resolve(undefined),
  createOutputChannel,
  createStatusBarItem,
  createTerminal,
  createTextEditorDecorationType: () => ({
    key: 'mock-decoration',
    dispose: noop,
  }),
  registerFileDecorationProvider: () => new Disposable(),
  registerWebviewViewProvider: () => new Disposable(),
  registerTreeDataProvider: () => new Disposable(),
  withProgress,
  onDidChangeActiveTextEditor: noopEvent,
  onDidChangeVisibleTextEditors: noopEvent,
  onDidChangeTextEditorSelection: noopEvent,
  onDidOpenTerminal: noopEvent,
  onDidCloseTerminal: noopEvent,
  onDidChangeActiveTerminal: noopEvent,
};

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

const registeredCommands = new Map<string, (...args: any[]) => unknown>();

export const commands = {
  registerCommand(
    id: string,
    callback: (...args: any[]) => unknown,
  ): Disposable {
    registeredCommands.set(id, callback);
    return new Disposable(() => registeredCommands.delete(id));
  },
  async executeCommand<T = unknown>(
    id: string,
    ...args: unknown[]
  ): Promise<T> {
    const cb = registeredCommands.get(id);
    return (cb ? await cb(...args) : undefined) as T;
  },
  async getCommands(): Promise<string[]> {
    return [...registeredCommands.keys()];
  },
};

// ---------------------------------------------------------------------------
// env, extensions, languages
// ---------------------------------------------------------------------------

export const env = {
  appName: 'Visual Studio Code (mock)',
  appRoot: '',
  language: 'en',
  machineId: 'mock-machine-id',
  sessionId: 'mock-session-id',
  uriScheme: 'vscode',
  remoteName: undefined as string | undefined,
  clipboard: {
    readText: () => Promise.resolve(''),
    writeText: () => Promise.resolve(),
  },
  openExternal: () => Promise.resolve(true),
};

export const extensions = {
  all: [] as unknown[],
  getExtension: () => undefined,
  onDidChange: noopEvent,
};

export const languages = {
  createDiagnosticCollection: (name?: string) => ({
    name,
    set: noop,
    delete: noop,
    clear: noop,
    dispose: noop,
  }),
};

// ---------------------------------------------------------------------------
// Test helpers (not part of the real vscode API)
// ---------------------------------------------------------------------------

/** Clears configuration values and registered commands set during a test. */
export function __resetMockState(): void {
  configStore.clear();
  registeredCommands.clear();
}

/** The directory the mock reports as the open workspace folder. */
export const __mockWorkspaceRoot = workspaceRoot;
