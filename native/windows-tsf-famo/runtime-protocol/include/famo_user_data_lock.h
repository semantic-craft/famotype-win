#pragma once

#include <string>

namespace famo::runtime {

// Serializes every runtime/configuration transaction with the managed settings
// writer. The implementation always takes the per-SID file lock and, when the
// Global object namespace is available, the matching named mutex as well.
// Acquisition is recursive on one thread so a higher-level transaction can
// call helpers that independently enforce the same lock.
class UserDataTransactionLock {
public:
  UserDataTransactionLock() = default;
  ~UserDataTransactionLock();
  UserDataTransactionLock(const UserDataTransactionLock &) = delete;
  UserDataTransactionLock &operator=(const UserDataTransactionLock &) = delete;

  bool Acquire(std::string *error = nullptr) noexcept;
  bool held() const noexcept { return held_; }

private:
  void Release() noexcept;
  bool held_ = false;
};

} // namespace famo::runtime
