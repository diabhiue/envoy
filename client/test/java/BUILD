licenses(["notice"])  # Apache 2

java_test(
    name = "EnvoyClientTest",
    srcs = ["io/envoyproxy/envoyclient/EnvoyClientTest.java"],
    test_class = "io.envoyproxy.envoyclient.EnvoyClientTest",
    deps = [
        "//client/library/java:envoyclient_java",
        "@maven//:junit_junit",
    ],
    data = [
        # JNI shared library loaded at runtime by System.loadLibrary.
        "//client/library/jni:envoy_client_jni",
    ],
    jvm_flags = [
        # Tell the JVM where to find the native shared library.
        "-Djava.library.path=$${RUNFILES_DIR}/envoy/client/library/jni",
    ],
)
