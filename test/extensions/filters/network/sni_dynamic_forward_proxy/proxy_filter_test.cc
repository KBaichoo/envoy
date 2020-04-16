#include "envoy/extensions/filters/network/sni_dynamic_forward_proxy/v3alpha/sni_dynamic_forward_proxy.pb.h"
#include "envoy/network/connection.h"

#include "extensions/filters/network/sni_dynamic_forward_proxy/proxy_filter.h"
#include "extensions/filters/network/well_known_names.h"

#include "test/extensions/common/dynamic_forward_proxy/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/mocks/upstream/transport_socket_match.h"

using testing::AtLeast;
using testing::Eq;
using testing::Return;
using testing::ReturnRef;

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SniDynamicForwardProxy {
namespace {

using LoadDnsCacheEntryStatus = Common::DynamicForwardProxy::DnsCache::LoadDnsCacheEntryStatus;
using MockLoadDnsCacheEntryResult =
    Common::DynamicForwardProxy::MockDnsCache::MockLoadDnsCacheEntryResult;

class ProxyFilterTest : public testing::Test,
                        public Extensions::Common::DynamicForwardProxy::DnsCacheManagerFactory {
public:
  ProxyFilterTest() {
    FilterConfig proto_config;
    proto_config.set_port_value(443);
    EXPECT_CALL(*dns_cache_manager_, getCache(_));
    filter_config_ = std::make_shared<ProxyFilterConfig>(proto_config, *this, cm_);
    filter_ = std::make_unique<ProxyFilter>(filter_config_);
    filter_->initializeReadFilterCallbacks(callbacks_);

    // Allow for an otherwise strict mock.
    ON_CALL(callbacks_, connection()).WillByDefault(ReturnRef(connection_));
    EXPECT_CALL(callbacks_, connection()).Times(AtLeast(0));

    // Configure max pending to 1 so we can test circuit breaking.
    // TODO(lizan): implement circuit breaker in SNI dynamic forward proxy
    cm_.thread_local_cluster_.cluster_.info_->resetResourceManager(0, 1, 0, 0, 0);
  }

  ~ProxyFilterTest() override {
    EXPECT_TRUE(
        cm_.thread_local_cluster_.cluster_.info_->resource_manager_->pendingRequests().canCreate());
  }

  Extensions::Common::DynamicForwardProxy::DnsCacheManagerSharedPtr get() override {
    return dns_cache_manager_;
  }

  std::shared_ptr<Extensions::Common::DynamicForwardProxy::MockDnsCacheManager> dns_cache_manager_{
      new Extensions::Common::DynamicForwardProxy::MockDnsCacheManager()};
  Upstream::MockClusterManager cm_;
  ProxyFilterConfigSharedPtr filter_config_;
  std::unique_ptr<ProxyFilter> filter_;
  Network::MockReadFilterCallbacks callbacks_;
  Network::MockConnection connection_;
};

// No SNI handling.
TEST_F(ProxyFilterTest, NoSNI) {
  EXPECT_CALL(connection_, requestedServerName()).WillRepeatedly(Return(""));
  EXPECT_EQ(Network::FilterStatus::Continue, filter_->onNewConnection());
}

TEST_F(ProxyFilterTest, LoadDnsCache) {
  EXPECT_CALL(connection_, requestedServerName()).WillRepeatedly(Return("foo"));
  Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle* handle =
      new Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle();
  EXPECT_CALL(*dns_cache_manager_->dns_cache_, loadDnsCacheEntry_(Eq("foo"), 443, _))
      .WillOnce(Return(MockLoadDnsCacheEntryResult{LoadDnsCacheEntryStatus::Loading, handle}));
  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onNewConnection());

  EXPECT_CALL(callbacks_, continueReading());
  filter_->onLoadDnsCacheComplete();

  EXPECT_CALL(*handle, onDestroy());
}

TEST_F(ProxyFilterTest, LoadDnsInCache) {
  EXPECT_CALL(connection_, requestedServerName()).WillRepeatedly(Return("foo"));
  EXPECT_CALL(*dns_cache_manager_->dns_cache_, loadDnsCacheEntry_(Eq("foo"), 443, _))
      .WillOnce(Return(MockLoadDnsCacheEntryResult{LoadDnsCacheEntryStatus::InCache, nullptr}));

  EXPECT_EQ(Network::FilterStatus::Continue, filter_->onNewConnection());
}

// Cache overflow.
TEST_F(ProxyFilterTest, CacheOverflow) {
  EXPECT_CALL(connection_, requestedServerName()).WillRepeatedly(Return("foo"));
  EXPECT_CALL(*dns_cache_manager_->dns_cache_, loadDnsCacheEntry_(Eq("foo"), 443, _))
      .WillOnce(Return(MockLoadDnsCacheEntryResult{LoadDnsCacheEntryStatus::Overflow, nullptr}));
  EXPECT_CALL(connection_, close(Network::ConnectionCloseType::NoFlush));
  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onNewConnection());
}

} // namespace

} // namespace SniDynamicForwardProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
