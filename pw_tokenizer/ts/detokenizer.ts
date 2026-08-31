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

/**  Decodes and detokenizes strings from binary or Base64 input. */
import { Frame } from 'pigweedjs/pw_hdlc';
import { base64ToBinaryString } from 'pigweedjs/pw_base64';
import { TokenDatabase } from './token_database';
import { PrintfDecoder } from './printf_decoder';
const MAX_RECURSIONS = 9;
const BASE64CHARS = '[A-Za-z0-9+\\-\\/_]';
const PATTERN = new RegExp(
  // Base64 tokenized strings start with the prefix character ($)
  '\\$' +
    // Tokenized strings contain 0 or more blocks of four Base64 chars.
    `(?:${BASE64CHARS}{4})*` +
    // The last block of 4 chars may have one or two padding chars (=).
    `(?:${BASE64CHARS}{3}=|${BASE64CHARS}{2}==)?`,
  'g',
);
const BASE10_TOKEN_REGEX = '(?<base10>[0-9]{10})';
const BASE16_TOKEN_REGEX = '(?<base16>[A-Fa-f0-9]{8})';
const BASE64_TOKEN_REGEX = `(?<base64>(?:${BASE64CHARS}{4})*(?:${BASE64CHARS}{3}=|${BASE64CHARS}{2}==)?)`;
const NESTED_TOKEN_BASE_PREFIX = '#';
const NESTED_DOMAIN_START_PREFIX = '{';
const NESTED_DOMAIN_END_PREFIX = '}';
const NESTED_TOKEN_PREFIX = '$';
const NESTED_TOKEN_FORMATS = [
  BASE10_TOKEN_REGEX,
  BASE16_TOKEN_REGEX,
  BASE64_TOKEN_REGEX,
];

function tokenRegex(prefix: string): RegExp {
  const escapedPrefix = prefix.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); // Escape special regex characters
  return new RegExp(
    // Tokenized strings start with the prefix character ($).
    escapedPrefix +
      // Optional; no domain specifier defaults to (empty) domain.
      // Brackets ({}) specifies domain string
      '(?<domainspec>(' +
      NESTED_DOMAIN_START_PREFIX +
      '(?<domain>\\s*|\\s*[a-zA-Z_:][a-zA-Z0-9_:\\s]*)' +
      NESTED_DOMAIN_END_PREFIX +
      '))?' +
      // Optional; no base specifier defaults to BASE64.
      // Hash (#) with no number specified defaults to Base-16.
      '(?<basespec>(?<base>[0-9]*)?' +
      NESTED_TOKEN_BASE_PREFIX +
      ')?' +
      // Match one of the following token formats.
      '(' +
      NESTED_TOKEN_FORMATS.join('|') +
      ')',
    'g',
  );
}

interface TokenAndArgs {
  token: number;
  args: Uint8Array;
}

export class Detokenizer {
  private database: TokenDatabase;
  private _token_regex: RegExp;
  private prefix: string;

  constructor(csvDatabase: string) {
    this.prefix = NESTED_TOKEN_PREFIX; // Initialize prefix first
    this.database = new TokenDatabase(csvDatabase);
    this._token_regex = tokenRegex(this.prefix); // Now it's safe to use this.prefix
  }

  /**
   * Detokenize frame data into actual string messages using the provided
   * token database.
   *
   * If the frame doesn't match any token from database, the frame will be
   * returned as string as is.
   */
  detokenize(
    tokenizedFrame: Frame,
    domain = '',
    maxRecursion: number = MAX_RECURSIONS,
  ): string {
    return this.detokenizeUint8Array(tokenizedFrame.data, domain, maxRecursion);
  }

  /**
   * Detokenize uint8 into actual string messages using the provided
   * token database.
   *
   * If the data doesn't match any token from database, the data will be
   * returned as string as is.
   */
  detokenizeUint8Array(
    data: Uint8Array,
    domain = '',
    recursion: number = MAX_RECURSIONS,
  ): string {
    const decodedString = new TextDecoder().decode(data);
    if (PATTERN.test(decodedString)) {
      return this.detokenizeBase64String(decodedString, domain, recursion);
    }
    const { token, args } = this.decodeUint8Array(data);
    // Parse arguments if this is printf-style text.
    const formatString = this.database.get(token, domain);
    if (!formatString) {
      return decodedString;
    }

    if (recursion > 0) {
      return this.detokenizeText(formatString, recursion);
    }

    return new PrintfDecoder().decode(String(formatString), args);
  }

  public detokenizeBase64String(
    base64String: string,
    domain: string,
    recursions: number,
  ): string {
    return base64String.replace(PATTERN, (base64Substring) => {
      const { token, args } = this.decodeBase64TokenFrame(base64Substring);
      const format = this.database.get(token, domain);
      // Parse arguments if this is printf-style text.
      if (format) {
        const decodedOriginal = new PrintfDecoder().decode(
          String(format),
          args,
        );
        // Detokenize nested Base64 tokens and their arguments.
        if (recursions > 0) {
          return this.detokenizeBase64String(
            decodedOriginal,
            // Update the domain value for nested tokens.
            this.getDomain(base64Substring),
            recursions - 1,
          );
        }
        return decodedOriginal;
      }
      return base64Substring;
    });
  }

  private decodeUint8Array(data: Uint8Array): TokenAndArgs {
    const token = new DataView(data.buffer, data.byteOffset, 4).getUint32(
      0,
      true,
    );
    const args = new Uint8Array(data.buffer.slice(data.byteOffset + 4));

    return { token, args };
  }

  private decodeBase64TokenFrame(base64Data: string): TokenAndArgs {
    // Remove the prefix '$' and convert from Base64.
    const prefixRemoved = base64Data.slice(1);
    const noBase64 = base64ToBinaryString(prefixRemoved);
    // Convert back to bytes and return token and arguments.
    const bytes = noBase64.split('').map((ch) => ch.charCodeAt(0));
    let uIntArray = new Uint8Array(bytes);
    // Check if there are enough bytes to create a DataView of length 4.
    if (uIntArray.length < 4) {
      // If there are not enough bytes, pad with zeroes.
      const paddedArray = new Uint8Array(4);
      paddedArray.set(uIntArray);
      uIntArray = paddedArray;
    }
    const token = new DataView(
      uIntArray.buffer,
      uIntArray.byteOffset,
      4,
    ).getUint32(0, true);
    const args = new Uint8Array(bytes.slice(4));

    return { token, args };
  }

  private detokenizeScan(match: string, ...args: any[]): string {
    const groups = args.pop();
    const basespec = groups.basespec;
    const base = groups.base;
    let domain = groups.domain;
    const base10 = groups.base10;
    const base16 = groups.base16;
    const base64 = groups.base64;

    if (domain === undefined) {
      domain = '';
    } else {
      domain = domain.replace(/\s/g, '');
    }

    if (base64 || basespec == undefined) {
      // Detokenize Base64-encoded frame data into actual string messages using the
      // provided token database.
      //
      // If the frame doesn't match any token from database, the frame will be
      // returned as string as is.
      const { token, args } = this.decodeBase64TokenFrame(match);
      const format = this.database.get(token, domain);
      if (format) {
        return new PrintfDecoder().decode(String(format), args);
      }
      return match;
    }

    let usedBase = base;
    if (!usedBase) {
      usedBase = '16';
    }

    let tokenString = '';
    if (base10) {
      tokenString = base10;
    } else if (base16) {
      tokenString = base16;
    }

    return this.detokenizeOnce(tokenString, usedBase, domain, match);
  }

  private detokenizeOnce(
    tokenString: string,
    base: string,
    domain: string,
    match: string,
  ): string {
    if (!this.database)
      return `${this.prefix}${
        domain.length > 0
          ? NESTED_DOMAIN_START_PREFIX + domain + NESTED_DOMAIN_END_PREFIX
          : ''
      }${base.length > 0 ? base + NESTED_TOKEN_BASE_PREFIX : ''}${tokenString}`;
    const token = parseInt(tokenString, parseInt(base));
    const entries = this.database.get(token, domain);
    if (entries) {
      return String(entries);
    }
    return match;
  }

  public detokenizeText(
    message: string,
    recursion: number = MAX_RECURSIONS,
  ): string {
    if (!this.database) {
      return message;
    }

    let result: string = message;
    for (let i = 0; i < recursion - 1; i++) {
      const newResult = result.replace(this._token_regex, (match, ...args) => {
        return this.detokenizeScan(match, ...args);
      });

      if (newResult === result) {
        return result;
      }
      result = newResult;
    }
    return result;
  }

  private getDomain(base64String: string): string {
    const result = base64String.replace(this._token_regex, (_, ...args) => {
      const groups = args.pop();
      let domain = groups.domain;
      if (domain === undefined) {
        domain = '';
      } else {
        domain = domain.replace(/\s/g, '');
      }
      return domain;
    });
    return result;
  }
}
