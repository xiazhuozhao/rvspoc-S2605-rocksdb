// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Slice is a simple structure containing a pointer into some external
// storage and a size.  The user of a Slice must ensure that the slice
// is not used after the corresponding external storage has been
// deallocated.
//
// Multiple threads can invoke const methods on a Slice without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same Slice must use
// external synchronization.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#if defined(__riscv_vector)
#include <riscv_vector.h>
#endif

#include "rocksdb/cleanable.h"

namespace ROCKSDB_NAMESPACE {

#if defined(__riscv)
namespace slice_riscv_detail {

// Find the first different byte without calling the generic libc memcmp.
// RVV naturally adapts to VLEN through vsetvl; a word-at-a-time fallback
// keeps short keys cheap and also supports scalar RISC-V builds.
inline size_t DifferenceOffset(const char* a, const char* b, size_t len) {
  // Comparator inputs commonly differ in their first byte. Avoid setting up
  // either word or vector operations for that latency-sensitive case.
  if (len == 0 || a[0] != b[0]) {
    return 0;
  }
#if defined(__riscv_vector)
  if (len >= 32) {
    size_t off = 0;
    while (off < len) {
      const size_t vl = __riscv_vsetvl_e8m1(len - off);
      const vuint8m1_t av = __riscv_vle8_v_u8m1(
          reinterpret_cast<const uint8_t*>(a + off), vl);
      const vuint8m1_t bv = __riscv_vle8_v_u8m1(
          reinterpret_cast<const uint8_t*>(b + off), vl);
      const vbool8_t different = __riscv_vmsne_vv_u8m1_b8(av, bv, vl);
      const long first = __riscv_vfirst_m_b8(different, vl);
      if (first >= 0) {
        return off + static_cast<size_t>(first);
      }
      off += vl;
    }
    return off;
  }
  size_t off = 0;
#else
  // On scalar RISC-V the existing offset-one loop gives the compiler a
  // better short-key schedule. Keep it as the fallback rather than forcing
  // the RVV-tuned aligned-word layout on non-vector targets.
  size_t off = 1;
#endif
  while (off + sizeof(uint64_t) <= len) {
    uint64_t av;
    uint64_t bv;
    memcpy(&av, a + off, sizeof(av));
    memcpy(&bv, b + off, sizeof(bv));
    const uint64_t different = av ^ bv;
    if (different != 0) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      return off + static_cast<size_t>(__builtin_ctzll(different) >> 3);
#else
      return off + static_cast<size_t>(__builtin_clzll(different) >> 3);
#endif
    }
    off += sizeof(uint64_t);
  }
  while (off < len && a[off] == b[off]) {
    ++off;
  }
  return off;
}

}  // namespace slice_riscv_detail
#endif

class Slice {
 public:
  // Create an empty slice.
  Slice() : data_(""), size_(0) {}

  // Create a slice that refers to d[0,n-1].
  Slice(const char* d, size_t n) : data_(d), size_(n) {}

  // Create a slice that refers to the contents of "s"
  /* implicit */
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}

  // Create a slice that refers to the same contents as "sv"
  /* implicit */
  Slice(const std::string_view& sv) : data_(sv.data()), size_(sv.size()) {}

  // Create a slice that refers to s[0,strlen(s)-1]
  /* implicit */
  Slice(const char* s) : data_(s) { size_ = (s == nullptr) ? 0 : strlen(s); }

  // Create a single slice from SliceParts using buf as storage.
  // buf must exist as long as the returned Slice exists.
  Slice(const struct SliceParts& parts, std::string* buf);

  // Return a pointer to the beginning of the referenced data
  const char* data() const { return data_; }

  // Return the length (in bytes) of the referenced data
  size_t size() const { return size_; }

  // Return true iff the length of the referenced data is zero
  bool empty() const { return size_ == 0; }

  // Return the ith byte in the referenced data.
  // REQUIRES: n < size()
  char operator[](size_t n) const {
    assert(n < size());
    return data_[n];
  }

  // Change this slice to refer to an empty array
  void clear() {
    data_ = "";
    size_ = 0;
  }

  // Drop the first "n" bytes from this slice.
  void remove_prefix(size_t n) {
    assert(n <= size());
    data_ += n;
    size_ -= n;
  }

  void remove_suffix(size_t n) {
    assert(n <= size());
    size_ -= n;
  }

  // Return a string that contains the copy of the referenced data.
  // when hex is true, returns a string of twice the length hex encoded (0-9A-F)
  std::string ToString(bool hex = false) const;

  // Return a string_view that references the same data as this slice.
  std::string_view ToStringView() const {
    return std::string_view(data_, size_);
  }

  // Decodes the current slice interpreted as an hexadecimal string into result,
  // if successful returns true, if this isn't a valid hex string
  // (e.g not coming from Slice::ToString(true)) DecodeHex returns false.
  // This slice is expected to have an even number of 0-9A-F characters
  // also accepts lowercase (a-f)
  bool DecodeHex(std::string* result) const;

  // Three-way comparison.  Returns value:
  //   <  0 iff "*this" <  "b",
  //   == 0 iff "*this" == "b",
  //   >  0 iff "*this" >  "b"
  int compare(const Slice& b) const;

  // Return true iff "x" is a prefix of "*this"
  bool starts_with(const Slice& x) const {
    return ((size_ >= x.size_) && (memcmp(data_, x.data_, x.size_) == 0));
  }

  bool ends_with(const Slice& x) const {
    return ((size_ >= x.size_) &&
            (memcmp(data_ + size_ - x.size_, x.data_, x.size_) == 0));
  }

  // Compare two slices and returns the first byte where they differ
  size_t difference_offset(const Slice& b) const;

  // private: make these public for rocksdbjni access
  const char* data_;
  size_t size_;

  // Intentionally copyable
};

// A likely more efficient alternative to std::optional<Slice>. For example,
// an empty key might be distinct from "not specified" (and Slice* as an
// optional is more troublesome to deal with).
class OptSlice {
 public:
  OptSlice() : slice_(nullptr, SIZE_MAX) {}
  /*implicit*/ OptSlice(const Slice& s) : slice_(s) {}
  /*implicit*/ OptSlice(const std::string& s) : slice_(s) {}
  /*implicit*/ OptSlice(const std::string_view& sv) : slice_(sv) {}
  /*implicit*/ OptSlice(const char* c_str) : slice_(c_str) {}
  // For easier migrating from APIs uing Slice* as an optional type.
  // CAUTION: OptSlice{nullptr} is "no value" while Slice{nullptr} is "empty"
  /*implicit*/ OptSlice(std::nullptr_t) : OptSlice() {}

  bool has_value() const noexcept { return slice_.size() != SIZE_MAX; }
  explicit operator bool() const noexcept { return has_value(); }

  const Slice& value() const noexcept {
    assert(has_value());
    return slice_;
  }
  const Slice& operator*() const noexcept { return value(); }
  const Slice* operator->() const noexcept { return &value(); }

  const Slice* AsPtr() const noexcept {
    return has_value() ? &slice_ : nullptr;
  }
  // Populate from an optional pointer. This is a very explicit conversion
  // to minimize risk of bugs as in
  //   Slice start, limit;
  //   RangeOpt rng = {&start, &limit};
  //   start = ...;  // BUG: would not affect rng
  static OptSlice CopyFromPtr(const Slice* ptr) {
    return ptr ? OptSlice{*ptr} : OptSlice{};
  }

 protected:
  Slice slice_;
};

/**
 * A Slice that can be pinned with some cleanup tasks, which will be run upon
 * ::Reset() or object destruction, whichever is invoked first. This can be used
 * to avoid memcpy by having the PinnableSlice object referring to the data
 * that is locked in the memory and release them after the data is consumed.
 */
class PinnableSlice : public Slice, public Cleanable {
 public:
  PinnableSlice() { buf_ = &self_space_; }
  explicit PinnableSlice(std::string* buf) { buf_ = buf; }

  PinnableSlice(PinnableSlice&& other);
  PinnableSlice& operator=(PinnableSlice&& other);

  // No copy constructor and copy assignment allowed.
  PinnableSlice(PinnableSlice&) = delete;
  PinnableSlice& operator=(PinnableSlice&) = delete;

  inline void PinSlice(const Slice& s, CleanupFunction f, void* arg1,
                       void* arg2) {
    assert(!pinned_);
    pinned_ = true;
    data_ = s.data();
    size_ = s.size();
    RegisterCleanup(f, arg1, arg2);
    assert(pinned_);
  }

  inline void PinSlice(const Slice& s, Cleanable* cleanable) {
    assert(!pinned_);
    pinned_ = true;
    data_ = s.data();
    size_ = s.size();
    if (cleanable != nullptr) {
      cleanable->DelegateCleanupsTo(this);
    }
    assert(pinned_);
  }

  inline void PinSelf(const Slice& slice) {
    assert(!pinned_);
    buf_->assign(slice.data(), slice.size());
    data_ = buf_->data();
    size_ = buf_->size();
    assert(!pinned_);
  }

  inline void PinSelf() {
    assert(!pinned_);
    data_ = buf_->data();
    size_ = buf_->size();
    assert(!pinned_);
  }

  void remove_suffix(size_t n) {
    assert(n <= size());
    if (pinned_) {
      size_ -= n;
    } else {
      buf_->erase(size() - n, n);
      PinSelf();
    }
  }

  void remove_prefix(size_t n) {
    assert(n <= size());
    if (pinned_) {
      data_ += n;
      size_ -= n;
    } else {
      buf_->erase(0, n);
      PinSelf();
    }
  }

  void Reset() {
    Cleanable::Reset();
    pinned_ = false;
    size_ = 0;
  }

  inline std::string* GetSelf() { return buf_; }

  inline bool IsPinned() const { return pinned_; }

 private:
  friend class PinnableSlice4Test;
  std::string self_space_;
  std::string* buf_;
  bool pinned_ = false;
};

// A set of Slices that are virtually concatenated together.  'parts' points
// to an array of Slices.  The number of elements in the array is 'num_parts'.
struct SliceParts {
  SliceParts(const Slice* _parts, int _num_parts)
      : parts(_parts), num_parts(_num_parts) {}
  SliceParts() : parts(nullptr), num_parts(0) {}

  const Slice* parts;
  int num_parts;
};

inline bool operator==(const Slice& x, const Slice& y) {
#if defined(__riscv_vector)
  return x.size() == y.size() &&
         slice_riscv_detail::DifferenceOffset(x.data(), y.data(), x.size()) ==
             x.size();
#else
  return ((x.size() == y.size()) &&
          (memcmp(x.data(), y.data(), x.size()) == 0));
#endif
}

inline bool operator!=(const Slice& x, const Slice& y) { return !(x == y); }

inline int Slice::compare(const Slice& b) const {
  assert(data_ != nullptr && b.data_ != nullptr);
  const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
#if defined(__riscv)
  const size_t diff = slice_riscv_detail::DifferenceOffset(data_, b.data_,
                                                           min_len);
  int r = diff == min_len
              ? 0
              : static_cast<int>(static_cast<unsigned char>(data_[diff])) -
                    static_cast<int>(static_cast<unsigned char>(b.data_[diff]));
#else
  int r = memcmp(data_, b.data_, min_len);
#endif
  if (r == 0) {
    if (size_ < b.size_)
      r = -1;
    else if (size_ > b.size_)
      r = +1;
  }
  return r;
}

inline size_t Slice::difference_offset(const Slice& b) const {
  const size_t len = (size_ < b.size_) ? size_ : b.size_;
#if defined(__riscv)
  return slice_riscv_detail::DifferenceOffset(data_, b.data_, len);
#else
  size_t off = 0;
  for (; off < len; off++) {
    if (data_[off] != b.data_[off]) break;
  }
  return off;
#endif
}

}  // namespace ROCKSDB_NAMESPACE
