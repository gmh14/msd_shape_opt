#pragma once

#include <spdlog/spdlog.h>

#include <memory>
#include <cstdlib>

namespace MedialAxisRepresentation
{
class Logger
{
public:
  static void init(const char *filename = nullptr);
  static inline std::shared_ptr<spdlog::logger> lgr() { return logger; }

protected:
  static std::shared_ptr<spdlog::logger> logger;
};

#define ALOG(cond)                                                                                \
  do {                                                                                            \
    if ((cond) == false) {                                                                        \
      SPDLOG_LOGGER_CRITICAL(MedialAxisRepresentation::Logger::lgr(), "{} failed. Abort", #cond); \
      std::abort();                                                                               \
    }                                                                                             \
  } while (0)

}  // namespace MedialAxisRepresentation