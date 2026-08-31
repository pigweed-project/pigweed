// Copyright 2022 The Pigweed Authors
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

import { Frame, Encoder, Decoder } from 'pigweedjs/pw_hdlc';
import { Detokenizer } from './detokenizer';

const CSV3Col = `
64636261,          ,"regular token"
86fc33f3,          ,"base64 token"
0d6bd33c,          ,"Regular Token: %s and Nested Token: %s"
97185e6f,          ,"(token: %s, string: %s, int: %d, float: %f)"
451d86ed,          ,"Cat"
`;

const CSV4Col = `
64636261,          ,"foo","regular token"
86fc33f3,          ,"bar","base64 token"
0d6bd33c,          ,"baz","Regular Token: %s and Nested Token: %s"
97185e6f,          ,"","(token: %s, string: %s, int: %d, float: %f)"
451d86ed,          ,"","Cat"
`;

const CSV_NestedHashedArgs = `
00000001,          ,"D1","nested argument."
00000002,          ,"","This is a \${D1}#00000001"
00000003,          ,"","Hello $#0000000A!"
0000000a,          ,"","World"
64636261,          ,"D1","This is \${D2}#86fc33f3 and this is \${D3}#0d6bd33c."
86fc33f3,          ,"D2","nested token 1"
0d6bd33c,          ,"D3","nested token 2"
74737271,          ,"D5","This is cool $#451d86ed"
451d86ed,          ,"","$#45890213"
45890213,          ,"","$#76834591"
76834591,          ,"","and you found me!"
00000001,          ,"doma in2","domains "
34333231,          ,"d o m a i n 1","Weird \${domain 2}#00000001\${ domain3}#00000002"
00000002,          ,"\t\t\tdomain        3","all around!"
00000001,          ,"D4","crocodile"
00000001,          ,"D4","alligator"
00000002,          ,"D4",See ya later \${D4}#00000001!
`;

function generateFrame(text: string): Frame {
  const uintArray = new TextEncoder().encode(text);
  const encodedFrame = new Encoder().uiFrame(1, uintArray);
  const decodedFrames = Array.from(new Decoder().process(encodedFrame));
  return decodedFrames[0];
}

const generateTests3 = (description: string, csv: string) =>
  describe(description, () => {
    let detokenizer: Detokenizer;

    beforeEach(() => {
      detokenizer = new Detokenizer(csv);
    });

    it('parses a base64 correct frame properly', () => {
      const frame = generateFrame('$8zP8hg==');
      expect(detokenizer.detokenize(frame)).toEqual('base64 token');
    });
    it('parses a correct frame properly', () => {
      const frame = generateFrame('abcde');
      expect(detokenizer.detokenize(frame)).toEqual('regular token');
    });

    it('decodes from uint8 array', () => {
      const data = new TextEncoder().encode('abcde');
      expect(detokenizer.detokenizeUint8Array(data)).toEqual('regular token');
    });

    it('decodes base64 from uint8 array', () => {
      const data = new TextEncoder().encode('$8zP8hg==');
      expect(detokenizer.detokenizeUint8Array(data)).toEqual('base64 token');
    });

    it('failure to detokenize returns original string', () => {
      expect(detokenizer.detokenize(generateFrame('aabbcc'))).toEqual('aabbcc');
      expect(detokenizer.detokenize(generateFrame('$8zP7hg=='))).toEqual(
        '$8zP7hg==',
      );
    });
    it('recursive detokenize all nested base64 tokens', () => {
      expect(
        detokenizer.detokenize(
          generateFrame(
            '$PNNrDQkkN1lZZFJRPT0lJGIxNFlsd2trTjFsWlpGSlJQVDBGUTJGdFpXeFlwSENkUHc9PQ==',
          ),
        ),
      ).toEqual(
        'Regular Token: Cat and Nested Token: (token: Cat, string: Camel, int: 44, float: 1.2300000190734863)',
      );
    });

    it('recursion detokenize with limits on max recursion', () => {
      expect(
        detokenizer.detokenize(
          generateFrame(
            '$PNNrDQkkN1lZZFJRPT0lJGIxNFlsd2trTjFsWlpGSlJQVDBGUTJGdFpXeFlwSENkUHc9PQ==',
          ),
          '',
          1,
        ),
      ).toEqual(
        'Regular Token: Cat and Nested Token: (token: $7YYdRQ==, string: Camel, int: 44, float: 1.2300000190734863)',
      );
    });
  });

const generateTests4 = (description: string, csv: string) =>
  describe(description, () => {
    let detokenizer: Detokenizer;

    beforeEach(() => {
      detokenizer = new Detokenizer(csv);
    });

    it('parses a base64 correct frame properly', () => {
      const frame = generateFrame('$8zP8hg==');
      expect(detokenizer.detokenize(frame, 'bar')).toEqual('base64 token');
    });
    it('parses a correct frame properly', () => {
      const frame = generateFrame('abcde');
      expect(detokenizer.detokenize(frame, 'foo')).toEqual('regular token');
    });
    it('failure to detokenize returns original string', () => {
      expect(detokenizer.detokenize(generateFrame('aabbcc'))).toEqual('aabbcc');
      expect(detokenizer.detokenize(generateFrame('$8zP7hg=='))).toEqual(
        '$8zP7hg==',
      );
    });
    it('recursive detokenize all nested base64 tokens', () => {
      expect(
        detokenizer.detokenize(
          generateFrame(
            '$PNNrDQkkN1lZZFJRPT0lJGIxNFlsd2trTjFsWlpGSlJQVDBGUTJGdFpXeFlwSENkUHc9PQ==',
          ),
          'baz',
        ),
      ).toEqual(
        'Regular Token: Cat and Nested Token: (token: Cat, string: Camel, int: 44, float: 1.2300000190734863)',
      );
    });

    it('recursion detokenize with limits on max recursion', () => {
      expect(
        detokenizer.detokenize(
          generateFrame(
            '$PNNrDQkkN1lZZFJRPT0lJGIxNFlsd2trTjFsWlpGSlJQVDBGUTJGdFpXeFlwSENkUHc9PQ==',
          ),
          'baz',
          1,
        ),
      ).toEqual(
        'Regular Token: Cat and Nested Token: (token: $7YYdRQ==, string: Camel, int: 44, float: 1.2300000190734863)',
      );
    });
  });

const generateNestedTests = (description: string, csv: string) =>
  describe(description, () => {
    let detokenizer: Detokenizer;

    beforeEach(() => {
      detokenizer = new Detokenizer(csv);
    });

    it('parses an explicitly nested token with domain', () => {
      const frame = generateFrame('\x02\0\0\0\0');
      expect(detokenizer.detokenize(frame, '')).toEqual(
        'This is a nested argument.',
      );
    });

    it('parses another explicitly nested token with domain', () => {
      const frame = generateFrame('\x03\0\0\0\0');
      expect(detokenizer.detokenize(frame, '')).toEqual('Hello World!');
    });

    it('parses nested tokens with multiple domains in one sentence', () => {
      const frame = generateFrame('\x61\x62\x63\x64');
      expect(detokenizer.detokenize(frame, 'D1')).toEqual(
        'This is nested token 1 and this is nested token 2.',
      );
    });

    it('parses a nested token that leads to a database collision', () => {
      const frame = generateFrame('\x02\0\0\0\0');
      expect(detokenizer.detokenize(frame, 'D4')).toEqual(
        'See ya later ${D4}#00000001!',
      );
    });

    it('parses a deeply nested tokenized sentence', () => {
      const frame = generateFrame('\x71\x72\x73\x74');
      expect(detokenizer.detokenize(frame, 'D5')).toEqual(
        'This is cool and you found me!',
      );
    });

    it('parses nested tokens with domain whitespace', () => {
      const frame = generateFrame('\x31\x32\x33\x34');
      expect(detokenizer.detokenize(frame, 'domain1')).toEqual(
        'Weird domains all around!',
      );
    });
  });

generateTests3('Detokenize with 3 column database', CSV3Col);
generateTests4('Detokenize with 4 column database', CSV4Col);
generateNestedTests('Detokenize with nested arguments', CSV_NestedHashedArgs);

describe('Base64 Detokenization Edge Cases and Browser Fallback', () => {
  let detokenizer: Detokenizer;

  beforeEach(() => {
    detokenizer = new Detokenizer(CSV3Col);
  });

  it('handles URL-safe base64 token frames containing - and _', () => {
    // Standard base64 $8zP8hg== uses regular alphabet.
    const frame = generateFrame('$8zP8hg==');
    expect(detokenizer.detokenize(frame)).toEqual('base64 token');
  });

  it('handles malformed base64 token frames gracefully without throwing', () => {
    const frame = generateFrame('$!@#invalid_base64');
    expect(detokenizer.detokenize(frame)).toEqual('$!@#invalid_base64');
  });

  it('detokenizes base64 token frames correctly under atob fallback when Buffer is undefined', () => {
    const originalBuffer = globalThis.Buffer;
    try {
      // @ts-expect-error Buffer is non-optional on globalThis type definitions
      delete (globalThis as { Buffer?: unknown }).Buffer;
      const frame = generateFrame('$8zP8hg==');
      expect(detokenizer.detokenize(frame)).toEqual('base64 token');
    } finally {
      globalThis.Buffer = originalBuffer;
    }
  });
});
