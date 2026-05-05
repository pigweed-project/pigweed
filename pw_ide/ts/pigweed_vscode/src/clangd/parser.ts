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

import { createHash, randomBytes } from 'crypto';
import * as fs_p from 'fs/promises';
import * as path from 'path';

/**
 * Given a target inference glob, infer which path positions contain tokens that
 * should be used as part of the target name.
 */
export function inferTargetPositions(targetGlob: string): number[] {
  const tokens = targetGlob.split(path.sep);
  const positions: number[] = [];

  for (const [position, token] of tokens.entries()) {
    switch (token) {
      case '?':
        positions.push(position);
        break;
      case '*':
        break;
      default:
        throw new Error(`Invalid target inference token: ${token}`);
    }
  }

  return positions;
}

/**
 * Infer the name of the target from the output path, given a target inference
 * glob.
 */
export function inferTarget(
  targetGlob: string,
  outputPath: string,
  rootPath?: string,
): string {
  const targetPositions = inferTargetPositions(targetGlob);

  // We can't infer a target name if the target inference glob doesn't specify
  // any path positions to be included in the name.
  if (targetPositions.length === 0) {
    throw new Error(`Invalid target inference: ${targetGlob}`);
  }

  const pathParts = rootPath
    ? path.relative(outputPath, rootPath).split(path.sep)
    : outputPath.split(path.sep);

  return targetPositions
    .map((pos) => pathParts[pos])
    .join('_')
    .replace('.', '_');
}

/** Supported compile command wrapper executables. */
const WRAPPER_EXECUTABLES: (string | undefined)[] = ['ccache'];

/**
 * Compile commands can be "wrapped" with `ccache`, which can be provided a
 * series of arguments in the form `KEY=VALUE`. This takes a `ccache` invocation
 * string, the next token in a compile command, and returns a new `ccache`
 * invocation string. If the next token is a `ccache` arg, it's appended to the
 * new `ccache` invocation. If not, this will return the same `ccache`
 * invocation that was provided, which is a signal that all `ccache` args have
 * been collected.
 *
 * @param args A `ccache` invocation string
 * @param token The next token in the compile command
 * @returns A `ccache` invocation string (a new one if the next token was a
 * `ccache` arg, or the provided one if not)
 */
const accumulateCcacheArgs = (args: string, token: string | undefined) =>
  token?.match(/(.*)=(.*)/) ? `${args} ${token}` : args;

/**
 * The "command" part of a compile command (whether specified as a string in the
 * `command` key or as an array of strings in the `arguments` key) contains, at
 * minimum, an invocation of the compiler executable and arguments to the
 * compiler. This type breaks up the "command" into parts we're interested in.
 *
 * The `command` key contains just the invocation to the compiler executable.
 *
 * One of the compiler arguments (`-o`) indicates the output object; we parse
 * its path and store it here, even though it might also be specified in the
 * `output` key of the compile command.
 *
 * The `args` key type stores all of the arguments provided to the compiler,
 * including the output arg.
 *
 * The command could be wrapped with something like `ccache`, meaning that the
 * first part of the command isn't necessarily the compiler invocation. If
 * that's the case, the wrapper invocation is stored separately in `wrapper`.
 */
interface CommandParts {
  command: string;
  output: string;
  args: string[];
  wrapper?: string;
}

/**
 * Given the "command" part of the compiler command, parse it into the
 * interesting parts.
 *
 * @param parts The command in the form of a string array
 * @returns The command parts as `CommandParts`
 * @throws When the command doesn't contain the expected parts
 */
function parseCommandParts(parts: string[]): CommandParts {
  // The command may or may not be prepended with a call to a wrapper.
  let wrapper: string | undefined;

  // The current token. We will iterate until we find the command, and the
  // wrapper invocation (if there is one).
  let curr: string | undefined = parts.shift();

  // If this command is wrapped, then we need to accumulate all the command
  // parts that belong to the wrapper, then expect to find the actual command
  // after that. If this command is not wrapped, then we expect that the first
  // token is the actual command.
  if (WRAPPER_EXECUTABLES.includes(curr)) {
    // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
    let currWrapper = curr!;

    do {
      // Get the next token.
      curr = parts.shift();

      // If the next token is a set of `ccache` args, append them to the
      // wrapper string. Otherwise this returns the original input.
      // Note: This is `ccache` specific and something more would need to
      // be done to support other wrappers.
      const newWrapper = accumulateCcacheArgs(currWrapper, curr);

      // If the wrapper string didn't change, then the current token is
      // not an arg to the wrapper, so we expect the current token to be
      // the actual command.
      if (newWrapper == currWrapper) {
        break;
        // If the wrapper string did change, we progress to the next token.
      } else {
        currWrapper = newWrapper;
      }
      // We iterate through the tokens until we find the actual command.
    } while (curr);
  }

  if (curr == undefined) {
    throw new Error('Compile command lacks a compiler invocation');
  }

  const outputPathIdx = parts.indexOf('-o');

  if (outputPathIdx < 0) {
    throw new Error('Compile command lacks an output argument');
  }

  const output = parts.at(outputPathIdx + 1);

  if (!output) {
    throw new Error('Compile command output flag has no argument');
  }

  return {
    command: curr,
    output,
    args: parts,
    wrapper,
  };
}

/** See: https://clang.llvm.org/docs/JSONCompilationDatabase.html */
type CompileCommandData = {
  directory: string;
  file: string;
  output?: string;
  arguments?: string[];
  command?: string;
};

const UNSUPPORTED_EXECUTABLES = ['_pw_invalid', 'python'];

/**
 * A representation of a `clangd` compilation database compile command.
 *
 * See: https://clang.llvm.org/docs/JSONCompilationDatabase.html
 */
export class CompileCommand {
  readonly data: CompileCommandData;
  private readonly commandParts?: CommandParts;
  readonly error?: Error;

  constructor(data: CompileCommandData) {
    this.data = data;

    // The spec requires one of these two to be present.
    if (this.data.arguments === undefined && this.data.command === undefined) {
      this.error = new Error(
        'Compile command must have a command or arguments',
      );
      return;
    }

    try {
      // The spec says that `arguments` should be preferred over `command`.
      // In practice, you shouldn't encounter cases where both are present.
      this.commandParts = this.data.arguments
        ? parseCommandParts([...this.data.arguments])
        : // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
          parseCommandParts(this.data.command!.split(/\s+/));
    } catch (e) {
      if (e instanceof Error) {
        this.error = e;
      } else {
        this.error = new Error(String(e));
      }
    }
  }

  get sourceFilePath() {
    return path.resolve(this.data.directory, this.data.file);
  }

  /** The directory path of the output file. */
  get outputPath() {
    if (!this.commandParts) {
      return '';
    }
    return path.dirname(this.commandParts.output);
  }

  process() {
    if (this.error || !this.commandParts) {
      return null;
    }
    const executable = path.resolve(this.commandParts.command);

    for (const unsupportedExecutable of UNSUPPORTED_EXECUTABLES) {
      if (executable.includes(unsupportedExecutable)) {
        // TODO(chadnorvell): Send this to the logger.
        // Requires access to a logger.
        return null;
      }
    }

    return this;
  }
}

/**
 * A representation of a `clangd` compilation database.
 *
 * See: https://clang.llvm.org/docs/JSONCompilationDatabase.html
 */
export class CompilationDatabase {
  fileHash: string = randomBytes(16).toString('hex');
  filePath: string | undefined;
  sourceFilePath: string | undefined;
  db: CompileCommand[] = [];

  /**
   * Load a compilation database from a JSON string. Mutates this instance.
   *
   * @param contents A JSON string containing a compilation database
   * @throws On any schema or invariant violations in the source data
   */
  loadFromString(contents: string): void {
    this.db = JSON.parse(contents).map(
      (c: CompileCommandData) => new CompileCommand(c),
    );
  }

  /**
   * Load a compilation database from a file. Mutates this instance.
   *
   * @param path A path to a compilation database file
   * @throws On any schema or invariant violations in the source data
   */
  async loadFromFile(path: string): Promise<void> {
    const contents = (await fs_p.readFile(path)).toString();
    this.loadFromString(contents);
    this.fileHash = createHash('md5').update(contents).digest('hex');
  }

  /**
   * Instantiate a compilation database from a JSON string.
   *
   * @param contents A JSON string containing a compilation database
   * @returns A `CompilationDatabase` with the contents of the string
   * @throws On any schema or invariant violations in the source data
   */
  static fromString(contents: string): CompilationDatabase {
    const compDb = new CompilationDatabase();
    compDb.loadFromString(contents);
    return compDb;
  }

  /**
   * Instantiate a compilation database from a file.
   *
   * @param path A path to a compilation database file
   * @param onFailure An optional callback to run when the file can't be loaded
   *   or parsed.
   * @returns A `CompilationDatabase` with the contents of the file
   * @throws On any schema or invariant violations in the source data, unless
   *   `onFailure` is provided instead.
   */
  static async fromFile(
    path: string,
    onFailure?: () => void,
  ): Promise<CompilationDatabase | null> {
    const compDb = new CompilationDatabase();

    try {
      await compDb.loadFromFile(path);
      return compDb;
    } catch (e) {
      if (onFailure) {
        onFailure();
        return null;
      } else {
        throw e;
      }
    }
  }

  /** Add a compile command to the database. */
  add(compileCommand: CompileCommand): void {
    this.db.push(compileCommand);
  }

  /**
   * Process this compilation database to produce one or more compilation
   * databases that are in the right format and can be put in the right place.
   *
   * The right format: A compilation database that contains compile commands for
   *   a single target with valid toolchain invocations.
   *
   * The right place: A compilation database (or a symlink to one) located in
   *   the directory structure specifying target name.
   *
   * Compilation databases can be sourced in three ways:
   *
   * 1. The build system generates them in the right format and puts them where
   *    Pigweed expects to find them.
   *
   * In this case, we don't need to do anything with the compilation databases.
   * We'll just find them and use them. Files generated by Bazel using the tools
   * provided by Pigweed will fall into this case. These do not need to be
   * processed.
   *
   * 2. The build system generates them in the right format and puts them
   *    somewhere in the build tree.
   *
   * In this case, we don't need to modify the compilation databases, but we do
   * need to symlink them to the right place in the compile commands directory
   * so Pigweed tooling can find them and associate them with their target. This
   * case applies to files generated by CMake.
   *
   * 3. The build system generates them in some format that requires
   *    post-processing and puts them somewhere in the build tree.
   *
   * In this case, we need to process the compilation databases to produce files
   * that are in the right format (e.g., that only include valid compile
   * commands for one particular target), and then we need to write the
   * processed files to the compile commands directory. This case applies to
   * files generated by GN.
   */
  process(targetInference: string): CompilationDatabaseMap | null {
    const cleanCompilationDatabases = new CompilationDatabaseMap();

    for (const compileCommand of this.db) {
      const processedCommand = compileCommand.process();

      if (processedCommand !== null) {
        const targetName = inferTarget(
          targetInference,
          processedCommand.outputPath,
        );

        const db = cleanCompilationDatabases.get(targetName);
        db.add(processedCommand);
        db.sourceFilePath = this.filePath;
        db.fileHash = this.fileHash;
      }
    }

    // Determine if the processed database is functionally identical to the
    // original. The criteria for "functionally identical" are:
    //
    // - Processing the original file yields compile commands for one target
    // - The same number of compile commands are in the input and output
    //
    // This indicates that the database didn't really need processing at all,
    // and we can probably link to it instead of to the processed result.
    if (
      cleanCompilationDatabases.size === 1 &&
      cleanCompilationDatabases.first()?.db.length === this.db.length
    ) {
      return null;
    }

    return cleanCompilationDatabases;
  }

  async write(filePath: string) {
    const fileData = JSON.stringify(
      this.db.map((c) => c.data),
      null,
      2,
    );
    await fs_p.mkdir(path.dirname(filePath), { recursive: true });
    await fs_p.writeFile(filePath, fileData);
  }
}

export class CompilationDatabaseMap extends Map<string, CompilationDatabase> {
  // It's like Python's `defaultdict` -- `get` creates if the key isn't present.
  get(key: string): CompilationDatabase {
    if (!this.has(key)) {
      this.set(key, new CompilationDatabase());
    }

    // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
    return super.get(key)!;
  }

  first(): CompilationDatabase | undefined {
    for (const [, entry] of this.entries()) {
      return entry;
    }

    return undefined;
  }

  map<T>(fn: (key: string, value: CompilationDatabase) => T) {
    const rtn: T[] = [];

    for (const key of this.keys()) {
      rtn.push(fn(key, this.get(key)));
    }

    return rtn;
  }

  async writeAll(
    workingDirPath: string,
    cdbFileDir: string,
    cdbFilename: string,
  ): Promise<void> {
    const promises: Promise<void>[] = [];

    for (const [targetName, compDb] of this.entries()) {
      const filePath = path.join(
        workingDirPath,
        cdbFileDir,
        targetName,
        cdbFilename,
      );
      promises.push(compDb.write(filePath));
    }

    await Promise.all(promises);
  }

  static merge(
    ...compDbMaps: CompilationDatabaseMap[]
  ): CompilationDatabaseMap {
    const merged = new CompilationDatabaseMap();

    for (const compDbMap of compDbMaps) {
      for (const [targetName, compDb] of compDbMap) {
        merged.set(targetName, compDb);
      }
    }

    return merged;
  }
}
