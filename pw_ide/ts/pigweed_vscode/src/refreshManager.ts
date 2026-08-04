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

/**
 * A device for managing "refreshes" of IDE support data.
 *
 * Certain IDE support data needs to be refreshed periodically or in response
 * user actions (for example, generating compile commands, building codegen
 * files, etc.). The `RefreshManager` provides a mechanism to do those refreshes
 * in a structured way and provide hooks that UI elements can use to surface
 * status to the user.
 *
 * The `RefreshManager` itself doesn't actually do anything except watch out for
 * a refresh signal (set by calling `refresh()`), then run through each of its
 * defined states, calling registered callbacks at each state. So it's up to the
 * caller to register callbacks that do the real work.
 *
 * As an example, if you have a function `refreshCompileCommands`, you can
 * register it to run during the `refreshing` state:
 *
 * `refreshManager.on(refreshCompileCommands, 'refreshing');`
 *
 * Now, when `refreshManager.refresh()` is called, the function will be called
 * when the process enters the `refreshing` stage. You can surface the status
 * in a UI element by registering callbacks to change a status line or icon
 * in stages like `idle`, `willRefresh`, and `fault`.
 *
 * The abort signal can be set at any time via `abort()` to cancel an
 * in-progress refresh process.
 */

import type { Logger } from './loggingTypes';

/** Refresh statuses that broadly represent a refresh in progress. */
export type RefreshStatusInProgress =
  'willRefresh' | 'refreshing' | 'didRefresh';

/** The status of the refresh process. */
export type RefreshStatus =
  'idle' | RefreshStatusInProgress | 'abort' | 'fault';

/** Refresh callback functions return this type. */
export type RefreshCallbackResult = { error: string | null };

/** A little helper for returning success from a refresh callback. */
export const OK: RefreshCallbackResult = { error: null };

/** The function signature for refresh callback functions. */
export type RefreshCallback = () =>
  RefreshCallbackResult | Promise<RefreshCallbackResult>;

/**
 * This type definition defines the refresh manager state machine.
 *
 * Essentially, given a current state, it becomes the intersection of valid
 * next states.
 */
type NextState<State extends RefreshStatus> =
  // prettier-ignore
  State extends 'idle' ? 'willRefresh' | 'fault' | 'abort' :
  State extends 'willRefresh' ? 'refreshing' | 'fault' | 'abort' :
  State extends 'refreshing' ? 'didRefresh' | 'fault' | 'abort' :
  State extends 'didRefresh' ? 'idle' | 'fault' | 'abort' :
  State extends 'abort' ? 'idle' | 'fault' | 'abort' :
  State extends 'fault' ? 'idle' | 'fault' | 'abort' :
  never;

/** Options for constructing a refresh manager. */
interface RefreshManagerOptions {
  /** A concrete logger instance to log to. */
  logger?: Logger;

  /** The timeout period (in ms) between handling the reset signal. */
  refreshSignalHandlerTimeout: number;

  /** Whether to enable the periodic refresh signal handler. */
  useRefreshSignalHandler: boolean;
}

const defaultRefreshManagerOptions: RefreshManagerOptions = {
  refreshSignalHandlerTimeout: 1000,
  useRefreshSignalHandler: true,
};

/**
 * Encapsulate the status of compile commands refreshing, store callbacks to
 * be called at each status transition, and provide triggers for status change.
 */
export class RefreshManager<State extends RefreshStatus> {
  private _logger: Logger = {
    info: (msg: string) => console.log(msg),
    warn: (msg: string) => console.warn(msg),
    error: (msg: string) => console.error(msg),
  };

  /** The current refresh status. */
  private _state: RefreshStatus;

  /**
   * Whether it's possible to start a new refresh process. This is essentially
   * a lock that ensures that we only have a single pending refresh active at
   * any given time.
   */
  private canStart = true;

  /** State of the refresh signal. Set via the `refresh()` method. */
  private refreshSignal = false;

  /** State of the abort signal. Set via the `abort()` method. */
  private abortSignal = false;

  /** Handle to the timer used for the refresh signal handler. */
  private refreshSignalHandlerInterval: NodeJS.Timeout | undefined;

  /** Stored callbacks associated with their state or state change. */
  private callbacks: Record<string, RefreshCallback[]> = {};

  /** Stored transient callbacks associated with their state or state change. */
  private transientCallbacks: Record<string, RefreshCallback[]> = {};

  constructor(state: RefreshStatus, options: RefreshManagerOptions) {
    this._state = state;

    if (options.logger) {
      this._logger = options.logger;
    }

    if (options.useRefreshSignalHandler) {
      this.refreshSignalHandlerInterval = setInterval(
        () => this.handleRefreshSignal(),
        options.refreshSignalHandlerTimeout,
      );
    }
  }

  /**
   * Create a `RefreshHandler`. Use this instead of the constructor.
   *
   * The advantage of using this over the constructor is that you get a
   * well-typed version of the refresh manager from this method, and not from
   * the constructor.
   *
   * Yes, the method overload list is kind of overwhelming. All it's trying to
   * do is make the most common case trivial (creating a handler in the idle
   * state), the second most common case easy (creating a handler in another
   * state), and other cases possible (custom options and/or other states).
   */
  static create(): RefreshManager<'idle'>;
  static create(
    options: Partial<RefreshManagerOptions>,
  ): RefreshManager<'idle'>;
  static create<SpecificRefreshStatus extends RefreshStatus>(
    state: SpecificRefreshStatus,
  ): RefreshManager<SpecificRefreshStatus>;
  static create<SpecificRefreshStatus extends RefreshStatus>(
    state: SpecificRefreshStatus,
    options: Partial<RefreshManagerOptions>,
  ): RefreshManager<SpecificRefreshStatus>;
  static create<SpecificRefreshStatus extends RefreshStatus>(
    stateOrOptions?: SpecificRefreshStatus | Partial<RefreshManagerOptions>,
    options?: Partial<RefreshManagerOptions>,
  ): RefreshManager<SpecificRefreshStatus> {
    if (!stateOrOptions) {
      return new RefreshManager<'idle'>('idle', defaultRefreshManagerOptions);
    }

    if (typeof stateOrOptions === 'string') {
      const _state = stateOrOptions;
      const _options = options ?? {};
      return new RefreshManager<SpecificRefreshStatus>(_state, {
        ...defaultRefreshManagerOptions,
        ..._options,
      });
    } else {
      const _options = stateOrOptions;
      return new RefreshManager<'idle'>('idle', {
        ...defaultRefreshManagerOptions,
        ..._options,
      });
    }
  }

  /** The current refresh status. */
  get state(): RefreshStatus {
    return this._state;
  }

  /** Refresh manager instances must be disposed. */
  dispose(): void {
    if (this.refreshSignalHandlerInterval) {
      clearInterval(this.refreshSignalHandlerInterval);
    }
  }

  /**
   * Transition to a valid next state.
   *
   * The `NextState<State>` type ensures that this method will only accept next
   * states that are valid for the current state. The `SpecificNextState` type
   * narrows the return type to the one state that was actually provided. For
   * example, if the current `State` is `'didRefresh'`, `NextState<State>` is
   * `'idle' | 'fault'`. But only one of those two can be provided, and we
   * don't want the return type to be `RefreshManager<'idle' | 'fault'>`. So
   * `SpecificNextState` ensures that the return type is narrowed down.
   *
   * Explicitly adding `'fault'` and `'abort'` to the variants for
   * `SpecificNextState` is just to reassure and convince the compiler that
   * those states can be transitioned to from any other state. This is clear
   * from the definition of `NextState`, but without it, the compiler concludes
   * that it can't conclude anything about what states can be transitioned to
   * from an unknown state.
   *
   * The fact that this method returns itself is just a type system convenience
   * that lets us do state machine accounting. There's still always just one
   * refresh manager behind the curtain.
   *
   * Something to keep in mind is that the type of the return represents the
   * "happy path". If the path was actually unhappy, the return will be in the
   * fault state, but we don't know about that at the type system level. But
   * you don't need to worry about that; just handle things as if they worked
   * out, and if they didn't, the next states will just execute an early return.
   *
   * @param nextState A valid next state to transition to
   * @return This object in its new state, or in the fault state
   */
  async move<SpecificNextState extends NextState<State> | 'fault' | 'abort'>(
    nextState: SpecificNextState,
  ): Promise<RefreshManager<SpecificNextState>> {
    // If a previous transition moved us into the fault state, and we're not
    // intentionally moving out of the fault state to idle or abort, this move
    // is a no-op. So you can transition between the states per the state
    // machine definition, but with a sort of early return if the terminal fault
    // state happens in between, and you don't explicitly have to handle the
    // fault state at each step.
    if (this._state === 'fault' && !['idle', 'abort'].includes(nextState)) {
      return this;
    }

    // Abort is similar to fault in the sense that it short circuits the
    // following stages. The difference is that abort is considered a voluntary
    // action, and as such you can register callbacks to be called when moving
    // into the abort state.
    if (this.abortSignal) {
      this.abortSignal = false;
      const aborted = await this.move('abort');
      this.transientCallbacks = {};
      return aborted;
    }

    const previous = this._state;
    this._state = nextState;

    if (nextState !== 'fault') {
      try {
        // Run the callbacks associated with this state transition.
        await this.runCallbacks(previous);
      } catch (err: unknown) {
        // An error occurred while running the callbacks associated with this
        // state change.

        // Report errors to the output window.
        // Errors can come from well-behaved refresh callbacks that return
        // an object like `{ error: '...' }`, but since those errors are
        // hoisted to here by `throw`ing them, we will also catch unexpected or
        // not-well-behaved errors as exceptions (`{ message: '...' }`-style).
        if (typeof err === 'string') {
          this._logger.error(err);
        } else {
          const { message } = err as { message: string | undefined };

          if (message) {
            this._logger.error(message);
          } else {
            this._logger.error('Unknown error occurred');
          }
        }

        // Move into the fault state, running the callbacks associated with
        // that state transition.
        const previous = this._state;
        this._state = 'fault';
        await this.runCallbacks(previous);

        // Clear all transient callbacks (whether they were called or not).
        this.transientCallbacks = {};
      }
    }

    return this;
  }

  /**
   * Register a callback to be called for the specified state transition.
   *
   * These callbacks are registered for the lifetime of the refresh manager.
   * You can specific just the next state, in which case the callback will be
   * called when entering that state regardless of what the previous state was.
   * Or you can also specify the previous state to register a callback that will
   * only be called in that specific state transition.
   *
   * @param cb A callback function
   * @param next The callback will be called when transitioned into this state
   * @param current The callback will be called when transitioned from this state
   */
  on<
    EventCurrentState extends RefreshStatus,
    EventNextState extends NextState<EventCurrentState>,
  >(cb: RefreshCallback, next: EventNextState, current?: EventCurrentState) {
    const key = current ? `${next}:${current}` : next;
    this.callbacks[key] = [...(this.callbacks[key] ?? []), cb];
  }

  /**
   * Register a transient callback for the specified state transition.
   *
   * These are the same as regular callbacks, with the exception that they will
   * be called at most one time, during the next refresh process that happens
   * after they are registered. All registered transient callbacks, including
   * both those that have been called and those that have not been called, will
   * be cleared when the refresh process reaches a terminal state (idle or
   * fault).
   *
   * @param cb A callback function
   * @param next The callback will be called when transitioned into this state
   * @param current The callback will be called when transitioned from this state
   */
  onOnce<
    EventCurrentState extends RefreshStatus,
    EventNextState extends NextState<EventCurrentState>,
  >(cb: RefreshCallback, next: EventNextState, current?: EventCurrentState) {
    const key = current ? `${next}:${current}` : next;
    this.transientCallbacks[key] = [
      ...(this.transientCallbacks[key] ?? []),
      cb,
    ];
  }

  /**
   * Run the callbacks registered for the current state transition.
   *
   * The next state is pulled from `this.state`, so the implication is that
   * the state transition per se (reassigning variables) has already occurred.
   *
   * @param previous The state we just transitioned from
   */
  private async runCallbacks<EventCurrentState extends RefreshStatus>(
    previous: EventCurrentState,
  ) {
    // Collect the callbacks that run on this state transition.
    // Those can include callbacks that run whenever we enter the next state,
    // and callbacks that only run on the specific transition from the current
    // to the next state.
    const {
      [this._state]: transientForState,
      [`${this._state}:${previous}`]: transientForTransition,
      ...remainingTransient
    } = this.transientCallbacks;

    this.transientCallbacks = remainingTransient;

    const callbacks = [
      ...(this.callbacks[`${this._state}:${previous}`] ?? []),
      ...(this.callbacks[this._state] ?? []),
      ...(transientForTransition ?? []),
      ...(transientForState ?? []),
    ];

    // Run each callback, throw an error if any of them return an error.
    // This is sequential, so the first error we get will terminate this
    // procedure, and the remaining callbacks won't be called.
    for (const cb of callbacks) {
      // If the abort signal is set during callback execution, return early.
      // The rest of the abort signal handling will be handled by this method's
      // caller.
      if (this.abortSignal) {
        return;
      }

      const { error } = await Promise.resolve(cb());

      if (error) {
        throw new Error(error);
      }
    }
  }

  /**
   * Start a refresh process.
   *
   * This executes the procedure of moving from the idle state through each
   * subsequent state, and those moves then trigger the callbacks associated
   * with those state transitions.
   *
   * If this is called when we're not in the idle or fault states, we assume
   * that a refresh is currently in progress. Then we wait for a while for that
   * process to complete (return to idle or fault state) before running the
   * next process.
   *
   * We don't want to be able to queue up a limitless number of pending refresh
   * processes; we just want to represent the concept that there can be a
   * refresh happening now, and a request for a refresh to happen again after
   * the first one is done. Any additional requests for refresh are redundant.
   * So `this.canStart` acts like a lock to enforce that behavior.
   *
   * The request to start a new refresh process is signaled by setting
   * `this.refreshSignal` (by calling `refresh()`). So you generally don't
   * want to call this method directly.
   */
  async start() {
    if (!this.canStart) {
      throw new Error('Refresh process already starting');
    }

    this.canStart = false;

    // If we're in the fault state, we can start the refresh, but we need to
    // move into idle first. This is mostly just type system accounting, but
    // callbacks could also be registered in the fault -> idle transition, and
    // they'll be run now.
    if (this._state === 'fault') {
      await (this as RefreshManager<'fault'>).move('idle');
    }

    // We can only start from the idle state, so if we're not there, we need to
    // wait until we get there.
    await this.waitFor('idle');

    // We can cancel this pending refresh start by sending an abort signal.
    if (this.abortSignal) {
      this.abortSignal = false;
      this.canStart = true;
      return;
    }

    // If we don't get to idle within the timeout period, give up.
    if (this._state !== 'idle') {
      this.canStart = true;
      return;
    }

    // Now that we're starting a new refresh, clear the refresh signal.
    this.refreshSignal = false;
    this.canStart = true;

    // This moves through each stage of the "happy path", running registered
    // callbacks at each transition.
    //
    // - If a fault occurs, the remaining transitions will be no-op
    //   passthroughs, and we'll end up in the terminal fault state.
    //
    // - If an abort signal is sent, the remaining transitions will also be
    //   no-op passthroughs as we transition to the abort state. At the end
    //   of the abort state, we normally return to the idle state (unless a
    //   fault occurs during a callback called in the abort state).
    const idle = this as RefreshManager<'idle'>;
    const willRefresh = await idle.move('willRefresh');
    const refreshing = await willRefresh.move('refreshing');
    const didRefresh = await refreshing.move('didRefresh');

    // Don't eagerly reset to the idle state if we ended up in a fault state.
    if (didRefresh.state !== 'fault') {
      await didRefresh.move('idle');
    }

    this.transientCallbacks = {};
  }

  /**
   * The refresh signal handler.
   *
   * This starts a pending refresh process if the refresh signal is set and
   * there isn't already a pending process.
   */
  async handleRefreshSignal() {
    if (!this.refreshSignal || !this.canStart) return;
    await this.start();
  }

  /** Set the refresh signal to request a refresh. */
  refresh() {
    this.refreshSignal = true;
  }

  /** Set the abort signal to abort all current processing. */
  abort() {
    this.abortSignal = true;
  }

  async waitFor(state: RefreshStatus, delay = 500, retries = 10) {
    while (this._state !== state && retries > 0 && !this.abortSignal) {
      await new Promise((resolve) => setTimeout(resolve, delay));
      retries--;
    }
  }
}
