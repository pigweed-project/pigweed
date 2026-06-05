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
#pragma once

#include <cstddef>
#include <cstdint>

#include "pw_kvs/flash_memory.h"

namespace pw::kvs {

// FlashPartition that supports combining multiple FlashMemory sectors in to a
// single logical FlashPartition sector. The number of FlashMemory sectors per
// logical sector is specified by flash_sectors_per_logical_sector.
//
// If the number of FlashMemory sectors is not a multiple of
// flash_sectors_per_logical_sector, then the FlashMemory sectors used in the
// partition is rounded down to the nearest multiple.
class FlashPartitionWithLogicalSectors : public FlashPartition {
 public:
  FlashPartitionWithLogicalSectors(
      FlashMemory* flash,
      size_t flash_sectors_per_logical_sector,
      uint32_t flash_start_sector_index,
      uint32_t flash_sector_count,
      uint32_t alignment_bytes = 0,  // Defaults to flash alignment
      PartitionPermission permission = PartitionPermission::kReadAndWrite)
      : FlashPartition(flash,
                       flash_start_sector_index,
                       flash_sector_count,
                       alignment_bytes,
                       permission),
        flash_sectors_per_logical_sector_(flash_sectors_per_logical_sector) {}

  FlashPartitionWithLogicalSectors(FlashMemory* flash,
                                   size_t flash_sectors_per_logical_sector)
      : FlashPartitionWithLogicalSectors(
            flash,
            flash_sectors_per_logical_sector,
            0,
            static_cast<uint32_t>(flash->sector_count()),
            static_cast<uint32_t>(flash->alignment_bytes())) {}

  size_t sector_size_bytes() const override {
    return flash_.sector_size_bytes() * flash_sectors_per_logical_sector_;
  }

  size_t sector_count() const override {
    return flash_sector_count_ / flash_sectors_per_logical_sector_;
  }

  Status Erase(Address address, size_t num_sectors) override {
    return flash_.Erase(address,
                        num_sectors * flash_sectors_per_logical_sector_);
  }

 private:
  size_t flash_sectors_per_logical_sector_;
};

}  // namespace pw::kvs
