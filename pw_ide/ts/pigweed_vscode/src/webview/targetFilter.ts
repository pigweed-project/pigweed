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

export interface TargetFilterItem {
  label: string;
  displayName?: string;
  hasCpp?: boolean;
  hasRust?: boolean;
}

/**
 * Filters a list of preconfigured targets by language type ('cpp' or 'rust').
 */
export function filterPreconfiguredTargets<T extends TargetFilterItem>(
  targets: T[] | undefined,
  langType: 'cpp' | 'rust',
): T[] {
  if (!targets) return [];
  return targets.filter((t) => (langType === 'cpp' ? t.hasCpp : t.hasRust));
}
