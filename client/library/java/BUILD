licenses(["notice"])  # Apache 2

java_library(
    name = "envoyclient_java",
    srcs = glob(["io/envoyproxy/envoyclient/*.java"]),
    visibility = ["//visibility:public"],
    # The JNI shared library is loaded at runtime via System.loadLibrary;
    # it is not a compile-time dependency.
)
