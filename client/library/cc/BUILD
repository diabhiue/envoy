load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_cc_library(
    name = "client_lib",
    srcs = ["client.cc"],
    hdrs = ["client.h"],
    repository = "@envoy",
    deps = [
        "//client/library/common:config_store_lib",
        "//client/library/common:engine_interface_lib",
        "//client/library/common:engine_lib",
        "//client/library/common:lb_context_lib",
        "//source/server:options_base",
    ],
)
