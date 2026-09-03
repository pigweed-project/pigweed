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

import googleProtobuf from 'google-protobuf';
import { ReadableStream, WritableStream } from 'stream/web';
import {
  TextEncoder as NodeTextEncoder,
  TextDecoder as NodeTextDecoder,
} from 'util';

globalThis.goog = googleProtobuf;
globalThis.jspb = googleProtobuf.jspb;
if (typeof window !== 'undefined') {
  window.goog = googleProtobuf;
  window.jspb = googleProtobuf.jspb;
}

Object.defineProperties(globalThis, {
  ReadableStream: { value: ReadableStream },
  WritableStream: { value: WritableStream },
});

class WrappedTextEncoder extends NodeTextEncoder {
  encode(input) {
    const result = super.encode(input);
    return new Uint8Array(result);
  }
}

if (typeof globalThis.TextEncoder === 'undefined') {
  globalThis.TextEncoder = WrappedTextEncoder;
}

if (typeof globalThis.TextDecoder === 'undefined') {
  globalThis.TextDecoder = NodeTextDecoder;
}

Object.defineProperty(window, 'matchMedia', {
  writable: true,
  value: jest.fn().mockImplementation((query) => ({
    matches: false,
    media: query,
    onchange: null,
    addListener: jest.fn(),
    removeListener: jest.fn(),
    addEventListener: jest.fn(),
    removeEventListener: jest.fn(),
    dispatchEvent: jest.fn(),
  })),
});
