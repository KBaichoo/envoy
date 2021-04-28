#pragma once

#include "common/buffer/buffer_impl.h"
#include "common/buffer/watermark_buffer.h"

#include "absl/container/node_hash_map.h"
#include "absl/synchronization/mutex.h"
#include "envoy/buffer/buffer.h"

namespace Envoy {
namespace Buffer {

// WatermarkBuffer subclass that hooks into updates to buffer size and buffer high watermark config.
class TrackedWatermarkBuffer : public Buffer::WatermarkBuffer {
public:
  TrackedWatermarkBuffer(
      std::function<void(uint64_t current_size)> update_size,
      std::function<void(uint32_t watermark)> update_high_watermark,
      std::function<void(TrackedWatermarkBuffer*)> on_delete,
      std::function<void(BufferMemoryAccountSharedPtr&, TrackedWatermarkBuffer*)> on_bind,
      std::function<void()> below_low_watermark, std::function<void()> above_high_watermark,
      std::function<void()> above_overflow_watermark)
      : WatermarkBuffer(below_low_watermark, above_high_watermark, above_overflow_watermark),
        update_size_(update_size), update_high_watermark_(update_high_watermark),
        on_delete_(on_delete), on_bind_(on_bind) {}
  ~TrackedWatermarkBuffer() override { on_delete_(this); }

  void setWatermarks(uint32_t watermark) override {
    update_high_watermark_(watermark);
    WatermarkBuffer::setWatermarks(watermark);
  }

  void bindAccount(BufferMemoryAccountSharedPtr account) override {
    on_bind_(account, this);
    WatermarkBuffer::bindAccount(account);
  }

protected:
  void checkHighAndOverflowWatermarks() override {
    update_size_(length());
    WatermarkBuffer::checkHighAndOverflowWatermarks();
  }

  void checkLowWatermark() override {
    update_size_(length());
    WatermarkBuffer::checkLowWatermark();
  }

private:
  std::function<void(uint64_t current_size)> update_size_;
  std::function<void(uint32_t)> update_high_watermark_;
  std::function<void(TrackedWatermarkBuffer*)> on_delete_;
  std::function<void(BufferMemoryAccountSharedPtr&, TrackedWatermarkBuffer*)> on_bind_;
};

// Factory that tracks how the created buffers are used.
class TrackedWatermarkBufferFactory : public Buffer::WatermarkFactory {
public:
  TrackedWatermarkBufferFactory() = default;
  ~TrackedWatermarkBufferFactory() override;
  // Buffer::WatermarkFactory
  Buffer::InstancePtr create(std::function<void()> below_low_watermark,
                             std::function<void()> above_high_watermark,
                             std::function<void()> above_overflow_watermark) override;

  // Number of buffers created.
  uint64_t numBuffersCreated() const;
  // Number of buffers still in use.
  uint64_t numBuffersActive() const;
  // Total bytes buffered.
  uint64_t totalBufferSize() const;
  // Size of the largest buffer.
  uint64_t maxBufferSize() const;
  // Sum of the max size of all known buffers.
  uint64_t sumMaxBufferSizes() const;
  // Get lower and upper bound on buffer high watermarks. A watermark of 0 indicates that watermark
  // functionality is disabled.
  std::pair<uint32_t, uint32_t> highWatermarkRange() const;

  // Total bytes currently buffered across all known buffers.
  uint64_t totalBytesBuffered() const {
    absl::MutexLock lock(&mutex_);
    return total_buffer_size_;
  }

  // Wait until total bytes buffered exceeds the a given size.
  bool waitUntilTotalBufferedExceeds(uint64_t byte_size, std::chrono::milliseconds timeout);

  bool waitUntilEachAccountChargedAtleast(uint64_t byte_size, uint32_t expected_num_accounts,
                                          std::chrono::milliseconds timeout);

  // Number of accounts bound to a buffer that's still in use.
  uint64_t numAccountsActive();
  // Number of active buffers that had a call to bind.
  uint64_t numBuffersActivelyBound() const;

  using AccountToBoundBuffersMap =
      absl::flat_hash_map<BufferMemoryAccountSharedPtr,
                          absl::flat_hash_set<TrackedWatermarkBuffer*>>;
  void inspectAccounts(std::function<void(AccountToBoundBuffersMap&)> func) {
    absl::MutexLock lock(&mutex_);
    func(account_infos_);
  }

private:
  // Remove "dangling" accounts; accounts where the account_info map is the only
  // entity still pointing to the account.
  void removeDanglingAccounts() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  struct BufferInfo {
    uint32_t watermark_ = 0;
    uint64_t current_size_ = 0;
    uint64_t max_size_ = 0;
  };

  mutable absl::Mutex mutex_;
  // Id of the next buffer to create.
  uint64_t next_idx_ ABSL_GUARDED_BY(mutex_) = 0;
  // Number of buffers currently in existence.
  uint64_t active_buffer_count_ ABSL_GUARDED_BY(mutex_) = 0;
  // total bytes buffered across all buffers.
  uint64_t total_buffer_size_ ABSL_GUARDED_BY(mutex_) = 0;
  // Info about the buffer, by buffer idx.
  absl::node_hash_map<uint64_t, BufferInfo> buffer_infos_ ABSL_GUARDED_BY(mutex_);
  // Map from accounts to buffers bound to that account.
  AccountToBoundBuffersMap account_infos_ ABSL_GUARDED_BY(mutex_);
  // Set of actively bound buffers. Used for asserting that buffers are bound
  // only once.
  absl::flat_hash_set<TrackedWatermarkBuffer*> actively_bound_buffers_ ABSL_GUARDED_BY(mutex_);
};

} // namespace Buffer
} // namespace Envoy
