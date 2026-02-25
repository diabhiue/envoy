load("@io_bazel_rules_go//go:def.bzl", "go_library")

licenses(["notice"])  # Apache 2

# CGo library that wraps the Envoy Client C ABI.
#
# Consumers link this library into their Go binary. The CGo directives in
# envoyclient.go handle include paths and the -lenvoyclient linker flag.
go_library(
    name = "envoyclient",
    srcs = [
        "callbacks.c",
        "envoyclient.go",
        "grpc_integration.go",
    ],
    cgo = True,
    importpath = "github.com/envoyproxy/envoy/client/library/go",
    visibility = ["//visibility:public"],
    cdeps = [
        "//client/library/c_api:envoy_client_lib",
    ],
    deps = [
        "@org_golang_google_grpc//:go_default_library",
        "@org_golang_google_grpc//attributes:go_default_library",
        "@org_golang_google_grpc//balancer:go_default_library",
        "@org_golang_google_grpc//balancer/base:go_default_library",
        "@org_golang_google_grpc//resolver:go_default_library",
    ],
)
