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
 * Converts Base64 (standard or URL-safe) to a binary (latin1) string.
 * Uses Node's Buffer when available; falls back to globalThis.atob() in browser runtimes.
 */
export function base64ToBinaryString(base64Data: string): string {
  const globalBuffer = (globalThis as { Buffer?: any }).Buffer;
  if (typeof globalBuffer !== 'undefined') {
    return globalBuffer
      .from(base64Data.replace(/-/g, '+').replace(/_/g, '/'), 'base64')
      .toString('binary');
  }
  if (typeof globalThis.atob !== 'undefined') {
    try {
      // atob() only accepts standard alphabet; normalize URL-safe chars (- and _)
      return globalThis.atob(base64Data.replace(/-/g, '+').replace(/_/g, '/'));
    } catch {
      // Malformed input: return empty string
      return '';
    }
  }
  return '';
}

/**
 * Decodes Base64 data (standard or URL-safe) to a Uint8Array byte array.
 */
export function decode(base64Data: string): Uint8Array {
  const binaryString = base64ToBinaryString(base64Data);
  const bytes = new Uint8Array(binaryString.length);
  for (let i = 0; i < binaryString.length; i++) {
    bytes[i] = binaryString.charCodeAt(i);
  }
  return bytes;
}

/**
 * Encodes a Uint8Array to a standard Base64 string.
 */
export function encode(bytes: Uint8Array): string {
  const globalBuffer = (globalThis as { Buffer?: any }).Buffer;
  if (typeof globalBuffer !== 'undefined') {
    return globalBuffer.from(bytes).toString('base64');
  }
  let binary = '';
  for (let i = 0; i < bytes.length; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return globalThis.btoa(binary);
}
