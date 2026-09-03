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

import { pathsToModuleNameMapper } from 'ts-jest';
import type { InitialOptionsTsJest } from 'ts-jest/dist/types';

const paths = {
  'pigweedjs/pw_*': ['../pw_*/ts'],
  'pigweedjs/protos/*': ['./dist/protos/*'],
};
const config: InitialOptionsTsJest = {
  preset: 'ts-jest/presets/js-with-ts',
  testRegex: '(/__tests__/.*|(\\_|/)(test|spec))\\.tsx?$',
  moduleNameMapper: {
    ...pathsToModuleNameMapper(paths, { prefix: '<rootDir>/' }),
    '^google-protobuf$': '<rootDir>/node_modules/google-protobuf',
    '^google-protobuf/(.*)$': '<rootDir>/node_modules/google-protobuf/$1',
    '^papaparse$': '<rootDir>/node_modules/papaparse',
    '^long$': '<rootDir>/node_modules/long',
  },
  setupFilesAfterEnv: ['./jest.polyfills.js'],
  transformIgnorePatterns: ['/node_modules/(?!rxjs)'],
  testEnvironment: 'jsdom',
  roots: ['<rootDir>/..'],
  testPathIgnorePatterns: [
    '/node_modules/',
    '/environment/',
    '/out/',
    '/dist/',
    '/bazel-out/',
    '/rules_js/',
    '/pw_ide/',
  ],
  modulePathIgnorePatterns: [
    '<rootDir>/../out',
    '<rootDir>/../dist',
    '<rootDir>/../bazel-out',
    '<rootDir>/../node_modules',
    '<rootDir>/../rules_js',
    '<rootDir>/../environment',
  ],
  moduleDirectories: ['node_modules', '<rootDir>/node_modules'],
  globals: {
    'ts-jest': {
      diagnostics: {
        ignoreCodes: [2307],
      },
    },
  },
};

export default config;
