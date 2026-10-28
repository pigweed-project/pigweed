// Copyright 2023 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/att/database.h"

#include <lib/fit/defer.h>
#include <pw_assert/check.h>

#include <algorithm>

#include "pw_bluetooth_sapphire/internal/host/att/error.h"
#include "pw_bluetooth_sapphire/internal/host/att/permissions.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"

namespace bt::att {
namespace {

bool StartLessThan(const AttributeGrouping& grp, const Handle handle) {
  return grp.start_handle() < handle;
}

bool EndLessThan(const AttributeGrouping& grp, const Handle handle) {
  return grp.end_handle() < handle;
}

}  // namespace

Database::Iterator::Iterator(GroupingList* list,
                             Handle start,
                             Handle end,
                             const UUID* type,
                             bool groups_only)
    : start_(start), end_(end), grp_only_(groups_only), attr_offset_(0u) {
  PW_DCHECK(list);
  grp_end_ = list->end();

  if (type)
    type_filter_ = *type;

  // Initialize the iterator by performing a binary search over the attributes.
  // If we were asked to iterate over groupings only, then look strictly within
  // the range. Otherwise we allow the first grouping to partially overlap the
  // range.
  grp_iter_ = std::lower_bound(
      list->begin(), grp_end_, start_, grp_only_ ? StartLessThan : EndLessThan);

  if (AtEnd())
    return;

  // If the first grouping is out of range then the iterator is done.
  if (grp_iter_->start_handle() > end) {
    MarkEnd();
    return;
  }

  if (start_ > grp_iter_->start_handle()) {
    attr_offset_ = start_ - grp_iter_->start_handle();
  }

  // If the first is inactive or if it doesn't match the current filter then
  // skip ahead.
  if (!grp_iter_->active() ||
      (type_filter_ &&
       grp_iter_->attributes()[attr_offset_].type() != *type_filter_)) {
    Advance();
  }
}

const Attribute* Database::Iterator::get() const {
  if (AtEnd() || !grp_iter_->active())
    return nullptr;

  PW_DCHECK(attr_offset_ < grp_iter_->attributes().size());
  return &grp_iter_->attributes()[attr_offset_];
}

void Database::Iterator::Advance() {
  if (AtEnd())
    return;

  do {
    if (!grp_only_ && grp_iter_->active()) {
      // If this grouping has more attributes to look at.
      if (attr_offset_ < grp_iter_->attributes().size() - 1) {
        size_t end_offset = grp_iter_->end_handle() - grp_iter_->start_handle();
        PW_DCHECK(end_offset < grp_iter_->attributes().size());

        // Advance.
        attr_offset_++;

        for (; attr_offset_ <= end_offset; ++attr_offset_) {
          const auto& attr = grp_iter_->attributes()[attr_offset_];

          // If |end_| is within this grouping and we go past it, the iterator
          // is done.
          if (attr.handle() > end_) {
            MarkEnd();
            return;
          }

          // If there is no filter then we're done. Otherwise, loop until an
          // attribute is found that matches the filter.
          if (!type_filter_ || attr.type() == *type_filter_)
            return;
        }
      }

      // We are done with the current grouping. Fall through and move to the
      // next group below.
      attr_offset_ = 0u;
    } else {
      PW_DCHECK(attr_offset_ == 0u);
    }

    // Advance the group.
    grp_iter_++;
    if (AtEnd())
      return;

    if (grp_iter_->start_handle() > end_) {
      MarkEnd();
      return;
    }

    if (!grp_iter_->active() || !grp_iter_->complete())
      continue;

    // If there is no filter then we're done. Otherwise, loop until an
    // attribute is found that matches the filter. (NOTE: the group type is the
    // type of the first attribute).
    if (!type_filter_ || (*type_filter_ == grp_iter_->group_type()))
      return;
  } while (true);
}

Database::Database(Handle range_start, Handle range_end)
    : WeakSelf(this), range_start_(range_start), range_end_(range_end) {
  PW_DCHECK(range_start_ < range_end_);
  PW_DCHECK(range_start_ >= kHandleMin);
  PW_DCHECK(range_end_ <= kHandleMax);
}

Database::Iterator Database::GetIterator(Handle start,
                                         Handle end,
                                         const UUID* type,
                                         bool groups_only) {
  PW_DCHECK(start >= range_start_);
  PW_DCHECK(end <= range_end_);
  PW_DCHECK(start <= end);

  return Iterator(&groupings_, start, end, type, groups_only);
}

AttributeGrouping* Database::NewGrouping(const UUID& group_type,
                                         size_t attr_count,
                                         const ByteBuffer& decl_value) {
  // This method looks for a |pos| before which to insert the new grouping.
  Handle start_handle;
  decltype(groupings_)::iterator pos;

  if (groupings_.empty()) {
    if (range_end_ - range_start_ < attr_count)
      return nullptr;

    start_handle = range_start_;
    pos = groupings_.end();
  } else if (groupings_.front().start_handle() - range_start_ > attr_count) {
    // There is room at the head of the list.
    start_handle = range_start_;
    pos = groupings_.begin();
  } else if (range_end_ - groupings_.back().end_handle() > attr_count) {
    // There is room at the tail end of the list.
    start_handle = groupings_.back().end_handle() + 1;
    pos = groupings_.end();
  } else {
    // Linearly search for a gap that fits the new grouping.
    // TODO(armansito): This is suboptimal for long running cases where the
    // database is fragmented. Think about using a better algorithm.

    auto prev = groupings_.begin();
    pos = prev;
    pos++;

    for (; pos != groupings_.end(); ++pos, ++prev) {
      size_t next_avail = pos->start_handle() - prev->end_handle() - 1;
      if (attr_count < next_avail)
        break;
    }

    if (pos == groupings_.end()) {
      bt_log(DEBUG, "att", "attribute database is out of space!");
      return nullptr;
    }

    start_handle = prev->end_handle() + 1;
  }

  auto iter =
      groupings_.emplace(pos, group_type, start_handle, attr_count, decl_value);
  PW_DCHECK(iter != groupings_.end());

  return &*iter;
}

bool Database::RemoveGrouping(Handle start_handle) {
  auto iter = std::lower_bound(
      groupings_.begin(), groupings_.end(), start_handle, StartLessThan);

  if (iter == groupings_.end() || iter->start_handle() != start_handle)
    return false;

  groupings_.erase(iter);
  return true;
}

const Attribute* Database::FindAttribute(Handle handle) {
  if (handle == kInvalidHandle)
    return nullptr;

  // Do a binary search to find the grouping that this handle is in.
  auto iter = std::lower_bound(
      groupings_.begin(), groupings_.end(), handle, EndLessThan);
  if (iter == groupings_.end() || iter->start_handle() > handle)
    return nullptr;

  if (!iter->active() || !iter->complete())
    return nullptr;

  size_t index = handle - iter->start_handle();
  PW_DCHECK(index < iter->attributes().size());

  return &iter->attributes()[index];
}

void Database::ExecuteWriteQueue(PeerId peer_id,
                                 PrepareWriteQueue write_queue,
                                 const sm::SecurityProperties& security,
                                 WriteCallback callback) {
  PW_CHECK(callback);

  // When destroyed, invokes |callback| with success if it hasn't already been
  // called
  auto deferred_succcess = fit::defer([client_cb = callback.share()]() mutable {
    if (client_cb) {
      client_cb(fit::ok());
    }
  });

  // Signal success without writing to any attributes if the queue is empty (see
  // Core Spec v5.3, Vol 3, Part F, 3.4.6.3).
  if (write_queue.empty()) {
    return;
  }

  // Continuation that keeps track of all outstanding write requests. This is
  // shared between writes in the queue, causing the captured |deferred_success|
  // to be destroyed only after all writes have completed. |callback| may be
  // called earlier (and consumed) if any error is received.
  fit::function<void(WriteQueueResult)> write_complete_fn =
      [client_cb = std::move(callback),
       d = std::move(deferred_succcess)](WriteQueueResult result) mutable {
        if (result.is_ok()) {
          return;
        }
        const auto& [handle, error] = result.error_value();
        bt_log(DEBUG,
               "att",
               "execute write result - handle: %#.4x, error: %s",
               handle,
               bt_str(Error(error)));
        if (!client_cb) {
          bt_log(
              TRACE, "att", "ignore execute write result - already responded");
          return;
        }
        client_cb(result);
      };

  while (!write_queue.empty()) {
    auto next = std::move(write_queue.front());
    write_queue.pop();

    auto attr_write_cb = [handle = next.handle(),
                          write_complete_function = write_complete_fn.share()](
                             fit::result<ErrorCode> status) {
      if (status.is_error()) {
        write_complete_function(
            fit::error(std::tuple(handle, status.error_value())));
      } else {
        bt_log(DEBUG, "att", "execute write to handle %#.4x - success", handle);
        write_complete_function(fit::ok());
      }
    };

    const auto* attr = FindAttribute(next.handle());
    if (!attr) {
      // The attribute is no longer valid, so we can respond with an error and
      // abort the rest of the queue.
      attr_write_cb(fit::error(ErrorCode::kInvalidHandle));
      break;
    }

    const size_t offset = next.offset();
    if (offset > kMaxAttributeValueLength) {
      bt_log(DEBUG,
             "att",
             "execute write to handle %#.4x - invalid offset: %zu",
             next.handle(),
             offset);
      attr_write_cb(fit::error(ErrorCode::kInvalidOffset));
      break;
    }

    if (offset + next.value().size() > kMaxAttributeValueLength) {
      attr_write_cb(fit::error(ErrorCode::kInvalidAttributeValueLength));
      break;
    }

    fit::result<ErrorCode> status =
        CheckWritePermissions(attr->write_reqs(), security);
    if (status.is_error()) {
      attr_write_cb(status);
      break;
    }

    // TODO(fxbug.dev/42179688): Consider removing the boolean return value in
    // favor of always reporting errors using the callback. That would simplify
    // the pattern here.
    if (!attr->WriteAsync(
            peer_id, next.offset(), next.value(), std::move(attr_write_cb))) {
      write_complete_fn(
          fit::error(std::tuple(next.handle(), ErrorCode::kWriteNotPermitted)));
      break;
    }
  }
}

}  // namespace bt::att
