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

import * as assert from 'assert';
import * as sinon from 'sinon';
import { isGeneratedOrExternalFile, fsUtils } from './inactiveFileDecoration';

suite('isGeneratedOrExternalFile', () => {
  test('detects generated bazel directories', () => {
    assert.strictEqual(
      isGeneratedOrExternalFile('/path/to/project/bazel-out/bin/some_file.h'),
      true,
    );
    assert.strictEqual(
      isGeneratedOrExternalFile('/path/to/project/bazel-bin/some_file.h'),
      true,
    );
    assert.strictEqual(
      isGeneratedOrExternalFile(
        'C:\\path\\to\\project\\bazel-out\\bin\\some_file.h',
      ),
      true,
    );
  });

  test('detects external directories', () => {
    assert.strictEqual(
      isGeneratedOrExternalFile('/path/to/project/external/nanopb/pb.h'),
      true,
    );
    assert.strictEqual(
      isGeneratedOrExternalFile(
        'C:\\path\\to\\project\\external\\nanopb\\pb.h',
      ),
      true,
    );
  });

  test('detects environment directories', () => {
    assert.strictEqual(
      isGeneratedOrExternalFile(
        '/path/to/project/environment/pigweed-venv/bin/python',
      ),
      true,
    );
  });

  test('returns false for standard source files', () => {
    assert.strictEqual(
      isGeneratedOrExternalFile(
        '/path/to/project/pw_rpc/nanopb/public/pw_rpc/echo_service_nanopb.h',
      ),
      false,
    );
    assert.strictEqual(
      isGeneratedOrExternalFile(
        '/path/to/project/pw_rpc/nanopb/echo_service_test.cc',
      ),
      false,
    );
    assert.strictEqual(
      isGeneratedOrExternalFile(
        'C:\\path\\to\\project\\pw_rpc\\nanopb\\echo_service_test.cc',
      ),
      false,
    );
  });

  suite('with custom symlink prefix and projectRoot', () => {
    let realpathSyncStub: sinon.SinonStub;

    setup(() => {
      realpathSyncStub = sinon.stub(fsUtils, 'realpathSync');
    });

    teardown(() => {
      realpathSyncStub.restore();
    });

    test('detects custom output files when resolved outside project root', () => {
      realpathSyncStub
        .withArgs('/path/to/project/custom_out/out/foo.h')
        .returns('/private/var/tmp/bazel-out/foo.h');
      realpathSyncStub.withArgs('/path/to/project').returns('/path/to/project');

      assert.strictEqual(
        isGeneratedOrExternalFile(
          '/path/to/project/custom_out/out/foo.h',
          '/path/to/project',
        ),
        true,
      );
    });

    test('returns false for source files resolved inside project root', () => {
      realpathSyncStub
        .withArgs('/path/to/project/pw_rpc/echo.cc')
        .returns('/path/to/project/pw_rpc/echo.cc');
      realpathSyncStub.withArgs('/path/to/project').returns('/path/to/project');

      assert.strictEqual(
        isGeneratedOrExternalFile(
          '/path/to/project/pw_rpc/echo.cc',
          '/path/to/project',
        ),
        false,
      );
    });
  });
});
