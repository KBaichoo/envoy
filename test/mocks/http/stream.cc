#include "test/mocks/http/stream.h"

using testing::_;
using testing::Invoke;
using testing::ReturnRef;

namespace Envoy {
namespace Http {

MockStream::MockStream() {
  ON_CALL(*this, addCallbacks(_)).WillByDefault(Invoke([this](StreamCallbacks& callbacks) -> void {
    std::cerr << "Adding callback: " << &callbacks << " on stream:" << this << std::endl;
    callbacks_.push_back(&callbacks);
  }));

  ON_CALL(*this, removeCallbacks(_))
      .WillByDefault(Invoke([this](StreamCallbacks& callbacks) -> void {
        std::cerr << "Removing callback: " << &callbacks << " on stream: " << this << std::endl;
        for (auto& callback : callbacks_) {
          if (callback == &callbacks) {
            callback = nullptr;
            return;
          }
        }
      }));

  ON_CALL(*this, resetStream(_)).WillByDefault(Invoke([this](StreamResetReason reason) -> void {
    std::cerr << "Mock Reset Stream has " << callbacks_.size() << " items on stream:" << this
              << std::endl;
    for (StreamCallbacks* callback : callbacks_) {
      // TODO(kbaichoo): do I need this?? Might not.
      std::cerr << "Callback: " << callback << std::endl;
      if (callback) {
        callback->onResetStream(reason, absl::string_view());
      }
      std::cerr << "Ran Callback: " << callback << std::endl;
    }

    callbacks_.clear();
  }));

  ON_CALL(*this, connectionLocalAddress()).WillByDefault(ReturnRef(connection_local_address_));

  ON_CALL(*this, setAccount(_))
      .WillByDefault(Invoke(
          [this](Buffer::BufferMemoryAccountSharedPtr account) -> void { account_ = account; }));
}

MockStream::~MockStream() {
  if (account_) {
    account_->clearDownstream();
  }

  std::cerr << "~MockStream: " << this << std::endl;
  for (StreamCallbacks* callback : callbacks_) {
    std::cerr << "Callback: " << callback << std::endl;
    if (callback) {
      callback->onCloseCodecStream();
    }
    std::cerr << "Ran Callback: " << callback << std::endl;
  }

  callbacks_.clear();
}

} // namespace Http
} // namespace Envoy
