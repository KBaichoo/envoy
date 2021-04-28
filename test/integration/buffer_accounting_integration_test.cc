
#include "envoy/config/bootstrap/v3/bootstrap.pb.h"
#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/extensions/filters/network/http_connection_manager/v3/http_connection_manager.pb.h"
#include "common/buffer/buffer_impl.h"
#include "fake_upstream.h"
#include "http_integration.h"
#include "socket_interface_swap.h"
#include "envoy/network/address.h"
#include "integration_stream_decoder.h"
#include "test/integration/tracked_watermark_buffer.h"
#include "test/integration/utility.h"
#include "test/mocks/http/mocks.h"
#include "gtest/gtest.h"
#include <sstream>
#include "test/integration/autonomous_upstream.h"

namespace Envoy {

class HttpBufferWatermarksTest : public SocketInterfaceSwap,
                                 public testing::TestWithParam<Network::Address::IpVersion>,
                                 public HttpIntegrationTest {
public:
  struct BufferParams {
    uint32_t connection_watermark;
    uint32_t downstream_h2_stream_window;
    uint32_t downstream_h2_conn_window;
    uint32_t upstream_h2_stream_window;
    uint32_t upstream_h2_conn_window;
  };

  // Configures the buffers with the given parameters.
  void initializeWithBufferConfig(const BufferParams& buffer_params, uint32_t num_responses) {
    config_helper_.setBufferLimits(buffer_params.connection_watermark,
                                   buffer_params.connection_watermark);

    config_helper_.addConfigModifier(
        [&](envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager&
                hcm) -> void {
          auto* h2_options = hcm.mutable_http2_protocol_options();
          h2_options->mutable_max_concurrent_streams()->set_value(num_responses);
          h2_options->mutable_initial_stream_window_size()->set_value(
              buffer_params.downstream_h2_stream_window);
          h2_options->mutable_initial_connection_window_size()->set_value(
              buffer_params.downstream_h2_conn_window);
        });

    config_helper_.addConfigModifier(
        [&](envoy::config::bootstrap::v3::Bootstrap& bootstrap) -> void {
          ConfigHelper::HttpProtocolOptions protocol_options;
          auto* upstream_h2_options =
              protocol_options.mutable_explicit_http_config()->mutable_http2_protocol_options();
          upstream_h2_options->mutable_max_concurrent_streams()->set_value(100);
          upstream_h2_options->mutable_initial_stream_window_size()->set_value(
              buffer_params.upstream_h2_stream_window);
          upstream_h2_options->mutable_initial_connection_window_size()->set_value(
              buffer_params.upstream_h2_conn_window);
          for (auto& cluster_config : *bootstrap.mutable_static_resources()->mutable_clusters()) {
            ConfigHelper::setProtocolOptions(cluster_config, protocol_options);
          }
        });

    autonomous_upstream_ = true;
    autonomous_allow_incomplete_streams_ = true;

    initialize();
  }

  std::vector<IntegrationStreamDecoderPtr>
  sendRequests(uint32_t num_responses, uint32_t request_body_size, uint32_t response_body_size) {
    std::vector<IntegrationStreamDecoderPtr> responses;
    std::cout << "Expected Response Size:" << 4096 << std::endl;

    Http::TestRequestHeaderMapImpl header_map{
        {"response_data_blocks", absl::StrCat(1)},
        {"response_size_bytes", absl::StrCat(response_body_size)},
        {"no_trailers", "0"}};
    header_map.copyFrom(default_request_headers_);
    header_map.setContentLength(request_body_size);

    for (uint32_t idx = 0; idx < num_responses; ++idx) {
      responses.emplace_back(codec_client_->makeRequestWithBody(header_map, request_body_size));
    }

    return responses;
  }

  // TODO(kbaichoo): Parameterize on the client codec type when other protocols
  // (H1, H3) support buffer accounting.
  HttpBufferWatermarksTest() : HttpIntegrationTest(Http::CodecClient::Type::HTTP2, GetParam()) {
    setServerBufferFactory(buffer_factory_);
    setDownstreamProtocol(Http::CodecClient::Type::HTTP2);
    setUpstreamProtocol(FakeHttpConnection::Type::HTTP2);
  }

protected:
  std::shared_ptr<Buffer::TrackedWatermarkBufferFactory> buffer_factory_ =
      std::make_shared<Buffer::TrackedWatermarkBufferFactory>();

  std::string printAccounts() {
    std::stringstream stream;
    auto print_map = [&stream]() {
      stream << "Printing Account map. Size: " << map.size() << '\n';
      for (auto& entry : map) {
        stream << "  Account: " << entry.first << " Charged Amount: "
               << static_cast<Buffer::BufferMemoryAccountImpl*>(entry.first.get())->balance()
               << '\n';
        for (auto& buffer : entry.second) {
          stream << "    Buffer: " << buffer << '\n';
        }
      }
    };

    buffer_factory_->inspectAccounts(printMap);
    return stream.str();
  }
};

INSTANTIATE_TEST_SUITE_P(IpVersions, HttpBufferWatermarksTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

// We should create four buffers each billing the same downstream request's
// account which originated the chain.
TEST_P(HttpBufferWatermarksTest, ShouldCreateFourBuffersPerAccount) {
  FakeStreamPtr upstream_request1;
  FakeStreamPtr upstream_request2;
  default_request_headers_.setContentLength(1000);

  initialize();
  codec_client_ = makeHttpConnection(lookupPort("http"));

  // Sends the first request.
  auto response1 = codec_client_->makeRequestWithBody(default_request_headers_, 1000);
  waitForNextUpstreamRequest();
  upstream_request1 = std::move(upstream_request_);

  // Check the expected number of buffers per account
  EXPECT_EQ(buffer_factory_->numBuffersActivelyBound(), 4);
  EXPECT_EQ(buffer_factory_->numAccountsActive(), 1);

  // Send the second request.
  auto response2 = codec_client_->makeRequestWithBody(default_request_headers_, 1000);
  waitForNextUpstreamRequest();
  upstream_request2 = std::move(upstream_request_);

  // Check the expected number of buffers per account
  EXPECT_EQ(buffer_factory_->numBuffersActivelyBound(), 8);
  EXPECT_EQ(buffer_factory_->numAccountsActive(), 2);

  // Respond to the first request and wait for complete
  upstream_request1->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, false);
  upstream_request1->encodeData(1000, true);
  ASSERT_TRUE(response1->waitForEndStream());
  ASSERT_TRUE(upstream_request1->complete());

  // Check the expected number of buffers per account
  EXPECT_EQ(buffer_factory_->numBuffersActivelyBound(), 4);
  EXPECT_EQ(buffer_factory_->numAccountsActive(), 1);

  // Respond to the second request and wait for complete
  upstream_request2->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, false);
  upstream_request2->encodeData(1000, true);
  ASSERT_TRUE(response2->waitForEndStream());
  ASSERT_TRUE(upstream_request2->complete());

  // Check the expected number of buffers per account
  EXPECT_EQ(buffer_factory_->numBuffersActivelyBound(), 0);
  EXPECT_EQ(buffer_factory_->numAccountsActive(), 0);
}

TEST_P(HttpBufferWatermarksTest, ShouldTrackAllocatedBytesToUpstream) {
  int num_requests = 5;
  uint32_t connection_watermark = 32768;
  uint32_t downstream_h2_stream_window = 512 * 1024;
  uint32_t downstream_h2_conn_window = 64 * 1024;
  uint32_t upstream_h2_stream_window = 64 * 1024;
  uint32_t upstream_h2_conn_window = 1024 * 1024 * 1024; // Effectively unlimited

  initializeWithBufferConfig({connection_watermark, downstream_h2_stream_window,
                              downstream_h2_conn_window, upstream_h2_stream_window,
                              upstream_h2_conn_window},
                             num_requests);

  // Makes us have Envoy's writes to upstream return EAGAIN
  writev_matcher_->setDestinationPort(fake_upstreams_[0]->localAddress()->ip()->port());
  writev_matcher_->setWritevReturnsEgain();

  codec_client_ = makeHttpConnection(lookupPort("http"));

  uint32_t request_body_size = 4096;
  uint32_t response_body_size = 4096;
  auto responses = sendRequests(num_requests, request_body_size, response_body_size);

  // Wait for all requests to have accounted for the requests we've sent.
  ASSERT_TRUE(buffer_factory_->waitUntilEachAccountChargedAtleast(request_body_size, num_requests,
                                                                  TestUtility::DefaultTimeout))
      << "buffer total: " << buffer_factory_->totalBufferSize()
      << " buffer max: " << buffer_factory_->maxBufferSize()
      << " active accounts: " << buffer_factory_->numAccountsActive()
      << " active bound buffers: " << buffer_factory_->numBuffersActivelyBound() << printAccounts();

  writev_matcher_->setResumeWrites();

  for (auto& response : responses) {
    ASSERT_TRUE(response->waitForEndStream());
    ASSERT_TRUE(response->complete());
  }
}

TEST_P(HttpBufferWatermarksTest, ShouldTrackAllocatedBytesToDownstream) {
  int num_requests = 5;
  uint32_t connection_watermark = 32768;
  uint32_t downstream_h2_stream_window = 512 * 1024;
  uint32_t downstream_h2_conn_window = 64 * 1024;
  uint32_t upstream_h2_stream_window = 64 * 1024;
  uint32_t upstream_h2_conn_window = 1024 * 1024 * 1024; // Effectively unlimited

  initializeWithBufferConfig({connection_watermark, downstream_h2_stream_window,
                              downstream_h2_conn_window, upstream_h2_stream_window,
                              upstream_h2_conn_window},
                             num_requests);
  writev_matcher_->setSourcePort(lookupPort("http"));
  codec_client_ = makeHttpConnection(lookupPort("http"));

  // Simulate TCP push back on the Envoy's downstream network socket, so that outbound frames
  // start to accumulate in the transport socket buffer.
  writev_matcher_->setWritevReturnsEgain();

  uint32_t request_body_size = 4096;
  uint32_t response_body_size = 16384;
  auto responses = sendRequests(num_requests, request_body_size, response_body_size);

  // Wait for all requests to buffered the response from upstream.
  ASSERT_TRUE(buffer_factory_->waitUntilEachAccountChargedAtleast(response_body_size, num_requests,
                                                                  TestUtility::DefaultTimeout))
      << "buffer total: " << buffer_factory_->totalBufferSize()
      << " buffer max: " << buffer_factory_->maxBufferSize()
      << " active accounts: " << buffer_factory_->numAccountsActive()
      << " active bound buffers: " << buffer_factory_->numBuffersActivelyBound() << printAccounts();

  writev_matcher_->setResumeWrites();

  // Wait for streams to terminate.
  for (auto& response : responses) {
    ASSERT_TRUE(response->waitForEndStream());
    ASSERT_TRUE(response->complete());
  }
}

} // namespace Envoy
