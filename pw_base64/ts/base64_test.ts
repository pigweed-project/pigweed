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

/* eslint-env browser */

import { base64ToBinaryString, decode, encode } from './base64';

describe('pw_base64', () => {
  it('encodes and decodes standard base64 data correctly', () => {
    const text = 'Hello Pigweed Base64!';
    const bytes = new TextEncoder().encode(text);
    const encoded = encode(bytes);
    expect(encoded).toEqual('SGVsbG8gUGlnd2VlZCBCYXNlNjQh');

    const decodedBytes = decode(encoded);
    const decodedText = new TextDecoder().decode(decodedBytes);
    expect(decodedText).toEqual(text);
  });

  it('handles URL-safe base64 data with - and _', () => {
    // URL-safe base64 containing '-' and '_'
    // standard base64 for [0xfb, 0xff, 0xfe] is "++/+"
    // URL-safe base64 is "--_-"
    const urlSafe = '--_-';
    const binaryString = base64ToBinaryString(urlSafe);
    expect(binaryString.length).toEqual(3);
    expect(binaryString.charCodeAt(0)).toEqual(0xfb);
    expect(binaryString.charCodeAt(1)).toEqual(0xff);
    expect(binaryString.charCodeAt(2)).toEqual(0xfe);
  });

  it('fallback to atob when globalThis.Buffer is undefined', () => {
    const originalBuffer = globalThis.Buffer;
    try {
      // @ts-ignore
      delete globalThis.Buffer;
      const text = 'Fallback to atob';
      const encoded = 'RmFsbGJhY2sgdG8gYXRvYg==';
      const binaryString = base64ToBinaryString(encoded);
      expect(binaryString).toEqual(text);
    } finally {
      globalThis.Buffer = originalBuffer;
    }
  });

  it('handles malformed base64 strings gracefully without throwing', () => {
    const malformed = '$invalid_base64!!!';
    expect(() => base64ToBinaryString(malformed)).not.toThrow();
    // Under Buffer or atob fallback, malformed returns '' or safely handles
    const binary = base64ToBinaryString(malformed);
    expect(typeof binary).toEqual('string');
  });

  it('decodes single byte test vectors', () => {
    const vectors: Array<[number, string]> = [
      [0x00, 'AA=='],
      [0x01, 'AQ=='],
      [0x41, 'QQ=='], // 'A'
      [0xff, '/w=='],
    ];

    for (const [byteVal, expectedEncoded] of vectors) {
      const decoded = decode(expectedEncoded);
      expect(decoded.length).toEqual(1);
      expect(decoded[0]).toEqual(byteVal);
    }
  });
});
