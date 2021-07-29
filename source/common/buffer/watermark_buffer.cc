#include "source/common/buffer/watermark_buffer.h"

#include <cstdint>
#include <memory>

#include "envoy/buffer/buffer.h"

#include "source/common/common/assert.h"
#include "source/common/common/logger.h"
#include "source/common/runtime/runtime_features.h"

namespace Envoy {
namespace Buffer {
namespace {

// TODO(kbaichoo): remove when msvc supports absl::bit_width as constexpr.
#ifndef WIN32
constexpr uint32_t kDefaultMinimumTrackingBytes = absl::bit_width(uint32_t(1024 * 256)) - 1;
#else
constexpr uint32_t kDefaultMinimumTrackingBytes = 18; // Computed from the expression above.
#endif
} // end namespace

void WatermarkBuffer::add(const void* data, uint64_t size) {
  OwnedImpl::add(data, size);
  checkHighAndOverflowWatermarks();
}

void WatermarkBuffer::add(absl::string_view data) {
  OwnedImpl::add(data);
  checkHighAndOverflowWatermarks();
}

void WatermarkBuffer::add(const Instance& data) {
  OwnedImpl::add(data);
  checkHighAndOverflowWatermarks();
}

void WatermarkBuffer::prepend(absl::string_view data) {
  OwnedImpl::prepend(data);
  checkHighAndOverflowWatermarks();
}

void WatermarkBuffer::prepend(Instance& data) {
  OwnedImpl::prepend(data);
  checkHighAndOverflowWatermarks();
}

void WatermarkBuffer::commit(uint64_t length, absl::Span<RawSlice> slices,
                             ReservationSlicesOwnerPtr slices_owner) {
  OwnedImpl::commit(length, slices, std::move(slices_owner));
  checkHighAndOverflowWatermarks();
}

void WatermarkBuffer::drain(uint64_t size) {
  OwnedImpl::drain(size);
  checkLowWatermark();
}

void WatermarkBuffer::move(Instance& rhs) {
  OwnedImpl::move(rhs);
  checkHighAndOverflowWatermarks();
}

void WatermarkBuffer::move(Instance& rhs, uint64_t length) {
  OwnedImpl::move(rhs, length);
  checkHighAndOverflowWatermarks();
}

SliceDataPtr WatermarkBuffer::extractMutableFrontSlice() {
  auto result = OwnedImpl::extractMutableFrontSlice();
  checkLowWatermark();
  return result;
}

// Adjust the reservation size based on space available before hitting
// the high watermark to avoid overshooting by a lot and thus violating the limits
// the watermark is imposing.
Reservation WatermarkBuffer::reserveForRead() {
  constexpr auto preferred_length = default_read_reservation_size_;
  uint64_t adjusted_length = preferred_length;

  if (high_watermark_ > 0 && preferred_length > 0) {
    const uint64_t current_length = OwnedImpl::length();
    if (current_length >= high_watermark_) {
      // Always allow a read of at least some data. The API doesn't allow returning
      // a zero-length reservation.
      adjusted_length = Slice::default_slice_size_;
    } else {
      const uint64_t available_length = high_watermark_ - current_length;
      adjusted_length = IntUtil::roundUpToMultiple(available_length, Slice::default_slice_size_);
      adjusted_length = std::min(adjusted_length, preferred_length);
    }
  }

  return OwnedImpl::reserveWithMaxLength(adjusted_length);
}

void WatermarkBuffer::appendSliceForTest(const void* data, uint64_t size) {
  OwnedImpl::appendSliceForTest(data, size);
  checkHighAndOverflowWatermarks();
}

void WatermarkBuffer::appendSliceForTest(absl::string_view data) {
  appendSliceForTest(data.data(), data.size());
}

void WatermarkBuffer::setWatermarks(uint32_t high_watermark) {
  uint32_t overflow_watermark_multiplier =
      Runtime::getInteger("envoy.buffer.overflow_multiplier", 0);
  if (overflow_watermark_multiplier > 0 &&
      (static_cast<uint64_t>(overflow_watermark_multiplier) * high_watermark) >
          std::numeric_limits<uint32_t>::max()) {
    ENVOY_LOG_MISC(debug, "Error setting overflow threshold: envoy.buffer.overflow_multiplier * "
                          "high_watermark is overflowing. Disabling overflow watermark.");
    overflow_watermark_multiplier = 0;
  }
  low_watermark_ = high_watermark / 2;
  high_watermark_ = high_watermark;
  overflow_watermark_ = overflow_watermark_multiplier * high_watermark;
  checkHighAndOverflowWatermarks();
  checkLowWatermark();
}

void WatermarkBuffer::checkLowWatermark() {
  if (!above_high_watermark_called_ ||
      (high_watermark_ != 0 && OwnedImpl::length() > low_watermark_)) {
    return;
  }

  above_high_watermark_called_ = false;
  below_low_watermark_();
}

void WatermarkBuffer::checkHighAndOverflowWatermarks() {
  if (high_watermark_ == 0 || OwnedImpl::length() <= high_watermark_) {
    return;
  }

  if (!above_high_watermark_called_) {
    above_high_watermark_called_ = true;
    above_high_watermark_();
  }

  // Check if overflow watermark is enabled, wasn't previously triggered,
  // and the buffer size is above the threshold
  if (overflow_watermark_ != 0 && !above_overflow_watermark_called_ &&
      OwnedImpl::length() > overflow_watermark_) {
    above_overflow_watermark_called_ = true;
    above_overflow_watermark_();
  }
}

BufferMemoryAccountSharedPtr
WatermarkBufferFactory::createAccount(Http::StreamResetHandler& reset_handler) {
  return BufferMemoryAccountImpl::createAccount(this, reset_handler);
}

void WatermarkBufferFactory::updateAccountClass(const BufferMemoryAccountSharedPtr& account,
                                                absl::optional<uint32_t> current_class,
                                                absl::optional<uint32_t> new_class) {
  ASSERT(current_class != new_class, "Expected the current_class and new_class to be different");

  if (!current_class.has_value() && new_class >= 0u) {
    // Start tracking
    ASSERT(!size_class_account_sets_[new_class.value()].contains(account));
    size_class_account_sets_[new_class.value()].insert(account);
  } else if (current_class >= 0u && !new_class.has_value()) {
    // No longer track
    ASSERT(size_class_account_sets_[current_class.value()].contains(account));
    size_class_account_sets_[current_class.value()].erase(account);
  } else {
    // Moving between buckets
    ASSERT(size_class_account_sets_[current_class.value()].contains(account));
    ASSERT(!size_class_account_sets_[new_class.value()].contains(account));
    size_class_account_sets_[new_class.value()].insert(
        std::move(size_class_account_sets_[current_class.value()].extract(account).value()));
  }
}

void WatermarkBufferFactory::unregisterAccount(const BufferMemoryAccountSharedPtr& account,
                                               absl::optional<uint32_t> current_class) {
  if (current_class.has_value()) {
    ASSERT(size_class_account_sets_[current_class.value()].contains(account));
    size_class_account_sets_[current_class.value()].erase(account);
  }
}

void WatermarkBufferFactory::resetAllAccountsInBucketsStartingWith(uint32_t bucket_idx) {
  ASSERT(bucket_idx >= 0 && bucket_idx < BufferMemoryAccountImpl::NUM_MEMORY_CLASSES_,
         "Provided bucket index is out of range.");
  while (bucket_idx < BufferMemoryAccountImpl::NUM_MEMORY_CLASSES_) {
    // TODO(kbaichoo): error -> info; error rn for debug purposes.
    ENVOY_LOG_MISC(error, "resetting {} streams in bucket {}.",
                   size_class_account_sets_[bucket_idx].size(), bucket_idx);

    auto it = size_class_account_sets_[bucket_idx].begin();
    while (it != size_class_account_sets_[bucket_idx].end()) {
      auto next = std::next(it);
      // This will trigger an erase, which avoids rehashing and invalidates the
      // iterator *it*. *next* is still valid.
      (*it)->resetDownstream();
      it = next;
    }

    ++bucket_idx;
  }
}

WatermarkBufferFactory::WatermarkBufferFactory(
    const envoy::config::bootstrap::v3::BufferFactoryConfig& config)
    : bitshift_(config.account_tracking_threshold_bytes()
                    ? absl::bit_width(config.account_tracking_threshold_bytes() - 1)
                    : kDefaultMinimumTrackingBytes) {
  RELEASE_ASSERT(config.account_tracking_threshold_bytes() == 0 ||
                     (config.account_tracking_threshold_bytes() &
                      (config.account_tracking_threshold_bytes() - 1)) == 0,
                 "Expected account_tracking_threshold_bytes to be a power of two.");
}

WatermarkBufferFactory::~WatermarkBufferFactory() {
  for (auto& account_set : size_class_account_sets_) {
    ASSERT(account_set.empty(),
           "Expected all Accounts to have unregistered from the Watermark Factory.");
  }
}

BufferMemoryAccountSharedPtr
BufferMemoryAccountImpl::createAccount(WatermarkBufferFactory* factory,
                                       Http::StreamResetHandler& reset_handler) {
  // We use shared_ptr ctor directly rather than make shared since the
  // constructor being invoked is private as we want users to use this static
  // method to createAccounts.
  auto account =
      std::shared_ptr<BufferMemoryAccount>(new BufferMemoryAccountImpl(factory, reset_handler));
  // Set shared_this_ in the account.
  static_cast<BufferMemoryAccountImpl*>(account.get())->shared_this_ = account;
  return account;
}

absl::optional<uint32_t> BufferMemoryAccountImpl::balanceToClassIndex() {
  static uint32_t bitshift = factory_->bitshift();
  uint64_t shifted_balance = buffer_memory_allocated_ >> bitshift;

  if (shifted_balance == 0) {
    return {}; // Not worth tracking anything < configured minimum threshold
  }

  const int class_idx = absl::bit_width(shifted_balance) - 1;
  return std::min<uint32_t>(class_idx, NUM_MEMORY_CLASSES_ - 1);
}

void BufferMemoryAccountImpl::updateAccountClass() {
  auto new_class = balanceToClassIndex();
  if (shared_this_ && new_class != current_bucket_idx_) {
    factory_->updateAccountClass(shared_this_, current_bucket_idx_, new_class);
    current_bucket_idx_ = new_class;
  }
}

void BufferMemoryAccountImpl::credit(uint64_t amount) {
  ASSERT(buffer_memory_allocated_ >= amount);
  buffer_memory_allocated_ -= amount;
  updateAccountClass();
}

void BufferMemoryAccountImpl::charge(uint64_t amount) {
  // Check overflow
  ASSERT(std::numeric_limits<uint64_t>::max() - buffer_memory_allocated_ >= amount);
  buffer_memory_allocated_ += amount;
  updateAccountClass();
}

void BufferMemoryAccountImpl::clearDownstream() {
  if (reset_handler_.has_value()) {
    reset_handler_.reset();
    factory_->unregisterAccount(shared_this_, current_bucket_idx_);
    current_bucket_idx_.reset();
    shared_this_ = nullptr;
  }
}

} // namespace Buffer
} // namespace Envoy
