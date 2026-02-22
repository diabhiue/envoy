load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_cc_library(
    name = "envoy_client_lib",
    srcs = ["envoy_client.cc"],
    hdrs = ["envoy_client.h"],
    repository = "@envoy",
    deps = [
        "//client/library/cc:client_lib",
    ],
)
