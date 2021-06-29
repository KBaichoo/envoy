#pragma once

#include <functional>
#include <string>

#include "envoy/buffer/buffer.h"

#include "source/common/buffer/buffer_impl.h"

namespace Envoy {
namespace Buffer {

// A subclass of OwnedImpl which does watermark validation.
// Each time the buffer is resized (written to or drained), the watermarks are checked. As the
// buffer size transitions from under the low watermark to above the high watermark, the
// above_high_watermark function is called one time. It will not be called again until the buffer
// is drained below the low watermark, at which point the below_low_watermark function is called.
// If the buffer size is above the overflow watermark, above_overflow_watermark is called.
// It is only called on the first time the buffer overflows.
class WatermarkBuffer : public OwnedImpl {
public:
  WatermarkBuffer(std::function<void()> below_low_watermark,
                  std::function<void()> above_high_watermark,
                  std::function<void()> above_overflow_watermark)
      : below_low_watermark_(below_low_watermark), above_high_watermark_(above_high_watermark),
        above_overflow_watermark_(above_overflow_watermark) {}

  // Override all functions from Instance which can result in changing the size
  // of the underlying buffer.
  void add(const void* data, uint64_t size) override;
  void add(absl::string_view data) override;
  void add(const Instance& data) override;
  void prepend(absl::string_view data) override;
  void prepend(Instance& data) override;
  void drain(uint64_t size) override;
  void move(Instance& rhs) override;
  void move(Instance& rhs, uint64_t length) override;
  SliceDataPtr extractMutableFrontSlice() override;
  Reservation reserveForRead() override;
  void postProcess() override { checkLowWatermark(); }
  void appendSliceForTest(const void* data, uint64_t size) override;
  void appendSliceForTest(absl::string_view data) override;

  void setWatermarks(uint32_t high_watermark) override;
  uint32_t highWatermark() const override { return high_watermark_; }
  // Returns true if the high watermark callbacks have been called more recently
  // than the low watermark callbacks.
  bool highWatermarkTriggered() const override { return above_high_watermark_called_; }

protected:
  virtual void checkHighAndOverflowWatermarks();
  virtual void checkLowWatermark();

private:
  void commit(uint64_t length, absl::Span<RawSlice> slices,
              ReservationSlicesOwnerPtr slices_owner) override;

  std::function<void()> below_low_watermark_;
  std::function<void()> above_high_watermark_;
  std::function<void()> above_overflow_watermark_;

  // Used for enforcing buffer limits (off by default). If these are set to non-zero by a call to
  // setWatermarks() the watermark callbacks will be called as described above.
  uint32_t high_watermark_{0};
  uint32_t low_watermark_{0};
  uint32_t overflow_watermark_{0};
  // Tracks the latest state of watermark callbacks.
  // True between the time above_high_watermark_ has been called until above_high_watermark_ has
  // been called.
  bool above_high_watermark_called_{false};
  // Set to true when above_overflow_watermark_ is called (and isn't cleared).
  bool above_overflow_watermark_called_{false};
};

using WatermarkBufferPtr = std::unique_ptr<WatermarkBuffer>;

class WatermarkBufferFactory;

/**
 * A BufferMemoryAccountImpl tracks allocated bytes across associated buffers and
 * slices that originate from those buffers, or are untagged and pass through an
 * associated buffer.
 *
 * This BufferMemoryAccount is produced by the *WatermarkBufferFactory*.
 */
class BufferMemoryAccountImpl : public BufferMemoryAccount {
public:
  // Used to create the account, and complete wiring with the factory
  // and shared_this_.
  static BufferMemoryAccountSharedPtr createAccount(WatermarkBufferFactory* factory,
                                                    Http::StreamResetHandler* reset_handler);
  ~BufferMemoryAccountImpl() override { ASSERT(buffer_memory_allocated_ == 0); }

  // Make not copyable
  BufferMemoryAccountImpl(const BufferMemoryAccountImpl&) = delete;
  BufferMemoryAccountImpl& operator=(const BufferMemoryAccountImpl&) = delete;

  // Make not movable.
  BufferMemoryAccountImpl(BufferMemoryAccountImpl&&) = delete;
  BufferMemoryAccountImpl& operator=(BufferMemoryAccountImpl&&) = delete;

  uint64_t balance() const override { return buffer_memory_allocated_; }
  void charge(uint64_t amount) override;
  void credit(uint64_t amount) override;

  void clearDownstream() override;

  void resetDownstream(Http::StreamResetReason reason) override {
    if (reset_handler_ != nullptr) {
      reset_handler_->resetStream(reason);
    }
  }

  // The number of memory classes the Account expects to exists.
  static constexpr uint32_t NUM_MEMORY_CLASSES_ = 8;

private:
  BufferMemoryAccountImpl(WatermarkBufferFactory* factory, Http::StreamResetHandler* reset_handler)
      : factory_(factory), reset_handler_(reset_handler) {}

  // Returns the class index based off of the buffer_memory_allocated_
  // This can differs with current_bucket_idx_ if buffer_memory_allocated_ was
  // just modified.
  // Returned class index range is [-1, NUM_MEMORY_CLASSES_).
  int balanceToClassIndex();

  uint64_t buffer_memory_allocated_ = 0;
  // Current bucket index where the account is being tracked in.
  int current_bucket_idx_ = -1;

  WatermarkBufferFactory* factory_ = nullptr;

  Http::StreamResetHandler* reset_handler_ = nullptr;
  // Keep a copy of the shared_ptr pointing to this account. We opted to go this
  // route rather than enable_shared_from_this to avoid wasteful atomic
  // operations e.g. when updating the tracking of the account.
  // This is set through the createAccount static method which is the only way to
  // instantiate an instance of this class. This should is cleared when
  // unregistering from the factory.
  BufferMemoryAccountSharedPtr shared_this_ = nullptr;
};

class WatermarkBufferFactory : public WatermarkFactory {
public:
  // Buffer::WatermarkFactory
  ~WatermarkBufferFactory() override;
  InstancePtr createBuffer(std::function<void()> below_low_watermark,
                           std::function<void()> above_high_watermark,
                           std::function<void()> above_overflow_watermark) override {
    return std::make_unique<WatermarkBuffer>(below_low_watermark, above_high_watermark,
                                             above_overflow_watermark);
  }

  BufferMemoryAccountSharedPtr createAccount(Http::StreamResetHandler* reset_handler) override;

  // Called by BufferMemoryAccountImpls created by the factory on account class
  // updated.
  void updateAccountClass(const BufferMemoryAccountSharedPtr& account, int current_class,
                          int new_class);

  // Unregister a buffer memory account.
  virtual void unregisterAccount(const BufferMemoryAccountSharedPtr& account, int current_class);

protected:
  // Enable subclasses to inspect the mapping.
  using MemoryClassesToAccountsSet = std::array<absl::flat_hash_set<BufferMemoryAccountSharedPtr>,
                                                BufferMemoryAccountImpl::NUM_MEMORY_CLASSES_>;
  MemoryClassesToAccountsSet size_class_account_sets_;
};

} // namespace Buffer
} // namespace Envoy
