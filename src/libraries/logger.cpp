#include "logger.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <cstring>

std::shared_ptr<spdlog::logger> MedialAxisRepresentation::Logger::logger;

void MedialAxisRepresentation::Logger::init(const char *filename)
{
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  console_sink->set_level(spdlog::level::trace);
  console_sink->set_pattern("%^[%D][%H:%M:%S:%e][%L][%n][%s:%#] %v%$");

  if (filename && std::strlen(filename)) {
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename, false);
    file_sink->set_level(spdlog::level::trace);
    file_sink->set_pattern("[%D][%H:%M:%S:%e][%L][%n][%s:%#] %v");

    logger = std::make_shared<spdlog::logger>("medial-axis", spdlog::sinks_init_list{ console_sink, file_sink });
  }
  else {
    logger = std::make_shared<spdlog::logger>("medial-axis", spdlog::sinks_init_list{ console_sink });
  }

  logger->set_level(spdlog::level::trace);
  spdlog::register_logger(logger);
}