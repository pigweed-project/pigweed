// Copyright 2020 The Pigweed Authors
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

#include "pw_unit_test/unit_test_service.h"

#include <mutex>
#include <string_view>

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_protobuf/decoder.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_string/util.h"

namespace pw::unit_test {
namespace {

Result<TestCaseFilter> DecodeTestCaseFilter(ConstByteSpan filter_data) {
  protobuf::Decoder filter_decoder(filter_data);
  std::string_view suite_name;
  std::string_view test_name;
  Status status;
  while ((status = filter_decoder.Next()).ok()) {
    switch (
        static_cast<pwpb::TestFilter::Fields>(filter_decoder.FieldNumber())) {
      case pwpb::TestFilter::Fields::kSuiteName:
        filter_decoder.ReadString(&suite_name).IgnoreError();
        break;
      case pwpb::TestFilter::Fields::kTestName:
        filter_decoder.ReadString(&test_name).IgnoreError();
        break;
    }
  }

  if (status != Status::OutOfRange()) {
    return status;
  }

  return TestCaseFilter{suite_name, test_name};
}

}  // namespace

void UnitTestThread::Reset() {
  std::lock_guard lock(mutex_);
  ResetLocked();
}

void UnitTestThread::ResetLocked() {
  PW_CHECK(!running_,
           "Attempted to reset unit test thread with an active test run");
  verbose_ = false;
  test_filters_to_run_.clear();
}

Status UnitTestThread::ScheduleTestRun(
    rpc::RawServerWriter&& writer,
    span<const TestCaseFilter> test_filters_to_run) {
  std::lock_guard lock(mutex_);

  if (running_) {
    writer.Finish(Status::Unavailable()).IgnoreError();
    return Status::Unavailable();
  }

  if (test_filters_to_run.size() > test_filters_to_run_.max_size()) {
    writer.Finish(Status::InvalidArgument()).IgnoreError();
    return Status::InvalidArgument();
  }

  test_filters_to_run_.clear();

  for (const TestCaseFilter& filter : test_filters_to_run) {
    test_filters_to_run_.emplace_back();
    if (!string::Assign(test_filters_to_run_.back().suite_name,
                        filter.suite_name)
             .ok() ||
        !string::Assign(test_filters_to_run_.back().test_name, filter.test_name)
             .ok()) {
      test_filters_to_run_.clear();
      writer.Finish(Status::InvalidArgument()).IgnoreError();
      return Status::InvalidArgument();
    }
  }

  writer_ = std::move(writer);

  notification_.release();
  return OkStatus();
}

void UnitTestThread::Run() {
  while (true) {
    notification_.acquire();

    {
      std::lock_guard lock(mutex_);
      PW_CHECK(!running_);
      running_ = true;
    }

    Vector<TestCaseFilter, kMaxTestFilters> test_filters;
    for (const auto& filter : test_filters_to_run_) {
      test_filters.push_back({filter.suite_name, filter.test_name});
    }

    PW_LOG_INFO("Starting unit test run");
    handler_.ExecuteTests(test_filters);
    PW_LOG_INFO("Unit test run complete");

    writer_.Finish(OkStatus()).IgnoreError();

    {
      std::lock_guard lock(mutex_);
      running_ = false;

      ResetLocked();
    }
  }
}

void UnitTestThread::Service::Run(ConstByteSpan request,
                                  RawServerWriter& writer) {
  if (thread_.running()) {
    PW_LOG_WARN("Unit test run requested while one is already in progress");
    writer.Finish(Status::Unavailable()).IgnoreError();
    return;
  }

  // List of test filters to run. The string views point to data in the raw
  // protobuf request message, so they are only valid for the duration of this
  // function.
  Vector<TestCaseFilter, UnitTestThread::kMaxTestFilters> test_filters;

  protobuf::Decoder decoder(request);

  Status decode_status;
  while ((decode_status = decoder.Next()).ok()) {
    switch (static_cast<pwpb::TestRunRequest::Fields>(decoder.FieldNumber())) {
      case pwpb::TestRunRequest::Fields::kReportPassedExpectations: {
        bool value;
        decode_status = decoder.ReadBool(&value);
        if (!decode_status.ok()) {
          break;
        }
        thread_.set_verbose(value);
        break;
      }

      case pwpb::TestRunRequest::Fields::kTestSuite: {
        if (test_filters.full()) {
          PW_LOG_ERROR("Maximum of %u test filters supported",
                       static_cast<unsigned>(test_filters.max_size()));
          writer.Finish(Status::InvalidArgument()).IgnoreError();
          return;
        }

        std::string_view suite_name;
        if (!decoder.ReadString(&suite_name).ok()) {
          break;
        }

        test_filters.push_back({suite_name, {}});
        break;
      }

      case pwpb::TestRunRequest::Fields::kTestFilters: {
        if (test_filters.full()) {
          PW_LOG_ERROR("Maximum of %u test filters supported",
                       static_cast<unsigned>(test_filters.max_size()));
          writer.Finish(Status::InvalidArgument()).IgnoreError();
          return;
        }

        ConstByteSpan filter_data;
        if (!decoder.ReadBytes(&filter_data).ok()) {
          break;
        }

        Result<TestCaseFilter> filter = DecodeTestCaseFilter(filter_data);
        if (!filter.ok()) {
          writer.Finish(filter.status()).IgnoreError();
          return;
        }

        test_filters.push_back(*filter);
        break;
      }
    }
  }

  if (decode_status != Status::OutOfRange()) {
    writer.Finish(decode_status).IgnoreError();
    return;
  }

  PW_LOG_INFO("Queueing unit test run");

  if (!thread_.ScheduleTestRun(std::move(writer), test_filters).ok()) {
    PW_LOG_ERROR("Failed to queue unit test run");
  }
}

void UnitTestThread::WriteTestRunStart() {
  // Write out the key for the start field (even though the message is empty).
  WriteEvent([&](pwpb::Event::StreamEncoder& event) {
    event.GetTestRunStartEncoder();
  });
}

void UnitTestThread::WriteTestRunEnd(const RunTestsSummary& summary) {
  WriteEvent([&](pwpb::Event::StreamEncoder& event) {
    pwpb::TestRunEnd::StreamEncoder test_run_end = event.GetTestRunEndEncoder();
    test_run_end.WritePassed(static_cast<uint32_t>(summary.passed_tests))
        .IgnoreError();
    test_run_end.WriteFailed(static_cast<uint32_t>(summary.failed_tests))
        .IgnoreError();
    test_run_end.WriteSkipped(static_cast<uint32_t>(summary.skipped_tests))
        .IgnoreError();
    test_run_end.WriteDisabled(static_cast<uint32_t>(summary.disabled_tests))
        .IgnoreError();
  });
}

void UnitTestThread::WriteTestCaseStart(const TestCase& test_case) {
  WriteEvent([&](pwpb::Event::StreamEncoder& event) {
    pwpb::TestCaseDescriptor::StreamEncoder descriptor =
        event.GetTestCaseStartEncoder();
    descriptor.WriteSuiteName(test_case.suite_name).IgnoreError();
    descriptor.WriteTestName(test_case.test_name).IgnoreError();
    descriptor.WriteFileName(test_case.file_name).IgnoreError();
  });
}

void UnitTestThread::WriteTestCaseEnd(TestResult result) {
  WriteEvent([&](pwpb::Event::StreamEncoder& event) {
    event.WriteTestCaseEnd(static_cast<pwpb::TestCaseResult>(result))
        .IgnoreError();
  });
}

void UnitTestThread::WriteTestCaseDisabled(const TestCase& test_case) {
  WriteEvent([&](pwpb::Event::StreamEncoder& event) {
    pwpb::TestCaseDescriptor::StreamEncoder descriptor =
        event.GetTestCaseDisabledEncoder();
    descriptor.WriteSuiteName(test_case.suite_name).IgnoreError();
    descriptor.WriteTestName(test_case.test_name).IgnoreError();
    descriptor.WriteFileName(test_case.file_name).IgnoreError();
  });
}

void UnitTestThread::WriteTestCaseExpectation(
    const TestExpectation& expectation) {
  if (!verbose_ && expectation.success) {
    return;
  }

  WriteEvent([&](pwpb::Event::StreamEncoder& event) {
    pwpb::TestCaseExpectation::StreamEncoder test_case_expectation =
        event.GetTestCaseExpectationEncoder();
    test_case_expectation.WriteExpression(expectation.expression).IgnoreError();
    test_case_expectation
        .WriteEvaluatedExpression(expectation.evaluated_expression)
        .IgnoreError();
    test_case_expectation.WriteFileName(expectation.file_name).IgnoreError();
    test_case_expectation
        .WriteLineNumber(static_cast<uint32_t>(expectation.line_number))
        .IgnoreError();
    test_case_expectation.WriteSuccess(expectation.success).IgnoreError();
  });
}

}  // namespace pw::unit_test
