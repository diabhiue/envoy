#pragma once

#include "absl/time/time.h"

#include "client/library/common/config_store.h"

namespace Envoy {
namespace Client {

/**
 * Abstract interface for ClientEngine.
 *
 * Exists to allow test injection of a mock engine into EnvoyClient::Client
 * without starting a real Envoy server.
 */
class ClientEngineInterface {
public:
  virtual ~ClientEngineInterface() = default;

  virtual bool waitReady(absl::Duration timeout) = 0;
  virtual void terminate() = 0;
  virtual bool isTerminated() const = 0;
  virtual ConfigStore& configStore() = 0;
};

} // namespace Client
} // namespace Envoy
