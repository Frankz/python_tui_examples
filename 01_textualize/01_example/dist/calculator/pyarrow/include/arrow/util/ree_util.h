// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>

#include "arrow/array/data.h"
#include "arrow/type_traits.h"
#include "arrow/util/macros.h"

namespace arrow {
namespace ree_util {

/// \brief Get the child array holding the run ends from an REE array
inline const ArraySpan& RunEndsArray(const ArraySpan& span) { return span.child_data[0]; }

/// \brief Get the child array holding the data values from an REE array
inline const ArraySpan& ValuesArray(const ArraySpan& span) { return span.child_data[1]; }

/// \brief Get a pointer to run ends values of an REE array
template <typename RunEndCType>
const RunEndCType* RunEnds(const ArraySpan& span) {
  assert(RunEndsArray(span).type->id() == CTypeTraits<RunEndCType>::ArrowType::type_id);
  return RunEndsArray(span).GetValues<RunEndCType>(1);
}

/// \brief Perform basic validations on the parameters of an REE array
/// and its two children arrays
///
/// All the checks complete in O(1) time. Consequently, this function:
/// - DOES NOT check that run_ends is sorted and all-positive
/// - DOES NOT check the actual contents of the run_ends and values arrays
Status ValidateRunEndEncodedChildren(const RunEndEncodedType& type,
                                     int64_t logical_length,
                                     const std::shared_ptr<ArrayData>& run_ends_data,
                                     const std::shared_ptr<ArrayData>& values_data,
                                     int64_t null_count, int64_t logical_offset);

/// \brief Compute the logical null count of an REE array
int64_t LogicalNullCount(const ArraySpan& span);

namespace internal {

/// \brief Uses binary-search to find the physical offset given a logical offset
/// and run-end values
///
/// \return the physical offset or run_ends_size if the physical offset is not
/// found in run_ends
template <typename RunEndCType>
int64_t FindPhysicalIndex(const RunEndCType* run_ends, int64_t run_ends_size, int64_t i,
                          int64_t absolute_offset) {
  assert(absolute_offset + i >= 0);
  auto it = std::upper_bound(run_ends, run_ends + run_ends_size, absolute_offset + i);
  int64_t result = std::distance(run_ends, it);
  assert(result <= run_ends_size);
  return result;
}

/// \brief Uses binary-search to calculate the number of physical values (and
/// run-ends) necessary to represent the logical range of values from
/// offset to length
template <typename RunEndCType>
int64_t FindPhysicalLength(const RunEndCType* run_ends, int64_t run_ends_size,
                           int64_t length, int64_t offset) {
  // The physical length is calculated by finding the offset of the last element
  // and adding 1 to it, so first we ensure there is at least one element.
  if (length == 0) {
    return 0;
  }
  const int64_t physical_offset =
      FindPhysicalIndex<RunEndCType>(run_ends, run_ends_size, 0, offset);
  const int64_t physical_index_of_last = FindPhysicalIndex<RunEndCType>(
      run_ends + physical_offset, run_ends_size - physical_offset, length - 1, offset);

  assert(physical_index_of_last < run_ends_size - physical_offset);
  return physical_index_of_last + 1;
}

/// \brief Find the physical index into the values array of the REE ArraySpan
///
/// This function uses binary-search, so it has a O(log N) cost.
template <typename RunEndCType>
int64_t FindPhysicalIndex(const ArraySpan& span, int64_t i, int64_t absolute_offset) {
  const int64_t run_ends_size = RunEndsArray(span).length;
  return FindPhysicalIndex(RunEnds<RunEndCType>(span), run_ends_size, i, absolute_offset);
}

/// \brief Find the physical length of an REE ArraySpan
///
/// The physical length of an REE is the number of physical values (and
/// run-ends) necessary to represent the logical range of values from
/// offset to length.
///
/// Avoid calling this function if the physical length can be estabilished in
/// some other way (e.g. when iterating over the runs sequentially until the
/// end). This function uses binary-search, so it has a O(log N) cost.
template <typename RunEndCType>
int64_t FindPhysicalLength(const ArraySpan& span) {
  return FindPhysicalLength(
      /*run_ends=*/RunEnds<RunEndCType>(span),
      /*run_ends_size=*/RunEndsArray(span).length,
      /*length=*/span.length,
      /*offset=*/span.offset);
}

}  // namespace internal

/// \brief Find the physical index into the values array of the REE ArraySpan
///
/// This function uses binary-search, so it has a O(log N) cost.
int64_t FindPhysicalIndex(const ArraySpan& span, int64_t i, int64_t absolute_offset);

/// \brief Find the physical length of an REE ArraySpan
///
/// The physical length of an REE is the number of physical values (and
/// run-ends) necessary to represent the logical range of values from
/// offset to length.
///
/// Avoid calling this function if the physical length can be estabilished in
/// some other way (e.g. when iterating over the runs sequentially until the
/// end). This function uses binary-search, so it has a O(log N) cost.
int64_t FindPhysicalLength(const ArraySpan& span);

template <typename RunEndCType>
class RunEndEncodedArraySpan {
 private:
  struct PrivateTag {};

 public:
  /// \brief Iterator representing the current run during iteration over a
  /// run-end encoded array
  class Iterator {
   public:
    Iterator(PrivateTag, const RunEndEncodedArraySpan& span, int64_t logical_pos,
             int64_t physical_pos)
        : span(span), logical_pos_(logical_pos), physical_pos_(physical_pos) {}

    /// \brief Return the physical index of the run
    ///
    /// The values array can be addressed with this index to get the value
    /// that makes up the run.
    ///
    /// NOTE: if this Iterator is equal to RunEndEncodedArraySpan::end(),
    /// the value returned is undefined.
    int64_t index_into_array() const { return physical_pos_; }

    /// \brief Return the initial logical position of the run
    ///
    /// If this Iterator is equal to RunEndEncodedArraySpan::end(), this is
    /// the same as RunEndEncodedArraySpan::length().
    int64_t logical_position() const { return logical_pos_; }

    /// \brief Return the logical position immediately after the run.
    ///
    /// Pre-condition: *this != RunEndEncodedArraySpan::end()
    int64_t run_end() const { return span.run_end(physical_pos_); }

    /// \brief Returns the logical length of the run.
    ///
    /// Pre-condition: *this != RunEndEncodedArraySpan::end()
    int64_t run_length() const { return run_end() - logical_pos_; }

    /// \brief Check if the iterator is at the end of the array.
    ///
    /// This can be used to avoid paying the cost of a call to
    /// RunEndEncodedArraySpan::end().
    ///
    /// \return true if the iterator is at the end of the array
    bool is_end(const RunEndEncodedArraySpan& span) const {
      return logical_pos_ >= span.length();
    }

    Iterator& operator++() {
      logical_pos_ = span.run_end(physical_pos_);
      physical_pos_ += 1;
      return *this;
    }

    Iterator operator++(int) {
      const Iterator prev = *this;
      ++(*this);
      return prev;
    }

    Iterator& operator--() {
      physical_pos_ -= 1;
      logical_pos_ = (physical_pos_ > 0) ? span.run_end(physical_pos_ - 1) : 0;
      return *this;
    }

    Iterator operator--(int) {
      const Iterator prev = *this;
      --(*this);
      return prev;
    }

    bool operator==(const Iterator& other) const {
      return logical_pos_ == other.logical_pos_;
    }

    bool operator!=(const Iterator& other) const {
      return logical_pos_ != other.logical_pos_;
    }

   public:
    const RunEndEncodedArraySpan& span;

   private:
    int64_t logical_pos_;
    int64_t physical_pos_;
  };

  explicit RunEndEncodedArraySpan(const ArrayData& data)
      : RunEndEncodedArraySpan(ArraySpan{data}) {}

  explicit RunEndEncodedArraySpan(const ArraySpan& array_span)
      : array_span{array_span}, run_ends_(RunEnds<RunEndCType>(array_span)) {
    assert(array_span.type->id() == Type::RUN_END_ENCODED);
  }

  int64_t length() const { return array_span.length; }
  int64_t offset() const { return array_span.offset; }

  int64_t PhysicalIndex(int64_t logical_pos) const {
    return internal::FindPhysicalIndex(run_ends_, RunEndsArray(array_span).length,
                                       logical_pos, offset());
  }

  /// \brief Create an iterator from a logical position and its
  /// pre-computed physical offset into the run ends array
  ///
  /// \param logical_pos is an index in the [0, length()] range
  /// \param physical_offset the pre-calculated PhysicalIndex(logical_pos)
  Iterator iterator(int64_t logical_pos, int64_t physical_offset) const {
    return Iterator{PrivateTag{}, *this, logical_pos, physical_offset};
  }

  /// \brief Create an iterator from a logical position
  ///
  /// \param logical_pos is an index in the [0, length()] range
  Iterator iterator(int64_t logical_pos) const {
    if (logical_pos < length()) {
      return iterator(logical_pos, PhysicalIndex(logical_pos));
    }
    // If logical_pos is above the valid range, use length() as the logical
    // position and calculate the physical address right after the last valid
    // physical position. Which is the physical index of the last logical
    // position, plus 1.
    return (length() == 0) ? iterator(0, PhysicalIndex(0))
                           : iterator(length(), PhysicalIndex(length() - 1) + 1);
  }

  /// \brief Create an iterator representing the logical begin of the run-end
  /// encoded array
  Iterator begin() const { return iterator(0, PhysicalIndex(0)); }

  /// \brief Create an iterator representing the first invalid logical position
  /// of the run-end encoded array
  ///
  /// \warning Avoid calling end() in a loop, as it will recompute the physical
  /// length of the array on each call (O(log N) cost per call).
  ///
  /// \par You can write your loops like this instead:
  /// \code
  /// for (auto it = array.begin(), end = array.end(); it != end; ++it) {
  ///   // ...
  /// }
  /// \endcode
  ///
  /// \par Or this version that does not look like idiomatic C++, but removes
  /// the need for calling end() completely:
  /// \code
  /// for (auto it = array.begin(); !it.is_end(array); ++it) {
  ///   // ...
  /// }
  /// \endcode
  Iterator end() const {
    return iterator(length(),
                    (length() == 0) ? PhysicalIndex(0) : PhysicalIndex(length() - 1) + 1);
  }

  // Pre-condition: physical_pos < RunEndsArray(array_span).length);
  inline int64_t run_end(int64_t physical_pos) const {
    assert(physical_pos < RunEndsArray(array_span).length);
    // Logical index of the end of the run at physical_pos with offset applied
    const int64_t logical_run_end =
        std::max<int64_t>(static_cast<int64_t>(run_ends_[physical_pos]) - offset(), 0);
    // The current run may go further than the logical length, cap it
    return std::min(logical_run_end, length());
  }

 public:
  const ArraySpan array_span;

 private:
  const RunEndCType* run_ends_;
};

/// \brief Iterate over two run-end encoded arrays in runs or sub-runs that are
/// inside run boundaries on both inputs
///
/// Both RunEndEncodedArraySpan should have the same logical length. Instances
/// of this iterator only hold references to the RunEndEncodedArraySpan inputs.
template <typename Left, typename Right>
class MergedRunsIterator {
 private:
  using LeftIterator = typename Left::Iterator;
  using RightIterator = typename Right::Iterator;

  MergedRunsIterator(LeftIterator left_it, RightIterator right_it,
                     int64_t common_logical_length, int64_t common_logical_pos)
      : ree_iterators_{std::move(left_it), std::move(right_it)},
        logical_length_(common_logical_length),
        logical_pos_(common_logical_pos) {}

 public:
  /// \brief Construct a MergedRunsIterator positioned at logical position 0.
  ///
  /// Pre-condition: left.length() == right.length()
  MergedRunsIterator(const Left& left, const Right& right)
      : MergedRunsIterator(left.begin(), right.begin(), left.length(), 0) {
    assert(left.length() == right.length());
  }

  static Result<MergedRunsIterator> MakeBegin(const Left& left, const Right& right) {
    if (left.length() != right.length()) {
      return Status::Invalid(
          "MergedRunsIterator expects RunEndEncodedArraySpans of the same length");
    }
    return MergedRunsIterator(left, right);
  }

  static Result<MergedRunsIterator> MakeEnd(const Left& left, const Right& right) {
    if (left.length() != right.length()) {
      return Status::Invalid(
          "MergedRunsIterator expects RunEndEncodedArraySpans of the same length");
    }
    return MergedRunsIterator(left.end(), right.end(), left.length(), left.length());
  }

  /// \brief Return the left RunEndEncodedArraySpan child
  const Left& left() const { return std::get<0>(ree_iterators_).span; }

  /// \brief Return the right RunEndEncodedArraySpan child
  const Right& right() const { return std::get<1>(ree_iterators_).span; }

  /// \brief Return the initial logical position of the run
  ///
  /// If is_end(), this is the same as length().
  int64_t logical_position() const { return logical_pos_; }

  /// \brief Whether the iterator is at logical position 0.
  bool is_begin() const { return logical_pos_ == 0; }

  /// \brief Whether the iterator has reached the end of both arrays
  bool is_end() const { return logical_pos_ == logical_length_; }

  /// \brief Return the logical position immediately after the run.
  ///
  /// Pre-condition: !is_end()
  int64_t run_end() const {
    const auto& left_it = std::get<0>(ree_iterators_);
    const auto& right_it = std::get<1>(ree_iterators_);
    return std::min(left_it.run_end(), right_it.run_end());
  }

  /// \brief returns the logical length of the current run
  ///
  /// Pre-condition: !is_end()
  int64_t run_length() const { return run_end() - logical_pos_; }

  /// \brief Return a physical index into the values array of a given input,
  /// pointing to the value of the current run
  template <size_t input_id>
  int64_t index_into_array() const {
    return std::get<input_id>(ree_iterators_).index_into_array();
  }

  int64_t index_into_left_array() const { return index_into_array<0>(); }
  int64_t index_into_right_array() const { return index_into_array<1>(); }

  MergedRunsIterator& operator++() {
    auto& left_it = std::get<0>(ree_iterators_);
    auto& right_it = std::get<1>(ree_iterators_);

    const int64_t left_run_end = left_it.run_end();
    const int64_t right_run_end = right_it.run_end();

    if (left_run_end < right_run_end) {
      logical_pos_ = left_run_end;
      ++left_it;
    } else if (left_run_end > right_run_end) {
      logical_pos_ = right_run_end;
      ++right_it;
    } else {
      logical_pos_ = left_run_end;
      ++left_it;
      ++right_it;
    }
    return *this;
  }

  MergedRunsIterator operator++(int) {
    MergedRunsIterator prev = *this;
    ++(*this);
    return prev;
  }

  MergedRunsIterator& operator--() {
    auto& left_it = std::get<0>(ree_iterators_);
    auto& right_it = std::get<1>(ree_iterators_);

    // The logical position of each iterator is the run_end() of the previous run.
    const int64_t left_logical_pos = left_it.logical_position();
    const int64_t right_logical_pos = right_it.logical_position();

    if (left_logical_pos < right_logical_pos) {
      --right_it;
      logical_pos_ = std::max(left_logical_pos, right_it.logical_position());
    } else if (left_logical_pos > right_logical_pos) {
      --left_it;
      logical_pos_ = std::max(left_it.logical_position(), right_logical_pos);
    } else {
      --left_it;
      --right_it;
      logical_pos_ = std::max(left_it.logical_position(), right_it.logical_position());
    }
    return *this;
  }

  MergedRunsIterator operator--(int) {
    MergedRunsIterator prev = *this;
    --(*this);
    return prev;
  }

  bool operator==(const MergedRunsIterator& other) const {
    return logical_pos_ == other.logical_position();
  }

  bool operator!=(const MergedRunsIterator& other) const { return !(*this == other); }

 private:
  std::tuple<LeftIterator, RightIterator> ree_iterators_;
  const int64_t logical_length_;
  int64_t logical_pos_;
};

}  // namespace ree_util
}  // namespace arrow
