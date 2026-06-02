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

export function decodeBazelName(name: string): string {
  if (name.startsWith('@@____')) {
    const cleaned = name.replace('@@____', '');
    const parts = cleaned.split('__');
    if (parts.length >= 2) {
      return `@${parts[0]}//${parts.slice(1, -1).join('/')}:${
        parts[parts.length - 1]
      }`;
    }
  }

  if (name.startsWith('@@')) {
    const decoded = '@' + name.slice(2);
    return decoded.replace(/____/g, '//').replace(/__/g, ':');
  }

  return name.replace(/____/g, '//').replace(/__/g, ':');
}
