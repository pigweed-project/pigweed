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

import fs from 'fs';
import path from 'path';
import * as argModule from 'arg';
const arg = argModule.default;
import { buildProtos } from './build';

const args = arg({
  '--outputCollectionName': String,
});

const outputCollectionName = args['--outputCollectionName'] || 'collection.ts';

const OUT_DIR = path.join(process.cwd(), 'dist', 'protos');
if (!fs.existsSync(OUT_DIR)) {
  fs.mkdirSync(OUT_DIR, { recursive: true });
}

const protos = [
  'pw_transfer/transfer.proto',
  'pw_rpc/ts/test.proto',
  'pw_rpc/echo.proto',
  'pw_protobuf/pw_protobuf_protos/status.proto',
  'pw_protobuf/pw_protobuf_protos/common.proto',
  'pw_tokenizer/pw_tokenizer_proto/options.proto',
  'pw_log/log.proto',
  'pw_rpc/ts/test2.proto',
  'pw_rpc/internal/packet.proto',
  'pw_protobuf_compiler/pw_protobuf_compiler_protos/nested/more_nesting/test.proto',
  'pw_protobuf_compiler/pw_protobuf_compiler_protos/test.proto',
  'pw_rpc2/cpp/pw_rpc2_test.proto',
  'pw_rpc2/cpp/examples/echo/echo.proto',
];

const remapProtos = {
  'pw_protobuf/pw_protobuf_protos/common.proto':
    'pw_protobuf_protos/common.proto',
  'pw_tokenizer/pw_tokenizer_proto/options.proto':
    'pw_tokenizer_proto/options.proto',
};

// Only modify the .proto files when running this builder and then restore any
// modified .proto files to their original states after the builder has finished
// running.
const newProtoPaths = [];
protos.forEach((protoPath) => {
  const protoFullPath = path.join(process.cwd(), '..', protoPath);
  const protoData = fs.readFileSync(protoFullPath, 'utf-8');
  // Remap file path
  if (remapProtos[protoPath]) {
    protoPath = remapProtos[protoPath];
  }
  newProtoPaths.push(protoPath);
  // Also write a copy to output directory.
  const protoDistPath = path.join(OUT_DIR, protoPath);
  if (!fs.existsSync(path.dirname(protoDistPath))) {
    fs.mkdirSync(path.dirname(protoDistPath), { recursive: true });
  }

  fs.writeFileSync(protoDistPath, protoData);
});

Promise.resolve(
  buildProtos(newProtoPaths, OUT_DIR, outputCollectionName, OUT_DIR),
).catch((err: unknown) => {
  console.error(err);
  process.exit(1);
});
