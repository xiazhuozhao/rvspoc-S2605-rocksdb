// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include <cstdint>

#include <jni.h>

#include "include/org_rocksdb_RemoveConsumeQueueCompactionFilter.h"
#include "rocksdb/compaction_filter.h"
#include "rocksjni/cplusplus_to_java_convert.h"

namespace {

class RemoveConsumeQueueCompactionFilter
    : public ROCKSDB_NAMESPACE::CompactionFilter {
 public:
  explicit RemoveConsumeQueueCompactionFilter(int64_t min_physical_offset)
      : min_physical_offset_(min_physical_offset) {}

  bool Filter(int /*level*/, const ROCKSDB_NAMESPACE::Slice& /*key*/,
              const ROCKSDB_NAMESPACE::Slice& existing_value,
              std::string* /*new_value*/,
              bool* /*value_changed*/) const override {
    if (existing_value.size() < sizeof(uint64_t)) {
      return false;
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(existing_value.data());
    uint64_t physical_offset = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
      physical_offset = (physical_offset << 8) | bytes[i];
    }
    return static_cast<int64_t>(physical_offset) < min_physical_offset_;
  }

  const char* Name() const override {
    return "RemoveConsumeQueueCompactionFilter";
  }

 private:
  int64_t min_physical_offset_;
};

}  // namespace

jlong Java_org_rocksdb_RemoveConsumeQueueCompactionFilter_createNewRemoveConsumeQueueCompactionFilter0(
    JNIEnv* /*env*/, jclass /*jcls*/, jlong min_physical_offset) {
  auto* compaction_filter = new RemoveConsumeQueueCompactionFilter(
      static_cast<int64_t>(min_physical_offset));
  return GET_CPLUSPLUS_POINTER(compaction_filter);
}
