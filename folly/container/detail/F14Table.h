/*
 * Copyright 2017-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <array>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/Bits.h>
#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/ScopeGuard.h>
#include <folly/Traits.h>
#include <folly/functional/ApplyTuple.h>
#include <folly/functional/Invoke.h>
#include <folly/lang/Align.h>
#include <folly/lang/Assume.h>
#include <folly/lang/Exception.h>
#include <folly/lang/Launder.h>
#include <folly/lang/SafeAssert.h>
#include <folly/portability/Builtins.h>

#include <folly/container/detail/F14Defaults.h>
#include <folly/container/detail/F14IntrinsicsAvailability.h>
#include <folly/container/detail/F14Memory.h>

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
#if FOLLY_AARCH64
#if __ARM_FEATURE_CRC32
#include <arm_acle.h> // __crc32cd
#endif
#include <arm_neon.h> // uint8x16t intrinsics
#else // SSE2
#include <immintrin.h> // __m128i intrinsics
#include <nmmintrin.h> // _mm_crc32_u64
#include <xmmintrin.h> // _mm_prefetch
#endif
#endif

#ifdef _WIN32
#include <intrin.h> // for _mul128
#endif

namespace folly {
struct F14TableStats {
  char const* policy;
  std::size_t size{0};
  std::size_t valueSize{0};
  std::size_t bucketCount{0};
  std::size_t chunkCount{0};
  std::vector<std::size_t> chunkOccupancyHisto;
  std::vector<std::size_t> chunkOutboundOverflowHisto;
  std::vector<std::size_t> chunkHostedOverflowHisto;
  std::vector<std::size_t> keyProbeLengthHisto;
  std::vector<std::size_t> missProbeLengthHisto;
  std::size_t totalBytes{0};
  std::size_t overheadBytes{0};

 private:
  template <typename T>
  static auto computeHelper(T const* m) -> decltype(m->computeStats()) {
    return m->computeStats();
  }

  static F14TableStats computeHelper(...) {
    return {};
  }

 public:
  template <typename T>
  static F14TableStats compute(T const& m) {
    return computeHelper(&m);
  }
};

namespace f14 {
namespace detail {

#if defined(_LIBCPP_VERSION)

template <typename K, typename V, typename H>
struct StdNodeReplica {
  void* next;
  std::size_t hash;
  V value;
};

#else

template <typename H>
struct StdIsFastHash : std::true_type {};
template <>
struct StdIsFastHash<std::hash<long double>> : std::false_type {};
template <typename... Args>
struct StdIsFastHash<std::hash<std::basic_string<Args...>>> : std::false_type {
};

// TODO: add specialization for std::basic_string_view

// mimic internal node of unordered containers in STL to estimate the size
template <typename K, typename V, typename H, typename Enable = void>
struct StdNodeReplica {
  void* next;
  V value;
};
template <typename K, typename V, typename H>
struct StdNodeReplica<
    K,
    V,
    H,
    std::enable_if_t<
        !StdIsFastHash<H>::value ||
        !folly::is_nothrow_invocable<H, K>::value>> {
  void* next;
  V value;
  std::size_t hash;
};

#endif

} // namespace detail
} // namespace f14

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
namespace f14 {
namespace detail {
template <typename Policy>
class F14Table;
} // namespace detail
} // namespace f14

class F14HashToken final {
 public:
  F14HashToken() = default;

 private:
  using HashPair = std::pair<std::size_t, uint8_t>;

  explicit F14HashToken(HashPair hp) : hp_(hp) {}
  explicit operator HashPair() const {
    return hp_;
  }

  HashPair hp_;

  template <typename Policy>
  friend class f14::detail::F14Table;
};

namespace f14 {
namespace detail {
//// Defaults should be selected using void

template <typename Arg, typename Default>
using VoidDefault =
    std::conditional_t<std::is_same<Arg, Default>::value, void, Arg>;

template <typename Arg, typename Default>
using Defaulted =
    typename std::conditional_t<std::is_same<Arg, void>::value, Default, Arg>;

template <
    typename Void,
    typename Hasher,
    typename KeyEqual,
    typename Key,
    typename T>
struct EnableIfIsTransparent {};

template <typename Hasher, typename KeyEqual, typename Key, typename T>
struct EnableIfIsTransparent<
    folly::void_t<
        typename Hasher::is_transparent,
        typename KeyEqual::is_transparent>,
    Hasher,
    KeyEqual,
    Key,
    T> {
  using type = T;
};

////////////////

template <typename T>
FOLLY_ALWAYS_INLINE static void prefetchAddr(T const* ptr) {
#if FOLLY_AARCH64
  __builtin_prefetch(static_cast<void const*>(ptr));
#else
  // _mm_prefetch is x86_64-specific and comes from xmmintrin.h.
  // It seems to compile to the same thing as __builtin_prefetch, but
  // also works on windows.
  _mm_prefetch(
      static_cast<char const*>(static_cast<void const*>(ptr)), _MM_HINT_T0);
#endif
}

#if FOLLY_AARCH64
using TagVector = uint8x16_t;
#else
using TagVector = __m128i;
#endif

extern TagVector kEmptyTagVector;

// Iterates a 64-bit mask where elements are strided by 8 and the elements
// at indexes 8 and higher are layered back over the bottom 64-bits with
// a 4-bit offset.
//
// bitIndex = ((tagIndex * 8) % 64) + (tagIndex >= 8 ? 4 : 0)
//
// Iteration occurs in bitIndex order, not tagIndex.  That should be fine
// for a sparse iterator, where we expect either 0 or 1 tag.
class Sparse8Interleaved4MaskIter {
  uint64_t mask_;

 public:
  explicit Sparse8Interleaved4MaskIter(uint64_t mask) : mask_{mask} {}

  bool hasNext() {
    return mask_ != 0;
  }

  unsigned next() {
    FOLLY_SAFE_DCHECK(hasNext(), "");
    unsigned mixed = __builtin_ctzll(mask_);
    FOLLY_SAFE_DCHECK((mixed % 4) == 0, "");
    mask_ &= (mask_ - 1);

    // mixed >> 3 has the bottom 3 bits of the result (no masking needed
    // because all of the higher bits will be empty).  mixed & 4 holds the
    // bit that should be result & 8.  We can merge it in either before or
    // after sliding.  Merging it before means we need to shift it left 4
    // (so that the right shift 3 turns it into a left 1), which happens
    // to be the same as multiplication by 17.
    return ((mixed * 0x11) >> 3) & 0xf;
  }
};

// Iterates downward on occupied indexes by just checking tags[i] instead
// of using a mask
class TagCheckingIter {
  uint8_t const* tags_;
  int nextIndex_;

 public:
  explicit TagCheckingIter(uint8_t const* tags, int maxIndex)
      : tags_{tags}, nextIndex_{maxIndex} {}

  bool hasNext() {
    return nextIndex_ >= 0;
  }

  unsigned next() {
    auto rv = static_cast<unsigned>(nextIndex_);
    do {
      --nextIndex_;
    } while (nextIndex_ >= 0 && tags_[nextIndex_] == 0);
    return rv;
  }
};

// Holds the result of an index query that has an optional result,
// interpreting an index of -1 to be the empty answer
class IndexHolder {
  int index_;

 public:
  explicit IndexHolder(int index) : index_{index} {}

  bool hasIndex() const {
    return index_ >= 0;
  }

  unsigned index() const {
    FOLLY_SAFE_DCHECK(hasIndex(), "");
    return static_cast<unsigned>(index_);
  }
};

// Iterates a mask, optimized for the case that only a few bits are set
class SparseMaskIter {
  unsigned mask_;

 public:
  explicit SparseMaskIter(unsigned mask) : mask_{mask} {}

  bool hasNext() {
    return mask_ != 0;
  }

  unsigned next() {
    FOLLY_SAFE_DCHECK(hasNext(), "");
    unsigned i = __builtin_ctz(mask_);
    mask_ &= (mask_ - 1);
    return i;
  }
};

// Iterates a mask, optimized for the case that most bits are set
class DenseMaskIter {
  unsigned mask_;
  unsigned index_{0};

 public:
  explicit DenseMaskIter(unsigned mask) : mask_{mask} {}

  bool hasNext() {
    return mask_ != 0;
  }

  unsigned next() {
    FOLLY_SAFE_DCHECK(hasNext(), "");
    if (LIKELY((mask_ & 1) != 0)) {
      mask_ >>= 1;
      return index_++;
    } else {
      unsigned s = __builtin_ctz(mask_);
      unsigned rv = index_ + s;
      mask_ >>= (s + 1);
      index_ = rv + 1;
      return rv;
    }
  }
};

// Holds the result of an index query that has an optional result,
// interpreting a mask of 0 to be the empty answer and the index of the
// last set bit to be the non-empty answer
class LastOccupiedInMask {
  unsigned mask_;

 public:
  explicit LastOccupiedInMask(unsigned mask) : mask_{mask} {}

  bool hasIndex() const {
    return mask_ != 0;
  }

  unsigned index() const {
    folly::assume(mask_ != 0);
    return folly::findLastSet(mask_) - 1;
  }
};

// Holds the result of an index query that has an optional result,
// interpreting a mask of 0 to be the empty answer and the index of the
// first set bit to be the non-empty answer
class FirstEmptyInMask {
  unsigned mask_;

 public:
  explicit FirstEmptyInMask(unsigned mask) : mask_{mask} {}

  bool hasIndex() const {
    return mask_ != 0;
  }

  unsigned index() const {
    FOLLY_SAFE_DCHECK(mask_ != 0, "");
    return __builtin_ctz(mask_);
  }
};

template <typename ItemType>
struct alignas(max_align_t) F14Chunk {
  using Item = ItemType;

  // Assuming alignof(max_align_t) == 16 (and assuming alignof(Item) >=
  // 4) kCapacity of 14 is the most space efficient.  Slightly smaller
  // or larger capacities can help with cache alignment in a couple of
  // cases without wasting too much space, but once the items are larger
  // then we're unlikely to get much benefit anyway.  The only case we
  // optimize is using kCapacity of 12 for 4 byte items, which makes the
  // chunk take exactly 1 cache line, and adding 16 bytes of padding for
  // 16 byte items so that a chunk takes exactly 4 cache lines.
  static constexpr unsigned kCapacity = sizeof(Item) == 4 ? 12 : 14;

  static constexpr unsigned kDesiredCapacity = kCapacity - 2;

  static constexpr unsigned kAllocatedCapacity =
      kCapacity + (sizeof(Item) == 16 ? 1 : 0);

  static constexpr unsigned kFullMask =
      static_cast<unsigned>(~(~uint64_t{0} << kCapacity));

  // Non-empty tags have their top bit set
  std::array<uint8_t, kCapacity> tags_;

  // Bits 0..3 record the actual capacity of the chunk if this is chunk
  // zero, or hold 0000 for other chunks.  Bits 4-7 are a 4-bit counter
  // of the number of values in this chunk that were placed because they
  // overflowed their desired chunk (hostedOverflowCount).
  uint8_t control_;

  // The number of values that would have been placed into this chunk if
  // there had been space, including values that also overflowed previous
  // full chunks.  This value saturates; once it becomes 255 it no longer
  // increases nor decreases.
  uint8_t outboundOverflowCount_;

  std::array<
      std::aligned_storage_t<sizeof(Item), alignof(Item)>,
      kAllocatedCapacity>
      rawItems_;

  static F14Chunk* emptyInstance() {
    auto rv = static_cast<F14Chunk*>(static_cast<void*>(&kEmptyTagVector));
    FOLLY_SAFE_DCHECK(
        !rv->occupied(0) && rv->chunk0Capacity() == 0 &&
            rv->outboundOverflowCount() == 0,
        "");
    return rv;
  }

  void clear() {
    // tags_ = {}; control_ = 0; outboundOverflowCount_ = 0;

    // gcc < 6 doesn't exploit chunk alignment to generate the optimal
    // SSE clear from memset.  This is very hot code, so it is worth
    // handling that case specially.
#if FOLLY_SSE >= 2 && __GNUC__ <= 5 && !__clang__
    // this doesn't violate strict aliasing rules because __m128i is
    // tagged as __may_alias__
    auto* v = static_cast<__m128i*>(static_cast<void*>(&tags_[0]));
    _mm_store_si128(v, _mm_setzero_si128());
#else
    std::memset(&tags_[0], '\0', 16);
#endif
  }

  void copyOverflowInfoFrom(F14Chunk const& rhs) {
    FOLLY_SAFE_DCHECK(hostedOverflowCount() == 0, "");
    control_ += rhs.control_ & 0xf0;
    outboundOverflowCount_ = rhs.outboundOverflowCount_;
  }

  unsigned hostedOverflowCount() const {
    return control_ >> 4;
  }

  static constexpr uint8_t kIncrHostedOverflowCount = 0x10;
  static constexpr uint8_t kDecrHostedOverflowCount =
      static_cast<uint8_t>(-0x10);

  void adjustHostedOverflowCount(uint8_t op) {
    control_ += op;
  }

  bool eof() const {
    return (control_ & 0xf) != 0;
  }

  std::size_t chunk0Capacity() const {
    return control_ & 0xf;
  }

  void markEof(std::size_t c0c) {
    FOLLY_SAFE_DCHECK(
        this != emptyInstance() && control_ == 0 && c0c > 0 && c0c <= 0xf &&
            c0c <= kCapacity,
        "");
    control_ = static_cast<uint8_t>(c0c);
  }

  unsigned outboundOverflowCount() const {
    return outboundOverflowCount_;
  }

  void incrOutboundOverflowCount() {
    if (outboundOverflowCount_ != 255) {
      ++outboundOverflowCount_;
    }
  }

  void decrOutboundOverflowCount() {
    if (outboundOverflowCount_ != 255) {
      --outboundOverflowCount_;
    }
  }

  uint8_t tag(std::size_t index) const {
    return tags_[index];
  }

  void setTag(std::size_t index, uint8_t tag) {
    FOLLY_SAFE_DCHECK(this != emptyInstance() && (tag & 0x80) != 0, "");
    tags_[index] = tag;
  }

  void clearTag(std::size_t index) {
    tags_[index] = 0;
  }

#if FOLLY_AARCH64
  ////////
  // Tag filtering using AArch64 Advanced SIMD (NEON) intrinsics

  Sparse8Interleaved4MaskIter tagMatchIter(uint8_t needle) const {
    FOLLY_SAFE_DCHECK((needle & 0x80) != 0, "");
    uint8x16_t tagV = vld1q_u8(&tags_[0]);
    auto needleV = vdupq_n_u8(needle);
    auto eqV = vceqq_u8(tagV, needleV);
    auto bitsV = vreinterpretq_u64_u8(vshrq_n_u8(eqV, 7));
    auto hi = vgetq_lane_u64(bitsV, 1);
    auto lo = vgetq_lane_u64(bitsV, 0);
    static_assert(kCapacity >= 8, "");
    hi &= ((uint64_t{1} << (8 * (kCapacity - 8))) - 1);
    auto mixed = (hi << 4) | lo;
    return Sparse8Interleaved4MaskIter{mixed};
  }

  template <typename F, std::size_t... I>
  static constexpr uint8x16_t fixedVectorHelper(
      F const& func,
      index_sequence<I...>) {
    return uint8x16_t{func(I)...};
  }

  template <typename F>
  static constexpr uint8x16_t fixedVector(F const& func) {
    return fixedVectorHelper(
        [&](std::size_t i) { return i < kCapacity ? func(i) : uint8_t{0}; },
        make_index_sequence<16>{});
  }

  int lastOccupiedIndex() const {
    uint8x16_t tagV = vld1q_u8(&tags_[0]);
    // signed shift extends top bit to all bits
    auto occupiedV =
        vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(tagV), 7));
    auto indexV =
        fixedVector([](std::size_t i) { return static_cast<uint8_t>(i + 1); });
    auto occupiedIndexV = vandq_u8(occupiedV, indexV);
    return vmaxvq_u8(occupiedIndexV) - 1;
  }

  TagCheckingIter occupiedIter() const {
    return TagCheckingIter{&tags_[0], lastOccupiedIndex()};
  }

  IndexHolder lastOccupied() const {
    return IndexHolder{lastOccupiedIndex()};
  }

  IndexHolder firstEmpty() const {
    uint8x16_t tagV = vld1q_u8(&tags_[0]);
    // occupied tags have sign bit set when interpreted as int8_t, so
    // empty ones are non-negative
    auto emptyV = vcgeq_s8(vreinterpretq_s8_u8(tagV), vdupq_n_s8(0));
    auto indexV =
        fixedVector([](std::size_t i) { return static_cast<uint8_t>(~i); });
    auto emptyIndexV = vandq_u8(emptyV, indexV);
    // none empty -> i == 0xff == int8_t{-1}
    int8_t i = static_cast<int8_t>(~vmaxvq_u8(emptyIndexV));
    return IndexHolder{i};
  }
#else
  ////////
  // Tag filtering using x86_64 SSE2 intrinsics

  TagVector const* tagVector() const {
    return static_cast<TagVector const*>(static_cast<void const*>(&tags_[0]));
  }

  SparseMaskIter tagMatchIter(uint8_t needle) const {
    FOLLY_SAFE_DCHECK((needle & 0x80) != 0, "");
    auto tagV = _mm_load_si128(tagVector());
    auto needleV = _mm_set1_epi8(needle);
    auto eqV = _mm_cmpeq_epi8(tagV, needleV);
    auto mask = _mm_movemask_epi8(eqV) & kFullMask;
    return SparseMaskIter{mask};
  }

  unsigned occupiedMask() const {
    auto tagV = _mm_load_si128(tagVector());
    return _mm_movemask_epi8(tagV) & kFullMask;
  }

  DenseMaskIter occupiedIter() const {
    return DenseMaskIter{occupiedMask()};
  }

  LastOccupiedInMask lastOccupied() const {
    return LastOccupiedInMask{occupiedMask()};
  }

  FirstEmptyInMask firstEmpty() const {
    return FirstEmptyInMask{occupiedMask() ^ kFullMask};
  }
#endif

  bool occupied(std::size_t index) const {
    FOLLY_SAFE_DCHECK(tags_[index] == 0 || (tags_[index] & 0x80) != 0, "");
    return tags_[index] != 0;
  }

  Item* itemAddr(std::size_t i) const {
    return static_cast<Item*>(
        const_cast<void*>(static_cast<void const*>(&rawItems_[i])));
  }

  Item& item(std::size_t i) {
    FOLLY_SAFE_DCHECK(this->occupied(i), "");
    return *folly::launder(itemAddr(i));
  }

  Item const& citem(std::size_t i) const {
    FOLLY_SAFE_DCHECK(this->occupied(i), "");
    return *folly::launder(itemAddr(i));
  }

  static F14Chunk& owner(Item& item, std::size_t index) {
    auto rawAddr =
        static_cast<uint8_t*>(static_cast<void*>(std::addressof(item))) -
        offsetof(F14Chunk, rawItems_) - index * sizeof(Item);
    auto chunkAddr = static_cast<F14Chunk*>(static_cast<void*>(rawAddr));
    FOLLY_SAFE_DCHECK(std::addressof(item) == chunkAddr->itemAddr(index), "");
    return *chunkAddr;
  }
};

////////////////

template <typename ChunkPtr>
class F14ItemIter {
 private:
  using Chunk = typename std::pointer_traits<ChunkPtr>::element_type;

 public:
  using Item = typename Chunk::Item;
  using ItemPtr = typename std::pointer_traits<ChunkPtr>::template rebind<Item>;
  using ItemConstPtr =
      typename std::pointer_traits<ChunkPtr>::template rebind<Item const>;

  using Packed = TaggedPtr<ItemPtr>;

  //// PUBLIC

  F14ItemIter() noexcept : itemPtr_{nullptr}, index_{0} {}

  // default copy and move constructors and assignment operators are correct

  explicit F14ItemIter(Packed const& packed)
      : itemPtr_{packed.ptr()}, index_{packed.extra()} {}

  F14ItemIter(ChunkPtr chunk, std::size_t index)
      : itemPtr_{std::pointer_traits<ItemPtr>::pointer_to(chunk->item(index))},
        index_{index} {
    FOLLY_SAFE_DCHECK(index < Chunk::kCapacity, "");
    folly::assume(
        std::pointer_traits<ItemPtr>::pointer_to(chunk->item(index)) !=
        nullptr);
    folly::assume(itemPtr_ != nullptr);
  }

  FOLLY_ALWAYS_INLINE void advance() {
    auto c = chunk();

    // common case is packed entries
    while (index_ > 0) {
      --index_;
      --itemPtr_;
      if (LIKELY(c->occupied(index_))) {
        return;
      }
    }

    // It's fairly common for an iterator to be advanced and then become
    // dead, for example in the return value from erase(iter) or in
    // the last step of a loop.  We'd like to make sure that the entire
    // advance() method can be eliminated by the compiler's dead code
    // elimination pass.  To do that it must eliminate the loops, which
    // requires it to prove that they have no side effects.  It's easy
    // to show that there are no escaping stores, but at the moment
    // compilers also consider an infinite loop to be a side effect.
    // (There are parts of the standard that would allow them to treat
    // this as undefined behavior, but at the moment they don't exploit
    // those clauses.)
    //
    // The following loop should really be a while loop, which would
    // save a register, some instructions, and a conditional branch,
    // but by writing it as a for loop the compiler can prove to itself
    // that it will eventually terminate.  (No matter that even if the
    // loop executed in a single cycle it would take about 200 years to
    // run all 2^64 iterations.)
    //
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=82776 has the bug we
    // filed about the issue.  while (true) {
    for (std::size_t i = 1; i != 0; ++i) {
      // exhausted the current chunk
      if (UNLIKELY(c->eof())) {
        FOLLY_SAFE_DCHECK(index_ == 0, "");
        itemPtr_ = nullptr;
        return;
      }
      --c;
      auto last = c->lastOccupied();
      if (LIKELY(last.hasIndex())) {
        index_ = last.index();
        itemPtr_ = std::pointer_traits<ItemPtr>::pointer_to(c->item(index_));
        return;
      }
    }
  }

  // precheckedAdvance requires knowledge that the current iterator
  // position isn't the last item
  void precheckedAdvance() {
    auto c = chunk();

    // common case is packed entries
    while (index_ > 0) {
      --index_;
      --itemPtr_;
      if (LIKELY(c->occupied(index_))) {
        return;
      }
    }

    while (true) {
      // exhausted the current chunk
      FOLLY_SAFE_DCHECK(!c->eof(), "");
      --c;
      auto last = c->lastOccupied();
      if (LIKELY(last.hasIndex())) {
        index_ = last.index();
        itemPtr_ = std::pointer_traits<ItemPtr>::pointer_to(c->item(index_));
        return;
      }
    }
  }

  ChunkPtr chunk() const {
    return std::pointer_traits<ChunkPtr>::pointer_to(
        Chunk::owner(*itemPtr_, index_));
  }

  std::size_t index() const {
    return index_;
  }

  Item* itemAddr() const {
    return std::addressof(*itemPtr_);
  }
  Item& item() const {
    return *itemPtr_;
  }
  Item const& citem() const {
    return *itemPtr_;
  }

  bool atEnd() const {
    return itemPtr_ == nullptr;
  }

  Packed pack() const {
    return Packed{itemPtr_, static_cast<uint8_t>(index_)};
  }

  bool operator==(F14ItemIter const& rhs) const {
    // this form makes iter == end() into a single null check after inlining
    // and constant propagation
    return itemPtr_ == rhs.itemPtr_;
  }

  bool operator!=(F14ItemIter const& rhs) const {
    return !(*this == rhs);
  }

 private:
  ItemPtr itemPtr_;
  std::size_t index_;
};

////////////////

template <typename SizeType, typename ItemIter, bool EnablePackedItemIter>
struct SizeAndPackedBegin {
  SizeType size_{0};

 private:
  typename ItemIter::Packed packedBegin_{ItemIter{}.pack()};

 public:
  typename ItemIter::Packed& packedBegin() {
    return packedBegin_;
  }

  typename ItemIter::Packed const& packedBegin() const {
    return packedBegin_;
  }
};

template <typename SizeType, typename ItemIter>
struct SizeAndPackedBegin<SizeType, ItemIter, false> {
  SizeType size_{0};

  [[noreturn]] typename ItemIter::Packed& packedBegin() {
    folly::assume_unreachable();
  }

  [[noreturn]] typename ItemIter::Packed const& packedBegin() const {
    folly::assume_unreachable();
  }
};

template <typename Policy>
class F14Table : public Policy {
 public:
  using typename Policy::Item;
  using value_type = typename Policy::Value;
  using allocator_type = typename Policy::Alloc;

 private:
  using Policy::kAllocIsAlwaysEqual;
  using Policy::kDefaultConstructIsNoexcept;
  using Policy::kSwapIsNoexcept;

  using AllocTraits = typename Policy::AllocTraits;

  using ByteAlloc = typename AllocTraits::template rebind_alloc<uint8_t>;
  using BytePtr = typename std::allocator_traits<ByteAlloc>::pointer;

  using Chunk = F14Chunk<Item>;
  using ChunkPtr =
      typename std::pointer_traits<BytePtr>::template rebind<Chunk>;

  using HashPair = typename F14HashToken::HashPair;

 public:
  using ItemIter = F14ItemIter<ChunkPtr>;

 private:
  //////// begin fields

  ChunkPtr chunks_{Chunk::emptyInstance()};
  typename Policy::InternalSizeType chunkMask_{0};
  SizeAndPackedBegin<
      typename Policy::InternalSizeType,
      ItemIter,
      Policy::kEnableItemIteration>
      sizeAndPackedBegin_;

  //////// end fields

  void swapContents(F14Table& rhs) noexcept {
    using std::swap;
    swap(chunks_, rhs.chunks_);
    swap(chunkMask_, rhs.chunkMask_);
    swap(sizeAndPackedBegin_.size_, rhs.sizeAndPackedBegin_.size_);
    if (Policy::kEnableItemIteration) {
      swap(
          sizeAndPackedBegin_.packedBegin(),
          rhs.sizeAndPackedBegin_.packedBegin());
    }
  }

 public:
  F14Table(
      std::size_t initialCapacity,
      typename Policy::Hasher const& hasher,
      typename Policy::KeyEqual const& keyEqual,
      typename Policy::Alloc const& alloc)
      : Policy{hasher, keyEqual, alloc} {
    if (initialCapacity > 0) {
      reserve(initialCapacity);
    }
  }

  F14Table(F14Table const& rhs) : Policy{rhs} {
    buildFromF14Table(rhs);
  }

  F14Table(F14Table const& rhs, typename Policy::Alloc const& alloc)
      : Policy{rhs, alloc} {
    buildFromF14Table(rhs);
  }

  F14Table(F14Table&& rhs) noexcept(
      std::is_nothrow_move_constructible<typename Policy::Hasher>::value&&
          std::is_nothrow_move_constructible<typename Policy::KeyEqual>::value&&
              std::is_nothrow_move_constructible<typename Policy::Alloc>::value)
      : Policy{std::move(rhs)} {
    swapContents(rhs);
  }

  F14Table(F14Table&& rhs, typename Policy::Alloc const& alloc) noexcept(
      kAllocIsAlwaysEqual)
      : Policy{std::move(rhs), alloc} {
    if (kAllocIsAlwaysEqual || this->alloc() == rhs.alloc()) {
      // move storage (common case)
      swapContents(rhs);
    } else {
      // new storage because allocators unequal, move values (rare case)
      buildFromF14Table(std::move(rhs));
    }
  }

  F14Table& operator=(F14Table const& rhs) {
    if (this != &rhs) {
      reset();
      static_cast<Policy&>(*this) = rhs;
      buildFromF14Table(rhs);
    }
    return *this;
  }

  F14Table& operator=(F14Table&& rhs) noexcept(
      std::is_nothrow_move_assignable<typename Policy::Hasher>::value&&
          std::is_nothrow_move_assignable<typename Policy::KeyEqual>::value &&
      (kAllocIsAlwaysEqual ||
       (AllocTraits::propagate_on_container_move_assignment::value &&
        std::is_nothrow_move_assignable<typename Policy::Alloc>::value))) {
    if (this != &rhs) {
      reset();
      static_cast<Policy&>(*this) = std::move(rhs);
      if (AllocTraits::propagate_on_container_move_assignment::value ||
          kAllocIsAlwaysEqual || this->alloc() == rhs.alloc()) {
        // move storage (common case)
        swapContents(rhs);
      } else {
        // new storage because allocators unequal, move values (rare case)
        buildFromF14Table(std::move(rhs));
      }
    }
    return *this;
  }

  ~F14Table() {
    reset();
  }

  void swap(F14Table& rhs) noexcept(kSwapIsNoexcept) {
    // If propagate_on_container_swap is false and allocators are
    // not equal, the only way to accomplish a swap would be to do
    // dynamic allocation and then move (or swap) each contained value.
    // AllocatorAwareContainer-s are not supposed to attempt this, but
    // rather are supposed to have undefined behavior in that case.
    FOLLY_SAFE_CHECK(
        AllocTraits::propagate_on_container_swap::value ||
            kAllocIsAlwaysEqual || this->alloc() == rhs.alloc(),
        "swap is undefined for unequal non-propagating allocators");
    this->swapPolicy(rhs);
    swapContents(rhs);
  }

 private:
  //////// hash helpers

  // Hash values are used to compute the desired position, which is the
  // chunk index at which we would like to place a value (if there is no
  // overflow), and the tag, which is an additional 8 bits of entropy.
  //
  // The standard's definition of hash function quality only refers to
  // the probability of collisions of the entire hash value, not to the
  // probability of collisions of the results of shifting or masking the
  // hash value.  Some hash functions, however, provide this stronger
  // guarantee (not quite the same as the definition of avalanching,
  // but similar).
  //
  // If the user-supplied hasher is an avalanching one (each bit of the
  // hash value has a 50% chance of being the same for differing hash
  // inputs), then we can just take 1 byte of the hash value for the tag
  // and the rest for the desired position.  Avalanching hashers also
  // let us map hash value to array index position with just a bitmask
  // without risking clumping.  (Many hash tables just accept the risk
  // and do it regardless.)
  //
  // std::hash<std::string> avalanches in all implementations we've
  // examined: libstdc++-v3 uses MurmurHash2, and libc++ uses CityHash
  // or MurmurHash2.  The other std::hash specializations, however, do not
  // have this property.  std::hash for integral and pointer values is the
  // identity function on libstdc++-v3 and libc++, in particular.  In our
  // experience it is also fairly common for user-defined specializations
  // of std::hash to combine fields in an ad-hoc way that does not evenly
  // distribute entropy among the bits of the result (a + 37 * b, for
  // example, where a and b are integer fields).
  //
  // For hash functions we don't trust to avalanche, we repair things by
  // applying a bit mixer to the user-supplied hash.

  static HashPair splitHash(std::size_t hash) {
    uint8_t tag;
    if (!Policy::isAvalanchingHasher()) {
#if FOLLY_SSE > 4 || (FOLLY_SSE == 4 && FOLLY_SSE_MINOR >= 2)
      // SSE4.2 CRC
      auto c = _mm_crc32_u64(0, hash);
      tag = static_cast<uint8_t>(~(c >> 25));
      hash += c;
#elif FOLLY_AARCH64 && __ARM_FEATURE_CRC32
      // AARCH64 CRC is Optional on armv8 (-march=armv8-a+crc), standard
      // on armv8.1
      auto c = __crc32cd(0, hash);
      tag = static_cast<uint8_t>(~(c >> 25));
      hash += c;
#else
      // The mixer below is not fully avalanching for all 64 bits of
      // output, but looks quite good for bits 18..63 and puts plenty
      // of entropy even lower when considering multiple bits together
      // (like the tag).  Importantly, when under register pressure it
      // uses fewer registers, instructions, and immediate constants
      // than the alternatives, resulting in compact code that is more
      // easily inlinable.  In one instantiation a modified Murmur mixer
      // was 48 bytes of assembly (even after using the same multiplicand
      // for both steps) and this one was 27 bytes, for example.
      auto const kMul = 0xc4ceb9fe1a85ec53ULL;
#ifdef _WIN32
      __int64 signedHi;
      __int64 signedLo = _mul128(
          static_cast<__int64>(hash), static_cast<__int64>(kMul), &signedHi);
      auto hi = static_cast<uint64_t>(signedHi);
      auto lo = static_cast<uint64_t>(signedLo);
#else
      auto hi = static_cast<uint64_t>(
          (static_cast<unsigned __int128>(hash) * kMul) >> 64);
      auto lo = hash * kMul;
#endif
      hash = hi ^ lo;
      hash *= kMul;
      tag = static_cast<uint8_t>(hash >> 15) | 0x80;
      hash >>= 22;
#endif
    } else {
      // we don't trust the top bit
      tag = (hash >> 56) | 0x80;
    }
    return std::make_pair(hash, tag);
  }

  //////// memory management helpers

  static std::size_t allocSize(
      std::size_t chunkCount,
      std::size_t maxSizeWithoutRehash) {
    if (chunkCount == 1) {
      auto n = offsetof(Chunk, rawItems_) + maxSizeWithoutRehash * sizeof(Item);
      FOLLY_SAFE_DCHECK((maxSizeWithoutRehash % 2) == 0, "");
      if ((sizeof(Item) % 8) != 0) {
        n = ((n - 1) | 15) + 1;
      }
      FOLLY_SAFE_DCHECK((n % 16) == 0, "");
      return n;
    } else {
      return sizeof(Chunk) * chunkCount;
    }
  }

  ChunkPtr newChunks(std::size_t chunkCount, std::size_t maxSizeWithoutRehash) {
    ByteAlloc a{this->alloc()};
    uint8_t* raw = &*std::allocator_traits<ByteAlloc>::allocate(
        a, allocSize(chunkCount, maxSizeWithoutRehash));
    static_assert(std::is_trivial<Chunk>::value, "F14Chunk should be POD");
    auto chunks = static_cast<Chunk*>(static_cast<void*>(raw));
    for (std::size_t i = 0; i < chunkCount; ++i) {
      chunks[i].clear();
    }
    chunks[0].markEof(chunkCount == 1 ? maxSizeWithoutRehash : 1);
    return std::pointer_traits<ChunkPtr>::pointer_to(*chunks);
  }

  void deleteChunks(
      ChunkPtr chunks,
      std::size_t chunkCount,
      std::size_t maxSizeWithoutRehash) {
    ByteAlloc a{this->alloc()};
    BytePtr bp = std::pointer_traits<BytePtr>::pointer_to(
        *static_cast<uint8_t*>(static_cast<void*>(&*chunks)));
    std::allocator_traits<ByteAlloc>::deallocate(
        a, bp, allocSize(chunkCount, maxSizeWithoutRehash));
  }

 public:
  ItemIter begin() const noexcept {
    FOLLY_SAFE_DCHECK(Policy::kEnableItemIteration, "");
    return ItemIter{sizeAndPackedBegin_.packedBegin()};
  }

  ItemIter end() const noexcept {
    return ItemIter{};
  }

  bool empty() const noexcept {
    return size() == 0;
  }

  std::size_t size() const noexcept {
    return sizeAndPackedBegin_.size_;
  }

  std::size_t max_size() const noexcept {
    auto& a = this->alloc();
    return std::min<std::size_t>(
        (std::numeric_limits<typename Policy::InternalSizeType>::max)(),
        AllocTraits::max_size(a));
  }

  std::size_t bucket_count() const noexcept {
    // bucket_count is just a synthetic construct for the outside world
    // so that size, bucket_count, load_factor, and max_load_factor are
    // all self-consistent.  The only one of those that is real is size().
    if (chunkMask_ != 0) {
      return (chunkMask_ + 1) * Chunk::kDesiredCapacity;
    } else {
      return chunks_->chunk0Capacity();
    }
  }

  std::size_t max_bucket_count() const noexcept {
    return max_size();
  }

  float load_factor() const noexcept {
    return empty()
        ? 0.0f
        : static_cast<float>(size()) / static_cast<float>(bucket_count());
  }

  float max_load_factor() const noexcept {
    return 1.0f;
  }

  void max_load_factor(float) noexcept {
    // Probing hash tables can't run load factors >= 1 (unlike chaining
    // tables).  In addition, we have measured that there is little or
    // no performance advantage to running a smaller load factor (cache
    // locality losses outweigh the small reduction in probe lengths,
    // often making it slower).  Therefore, we've decided to just fix
    // max_load_factor at 1.0f regardless of what the user requests.
    // This has an additional advantage that we don't have to store it.
    // Taking alignment into consideration this makes every F14 table
    // 8 bytes smaller, and is part of the reason an empty F14NodeMap
    // is almost half the size of an empty std::unordered_map (32 vs
    // 56 bytes).
    //
    // I don't have a strong opinion on whether we should remove this
    // method or leave a stub, let ngbronson or xshi know if you have a
    // compelling argument either way.
  }

 private:
  // Our probe strategy is to advance through additional chunks with
  // a stride that is key-specific.  This is called double hashing,
  // and is a well known and high quality probing strategy.  So long as
  // the stride and the chunk count are relatively prime, we will visit
  // every chunk once and then return to the original chunk, letting us
  // detect and end the cycle.  The chunk count is a power of two, so
  // we can satisfy the relatively prime part by choosing an odd stride.
  // We've already computed a high quality secondary hash value for the
  // tag, so we just use it for the second probe hash as well.
  //
  // At the maximum load factor of 12/14, expected probe length for a
  // find hit is 1.041, with 99% of keys found in the first three chunks.
  // Expected probe length for a find miss (or insert) is 1.275, with a
  // p99 probe length of 4 (fewer than 1% of failing find look at 5 or
  // more chunks).
  //
  // This code is structured so you can try various ways of encoding
  // the current probe state.  For example, at the moment the probe's
  // state is the position in the cycle and the resulting chunk index is
  // computed from that inside probeCurrentIndex.  We could also make the
  // probe state the chunk index, and then increment it by hp.second *
  // 2 + 1 in probeAdvance.  Wrapping can be applied early or late as
  // well.  This particular code seems to be easier for the optimizer
  // to understand.
  //
  // We could also implement probing strategies that resulted in the same
  // tour for every key initially assigned to a chunk (linear probing or
  // quadratic), but that results in longer probe lengths.  In particular,
  // the cache locality wins of linear probing are not worth the increase
  // in probe lengths (extra work and less branch predictability) in
  // our experiments.

  std::size_t probeDelta(HashPair hp) const {
    return 2 * hp.second + 1;
  }

  template <typename K>
  FOLLY_ALWAYS_INLINE ItemIter findImpl(HashPair hp, K const& key) const {
    std::size_t index = hp.first;
    std::size_t step = probeDelta(hp);
    for (std::size_t tries = 0; tries <= chunkMask_; ++tries) {
      ChunkPtr chunk = chunks_ + (index & chunkMask_);
      if (sizeof(Chunk) > 64) {
        prefetchAddr(chunk->itemAddr(8));
      }
      auto hits = chunk->tagMatchIter(hp.second);
      while (hits.hasNext()) {
        auto i = hits.next();
        if (LIKELY(this->keyMatchesItem(key, chunk->item(i)))) {
          // Tag match and key match were both successful.  The chance
          // of a false tag match is 1/128 for each key in the chunk
          // (with a proper hash function).
          return ItemIter{chunk, i};
        }
      }
      if (LIKELY(chunk->outboundOverflowCount() == 0)) {
        // No keys that wanted to be placed in this chunk were denied
        // entry, so our search is over.  This is the common case.
        break;
      }
      index += step;
    }
    // Loop exit because tries is exhausted is rare, but possible.
    // That means that for every chunk there is currently a key present
    // in the map that visited that chunk on its probe search but ended
    // up somewhere else, and we have searched every chunk.
    return ItemIter{};
  }

 public:
  // Prehashing splits the work of find(key) into two calls, enabling you
  // to manually implement loop pipelining for hot bulk lookups.  prehash
  // computes the hash and prefetches the first computed memory location,
  // and the two-arg find(F14HashToken,K) performs the rest of the search.
  template <typename K>
  F14HashToken prehash(K const& key) const {
    FOLLY_SAFE_DCHECK(chunks_ != nullptr, "");
    auto hp = splitHash(this->computeKeyHash(key));
    ChunkPtr firstChunk = chunks_ + (hp.first & chunkMask_);
    prefetchAddr(firstChunk);
    return F14HashToken(std::move(hp));
  }

  template <typename K>
  FOLLY_ALWAYS_INLINE ItemIter find(K const& key) const {
    auto hp = splitHash(this->computeKeyHash(key));
    return findImpl(hp, key);
  }

  template <typename K>
  FOLLY_ALWAYS_INLINE ItemIter
  find(F14HashToken const& token, K const& key) const {
    FOLLY_SAFE_DCHECK(
        splitHash(this->computeKeyHash(key)) == static_cast<HashPair>(token),
        "");
    return findImpl(static_cast<HashPair>(token), key);
  }

 private:
  void adjustSizeAndBeginAfterInsert(ItemIter iter) {
    if (Policy::kEnableItemIteration) {
      // packedBegin is the max of all valid ItemIter::pack()
      auto packed = iter.pack();
      if (sizeAndPackedBegin_.packedBegin() < packed) {
        sizeAndPackedBegin_.packedBegin() = packed;
      }
    }

    ++sizeAndPackedBegin_.size_;
  }

  // Ignores hp if pos.chunk()->hostedOverflowCount() == 0
  void eraseBlank(ItemIter iter, HashPair hp) {
    iter.chunk()->clearTag(iter.index());

    if (iter.chunk()->hostedOverflowCount() != 0) {
      // clean up
      std::size_t index = hp.first;
      std::size_t delta = probeDelta(hp);
      uint8_t hostedOp = 0;
      while (true) {
        ChunkPtr chunk = chunks_ + (index & chunkMask_);
        if (chunk == iter.chunk()) {
          chunk->adjustHostedOverflowCount(hostedOp);
          break;
        }
        chunk->decrOutboundOverflowCount();
        hostedOp = Chunk::kDecrHostedOverflowCount;
        index += delta;
      }
    }
  }

  void adjustSizeAndBeginBeforeErase(ItemIter iter) {
    --sizeAndPackedBegin_.size_;
    if (Policy::kEnableItemIteration) {
      if (iter.pack() == sizeAndPackedBegin_.packedBegin()) {
        if (size() == 0) {
          iter = ItemIter{};
        } else {
          iter.precheckedAdvance();
        }
        sizeAndPackedBegin_.packedBegin() = iter.pack();
      }
    }
  }

  template <typename... Args>
  void insertAtBlank(ItemIter pos, HashPair hp, Args&&... args) {
    try {
      auto dst = pos.itemAddr();
      folly::assume(dst != nullptr);
      this->constructValueAtItem(size(), dst, std::forward<Args>(args)...);
    } catch (...) {
      eraseBlank(pos, hp);
      throw;
    }
    adjustSizeAndBeginAfterInsert(pos);
  }

  ItemIter allocateTag(uint8_t* fullness, HashPair hp) {
    ChunkPtr chunk;
    std::size_t index = hp.first;
    std::size_t delta = probeDelta(hp);
    uint8_t hostedOp = 0;
    while (true) {
      index &= chunkMask_;
      chunk = chunks_ + index;
      if (LIKELY(fullness[index] < Chunk::kCapacity)) {
        break;
      }
      chunk->incrOutboundOverflowCount();
      hostedOp = Chunk::kIncrHostedOverflowCount;
      index += delta;
    }
    unsigned itemIndex = fullness[index]++;
    FOLLY_SAFE_DCHECK(!chunk->occupied(itemIndex), "");
    chunk->setTag(itemIndex, hp.second);
    chunk->adjustHostedOverflowCount(hostedOp);
    return ItemIter{chunk, itemIndex};
  }

  ChunkPtr lastOccupiedChunk() const {
    if (Policy::kEnableItemIteration) {
      return begin().chunk();
    } else {
      return chunks_ + chunkMask_;
    }
  }

  template <typename T>
  void directBuildFrom(T&& src) {
    FOLLY_SAFE_DCHECK(src.size() > 0 && chunkMask_ == src.chunkMask_, "");

    // We use std::forward<T> to allow portions of src to be moved out by
    // either beforeBuild or afterBuild, but we are just relying on good
    // behavior of our Policy superclass to ensure that any particular
    // field of this is a donor at most once.

    auto undoState =
        this->beforeBuild(src.size(), bucket_count(), std::forward<T>(src));
    bool success = false;
    SCOPE_EXIT {
      this->afterBuild(
          undoState, success, src.size(), bucket_count(), std::forward<T>(src));
    };

    // Copy can fail part-way through if a Value copy constructor throws.
    // Failing afterBuild is limited in its cleanup power in this case,
    // because it can't enumerate the items that were actually copied.
    // Fortunately we can divide the situation into cases where all of
    // the state is owned by the table itself (F14Node and F14Value),
    // for which clearImpl() can do partial cleanup, and cases where all
    // of the values are owned by the policy (F14Vector), in which case
    // partial failure should not occur.  Sorry for the subtle invariants
    // in the Policy API.

    if (folly::is_trivially_copyable<Item>::value &&
        !this->destroyItemOnClear() && bucket_count() == src.bucket_count()) {
      // most happy path
      auto n = allocSize(chunkMask_ + 1, bucket_count());
      std::memcpy(&chunks_[0], &src.chunks_[0], n);
      sizeAndPackedBegin_.size_ = src.size();
      if (Policy::kEnableItemIteration) {
        auto srcBegin = src.begin();
        sizeAndPackedBegin_.packedBegin() =
            ItemIter{chunks_ + (srcBegin.chunk() - src.chunks_),
                     srcBegin.index()}
                .pack();
      }
    } else {
      std::size_t maxChunkIndex = src.lastOccupiedChunk() - src.chunks_;

      // happy path, no rehash but pack items toward bottom of chunk and
      // use copy constructor
      auto srcChunk = &src.chunks_[maxChunkIndex];
      Chunk* dstChunk = &chunks_[maxChunkIndex];
      do {
        dstChunk->copyOverflowInfoFrom(*srcChunk);

        auto iter = srcChunk->occupiedIter();
        if (Policy::prefetchBeforeCopy()) {
          for (auto piter = iter; piter.hasNext();) {
            this->prefetchValue(srcChunk->citem(piter.next()));
          }
        }

        std::size_t dstI = 0;
        for (; iter.hasNext(); ++dstI) {
          auto srcI = iter.next();
          auto&& srcArg =
              std::forward<T>(src).buildArgForItem(srcChunk->item(srcI));
          auto dst = dstChunk->itemAddr(dstI);
          folly::assume(dst != nullptr);
          this->constructValueAtItem(
              0, dst, std::forward<decltype(srcArg)>(srcArg));
          dstChunk->setTag(dstI, srcChunk->tag(srcI));
          ++sizeAndPackedBegin_.size_;
        }

        --srcChunk;
        --dstChunk;
      } while (size() != src.size());

      // reset doesn't care about packedBegin, so we don't fix it until the end
      if (Policy::kEnableItemIteration) {
        sizeAndPackedBegin_.packedBegin() =
            ItemIter{chunks_ + maxChunkIndex,
                     chunks_[maxChunkIndex].lastOccupied().index()}
                .pack();
      }
    }

    success = true;
  }

  template <typename T>
  void rehashBuildFrom(T&& src) {
    FOLLY_SAFE_DCHECK(src.chunkMask_ > chunkMask_, "");

    // 1 byte per chunk means < 1 bit per value temporary overhead
    std::array<uint8_t, 256> stackBuf;
    uint8_t* fullness;
    auto cc = chunkMask_ + 1;
    if (cc <= stackBuf.size()) {
      fullness = stackBuf.data();
    } else {
      ByteAlloc a{this->alloc()};
      fullness = &*std::allocator_traits<ByteAlloc>::allocate(a, cc);
    }
    SCOPE_EXIT {
      if (cc > stackBuf.size()) {
        ByteAlloc a{this->alloc()};
        std::allocator_traits<ByteAlloc>::deallocate(
            a,
            std::pointer_traits<typename std::allocator_traits<
                ByteAlloc>::pointer>::pointer_to(*fullness),
            cc);
      }
    };
    std::memset(fullness, '\0', cc);

    // We use std::forward<T> to allow portions of src to be moved out by
    // either beforeBuild or afterBuild, but we are just relying on good
    // behavior of our Policy superclass to ensure that any particular
    // field of this is a donor at most once.

    // Exception safety requires beforeBuild to happen after all of the
    // allocate() calls.
    auto undoState =
        this->beforeBuild(src.size(), bucket_count(), std::forward<T>(src));
    bool success = false;
    SCOPE_EXIT {
      this->afterBuild(
          undoState, success, src.size(), bucket_count(), std::forward<T>(src));
    };

    // The current table is at a valid state at all points for policies
    // in which non-trivial values are owned by the main table (F14Node
    // and F14Value), so reset() will clean things up properly if we
    // fail partway through.  For the case that the policy manages value
    // lifecycle (F14Vector) then nothing after beforeBuild can throw and
    // we don't have to worry about partial failure.

    std::size_t srcChunkIndex = src.lastOccupiedChunk() - src.chunks_;
    while (true) {
      auto srcChunk = &src.chunks_[srcChunkIndex];
      auto iter = srcChunk->occupiedIter();
      if (Policy::prefetchBeforeRehash()) {
        for (auto piter = iter; piter.hasNext();) {
          this->prefetchValue(srcChunk->item(piter.next()));
        }
      }
      if (srcChunk->hostedOverflowCount() == 0) {
        // all items are in their preferred chunk (no probing), so we
        // don't need to compute any hash values
        while (iter.hasNext()) {
          auto i = iter.next();
          auto& srcItem = srcChunk->item(i);
          auto&& srcArg = std::forward<T>(src).buildArgForItem(srcItem);
          HashPair hp{srcChunkIndex, srcChunk->tag(i)};
          insertAtBlank(
              allocateTag(fullness, hp),
              hp,
              std::forward<decltype(srcArg)>(srcArg));
        }
      } else {
        // any chunk's items might be in here
        while (iter.hasNext()) {
          auto i = iter.next();
          auto& srcItem = srcChunk->item(i);
          auto&& srcArg = std::forward<T>(src).buildArgForItem(srcItem);
          auto const& srcKey = src.keyForValue(srcArg);
          auto hp = splitHash(this->computeKeyHash(srcKey));
          FOLLY_SAFE_DCHECK(hp.second == srcChunk->tag(i), "");
          insertAtBlank(
              allocateTag(fullness, hp),
              hp,
              std::forward<decltype(srcArg)>(srcArg));
        }
      }
      if (srcChunkIndex == 0) {
        break;
      }
      --srcChunkIndex;
    }

    success = true;
  }

  template <typename T>
  FOLLY_NOINLINE void buildFromF14Table(T&& src) {
    FOLLY_SAFE_DCHECK(size() == 0, "");
    if (src.size() == 0) {
      return;
    }

    reserveForInsert(src.size());
    try {
      if (chunkMask_ == src.chunkMask_) {
        directBuildFrom(std::forward<T>(src));
      } else {
        rehashBuildFrom(std::forward<T>(src));
      }
    } catch (...) {
      reset();
      throw;
    }
  }

  FOLLY_NOINLINE void rehashImpl(
      std::size_t newChunkCount,
      std::size_t newMaxSizeWithoutRehash) {
    FOLLY_SAFE_DCHECK(newMaxSizeWithoutRehash > 0, "");

    auto origChunks = chunks_;
    const auto origChunkCount = chunkMask_ + 1;
    const auto origMaxSizeWithoutRehash = bucket_count();

    auto undoState = this->beforeRehash(
        size(), origMaxSizeWithoutRehash, newMaxSizeWithoutRehash);
    bool success = false;
    SCOPE_EXIT {
      this->afterRehash(
          std::move(undoState),
          success,
          size(),
          origMaxSizeWithoutRehash,
          newMaxSizeWithoutRehash);
    };

    chunks_ = newChunks(newChunkCount, newMaxSizeWithoutRehash);
    chunkMask_ = newChunkCount - 1;

    if (size() == 0) {
      // nothing to do
    } else if (origChunkCount == 1 && newChunkCount == 1) {
      // no mask, no chunk scan, no hash computation, no probing
      auto srcChunk = origChunks;
      auto dstChunk = chunks_;
      std::size_t srcI = 0;
      std::size_t dstI = 0;
      while (dstI < size()) {
        if (LIKELY(srcChunk->occupied(srcI))) {
          dstChunk->setTag(dstI, srcChunk->tag(srcI));
          this->moveItemDuringRehash(
              dstChunk->itemAddr(dstI), srcChunk->item(srcI));
          ++dstI;
        }
        ++srcI;
      }
      if (Policy::kEnableItemIteration) {
        sizeAndPackedBegin_.packedBegin() = ItemIter{dstChunk, dstI - 1}.pack();
      }
    } else {
      // 1 byte per chunk means < 1 bit per value temporary overhead
      std::array<uint8_t, 256> stackBuf;
      uint8_t* fullness;
      if (newChunkCount <= stackBuf.size()) {
        fullness = stackBuf.data();
      } else {
        try {
          ByteAlloc a{this->alloc()};
          fullness =
              &*std::allocator_traits<ByteAlloc>::allocate(a, newChunkCount);
        } catch (...) {
          deleteChunks(chunks_, newChunkCount, newMaxSizeWithoutRehash);
          chunks_ = origChunks;
          chunkMask_ = origChunkCount - 1;
          throw;
        }
      }
      std::memset(fullness, '\0', newChunkCount);
      SCOPE_EXIT {
        if (newChunkCount > stackBuf.size()) {
          ByteAlloc a{this->alloc()};
          std::allocator_traits<ByteAlloc>::deallocate(
              a,
              std::pointer_traits<typename std::allocator_traits<
                  ByteAlloc>::pointer>::pointer_to(*fullness),
              newChunkCount);
        }
      };

      auto srcChunk = origChunks + origChunkCount - 1;
      std::size_t remaining = size();
      while (remaining > 0) {
        auto iter = srcChunk->occupiedIter();
        if (Policy::prefetchBeforeRehash()) {
          for (auto piter = iter; piter.hasNext();) {
            this->prefetchValue(srcChunk->item(piter.next()));
          }
        }
        while (iter.hasNext()) {
          --remaining;
          auto srcI = iter.next();
          Item& srcItem = srcChunk->item(srcI);
          auto hp = splitHash(
              this->computeItemHash(const_cast<Item const&>(srcItem)));
          FOLLY_SAFE_DCHECK(hp.second == srcChunk->tag(srcI), "");

          auto dstIter = allocateTag(fullness, hp);
          this->moveItemDuringRehash(dstIter.itemAddr(), srcItem);
        }
        --srcChunk;
      }

      if (Policy::kEnableItemIteration) {
        // this code replaces size invocations of adjustSizeAndBeginAfterInsert
        std::size_t i = chunkMask_;
        while (fullness[i] == 0) {
          --i;
        }
        sizeAndPackedBegin_.packedBegin() =
            ItemIter{chunks_ + i, std::size_t{fullness[i]} - 1}.pack();
      }
    }

    if (origMaxSizeWithoutRehash != 0) {
      deleteChunks(origChunks, origChunkCount, origMaxSizeWithoutRehash);
    }
    success = true;
  }

 public:
  // user has no control over max_load_factor

  void rehash(std::size_t capacity) {
    if (capacity < size()) {
      capacity = size();
    }

    auto unroundedLimit = max_size();
    std::size_t exactLimit = Chunk::kDesiredCapacity;
    while (exactLimit <= unroundedLimit / 2) {
      exactLimit *= 2;
    }
    if (UNLIKELY(capacity > exactLimit)) {
      throw_exception<std::bad_alloc>();
    }

    std::size_t const kInitialCapacity = 2;
    std::size_t const kHalfChunkCapacity =
        (Chunk::kDesiredCapacity / 2) & ~std::size_t{1};
    std::size_t maxSizeWithoutRehash;
    std::size_t chunkCount;
    if (capacity <= kInitialCapacity) {
      chunkCount = 1;
      maxSizeWithoutRehash = kInitialCapacity;
    } else if (capacity <= kHalfChunkCapacity) {
      chunkCount = 1;
      maxSizeWithoutRehash = kHalfChunkCapacity;
    } else {
      chunkCount = 1;
      while (chunkCount * Chunk::kDesiredCapacity < capacity) {
        chunkCount *= 2;
      }
      maxSizeWithoutRehash = chunkCount * Chunk::kDesiredCapacity;
    }
    if (bucket_count() != maxSizeWithoutRehash) {
      rehashImpl(chunkCount, maxSizeWithoutRehash);
    }
  }

  void reserve(std::size_t capacity) {
    rehash(capacity);
  }

  // Returns true iff a rehash was performed
  void reserveForInsert(size_t incoming = 1) {
    if (size() + incoming - 1 >= bucket_count()) {
      reserveForInsertImpl(incoming);
    }
  }

  FOLLY_NOINLINE void reserveForInsertImpl(size_t incoming) {
    rehash(size() + incoming);
  }

  // Returns pos,true if construct, pos,false if found.  key is only used
  // during the search; all constructor args for an inserted value come
  // from args...  key won't be accessed after args are touched.
  template <typename K, typename... Args>
  std::pair<ItemIter, bool> tryEmplaceValue(K const& key, Args&&... args) {
    const auto hp = splitHash(this->computeKeyHash(key));

    auto existing = findImpl(hp, key);
    if (!existing.atEnd()) {
      return std::make_pair(existing, false);
    }

    reserveForInsert();

    std::size_t index = hp.first;
    ChunkPtr chunk = chunks_ + (index & chunkMask_);
    auto firstEmpty = chunk->firstEmpty();

    if (!firstEmpty.hasIndex()) {
      std::size_t delta = probeDelta(hp);
      do {
        chunk->incrOutboundOverflowCount();
        index += delta;
        chunk = chunks_ + (index & chunkMask_);
        firstEmpty = chunk->firstEmpty();
      } while (!firstEmpty.hasIndex());
      chunk->adjustHostedOverflowCount(Chunk::kIncrHostedOverflowCount);
    }
    std::size_t itemIndex = firstEmpty.index();

    chunk->setTag(itemIndex, hp.second);
    ItemIter iter{chunk, itemIndex};

    // insertAtBlank will clear the tag if the constructor throws
    insertAtBlank(iter, hp, std::forward<Args>(args)...);
    return std::make_pair(iter, true);
  }

 private:
  template <bool Reset>
  void clearImpl() noexcept {
    if (chunks_ == Chunk::emptyInstance()) {
      FOLLY_SAFE_DCHECK(empty() && bucket_count() == 0, "");
      return;
    }

    // turn clear into reset if the table is >= 16 chunks so that
    // we don't get too low a load factor
    bool willReset = Reset || chunkMask_ + 1 >= 16;

    if (willReset) {
      this->beforeReset(size(), bucket_count());
    } else {
      this->beforeClear(size(), bucket_count());
    }

    if (!empty()) {
      if (Policy::destroyItemOnClear()) {
        for (std::size_t ci = 0; ci <= chunkMask_; ++ci) {
          ChunkPtr chunk = chunks_ + ci;
          auto iter = chunk->occupiedIter();
          if (Policy::prefetchBeforeDestroy()) {
            for (auto piter = iter; piter.hasNext();) {
              this->prefetchValue(chunk->item(piter.next()));
            }
          }
          while (iter.hasNext()) {
            this->destroyItem(chunk->item(iter.next()));
          }
        }
      }
      if (!willReset) {
        // It's okay to do this in a separate loop because we only do it
        // when the chunk count is small.  That avoids a branch when we
        // are promoting a clear to a reset for a large table.
        auto c0c = chunks_[0].chunk0Capacity();
        for (std::size_t ci = 0; ci <= chunkMask_; ++ci) {
          chunks_[ci].clear();
        }
        chunks_[0].markEof(c0c);
      }
      if (Policy::kEnableItemIteration) {
        sizeAndPackedBegin_.packedBegin() = ItemIter{}.pack();
      }
      sizeAndPackedBegin_.size_ = 0;
    }

    if (willReset) {
      deleteChunks(chunks_, chunkMask_ + 1, bucket_count());
      chunks_ = Chunk::emptyInstance();
      chunkMask_ = 0;

      this->afterReset();
    } else {
      this->afterClear(bucket_count());
    }
  }

  void eraseImpl(ItemIter pos, HashPair hp) {
    this->destroyItem(pos.item());
    adjustSizeAndBeginBeforeErase(pos);
    eraseBlank(pos, hp);
  }

 public:
  // The item needs to still be hashable during this call.  If you want
  // to intercept the value before it is destroyed (to extract it, for
  // example), use eraseInto(pos, beforeDestroy).
  void erase(ItemIter pos) {
    eraseInto(pos, [](value_type&&) {});
  }

  // The item needs to still be hashable during this call.  If you want
  // to intercept the value before it is destroyed (to extract it, for
  // example), do so in the beforeDestroy callback.
  template <typename BeforeDestroy>
  void eraseInto(ItemIter pos, BeforeDestroy&& beforeDestroy) {
    HashPair hp{};
    if (pos.chunk()->hostedOverflowCount() != 0) {
      hp = splitHash(this->computeItemHash(pos.citem()));
    }
    beforeDestroy(this->valueAtItemForExtract(pos.item()));
    eraseImpl(pos, hp);
  }

  template <typename K>
  std::size_t erase(K const& key) {
    return eraseInto(key, [](value_type&&) {});
  }

  template <typename K, typename BeforeDestroy>
  std::size_t eraseInto(K const& key, BeforeDestroy&& beforeDestroy) {
    if (UNLIKELY(size() == 0)) {
      return 0;
    }
    auto hp = splitHash(this->computeKeyHash(key));
    auto iter = findImpl(hp, key);
    if (!iter.atEnd()) {
      beforeDestroy(this->valueAtItemForExtract(iter.item()));
      eraseImpl(iter, hp);
      return 1;
    } else {
      return 0;
    }
  }

  void clear() noexcept {
    clearImpl<false>();
  }

  // Like clear(), but always frees all dynamic storage allocated
  // by the table.
  void reset() noexcept {
    clearImpl<true>();
  }

  // Get memory footprint, not including sizeof(*this).
  std::size_t getAllocatedMemorySize() const {
    std::size_t sum = 0;
    visitAllocationClasses(
        [&sum](std::size_t bytes, std::size_t n) { sum += bytes * n; });
    return sum;
  }

  // Enumerates classes of allocated memory blocks currently owned
  // by this table, calling visitor(allocationSize, allocationCount).
  // This can be used to get a more accurate indication of memory footprint
  // than getAllocatedMemorySize() if you have some way of computing the
  // internal fragmentation of the allocator, such as JEMalloc's nallocx.
  // The visitor might be called twice with the same allocationSize. The
  // visitor's computation should produce the same result for visitor(8,
  // 2) as for two calls to visitor(8, 1), for example.  The visitor may
  // be called with a zero allocationCount.
  template <typename V>
  void visitAllocationClasses(V&& visitor) const {
    auto bc = bucket_count();
    if (bc != 0) {
      visitor(allocSize(chunkMask_ + 1, bc), 1);
    }
    this->visitPolicyAllocationClasses(size(), bc, visitor);
  }

 private:
  static std::size_t& histoAt(
      std::vector<std::size_t>& histo,
      std::size_t index) {
    if (histo.size() <= index) {
      histo.resize(index + 1);
    }
    return histo.at(index);
  }

 public:
  // Expensive
  F14TableStats computeStats() const {
    F14TableStats stats;

    if (folly::kIsDebug && Policy::kEnableItemIteration) {
      // validate iteration
      std::size_t n = 0;
      ItemIter prev;
      for (auto iter = begin(); iter != end(); iter.advance()) {
        FOLLY_SAFE_DCHECK(n == 0 || iter.pack() < prev.pack(), "");
        ++n;
        prev = iter;
      }
      FOLLY_SAFE_DCHECK(n == size(), "");
    }

    FOLLY_SAFE_DCHECK(
        (chunks_ == Chunk::emptyInstance()) == (bucket_count() == 0), "");

    std::size_t n1 = 0;
    std::size_t n2 = 0;
    auto cc = bucket_count() == 0 ? 0 : chunkMask_ + 1;
    for (std::size_t ci = 0; ci < cc; ++ci) {
      ChunkPtr chunk = chunks_ + ci;
      FOLLY_SAFE_DCHECK(chunk->eof() == (ci == 0), "");

      auto iter = chunk->occupiedIter();

      std::size_t chunkOccupied = 0;
      for (auto piter = iter; piter.hasNext(); piter.next()) {
        ++chunkOccupied;
      }
      n1 += chunkOccupied;

      histoAt(stats.chunkOccupancyHisto, chunkOccupied)++;
      histoAt(
          stats.chunkOutboundOverflowHisto, chunk->outboundOverflowCount())++;
      histoAt(stats.chunkHostedOverflowHisto, chunk->hostedOverflowCount())++;

      while (iter.hasNext()) {
        auto ii = iter.next();
        ++n2;

        {
          auto& item = chunk->citem(ii);
          auto hp = splitHash(this->computeItemHash(item));
          FOLLY_SAFE_DCHECK(chunk->tag(ii) == hp.second, "");

          std::size_t dist = 1;
          std::size_t index = hp.first;
          std::size_t delta = probeDelta(hp);
          while ((index & chunkMask_) != ci) {
            index += delta;
            ++dist;
          }

          histoAt(stats.keyProbeLengthHisto, dist)++;
        }

        // misses could have any tag, so we do the dumb but accurate
        // thing and just try them all
        for (std::size_t ti = 0; ti < 256; ++ti) {
          uint8_t tag = static_cast<uint8_t>(ti == 0 ? 1 : 0);
          HashPair hp{ci, tag};

          std::size_t dist = 1;
          std::size_t index = hp.first;
          std::size_t delta = probeDelta(hp);
          for (std::size_t tries = 0; tries <= chunkMask_ &&
               chunks_[index & chunkMask_].outboundOverflowCount() != 0;
               ++tries) {
            index += delta;
            ++dist;
          }

          histoAt(stats.missProbeLengthHisto, dist)++;
        }
      }
    }

    FOLLY_SAFE_DCHECK(n1 == size(), "");
    FOLLY_SAFE_DCHECK(n2 == size(), "");

    stats.policy = typeid(Policy).name();
    stats.size = size();
    stats.valueSize = sizeof(value_type);
    stats.bucketCount = bucket_count();
    stats.chunkCount = cc;

    stats.totalBytes = sizeof(*this) + getAllocatedMemorySize();
    stats.overheadBytes = stats.totalBytes - size() * sizeof(value_type);

    return stats;
  }
};
} // namespace detail
} // namespace f14

#endif // FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

} // namespace folly
