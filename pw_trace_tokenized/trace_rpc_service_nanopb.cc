// Copyright 2021 The Pigweed Authors
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
//==============================================================================

#include "pw_trace_tokenized/trace_rpc_service_nanopb.h"

#include "pw_log/log.h"
#include "pw_trace_tokenized/trace_buffer.h"
#include "pw_trace_tokenized/trace_tokenized.h"

namespace pw::trace {

TraceService::TraceService(TokenizedTracer& tokenized_tracer)
    : tokenized_tracer_(tokenized_tracer) {}

pw::Status TraceService::Enable(const pw_trace_TraceEnableMessage& request,
                                pw_trace_TraceEnableMessage& response) {
  tokenized_tracer_.Enable(request.enable);
  response.enable = tokenized_tracer_.IsEnabled();
  return PW_STATUS_OK;
}

pw::Status TraceService::IsEnabled(const pw_trace_Empty&,
                                   pw_trace_TraceEnableMessage& response) {
  response.enable = tokenized_tracer_.IsEnabled();
  return PW_STATUS_OK;
}

void TraceService::GetTraceData(
    const pw_trace_Empty&, ServerWriter<pw_trace_TraceDataMessage>& writer) {
  pw_trace_TraceDataMessage buffer = pw_trace_TraceDataMessage_init_default;
  size_t size = 0;
  pw::trace::TraceBuffer* trace_buffer = pw::trace::GetBuffer();

  while (trace_buffer->PeekFront(as_writable_bytes(span(buffer.data.bytes)),
                                 &size) != pw::Status::OutOfRange()) {
    trace_buffer->PopFront()
        .IgnoreError();  // TODO: b/242598609 - Handle Status properly
    buffer.data.size = static_cast<pb_size_t>(size);
    pw::Status status = writer.Write(buffer);
    if (!status.ok()) {
      PW_LOG_ERROR("Error sending trace; abandoning trace dump. Error: %s",
                   status.str());
      break;
    }
  }
  writer.Finish().IgnoreError();  // TODO: b/242598609 - Handle Status properly
}
}  // namespace pw::trace
