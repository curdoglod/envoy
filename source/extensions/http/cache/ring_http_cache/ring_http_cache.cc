#include "source/extensions/http/cache/ring_http_cache/ring_http_cache.h"

#include "envoy/common/exception.h"
#include "envoy/registry/registry.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/header_map_impl.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace {

// Returns a Key with the vary header added to custom_fields.
// It is an error to call this with headers that don't include vary.
// Returns nullopt if the vary headers in the response are not
// compatible with the VaryAllowList in the LookupRequest.
absl::optional<Key> variedRequestKey(const LookupRequest& request,
                                     const Http::ResponseHeaderMap& response_headers) {
  absl::btree_set<absl::string_view> vary_header_values =
      VaryHeaderUtils::getVaryValues(response_headers);
  ASSERT(!vary_header_values.empty());
  const absl::optional<std::string> vary_identifier = VaryHeaderUtils::createVaryIdentifier(
      request.varyAllowList(), vary_header_values, request.requestHeaders());
  if (!vary_identifier.has_value()) {
    return absl::nullopt;
  }
  Key varied_request_key = request.key();
  varied_request_key.add_custom_fields(vary_identifier.value());
  return varied_request_key;
}

class RingLookupContext : public LookupContext {
public:
  RingLookupContext(Event::Dispatcher& dispatcher, RingHttpCache& cache, LookupRequest&& request)
      : dispatcher_(dispatcher), cache_(cache), request_(std::move(request)) {}

  void getHeaders(LookupHeadersCallback&& cb) override {
    auto entry = cache_.lookup(request_);
    body_ = std::move(entry.body_);
    trailers_ = std::move(entry.trailers_);
    LookupResult result = entry.response_headers_
                              ? request_.makeLookupResult(std::move(entry.response_headers_),
                                                          std::move(entry.metadata_), body_.size())
                              : LookupResult{};
    bool end_stream = body_.empty() && trailers_ == nullptr;
    dispatcher_.post([result = std::move(result), cb = std::move(cb), end_stream,
                      cancelled = cancelled_]() mutable {
      if (!*cancelled) {
        std::move(cb)(std::move(result), end_stream);
      }
    });
  }

  void getBody(const AdjustedByteRange& range, LookupBodyCallback&& cb) override {
    ASSERT(range.end() <= body_.length(), "Attempt to read past end of body.");
    auto result = std::make_unique<Buffer::OwnedImpl>(&body_[range.begin()], range.length());
    bool end_stream = trailers_ == nullptr && range.end() == body_.length();
    dispatcher_.post([result = std::move(result), cb = std::move(cb), end_stream,
                      cancelled = cancelled_]() mutable {
      if (!*cancelled) {
        std::move(cb)(std::move(result), end_stream);
      }
    });
  }

  // The cache must call cb with the cached trailers.
  void getTrailers(LookupTrailersCallback&& cb) override {
    ASSERT(trailers_);
    dispatcher_.post(
        [cb = std::move(cb), trailers = std::move(trailers_), cancelled = cancelled_]() mutable {
          if (!*cancelled) {
            std::move(cb)(std::move(trailers));
          }
        });
  }

  const LookupRequest& request() const { return request_; }
  void onDestroy() override { *cancelled_ = true; }
  Event::Dispatcher& dispatcher() const { return dispatcher_; }

private:
  Event::Dispatcher& dispatcher_;
  std::shared_ptr<bool> cancelled_ = std::make_shared<bool>(false);
  RingHttpCache& cache_;
  const LookupRequest request_;
  std::string body_;
  Http::ResponseTrailerMapPtr trailers_;
};

class RingInsertContext : public InsertContext {
public:
  RingInsertContext(RingLookupContext& lookup_context, RingHttpCache& cache)
      : dispatcher_(lookup_context.dispatcher()), key_(lookup_context.request().key()),
        request_headers_(lookup_context.request().requestHeaders()),
        vary_allow_list_(lookup_context.request().varyAllowList()), cache_(cache) {}

  void post(InsertCallback cb, bool result) {
    dispatcher_.post([cb = std::move(cb), result = result, cancelled = cancelled_]() mutable {
      if (!*cancelled) {
        std::move(cb)(result);
      }
    });
  }

  void insertHeaders(const Http::ResponseHeaderMap& response_headers,
                     const ResponseMetadata& metadata, InsertCallback insert_success,
                     bool end_stream) override {
    ASSERT(!committed_);
    response_headers_ = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(response_headers);
    metadata_ = metadata;
    if (end_stream) {
      post(std::move(insert_success), commit());
    } else {
      post(std::move(insert_success), true);
    }
  }

  void insertBody(const Buffer::Instance& chunk, InsertCallback ready_for_next_chunk,
                  bool end_stream) override {
    ASSERT(!committed_);
    ASSERT(ready_for_next_chunk || end_stream);

    body_.add(chunk);
    if (end_stream) {
      post(std::move(ready_for_next_chunk), commit());
    } else {
      post(std::move(ready_for_next_chunk), true);
    }
  }

  void insertTrailers(const Http::ResponseTrailerMap& trailers,
                      InsertCallback insert_complete) override {
    ASSERT(!committed_);
    trailers_ = Http::createHeaderMap<Http::ResponseTrailerMapImpl>(trailers);
    post(std::move(insert_complete), commit());
  }

  void onDestroy() override { *cancelled_ = true; }

private:
  bool commit() {
    committed_ = true;
    if (VaryHeaderUtils::hasVary(*response_headers_)) {
      return cache_.varyInsert(key_, std::move(response_headers_), std::move(metadata_),
                               body_.toString(), request_headers_, vary_allow_list_,
                               std::move(trailers_));
    } else {
      return cache_.insert(key_, std::move(response_headers_), std::move(metadata_),
                           body_.toString(), std::move(trailers_));
    }
  }

  Event::Dispatcher& dispatcher_;
  std::shared_ptr<bool> cancelled_ = std::make_shared<bool>(false);
  Key key_;
  const Http::RequestHeaderMap& request_headers_;
  const VaryAllowList& vary_allow_list_;
  Http::ResponseHeaderMapPtr response_headers_;
  ResponseMetadata metadata_;
  RingHttpCache& cache_;
  Buffer::OwnedImpl body_;
  bool committed_ = false;
  Http::ResponseTrailerMapPtr trailers_;
};
} // namespace

RingHttpCache::RingHttpCache(const ConfigProto& config)
    : ring_size_(config.ring_size()), slots_(ring_size_) {
  if (ring_size_ == 0) {
    throw EnvoyException("RingHttpCacheConfig.ring_size must be > 0");
  }
}

LookupContextPtr RingHttpCache::makeLookupContext(LookupRequest&& request,
                                                  Http::StreamFilterCallbacks& callbacks) {
  return std::make_unique<RingLookupContext>(callbacks.dispatcher(), *this, std::move(request));
}

void RingHttpCache::updateHeaders(const LookupContext& lookup_context,
                                  const Http::ResponseHeaderMap& response_headers,
                                  const ResponseMetadata& metadata,
                                  UpdateHeadersCallback on_complete) {
  const auto& ring_lookup_context = static_cast<const RingLookupContext&>(lookup_context);
  const Key& key = ring_lookup_context.request().key();

  auto post_complete = [on_complete = std::move(on_complete),
                        &dispatcher = ring_lookup_context.dispatcher()](bool result) mutable {
    dispatcher.post([on_complete = std::move(on_complete), result]() mutable {
      std::move(on_complete)(result);
    });
  };

  absl::WriterMutexLock lock(mutex_);

  auto it = index_.find(key);
  if (it == index_.end()) {
    std::move(post_complete)(false);
    return;
  }
  Slot* target = &slots_[it->second];
  if (!target->occupied || !target->entry.response_headers_) {
    std::move(post_complete)(false);
    return;
  }

  if (VaryHeaderUtils::hasVary(*target->entry.response_headers_)) {
    absl::optional<Key> varied_key =
        variedRequestKey(ring_lookup_context.request(), *target->entry.response_headers_);
    if (!varied_key.has_value()) {
      std::move(post_complete)(false);
      return;
    }
    auto vit = index_.find(varied_key.value());
    if (vit == index_.end()) {
      std::move(post_complete)(false);
      return;
    }
    target = &slots_[vit->second];
    if (!target->occupied || !target->entry.response_headers_) {
      std::move(post_complete)(false);
      return;
    }
  }

  applyHeaderUpdate(response_headers, *target->entry.response_headers_);
  target->entry.metadata_ = metadata;
  std::move(post_complete)(true);
}

RingHttpCache::Entry RingHttpCache::lookup(const LookupRequest& request) {
  absl::ReaderMutexLock lock(mutex_);
  auto it = index_.find(request.key());
  if (it == index_.end()) {
    return Entry{};
  }
  const Slot* target = &slots_[it->second];
  if (!target->occupied || !target->entry.response_headers_) {
    return Entry{};
  }

  if (VaryHeaderUtils::hasVary(*target->entry.response_headers_)) {
    return varyLookup(request, target->entry.response_headers_);
  } else {
    Http::ResponseTrailerMapPtr trailers_map;
    if (target->entry.trailers_) {
      trailers_map =
          Http::createHeaderMap<Http::ResponseTrailerMapImpl>(*target->entry.trailers_);
    }
    return RingHttpCache::Entry{
        Http::createHeaderMap<Http::ResponseHeaderMapImpl>(*target->entry.response_headers_),
        target->entry.metadata_, target->entry.body_, std::move(trailers_map)};
  }
}

bool RingHttpCache::insertOrUpdateSlot(const Key& key, Entry&& entry) {
  if (ring_size_ == 0 || slots_.empty()) {
    return false;
  }

  auto existing = index_.find(key);
  if (existing != index_.end()) {
    Slot& slot = slots_[existing->second];
    if (slot.occupied) {
      slot.key = key;
      slot.entry = std::move(entry);
      return true;
    }
    index_.erase(existing);
  }

  Slot& slot = slots_[next_slot_];
  if (slot.occupied) {
    index_.erase(slot.key);
  }

  slot.key = key;
  slot.entry = std::move(entry);
  slot.occupied = true;
  index_[key] = next_slot_;
  next_slot_ = (next_slot_ + 1) % ring_size_;
  return true;
}

bool RingHttpCache::insert(const Key& key, Http::ResponseHeaderMapPtr&& response_headers,
                           ResponseMetadata&& metadata, std::string&& body,
                           Http::ResponseTrailerMapPtr&& trailers) {
  absl::WriterMutexLock lock(mutex_);
  return insertOrUpdateSlot(
      key, RingHttpCache::Entry{std::move(response_headers), std::move(metadata),
                                std::move(body), std::move(trailers)});
}

RingHttpCache::Entry
RingHttpCache::varyLookup(const LookupRequest& request,
                          const Http::ResponseHeaderMapPtr& response_headers) {
  // This method should be called from lookup, which holds the mutex for reading.
  mutex_.AssertReaderHeld();

  absl::optional<Key> varied_key = variedRequestKey(request, *response_headers);
  if (!varied_key.has_value()) {
    return RingHttpCache::Entry{};
  }
  Key& varied_request_key = varied_key.value();

  auto it = index_.find(varied_request_key);
  if (it == index_.end()) {
    return RingHttpCache::Entry{};
  }
  const Slot* target = &slots_[it->second];
  if (!target->occupied || !target->entry.response_headers_) {
    return RingHttpCache::Entry{};
  }

  Http::ResponseTrailerMapPtr trailers_map;
  if (target->entry.trailers_) {
    trailers_map = Http::createHeaderMap<Http::ResponseTrailerMapImpl>(*target->entry.trailers_);
  }

  return RingHttpCache::Entry{
      Http::createHeaderMap<Http::ResponseHeaderMapImpl>(*target->entry.response_headers_),
      target->entry.metadata_, target->entry.body_, std::move(trailers_map)};
}

bool RingHttpCache::varyInsert(const Key& request_key,
                               Http::ResponseHeaderMapPtr&& response_headers,
                               ResponseMetadata&& metadata, std::string&& body,
                               const Http::RequestHeaderMap& request_headers,
                               const VaryAllowList& vary_allow_list,
                               Http::ResponseTrailerMapPtr&& trailers) {
  absl::WriterMutexLock lock(mutex_);

  absl::btree_set<absl::string_view> vary_header_values =
      VaryHeaderUtils::getVaryValues(*response_headers);
  ASSERT(!vary_header_values.empty());

  // Insert the varied response.
  Key varied_request_key = request_key;
  const absl::optional<std::string> vary_identifier =
      VaryHeaderUtils::createVaryIdentifier(vary_allow_list, vary_header_values, request_headers);
  if (!vary_identifier.has_value()) {
    // Skip the insert if we are unable to create a vary key.
    return false;
  }

  varied_request_key.add_custom_fields(vary_identifier.value());
  const bool varied_inserted =
      insertOrUpdateSlot(varied_request_key,
                         RingHttpCache::Entry{std::move(response_headers), std::move(metadata),
                                              std::move(body), std::move(trailers)});
  if (!varied_inserted) {
    return false;
  }

  // Add a special entry to flag that this request generates varied responses.
  auto it = index_.find(request_key);
  if (it == index_.end() || !slots_[it->second].occupied) {
    if (it != index_.end()) {
      index_.erase(it);
    }

    Envoy::Http::ResponseHeaderMapPtr vary_only_map =
        Envoy::Http::createHeaderMap<Envoy::Http::ResponseHeaderMapImpl>({});
    vary_only_map->setCopy(Envoy::Http::CustomHeaders::get().Vary,
                           absl::StrJoin(vary_header_values, ","));
    // TODO(cbdm): In a cache that evicts entries, we could maintain a list of the "varykey"s that
    // we have inserted as the body for this first lookup. This way, we would know which keys we
    // have inserted for that resource. For the first entry simply use vary_identifier as the
    // entry_list; for future entries append vary_identifier to existing list.
    std::string entry_list;
    return insertOrUpdateSlot(
        request_key,
        RingHttpCache::Entry{std::move(vary_only_map), {}, std::move(entry_list), {}});
  }

  return true;
}

InsertContextPtr RingHttpCache::makeInsertContext(LookupContextPtr&& lookup_context,
                                                  Http::StreamFilterCallbacks&) {
  ASSERT(lookup_context != nullptr);
  auto ret = std::make_unique<RingInsertContext>(
      dynamic_cast<RingLookupContext&>(*lookup_context), *this);
  lookup_context->onDestroy();
  return ret;
}

constexpr absl::string_view Name = "envoy.extensions.http.cache.ring";

CacheInfo RingHttpCache::cacheInfo() const {
  CacheInfo cache_info;
  cache_info.name_ = Name;
  return cache_info;
}

SINGLETON_MANAGER_REGISTRATION(ring_http_cache_singleton);

class RingHttpCacheFactory : public HttpCacheFactory {
public:
  // From UntypedFactory
  std::string name() const override { return std::string(Name); }
  // From TypedFactory
  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<ConfigProto>();
  }
  // From HttpCacheFactory
  std::shared_ptr<HttpCache>
  getCache(const envoy::extensions::filters::http::cache::v3::CacheConfig& filter_config,
           Server::Configuration::FactoryContext& context) override {

      ConfigProto config;
      THROW_IF_NOT_OK(MessageUtil::unpackTo(filter_config.typed_config(), config));

      return context.serverFactoryContext().singletonManager().getTyped<RingHttpCache>(
        SINGLETON_MANAGER_REGISTERED_NAME(ring_http_cache_singleton), [config] {
          return std::make_shared<RingHttpCache>(config);
        });
  }
};

static Registry::RegisterFactory<RingHttpCacheFactory, HttpCacheFactory> register_;

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
