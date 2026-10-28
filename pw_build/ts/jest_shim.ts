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

import * as util from 'util';
import * as googleProtobuf from 'google-protobuf';

const gp: any = (googleProtobuf as any).default ?? googleProtobuf;
(globalThis as any).goog = gp;
(globalThis as any).jspb = gp.jspb ?? gp;

type Fn = () => void | Promise<void>;
class Suite {
  beforeAll: Fn[] = [];
  afterAll: Fn[] = [];
  beforeEach: Fn[] = [];
  afterEach: Fn[] = [];
  tests: Array<{ name: string; fn: Fn }> = [];
  suites: Suite[] = [];
  constructor(readonly name = '') {}
}

const rootSuite = new Suite();
let currentSuite = rootSuite;
let expectedAssertions: number | null = null;
let assertionCount = 0;

function norm(v: any, strict: boolean, seen = new WeakMap()): any {
  if (!v || typeof v !== 'object') return v;
  if (seen.has(v)) return seen.get(v);
  if (v instanceof Uint8Array) {
    return new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
  }
  if (typeof v.toObject === 'function') {
    const out: Record<string, any> = { __ctor: v.constructor };
    seen.set(v, out);
    return Object.assign(out, norm(v.toObject(), strict, seen));
  }
  if (v instanceof Error) return { __err: v.name, message: v.message };
  if (Array.isArray(v)) {
    const arr: any[] = [];
    seen.set(v, arr);
    for (const x of v) arr.push(norm(x, strict, seen));
    return arr;
  }
  if (v instanceof Map) {
    const map = new Map();
    seen.set(v, map);
    for (const [k, x] of v) map.set(k, norm(x, strict, seen));
    return map;
  }
  if (
    v instanceof Date ||
    v instanceof RegExp ||
    v instanceof Set ||
    ArrayBuffer.isView(v)
  ) {
    return v;
  }
  const out: Record<string, any> = strict ? { __ctor: v.constructor } : {};
  seen.set(v, out);
  for (const k of Object.keys(v)) {
    if (strict || v[k] !== undefined) out[k] = norm(v[k], strict, seen);
  }
  return out;
}

const eq = (a: any, b: any, strict = false): boolean =>
  util.isDeepStrictEqual(norm(a, strict), norm(b, strict));

function sub(a: any, b: any): boolean {
  if (eq(a, b)) return true;
  if (!a || !b || typeof a !== 'object' || typeof b !== 'object') return false;
  return Object.keys(b).every((k) => sub(a[k], b[k]));
}

function asyncMatchers(
  p: Promise<any>,
  expectRej: boolean,
  isNot = false,
): any {
  return new Proxy(
    {},
    {
      get(_t, prop: string) {
        if (prop === 'not') return asyncMatchers(p, expectRej, !isNot);
        return async (...args: any[]) => {
          let val: any;
          let rej = false;
          try {
            val = await p;
          } catch (err) {
            val = err;
            rej = true;
          }
          if (expectRej !== rej) {
            throw new Error(`Unexpected settlement: ${util.inspect(val)}`);
          }
          return createMatchers(val, isNot, expectRej)[prop](...args);
        };
      },
    },
  );
}

function matchesThrown(thrown: any, exp: any): boolean {
  if (exp === undefined) return true;
  const msg = String(thrown?.message ?? thrown);
  if (typeof exp === 'string') return msg.includes(exp);
  if (exp instanceof RegExp) return exp.test(msg);
  if (exp instanceof Error) {
    return thrown instanceof exp.constructor && msg.includes(exp.message);
  }
  if (typeof exp === 'function') {
    return exp === Error ||
      exp.prototype instanceof Error ||
      thrown instanceof exp
      ? thrown instanceof exp
      : Boolean(exp(thrown));
  }
  return Boolean(exp && typeof exp === 'object' && sub(thrown, exp));
}

function createMatchers(a: any, isNot = false, isRej = false): any {
  const ins = util.inspect;
  const check = (pass: boolean, label: string, ...exp: any[]) => {
    if (isNot ? pass : !pass) {
      const s = exp.length ? ` ${ins(exp[0])}` : '';
      throw new Error(`Expected ${ins(a)} ${isNot ? 'not ' : ''}${label}${s}`);
    }
  };
  const calls = (): any[][] => {
    if (!Array.isArray(a?.mock?.calls)) throw new Error('Not a mock fn');
    return a.mock.calls;
  };

  const m: Record<string, any> = {
    toBe: (e: any) => check(Object.is(a, e), 'toBe', e),
    toEqual: (e: any) => check(eq(a, e), 'toEqual', e),
    toStrictEqual: (e: any) => check(eq(a, e, true), 'toStrictEqual', e),
    toMatchObject: (e: any) => check(sub(a, e), 'toMatchObject', e),
    toBeUndefined: () => check(a === undefined, 'toBeUndefined'),
    toBeDefined: () => check(a !== undefined, 'toBeDefined'),
    toBeNull: () => check(a === null, 'toBeNull'),
    toBeTruthy: () => check(Boolean(a), 'toBeTruthy'),
    toBeFalsy: () => check(!a, 'toBeFalsy'),
    toBeNaN: () => check(Number.isNaN(a), 'toBeNaN'),
    toBeGreaterThan: (e: any) => check(a > e, '>', e),
    toBeLessThan: (e: any) => check(a < e, '<', e),
    toBeGreaterThanOrEqual: (e: any) => check(a >= e, '>=', e),
    toBeLessThanOrEqual: (e: any) => check(a <= e, '<=', e),
    toBeCloseTo: (e: number, p = 2) =>
      check(Math.abs(e - a) < Math.pow(10, -p) / 2, 'toBeCloseTo', e),
    toBeInstanceOf: (e: any) => check(a instanceof e, 'toBeInstanceOf', e),
    toContain: (e: any) => {
      const ok =
        typeof a === 'string'
          ? a.includes(e)
          : a instanceof Set
            ? a.has(e)
            : Array.from(a ?? []).includes(e);
      check(ok, 'toContain', e);
    },
    toContainEqual: (e: any) =>
      check(Array.isArray(a) && a.some((x) => eq(x, e)), 'toContainEqual', e),
    toHaveLength: (e: number) => check(a?.length === e, 'toHaveLength', e),
    toMatch: (e: string | RegExp) => {
      const s = String(a);
      check(typeof e === 'string' ? s.includes(e) : e.test(s), 'toMatch', e);
    },
    toThrow(exp?: any) {
      let threw = isRej;
      let thrown = a;
      if (!isRej) {
        if (typeof a !== 'function') throw new Error('Expected function');
        try {
          a();
        } catch (err) {
          threw = true;
          thrown = err;
        }
      }
      check(threw && matchesThrown(thrown, exp), 'toThrow', exp);
    },
    toHaveBeenCalled: () => check(calls().length > 0, 'toHaveBeenCalled'),
    toHaveBeenCalledTimes: (n: number) =>
      check(calls().length === n, 'toHaveBeenCalledTimes', n),
    toHaveBeenCalledWith: (...e: any[]) =>
      check(
        calls().some((c) => eq(c, e)),
        'calledWith',
        e,
      ),
    toHaveBeenNthCalledWith: (n: number, ...e: any[]) =>
      check(eq(calls()[n - 1], e), 'toHaveBeenNthCalledWith', e),
    toHaveBeenLastCalledWith: (...e: any[]) => {
      const c = calls();
      check(c.length > 0 && eq(c[c.length - 1], e), 'lastCalledWith', e);
    },
  };

  Object.assign(m, {
    toThrowError: m.toThrow,
    toBeCalled: m.toHaveBeenCalled,
    toBeCalledTimes: m.toHaveBeenCalledTimes,
    toBeCalledWith: m.toHaveBeenCalledWith,
    nthCalledWith: m.toHaveBeenNthCalledWith,
    lastCalledWith: m.toHaveBeenLastCalledWith,
  });
  Object.defineProperties(m, {
    not: { get: () => createMatchers(a, !isNot, isRej) },
    resolves: {
      get: () =>
        asyncMatchers(
          typeof a === 'function'
            ? new Promise((r) => r(a()))
            : Promise.resolve(a),
          false,
          isNot,
        ),
    },
    rejects: {
      get: () =>
        asyncMatchers(
          typeof a === 'function'
            ? new Promise((r) => r(a()))
            : Promise.resolve(a),
          true,
          isNot,
        ),
    },
  });
  return m;
}

export function describe(name: string, fn: () => void): void {
  const parent = currentSuite;
  const suite = new Suite(name);
  parent.suites.push(suite);
  currentSuite = suite;
  try {
    fn();
  } finally {
    currentSuite = parent;
  }
}

export const it = (name: string, fn: Fn) =>
  void currentSuite.tests.push({ name, fn });
export const test = it;
export const beforeEach = (fn: Fn) => void currentSuite.beforeEach.push(fn);
export const afterEach = (fn: Fn) => void currentSuite.afterEach.push(fn);
export const beforeAll = (fn: Fn) => void currentSuite.beforeAll.push(fn);
export const afterAll = (fn: Fn) => void currentSuite.afterAll.push(fn);
export const fail = (msg = 'Test failed'): never => {
  throw new Error(msg);
};
export const expect: any = Object.assign(
  (a: any) => ((assertionCount += 1), createMatchers(a)),
  { assertions: (n: number) => void (expectedAssertions = n) },
);

function createMockFn(impl?: (...args: any[]) => any): any {
  let def = impl;
  const once: Array<(...args: any[]) => any> = [];
  const fn: any = function (this: any, ...args: any[]) {
    fn.mock.calls.push(args);
    const active = once.shift() ?? def;
    try {
      const value = active ? active.apply(this, args) : undefined;
      fn.mock.results.push({ type: 'return', value });
      return value;
    } catch (err) {
      fn.mock.results.push({ type: 'throw', value: err });
      throw err;
    }
  };
  const set = (f: any) => ((def = f), fn);
  const push = (f: any) => (once.push(f), fn);
  return Object.assign(fn, {
    mock: { calls: [] as any[][], results: [] as any[] },
    mockClear: () => ((fn.mock.calls = []), (fn.mock.results = []), fn),
    mockReset: () => ((def = undefined), (once.length = 0), fn.mockClear()),
    mockImplementation: set,
    mockImplementationOnce: push,
    mockReturnValue: (v: any) => set(() => v),
    mockReturnValueOnce: (v: any) => push(() => v),
    mockResolvedValue: (v: any) => set(() => Promise.resolve(v)),
    mockResolvedValueOnce: (v: any) => push(() => Promise.resolve(v)),
    mockRejectedValue: (e: any) => set(() => Promise.reject(e)),
    mockRejectedValueOnce: (e: any) => push(() => Promise.reject(e)),
  });
}

export const jest = {
  fn: createMockFn,
  spyOn(obj: any, key: string) {
    const orig = obj[key];
    const mock = createMockFn(
      typeof orig === 'function' ? orig.bind(obj) : undefined,
    );
    mock.mockRestore = () => void (obj[key] = orig);
    return (obj[key] = mock);
  },
};

Object.assign(globalThis, {
  describe,
  it,
  test,
  beforeEach,
  afterEach,
  beforeAll,
  afterAll,
  expect,
  fail,
  jest,
});

const unhandledRejections: unknown[] = [];
process.on('unhandledRejection', (reason) => {
  unhandledRejections.push(reason);
});

async function runSuite(
  suite: Suite,
  prefix = '',
  before: Fn[] = [],
  after: Fn[] = [],
): Promise<{ passed: number; failed: number }> {
  let passed = 0;
  let failed = 0;
  const path =
    prefix && suite.name ? `${prefix} > ${suite.name}` : prefix || suite.name;
  for (const h of suite.beforeAll) await h();
  const allBefore = [...before, ...suite.beforeEach];
  const allAfter = [...suite.afterEach, ...after];

  for (const tc of suite.tests) {
    const fullName = path ? `${path} > ${tc.name}` : tc.name;
    expectedAssertions = null;
    assertionCount = 0;
    const errors: unknown[] = [];
    try {
      for (const h of allBefore) await h();
      await tc.fn();
      if (expectedAssertions !== null) {
        await new Promise((r) => setTimeout(r, 0));
        if (assertionCount !== expectedAssertions) {
          throw new Error(`Expected ${expectedAssertions} assertions`);
        }
      }
    } catch (err) {
      errors.push(err);
    }
    for (const h of allAfter) {
      try {
        await h();
      } catch (afterErr) {
        errors.push(afterErr);
      }
    }
    if (unhandledRejections.length > 0) {
      for (const rej of unhandledRejections.splice(0)) {
        errors.push(
          rej instanceof Error
            ? rej
            : new Error(`Unhandled rejection: ${util.inspect(rej)}`),
        );
      }
    }
    if (errors.length === 0) {
      passed += 1;
      console.log(`PASS ${fullName}`);
    } else {
      failed += 1;
      const err =
        errors.length === 1
          ? errors[0]
          : new AggregateError(
              errors,
              errors
                .map((e) => (e instanceof Error ? e.message : String(e)))
                .join('\n  afterEach: '),
            );
      console.error(`FAIL ${fullName}\n`, err);
    }
  }
  for (const child of suite.suites) {
    const res = await runSuite(child, path, allBefore, allAfter);
    passed += res.passed;
    failed += res.failed;
  }
  for (const h of suite.afterAll) await h();
  return { passed, failed };
}

let started = false;
export async function runAllTests(): Promise<void> {
  if (started) return;
  started = true;
  let passed = 0;
  let failed = 0;
  try {
    const res = await runSuite(rootSuite);
    passed = res.passed;
    failed = res.failed;
  } catch (err) {
    failed += 1;
    console.error('FAIL suite hook\n', err);
  }
  if (unhandledRejections.length > 0) {
    for (const rej of unhandledRejections.splice(0)) {
      failed += 1;
      console.error('FAIL unhandledRejection\n', rej);
    }
  }
  console.log(`\nTests: ${passed} passed, ${failed} failed`);
  process.exit(failed > 0 ? 1 : 0);
}

setImmediate(() => {
  runAllTests().catch((err) => {
    console.error('FAIL test runner\n', err);
    process.exit(1);
  });
});
