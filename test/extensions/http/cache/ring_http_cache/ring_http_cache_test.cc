#include "envoy/http/header_map.h"
#include "envoy/registry/registry.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/extensions/filters/http/cache/cache_entry_utils.h"
#include "source/extensions/filters/http/cache/cache_headers_utils.h"
#include "source/extensions/http/cache/ring_http_cache/ring_http_cache.h"

#include "test/extensions/filters/http/cache/http_cache_implementation_test_common.h"
#include "test/mocks/server/factory_context.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace {

LookupRequest makeLookupRequest(absl::string_view request_path,
                                const VaryAllowList& vary_allow_list) {
  Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":authority", "example.com"},
                                                 {":path", std::string(request_path)}};
  return LookupRequest(request_headers, std::chrono::system_clock::now(), vary_allow_list);
}

bool insertResponse(RingHttpCache& cache, absl::string_view request_path, absl::string_view body,
                    const VaryAllowList& vary_allow_list) {
  LookupRequest request = makeLookupRequest(request_path, vary_allow_list);
  DateFormatter formatter{"%a, %d %b %Y %H:%M:%S GMT"};
  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"},
                                                   {"date", formatter.fromTime(
                                                                std::chrono::system_clock::now())},
                                                   {"cache-control", "public,max-age=3600"}};

  return cache.insert(request.key(),
                      Http::createHeaderMap<Http::ResponseHeaderMapImpl>(response_headers),
                      ResponseMetadata{std::chrono::system_clock::now()}, std::string(body),
                      Http::ResponseTrailerMapPtr{});
}

bool hasCachedResponse(RingHttpCache& cache, absl::string_view request_path,
                       const VaryAllowList& vary_allow_list) {
  LookupRequest request = makeLookupRequest(request_path, vary_allow_list);
  auto entry = cache.lookup(request);
  return entry.response_headers_ != nullptr;
}

class RingHttpCacheTestDelegate : public HttpCacheTestDelegate {
public:
  RingHttpCacheTestDelegate() {
    config_.set_ring_size(1024);
    cache_ = std::make_shared<RingHttpCache>(config_);
  }

  std::shared_ptr<HttpCache> cache() override { return cache_; }
  bool validationEnabled() const override { return true; }

private:
  ConfigProto config_;
  std::shared_ptr<RingHttpCache> cache_;
};

INSTANTIATE_TEST_SUITE_P(RingHttpCacheTest, HttpCacheImplementationTest,
			 testing::Values(std::make_unique<RingHttpCacheTestDelegate>),
			 [](const testing::TestParamInfo<HttpCacheImplementationTest::ParamType>&) {
			   return "RingHttpCache";
			 });

TEST(Registration, GetFactory) {
  HttpCacheFactory* factory = Registry::FactoryRegistry<HttpCacheFactory>::getFactoryByType(
      "envoy.extensions.http.cache.ring_http_cache.v3.RingHttpCacheConfig");
  ASSERT_NE(factory, nullptr);
  envoy::extensions::filters::http::cache::v3::CacheConfig config;
  ConfigProto ring_cache_config;
  ring_cache_config.set_ring_size(1);
  testing::NiceMock<Server::Configuration::MockFactoryContext> factory_context;
  config.mutable_typed_config()->PackFrom(ring_cache_config);
  EXPECT_EQ(factory->getCache(config, factory_context)->cacheInfo().name_,
	    "envoy.extensions.http.cache.ring");
}

TEST(RingHttpCacheConfigValidation, RejectsZeroRingSize) {
  ConfigProto config;
  config.set_ring_size(0);
  EXPECT_THROW_WITH_MESSAGE(({ RingHttpCache cache(config); }), EnvoyException,
                          "RingHttpCacheConfig.ring_size must be > 0");
}

TEST(RingHttpCacheEviction, EvictsOldestEntryWhenFull) {
  ConfigProto config;
  config.set_ring_size(1);
  RingHttpCache cache(config);

  envoy::extensions::filters::http::cache::v3::CacheConfig cache_filter_config;
  cache_filter_config.add_allowed_vary_headers()->set_exact("accept");
  testing::NiceMock<Server::Configuration::MockServerFactoryContext> factory_context;
  VaryAllowList vary_allow_list(cache_filter_config.allowed_vary_headers(), factory_context);

  ASSERT_TRUE(insertResponse(cache, "/a", "body-a", vary_allow_list));
  EXPECT_TRUE(hasCachedResponse(cache, "/a", vary_allow_list));

  ASSERT_TRUE(insertResponse(cache, "/b", "body-b", vary_allow_list));

  EXPECT_FALSE(hasCachedResponse(cache, "/a", vary_allow_list));
  EXPECT_TRUE(hasCachedResponse(cache, "/b", vary_allow_list));
}

} // namespace
} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
