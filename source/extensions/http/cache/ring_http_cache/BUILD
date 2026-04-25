load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

## WIP: Ring in-memory cache storage plugin. Not ready for deployment.

envoy_extension_package()

envoy_cc_extension(
    name = "config",
    srcs = ["ring_http_cache.cc"],
    hdrs = ["ring_http_cache.h"],
    deps = [
        "//envoy/common:exception_lib",
        "//envoy/registry",
        "//envoy/runtime:runtime_interface",
        "//source/common/buffer:buffer_lib",
        "//source/common/common:macros",
        "//source/common/http:header_map_lib",
        "//source/common/http:headers_lib",
        "//source/common/protobuf",
        "//source/extensions/filters/http/cache:http_cache_lib",
        "//source/extensions/filters/http/common:pass_through_filter_lib",
        "@envoy_api//envoy/extensions/http/cache/ring_http_cache/v3:pkg_cc_proto",
    ],
)
