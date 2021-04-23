#pragma once

#include "common/network/socket_interface.h"
#include "test/integration/filters/test_socket_interface.h"

namespace Envoy {

// TODO(kbaichoo): comment class.
class SocketInterfaceSwap {
public:
  // Object of this class hold the state determining the IoHandle which
  // should return EAGAIN from the `writev` call.
  struct IoHandleMatcher {
    bool shouldReturnEgain(uint32_t src_port, uint32_t dst_port) const {
      absl::ReaderMutexLock lock(&mutex_);
      return writev_returns_egain_ && (src_port == src_port_ || dst_port == dst_port_);
    }

    // Source port to match. The port specified should be associated with a listener.
    void setSourcePort(uint32_t port) {
      absl::WriterMutexLock lock(&mutex_);
      src_port_ = port;
    }

    // Destination port to match. The port specified should be associated with a listener.
    void setDestinationPort(uint32_t port) {
      absl::WriterMutexLock lock(&mutex_);
      dst_port_ = port;
    }

    void setWritevReturnsEgain() {
      absl::WriterMutexLock lock(&mutex_);
      ASSERT(src_port_ != 0 || dst_port_ != 0);
      writev_returns_egain_ = true;
    }

  private:
    mutable absl::Mutex mutex_;
    uint32_t src_port_ ABSL_GUARDED_BY(mutex_) = 0;
    uint32_t dst_port_ ABSL_GUARDED_BY(mutex_) = 0;
    bool writev_returns_egain_ ABSL_GUARDED_BY(mutex_) = false;
  };

  SocketInterfaceSwap();

  ~SocketInterfaceSwap() {
    test_socket_interface_loader_.reset();
    Envoy::Network::SocketInterfaceSingleton::initialize(previous_socket_interface_);
  }

protected:
  Envoy::Network::SocketInterface* const previous_socket_interface_{
      Envoy::Network::SocketInterfaceSingleton::getExisting()};
  std::shared_ptr<IoHandleMatcher> writev_matcher_{std::make_shared<IoHandleMatcher>()};
  std::unique_ptr<Envoy::Network::SocketInterfaceLoader> test_socket_interface_loader_;
};

} // namespace Envoy
