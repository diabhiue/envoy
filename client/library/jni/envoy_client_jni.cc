#include <jni.h>

#include <cstring>
#include <memory>
#include <string>

#include "client/library/c_api/envoy_client.h"

// ---------------------------------------------------------------------------
// JVM reference — stored at JNI_OnLoad for use in async callbacks that fire
// on non-Java threads (e.g. the Envoy dispatcher thread).
// ---------------------------------------------------------------------------

static JavaVM* g_jvm = nullptr;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  g_jvm = vm;
  return JNI_VERSION_1_6;
}

// ---------------------------------------------------------------------------
// Thread-attachment helpers
// ---------------------------------------------------------------------------

namespace {

// Returns the JNIEnv for the current thread, attaching it to the JVM if
// necessary. Sets *attached to true when the thread was newly attached so the
// caller can detach it afterwards.
JNIEnv* getEnv(bool* attached) {
  *attached = false;
  if (g_jvm == nullptr) return nullptr;

  JNIEnv* env = nullptr;
  jint rc = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (rc == JNI_OK) {
    return env;
  }
  if (rc == JNI_EDETACHED) {
    JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("EnvoyClientCB"), nullptr};
    if (g_jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), &args) == JNI_OK) {
      *attached = true;
      return env;
    }
  }
  return nullptr;
}

void maybeDetach(bool attached) {
  if (attached && g_jvm != nullptr) {
    g_jvm->DetachCurrentThread();
  }
}

// Converts a jstring to a std::string. Returns "" if js is null.
std::string jstringToString(JNIEnv* env, jstring js) {
  if (js == nullptr) return "";
  const char* chars = env->GetStringUTFChars(js, nullptr);
  std::string result(chars);
  env->ReleaseStringUTFChars(js, chars);
  return result;
}

// Creates a jstring from a const char*. Returns null if s is nullptr.
jstring newJString(JNIEnv* env, const char* s) {
  return s != nullptr ? env->NewStringUTF(s) : nullptr;
}

// ---------------------------------------------------------------------------
// Endpoint conversion: C → Java
// ---------------------------------------------------------------------------

jobject buildEndpoint(JNIEnv* env, const envoy_client_endpoint& ep) {
  // Locate io.envoyproxy.envoyclient.Endpoint and its constructor.
  jclass cls = env->FindClass("io/envoyproxy/envoyclient/Endpoint");
  if (cls == nullptr) return nullptr;

  // HealthStatus enum
  jclass hsCls = env->FindClass("io/envoyproxy/envoyclient/Endpoint$HealthStatus");
  jmethodID fromInt =
      env->GetStaticMethodID(hsCls, "fromInt", "(I)Lio/envoyproxy/envoyclient/Endpoint$HealthStatus;");
  jobject healthStatus = env->CallStaticObjectMethod(hsCls, fromInt, (jint)ep.health_status);

  jmethodID ctor = env->GetMethodID(
      cls, "<init>",
      "(Ljava/lang/String;IIILio/envoyproxy/envoyclient/Endpoint$HealthStatus;)V");
  jstring addr = newJString(env, ep.address);
  jobject obj = env->NewObject(cls, ctor, addr, (jint)ep.port, (jint)ep.weight,
                               (jint)ep.priority, healthStatus);
  env->DeleteLocalRef(addr);
  env->DeleteLocalRef(hsCls);
  env->DeleteLocalRef(cls);
  return obj;
}

// ---------------------------------------------------------------------------
// Config-watcher callback context
// ---------------------------------------------------------------------------

struct ConfigWatcherCtx {
  jobject watcherGlobalRef; // io.envoyproxy.envoyclient.ConfigWatcher
};

void configCCallback(const char* resource_type, const char* resource_name,
                     envoy_client_config_event event, void* context) {
  auto* ctx = static_cast<ConfigWatcherCtx*>(context);
  bool attached = false;
  JNIEnv* env = getEnv(&attached);
  if (env == nullptr) return;

  jclass eventCls = env->FindClass("io/envoyproxy/envoyclient/ConfigEvent");
  jclass typeCls = env->FindClass("io/envoyproxy/envoyclient/ConfigEvent$Type");

  // Map C enum to Java enum ordinal.
  const char* typeName = nullptr;
  switch (event) {
  case ENVOY_CLIENT_CONFIG_ADDED:
    typeName = "ADDED";
    break;
  case ENVOY_CLIENT_CONFIG_UPDATED:
    typeName = "UPDATED";
    break;
  default:
    typeName = "REMOVED";
    break;
  }
  jfieldID typeField =
      env->GetStaticFieldID(typeCls, typeName, "Lio/envoyproxy/envoyclient/ConfigEvent$Type;");
  jobject typeObj = env->GetStaticObjectField(typeCls, typeField);

  jmethodID ctor = env->GetMethodID(
      eventCls, "<init>",
      "(Ljava/lang/String;Ljava/lang/String;Lio/envoyproxy/envoyclient/ConfigEvent$Type;)V");
  jstring jType = newJString(env, resource_type);
  jstring jName = newJString(env, resource_name);
  jobject eventObj = env->NewObject(eventCls, ctor, jType, jName, typeObj);

  jclass watcherCls = env->GetObjectClass(ctx->watcherGlobalRef);
  jmethodID onEvent =
      env->GetMethodID(watcherCls, "onConfigEvent", "(Lio/envoyproxy/envoyclient/ConfigEvent;)V");
  env->CallVoidMethod(ctx->watcherGlobalRef, onEvent, eventObj);

  env->DeleteLocalRef(eventObj);
  env->DeleteLocalRef(jType);
  env->DeleteLocalRef(jName);
  env->DeleteLocalRef(typeObj);
  env->DeleteLocalRef(typeCls);
  env->DeleteLocalRef(eventCls);
  env->DeleteLocalRef(watcherCls);

  maybeDetach(attached);
}

// ---------------------------------------------------------------------------
// LB context provider callback context
// ---------------------------------------------------------------------------

struct LbContextProviderCtx {
  jobject providerGlobalRef; // io.envoyproxy.envoyclient.LbContextProvider
};

void lbContextCallback(const char* cluster_name, envoy_client_request_context* ctx,
                       void* context) {
  auto* pctx = static_cast<LbContextProviderCtx*>(context);
  bool attached = false;
  JNIEnv* env = getEnv(&attached);
  if (env == nullptr || ctx == nullptr) return;

  // Build a Java RequestContext from the C struct.
  jclass rcCls = env->FindClass("io/envoyproxy/envoyclient/RequestContext");
  jmethodID ctor = env->GetMethodID(rcCls, "<init>", "()V");
  jobject rcObj = env->NewObject(rcCls, ctor);

  auto setStr = [&](const char* methodName, const char* value) {
    if (value == nullptr) return;
    jmethodID mid = env->GetMethodID(rcCls, methodName,
                                     "(Ljava/lang/String;)Lio/envoyproxy/envoyclient/RequestContext;");
    jstring js = env->NewStringUTF(value);
    env->CallObjectMethod(rcObj, mid, js);
    env->DeleteLocalRef(js);
  };

  setStr("setPath", ctx->path);
  setStr("setAuthority", ctx->authority);
  setStr("setOverrideHost", ctx->override_host);
  if (ctx->override_host_strict) {
    jmethodID mid = env->GetMethodID(
        rcCls, "setOverrideHostStrict",
        "(Z)Lio/envoyproxy/envoyclient/RequestContext;");
    env->CallObjectMethod(rcObj, mid, JNI_TRUE);
  }
  if (ctx->hash_key != nullptr && ctx->hash_key_len > 0) {
    jstring js = env->NewStringUTF(std::string(ctx->hash_key, ctx->hash_key_len).c_str());
    jmethodID mid = env->GetMethodID(
        rcCls, "setHashKey",
        "(Ljava/lang/String;)Lio/envoyproxy/envoyclient/RequestContext;");
    env->CallObjectMethod(rcObj, mid, js);
    env->DeleteLocalRef(js);
  }

  // Invoke the Java provider.
  jclass providerCls = env->GetObjectClass(pctx->providerGlobalRef);
  jmethodID provide = env->GetMethodID(
      providerCls, "provideLbContext",
      "(Ljava/lang/String;Lio/envoyproxy/envoyclient/RequestContext;)V");
  jstring jCluster = newJString(env, cluster_name);
  env->CallVoidMethod(pctx->providerGlobalRef, provide, jCluster, rcObj);
  env->DeleteLocalRef(jCluster);

  // Read mutations back from the Java RequestContext into the C struct.
  // The native library copies string values before this function returns.
  auto getStr = [&](const char* methodName) -> std::string {
    jmethodID mid =
        env->GetMethodID(rcCls, methodName, "()Ljava/lang/String;");
    auto js = (jstring)env->CallObjectMethod(rcObj, mid);
    if (js == nullptr) return "";
    std::string result = jstringToString(env, js);
    env->DeleteLocalRef(js);
    return result;
  };

  std::string newOverride = getStr("getOverrideHost");
  if (!newOverride.empty()) {
    // The C struct's char* fields are const; the callback contract says the
    // library copies them before we return, so a stack-local is fine.
    static thread_local std::string s_override;
    s_override = std::move(newOverride);
    ctx->override_host = s_override.c_str();
  }

  std::string newHash = getStr("getHashKey");
  if (!newHash.empty()) {
    static thread_local std::string s_hash;
    s_hash = std::move(newHash);
    ctx->hash_key = s_hash.c_str();
    ctx->hash_key_len = s_hash.size();
  }

  jmethodID isStrict = env->GetMethodID(rcCls, "isOverrideHostStrict", "()Z");
  ctx->override_host_strict = env->CallBooleanMethod(rcObj, isStrict) ? 1u : 0u;

  env->DeleteLocalRef(rcObj);
  env->DeleteLocalRef(rcCls);
  env->DeleteLocalRef(providerCls);

  maybeDetach(attached);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// JNI function implementations
// ---------------------------------------------------------------------------

extern "C" {

JNIEXPORT jlong JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeCreate(JNIEnv* env, jclass /*cls*/,
                                                        jbyteArray bootstrapConfig) {
  jsize len = env->GetArrayLength(bootstrapConfig);
  jbyte* bytes = env->GetByteArrayElements(bootstrapConfig, nullptr);
  envoy_client_handle handle =
      envoy_client_create(reinterpret_cast<const char*>(bytes), static_cast<size_t>(len));
  env->ReleaseByteArrayElements(bootstrapConfig, bytes, JNI_ABORT);
  return reinterpret_cast<jlong>(handle);
}

JNIEXPORT jint JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeWaitReady(JNIEnv* /*env*/, jclass /*cls*/,
                                                           jlong handle, jint timeoutSeconds) {
  return static_cast<jint>(
      envoy_client_wait_ready(reinterpret_cast<envoy_client_handle>(handle),
                              static_cast<uint32_t>(timeoutSeconds)));
}

JNIEXPORT void JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeDestroy(JNIEnv* /*env*/, jclass /*cls*/,
                                                         jlong handle) {
  envoy_client_destroy(reinterpret_cast<envoy_client_handle>(handle));
}

JNIEXPORT jobjectArray JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeResolve(JNIEnv* env, jclass /*cls*/,
                                                         jlong handle, jstring clusterName) {
  std::string cluster = jstringToString(env, clusterName);
  envoy_client_endpoint_list list{};

  jclass endpointCls = env->FindClass("io/envoyproxy/envoyclient/Endpoint");
  auto h = reinterpret_cast<envoy_client_handle>(handle);
  if (envoy_client_resolve(h, cluster.c_str(), &list) != ENVOY_CLIENT_OK || list.count == 0) {
    envoy_client_free_endpoints(&list);
    return env->NewObjectArray(0, endpointCls, nullptr);
  }

  jobjectArray result = env->NewObjectArray(static_cast<jsize>(list.count), endpointCls, nullptr);
  for (size_t i = 0; i < list.count; ++i) {
    jobject ep = buildEndpoint(env, list.endpoints[i]);
    env->SetObjectArrayElement(result, static_cast<jsize>(i), ep);
    env->DeleteLocalRef(ep);
  }
  envoy_client_free_endpoints(&list);
  env->DeleteLocalRef(endpointCls);
  return result;
}

JNIEXPORT jobject JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativePickEndpoint(JNIEnv* env, jclass /*cls*/,
                                                              jlong handle, jstring clusterName,
                                                              jobject requestCtx) {
  std::string cluster = jstringToString(env, clusterName);

  envoy_client_request_context cctx{};
  std::string path, authority, overrideHost, hashKey;

  if (requestCtx != nullptr) {
    jclass rcCls = env->GetObjectClass(requestCtx);
    auto getStr = [&](const char* method) -> std::string {
      jmethodID mid = env->GetMethodID(rcCls, method, "()Ljava/lang/String;");
      auto js = (jstring)env->CallObjectMethod(requestCtx, mid);
      if (js == nullptr) return "";
      std::string s = jstringToString(env, js);
      env->DeleteLocalRef(js);
      return s;
    };

    path = getStr("getPath");
    authority = getStr("getAuthority");
    overrideHost = getStr("getOverrideHost");
    hashKey = getStr("getHashKey");

    jmethodID isStrict = env->GetMethodID(rcCls, "isOverrideHostStrict", "()Z");
    cctx.override_host_strict = env->CallBooleanMethod(requestCtx, isStrict) ? 1u : 0u;

    if (!path.empty())         cctx.path          = path.c_str();
    if (!authority.empty())    cctx.authority      = authority.c_str();
    if (!overrideHost.empty()) cctx.override_host  = overrideHost.c_str();
    if (!hashKey.empty()) {
      cctx.hash_key     = hashKey.c_str();
      cctx.hash_key_len = hashKey.size();
    }
    env->DeleteLocalRef(rcCls);
  }

  envoy_client_endpoint out{};
  auto h = reinterpret_cast<envoy_client_handle>(handle);
  envoy_client_status status =
      envoy_client_pick_endpoint(h, cluster.c_str(), requestCtx ? &cctx : nullptr, &out);
  if (status != ENVOY_CLIENT_OK) return nullptr;

  jobject result = buildEndpoint(env, out);
  // The C ABI allocates the address string on the heap; free it with free()
  // since the ABI is C-language and callers must not assume C++ new[].
  free(const_cast<char*>(out.address));
  return result;
}

JNIEXPORT void JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeReportResult(JNIEnv* env, jclass /*cls*/,
                                                              jlong handle, jstring address,
                                                              jint port, jint statusCode,
                                                              jlong latencyMs) {
  std::string addr = jstringToString(env, address);
  envoy_client_endpoint ep{};
  ep.address = addr.c_str();
  ep.port    = static_cast<uint32_t>(port);

  envoy_client_report_result(reinterpret_cast<envoy_client_handle>(handle), &ep,
                             static_cast<uint32_t>(statusCode),
                             static_cast<uint64_t>(latencyMs));
}

JNIEXPORT jint JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeSetClusterLbPolicy(JNIEnv* env, jclass /*cls*/,
                                                                    jlong handle,
                                                                    jstring clusterName,
                                                                    jstring lbPolicyName) {
  std::string cluster = jstringToString(env, clusterName);
  std::string policy  = jstringToString(env, lbPolicyName);
  return static_cast<jint>(
      envoy_client_set_cluster_lb_policy(reinterpret_cast<envoy_client_handle>(handle),
                                         cluster.c_str(), policy.c_str()));
}

JNIEXPORT jint JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeSetDefaultLbPolicy(JNIEnv* env, jclass /*cls*/,
                                                                    jlong handle,
                                                                    jstring lbPolicyName) {
  std::string policy = jstringToString(env, lbPolicyName);
  return static_cast<jint>(
      envoy_client_set_default_lb_policy(reinterpret_cast<envoy_client_handle>(handle),
                                         policy.c_str()));
}

JNIEXPORT jint JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeWatchConfig(JNIEnv* env, jclass /*cls*/,
                                                             jlong handle, jstring resourceType,
                                                             jobject watcher) {
  std::string type = jstringToString(env, resourceType);

  auto* ctx = new ConfigWatcherCtx();
  ctx->watcherGlobalRef = env->NewGlobalRef(watcher);

  return static_cast<jint>(
      envoy_client_watch_config(reinterpret_cast<envoy_client_handle>(handle),
                                type.empty() ? nullptr : type.c_str(), configCCallback, ctx));
}

JNIEXPORT jint JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeSetLbContextProvider(JNIEnv* env, jclass /*cls*/,
                                                                       jlong handle,
                                                                       jobject provider) {
  auto* ctx = new LbContextProviderCtx();
  ctx->providerGlobalRef = env->NewGlobalRef(provider);

  return static_cast<jint>(
      envoy_client_set_lb_context_provider(reinterpret_cast<envoy_client_handle>(handle),
                                           lbContextCallback, ctx));
}

} // extern "C"
