#include <jni.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------
// Filter-chain callback context
// ---------------------------------------------------------------------------

struct FilterCallbackCtx {
  // Channel between the C callback and the Java thread waiting for results.
  // We use a global ref to a java.util.concurrent.SynchronousQueue.
  // But since Phase 2 is synchronous (callback fires before the C function
  // returns), we just store the result directly in a pair of fields and use
  // a simple mutex+condvar. The JNI thread blocks on condvar until result_ready
  // is set by the callback on the same (or dispatcher) thread.
  //
  // Because Phase 2 is truly synchronous (callback fires on the calling thread
  // before envoy_client_apply_request_filters returns), the simplest approach
  // is to store the result in a struct that the caller can read after the call.
  bool result_ready = false;
  envoy_client_status status = ENVOY_CLIENT_ERROR;
  jobjectArray result_headers = nullptr; // Local ref; caller must delete
};

// Converts a flat Java String[] [k0,v0,k1,v1,...] to envoy_client_headers.
// Caller must call FilterChainManager::freeHeaders or envoy_client_free_headers.
static envoy_client_headers* jstringArrayToHeaders(JNIEnv* env, jobjectArray arr) {
  if (arr == nullptr) {
    auto* h = static_cast<envoy_client_headers*>(malloc(sizeof(envoy_client_headers)));
    h->headers = nullptr;
    h->count = 0;
    return h;
  }
  jsize total = env->GetArrayLength(arr);
  jsize n = total / 2;
  auto* h = static_cast<envoy_client_headers*>(malloc(sizeof(envoy_client_headers)));
  h->count = static_cast<size_t>(n);
  if (n == 0) {
    h->headers = nullptr;
    return h;
  }
  h->headers =
      static_cast<envoy_client_header*>(calloc(static_cast<size_t>(n), sizeof(envoy_client_header)));
  for (jsize i = 0; i < n; ++i) {
    auto key_js = (jstring)env->GetObjectArrayElement(arr, i * 2);
    auto val_js = (jstring)env->GetObjectArrayElement(arr, i * 2 + 1);
    std::string key = jstringToString(env, key_js);
    std::string val = jstringToString(env, val_js);
    env->DeleteLocalRef(key_js);
    env->DeleteLocalRef(val_js);
    h->headers[i].key = strdup(key.c_str());
    h->headers[i].key_len = key.size();
    h->headers[i].value = strdup(val.c_str());
    h->headers[i].value_len = val.size();
  }
  return h;
}

// Converts envoy_client_headers to a flat Java String[] [k0,v0,k1,v1,...].
static jobjectArray headersToJstringArray(JNIEnv* env, const envoy_client_headers* h) {
  jclass strCls = env->FindClass("java/lang/String");
  if (h == nullptr || h->count == 0 || h->headers == nullptr) {
    jobjectArray empty = env->NewObjectArray(0, strCls, nullptr);
    env->DeleteLocalRef(strCls);
    return empty;
  }
  jsize total = static_cast<jsize>(h->count) * 2;
  jobjectArray arr = env->NewObjectArray(total, strCls, nullptr);
  env->DeleteLocalRef(strCls);
  for (size_t i = 0; i < h->count; ++i) {
    jstring key = newJString(env, h->headers[i].key);
    jstring val = newJString(env, h->headers[i].value);
    env->SetObjectArrayElement(arr, static_cast<jsize>(i * 2), key);
    env->SetObjectArrayElement(arr, static_cast<jsize>(i * 2 + 1), val);
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(val);
  }
  return arr;
}

// C filter callback used from nativeApplyRequestFilters / nativeApplyResponseFilters.
// Since Phase 2 is synchronous, this fires on the calling thread before
// envoy_client_apply_request_filters returns.
static void filterCallback(envoy_client_status status, envoy_client_headers* modified_headers,
                            void* context) {
  auto* ctx = static_cast<FilterCallbackCtx*>(context);
  ctx->status = status;

  // Convert C headers → Java. We must do this here because we own modified_headers.
  bool attached = false;
  JNIEnv* env = getEnv(&attached);
  if (env != nullptr && modified_headers != nullptr) {
    ctx->result_headers = headersToJstringArray(env, modified_headers);
  }
  envoy_client_free_headers(modified_headers);
  maybeDetach(attached);

  ctx->result_ready = true;
}

// ---------------------------------------------------------------------------
// Interceptor callback context
// ---------------------------------------------------------------------------

struct InterceptorCtx {
  jobject interceptorGlobalRef; // io.envoyproxy.envoyclient.Interceptor
};

static envoy_client_status interceptorCCallback(envoy_client_headers* headers,
                                                  const char* cluster_name,
                                                  envoy_client_interceptor_phase phase,
                                                  void* context) {
  auto* ctx = static_cast<InterceptorCtx*>(context);
  bool attached = false;
  JNIEnv* env = getEnv(&attached);
  if (env == nullptr) return ENVOY_CLIENT_ERROR;

  // Build Java String[] from C headers.
  jobjectArray jHeaders = headersToJstringArray(env, headers);

  // Build InterceptorPhase enum.
  jclass phaseCls = env->FindClass("io/envoyproxy/envoyclient/InterceptorPhase");
  jmethodID fromInt =
      env->GetStaticMethodID(phaseCls, "fromInt", "(I)Lio/envoyproxy/envoyclient/InterceptorPhase;");
  jobject phaseObj = env->CallStaticObjectMethod(phaseCls, fromInt, (jint)phase);

  // Call Interceptor.onHeaders(Map<String,String>, String, InterceptorPhase).
  // We pass the flat String[] and cluster as an "encoded map"; the Interceptor
  // interface works with Map<String,String>, so we bridge via a helper that
  // converts the array to a LinkedHashMap, calls onHeaders, then reads back.
  //
  // Since Interceptor.onHeaders takes Map<String,String>, we build a
  // LinkedHashMap first.
  jclass mapCls = env->FindClass("java/util/LinkedHashMap");
  jmethodID mapCtor = env->GetMethodID(mapCls, "<init>", "()V");
  jobject mapObj = env->NewObject(mapCls, mapCtor);
  jmethodID putMethod =
      env->GetMethodID(mapCls, "put",
                       "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

  jsize total = env->GetArrayLength(jHeaders);
  for (jsize i = 0; i + 1 < total; i += 2) {
    jobject k = env->GetObjectArrayElement(jHeaders, i);
    jobject v = env->GetObjectArrayElement(jHeaders, i + 1);
    env->CallObjectMethod(mapObj, putMethod, k, v);
    env->DeleteLocalRef(k);
    env->DeleteLocalRef(v);
  }

  jstring jCluster = newJString(env, cluster_name);
  jclass interceptorCls = env->GetObjectClass(ctx->interceptorGlobalRef);
  jmethodID onHeaders = env->GetMethodID(
      interceptorCls, "onHeaders",
      "(Ljava/util/Map;Ljava/lang/String;Lio/envoyproxy/envoyclient/InterceptorPhase;)Z");
  jboolean cont =
      env->CallBooleanMethod(ctx->interceptorGlobalRef, onHeaders, mapObj, jCluster, phaseObj);

  if (cont && headers != nullptr) {
    // Read back modified entries from the map and write into C headers.
    // We iterate over the map's entrySet to pick up any mutations.
    jclass setClass = env->FindClass("java/util/Set");
    jmethodID entrySet = env->GetMethodID(mapCls, "entrySet", "()Ljava/util/Set;");
    jobject entries = env->CallObjectMethod(mapObj, entrySet);
    jmethodID iterator = env->GetMethodID(setClass, "iterator", "()Ljava/util/Iterator;");
    jclass iterClass = env->FindClass("java/util/Iterator");
    jobject iter = env->CallObjectMethod(entries, iterator);
    jmethodID hasNext = env->GetMethodID(iterClass, "hasNext", "()Z");
    jmethodID next = env->GetMethodID(iterClass, "next", "()Ljava/lang/Object;");
    jclass entryClass = env->FindClass("java/util/Map$Entry");
    jmethodID getKey =
        env->GetMethodID(entryClass, "getKey", "()Ljava/lang/Object;");
    jmethodID getValue =
        env->GetMethodID(entryClass, "getValue", "()Ljava/lang/Object;");

    // Collect new entries
    std::vector<std::string> keys, values;
    while (env->CallBooleanMethod(iter, hasNext)) {
      jobject entry = env->CallObjectMethod(iter, next);
      auto k = (jstring)env->CallObjectMethod(entry, getKey);
      auto v = (jstring)env->CallObjectMethod(entry, getValue);
      keys.push_back(jstringToString(env, k));
      values.push_back(jstringToString(env, v));
      env->DeleteLocalRef(k);
      env->DeleteLocalRef(v);
      env->DeleteLocalRef(entry);
    }

    // Rebuild C headers in-place.
    for (size_t i = 0; i < headers->count; ++i) {
      free(const_cast<char*>(headers->headers[i].key));
      free(const_cast<char*>(headers->headers[i].value));
    }
    free(headers->headers);
    headers->count = keys.size();
    headers->headers = static_cast<envoy_client_header*>(
        calloc(keys.size(), sizeof(envoy_client_header)));
    for (size_t i = 0; i < keys.size(); ++i) {
      headers->headers[i].key = strdup(keys[i].c_str());
      headers->headers[i].key_len = keys[i].size();
      headers->headers[i].value = strdup(values[i].c_str());
      headers->headers[i].value_len = values[i].size();
    }

    env->DeleteLocalRef(iter);
    env->DeleteLocalRef(entries);
    env->DeleteLocalRef(setClass);
    env->DeleteLocalRef(iterClass);
    env->DeleteLocalRef(entryClass);
  }

  env->DeleteLocalRef(mapObj);
  env->DeleteLocalRef(mapCls);
  env->DeleteLocalRef(jHeaders);
  env->DeleteLocalRef(jCluster);
  env->DeleteLocalRef(phaseObj);
  env->DeleteLocalRef(phaseCls);
  env->DeleteLocalRef(interceptorCls);

  maybeDetach(attached);
  return cont ? ENVOY_CLIENT_OK : ENVOY_CLIENT_DENIED;
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

JNIEXPORT jobjectArray JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeApplyRequestFilters(
    JNIEnv* env, jclass /*cls*/, jlong handle, jstring clusterName, jobjectArray headers) {
  std::string cluster = jstringToString(env, clusterName);
  envoy_client_headers* cHeaders = jstringArrayToHeaders(env, headers);

  FilterCallbackCtx ctx;
  ctx.result_ready = false;
  ctx.result_headers = nullptr;

  auto h = reinterpret_cast<envoy_client_handle>(handle);
  envoy_client_apply_request_filters(h, cluster.c_str(), cHeaders, filterCallback, &ctx);

  // Free the input headers copy (library made its own copy).
  for (size_t i = 0; i < cHeaders->count; ++i) {
    free(const_cast<char*>(cHeaders->headers[i].key));
    free(const_cast<char*>(cHeaders->headers[i].value));
  }
  if (cHeaders->headers) free(cHeaders->headers);
  free(cHeaders);

  if (!ctx.result_ready || ctx.status == ENVOY_CLIENT_DENIED) return nullptr;
  return ctx.result_headers;
}

JNIEXPORT jobjectArray JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeApplyResponseFilters(
    JNIEnv* env, jclass /*cls*/, jlong handle, jstring clusterName, jobjectArray headers) {
  std::string cluster = jstringToString(env, clusterName);
  envoy_client_headers* cHeaders = jstringArrayToHeaders(env, headers);

  FilterCallbackCtx ctx;
  ctx.result_ready = false;
  ctx.result_headers = nullptr;

  auto h = reinterpret_cast<envoy_client_handle>(handle);
  envoy_client_apply_response_filters(h, cluster.c_str(), cHeaders, filterCallback, &ctx);

  for (size_t i = 0; i < cHeaders->count; ++i) {
    free(const_cast<char*>(cHeaders->headers[i].key));
    free(const_cast<char*>(cHeaders->headers[i].value));
  }
  if (cHeaders->headers) free(cHeaders->headers);
  free(cHeaders);

  if (!ctx.result_ready || ctx.status == ENVOY_CLIENT_DENIED) return nullptr;
  return ctx.result_headers;
}

JNIEXPORT jint JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeAddInterceptor(JNIEnv* env, jclass /*cls*/,
                                                                 jlong handle, jstring name,
                                                                 jobject interceptor) {
  std::string nameStr = jstringToString(env, name);
  auto* ctx = new InterceptorCtx();
  ctx->interceptorGlobalRef = env->NewGlobalRef(interceptor);

  return static_cast<jint>(
      envoy_client_add_interceptor(reinterpret_cast<envoy_client_handle>(handle), nameStr.c_str(),
                                    interceptorCCallback, ctx));
}

JNIEXPORT jint JNICALL
Java_io_envoyproxy_envoyclient_EnvoyClient_nativeRemoveInterceptor(JNIEnv* env, jclass /*cls*/,
                                                                    jlong handle, jstring name) {
  std::string nameStr = jstringToString(env, name);
  return static_cast<jint>(
      envoy_client_remove_interceptor(reinterpret_cast<envoy_client_handle>(handle),
                                       nameStr.c_str()));
}

} // extern "C"
