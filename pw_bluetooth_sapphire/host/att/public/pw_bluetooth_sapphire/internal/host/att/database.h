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

#pragma once
#include <list>
#include <memory>

#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/att/attribute.h"
#include "pw_bluetooth_sapphire/internal/host/att/write_queue.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace bt::att {

// This class provides a simple attribute database abstraction. Attributes can
// be populated directly and queried to fulfill ATT protocol requests.
//
// Many Database instances can be created as long as care is taken that the
// referenced handle ranges are distinct. While this class is primarily intended
// to be used as a local ATT server database, it could also be used to represent
// a remote attribute cache.
class Database final : public WeakSelf<Database> {
  using GroupingList = std::list<AttributeGrouping>;

 public:
  // This type allows iteration over the attributes in a database. An iterator
  // is always initialzed with a handle range and options to skip attributes or
  // groupings based on attribute type. An iterator always skips
  // inactive/incomplete groupings.
  //
  // Modifying a database invalidates its iterators.
  class Iterator final {
   public:
    // Returns the current attribute. Returns nullptr if the end of the handle
    // range has been reached.
    const Attribute* get() const;

    // Advances the iterator forward. Skips over non-matching attributes if a
    // type filter has been set. Has no effect if the end of the range was
    // reached.
    void Advance();

    // If set, |next()| will only return attributes with the given |type|. No
    // filter is set by default.
    void set_type_filter(const UUID& type) { type_filter_ = type; }

    // Returns true if the iterator cannot be advanced any further.
    inline bool AtEnd() const { return grp_iter_ == grp_end_; }

   private:
    inline void MarkEnd() { grp_iter_ = grp_end_; }

    friend class Database;
    Iterator(GroupingList* list,
             Handle start,
             Handle end,
             const UUID* type,
             bool groups_only);

    Handle start_;
    Handle end_;
    bool grp_only_;
    GroupingList::iterator grp_end_;
    GroupingList::iterator grp_iter_;
    uint16_t attr_offset_;
    static_assert(std::numeric_limits<decltype(attr_offset_)>::max() >=
                      kHandleMax,
                  "attr_offset_ must be able to fit kMaxHandle!");
    std::optional<UUID> type_filter_;
  };

  // Initializes this database to span the attribute handle range given by
  // |range_start| and |range_end|. This allows the upper layer to segment the
  // handle range into multiple contiguous regions by instantiating multiple
  // Database objects.
  //
  // Note: This is to make it easy for the GATT layer to group service
  // declarations with 16-bit UUIDs and 128-bit UUIDs separately as recommended
  // by the GATT specification (see Vol 3, Part G, 3.1).
  explicit Database(Handle range_start = kHandleMin,
                    Handle range_end = kHandleMax);
  ~Database() = default;

  // Returns an iterator that covers the handle range defined by |start| and
  // |end| (inclusive). If |groups_only| is true, then the returned iterator
  // will only return group declaration attributes (this allows quicker
  // iteration over groupings while handling the ATT Read By Group Type
  // request).
  //
  // If |type| is not a nullptr, it will be assigned as the iterator's type
  // filter.
  Iterator GetIterator(Handle start,
                       Handle end,
                       const UUID* type = nullptr,
                       bool groups_only = false);

  // Creates a new attribute grouping with the given |type|. The grouping will
  // be initialized to contain |attr_count| attributes (excluding the
  // group declaration attribute) and |value| will be assigned as the group
  // declaration attribute value.
  //
  // Returns a pointer to the new grouping, which can be used to populate
  // attributes. Returns nullptr if the requested grouping could not be
  // created due to insufficient handles.
  //
  // The returned pointer is owned and managed by this Database and should not
  // be retained by the caller. Removing the grouping will invalidate the
  // returned pointer.
  AttributeGrouping* NewGrouping(const UUID& group_type,
                                 size_t attr_count,
                                 const ByteBuffer& decl_value);

  // Removes the attribute grouping that has the given starting handle. Returns
  // false if no such grouping was found.
  bool RemoveGrouping(Handle start_handle);

  const std::list<AttributeGrouping>& groupings() const { return groupings_; }

  // Finds and returns the attribute with the given handle. Returns nullptr if
  // the attribute cannot be found or is part of a grouping that is inactive
  // or incomplete.
  const Attribute* FindAttribute(Handle handle);

  // Applies all write requests in |write_queue| and reports the result in
  // |callback|. All requests will be delivered to the attribute write handlers
  // at once, in order, without waiting for a response on individual writes.
  //
  // If one or more requests result in an application protocol error,
  // |callback| will be invoked with the value of the first error received and
  // all subsequent results will be ignored. Otherwise, |callback| will be
  // called with a success status once a reply has been received for all
  // requests in the queue.
  //
  // This function performs any security checks even though these are expected
  // to be done during the "prepare" phase. This is because the attribute
  // database can change from the time writes are queued until they are
  // committed. |security| should match the security properties of the link
  // under which the execute write request was initiatied.
  //
  // Attribute value validation (such as offset and length checks) are expected
  // to be done by the application that has set up the attribute. This function
  // does check for the validity of a given attribute handle, whether the
  // attribute supports writes, and that each write fits within
  // kMaxAttributeValueLength.
  using WriteQueueResult = fit::result<std::tuple<Handle, ErrorCode>>;
  using WriteCallback = fit::callback<void(WriteQueueResult)>;
  void ExecuteWriteQueue(PeerId peer_id,
                         PrepareWriteQueue write_queue,
                         const sm::SecurityProperties& security,
                         WriteCallback callback);

 private:
  Handle range_start_;
  Handle range_end_;

  // The list of groupings is sorted by handle where each grouping maps to a
  // non-overlapping handle range. Successive groupings don't necessarily
  // represent contiguous handle ranges as any grouping can be removed.
  GroupingList groupings_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Database);
};

}  // namespace bt::att
