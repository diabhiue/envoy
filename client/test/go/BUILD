load("@io_bazel_rules_go//go:def.bzl", "go_test")

licenses(["notice"])  # Apache 2

go_test(
    name = "envoyclient_test",
    srcs = ["envoyclient_test.go"],
    importpath = "github.com/envoyproxy/envoy/client/test/go",
    deps = [
        "//client/library/go:envoyclient",
    ],
)
