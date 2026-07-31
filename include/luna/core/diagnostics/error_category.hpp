#pragma once

namespace Luna {

enum class ErrorCategory {
  StateNotReady,
  InvalidGlobalName,
  DuplicateGlobalName,
  NullCallable,
  Compilation,
  Runtime,
  Interrupted,
  Internal
};

}
