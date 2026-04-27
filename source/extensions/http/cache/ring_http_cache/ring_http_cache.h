#pragma once

#include <functional>
#include <memory>

#include "envoy/extensions/http/cache/ring_http_cache/v3/config.pb.h"

#include "source/common/protobuf/utility.h"
#include "source/extensions/filters/http/cache/http_cache.h"

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {

using ConfigProto =
  envoy::extensions::http::cache::ring_http_cache::v3::RingHttpCacheConfig;

class RingHttpCache : public HttpCache, public Singleton::Instance {
private:
  struct Entry {
    Http::ResponseHeaderMapPtr response_headers_;
    ResponseMetadata metadata_;
    std::string body_;
    Http::ResponseTrailerMapPtr trailers_;
  };

  struct Slot {
    Key key;
    Entry entry;
    bool occupied = false;
  };

  struct Subscriber {
    Event::Dispatcher* dispatcher;
    std::function<void()> wake;
    std::shared_ptr<bool> cancelled;
  };

  struct Inflight {
    std::vector<Subscriber> subscribers;
  };


  const uint32_t ring_size_;
  absl::Mutex mutex_;
  std::vector<Slot> slots_ ABSL_GUARDED_BY(mutex_);
  uint32_t next_slot_ ABSL_GUARDED_BY(mutex_) = 0;
  absl::flat_hash_map<Key, uint32_t, MessageUtil, MessageUtil> index_ ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<Key, Inflight, MessageUtil, MessageUtil> inflight_ ABSL_GUARDED_BY(mutex_);

  bool insertOrUpdateSlot(const Key& key, Entry&& entry) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  Entry lookupLocked(const LookupRequest& request) ABSL_SHARED_LOCKS_REQUIRED(mutex_);

  // Looks for a response that has been varied. Only called from lookup methods.
  Entry varyLookup(const LookupRequest& request, const Http::ResponseHeaderMapPtr& response_headers)
      ABSL_SHARED_LOCKS_REQUIRED(mutex_);

  // A list of headers that we do not want to update upon validation
  // We skip these headers because either it's updated by other application logic
  // or they are fall into categories defined in the IETF doc below
  // https://www.ietf.org/archive/id/draft-ietf-httpbis-cache-18.html s3.2
  static const absl::flat_hash_set<Http::LowerCaseString> headersNotToUpdate();

public:
  enum class LookupOutcome { Hit, MissBecameLeader, MissSubscribed };

  explicit RingHttpCache(const ConfigProto& config);
  // HttpCache
  LookupContextPtr makeLookupContext(LookupRequest&& request,
                                     Http::StreamFilterCallbacks& callbacks) override;
  InsertContextPtr makeInsertContext(LookupContextPtr&& lookup_context,
                                     Http::StreamFilterCallbacks& callbacks) override;
  void updateHeaders(const LookupContext& lookup_context,
                     const Http::ResponseHeaderMap& response_headers,
                     const ResponseMetadata& metadata, UpdateHeadersCallback on_complete) override;
  CacheInfo cacheInfo() const override;

  Entry lookup(const LookupRequest& request);
  LookupOutcome lookupOrSubscribe(const LookupRequest& request, Entry& out_entry,
                                  Event::Dispatcher& dispatcher, std::function<void()> wake,
                                  std::shared_ptr<bool> cancelled);
  void completeInflight(const Key& key);

  bool insert(const Key& key, Http::ResponseHeaderMapPtr&& response_headers,
              ResponseMetadata&& metadata, std::string&& body,
              Http::ResponseTrailerMapPtr&& trailers);

  // Inserts a response that has been varied on certain headers.
  bool varyInsert(const Key& request_key, Http::ResponseHeaderMapPtr&& response_headers,
                  ResponseMetadata&& metadata, std::string&& body,
                  const Http::RequestHeaderMap& request_headers,
                  const VaryAllowList& vary_allow_list, Http::ResponseTrailerMapPtr&& trailers);
};

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
