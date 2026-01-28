#pragma once
#define QUILL_DISABLE_NON_PREFIXED_MACROS

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"
#include <string_view>

namespace {
    inline quill::Logger* initialize_logger() {
        static quill::Logger* logger = []() {
            quill::Backend::start();

            return quill::Frontend::create_or_get_logger(
                "root", 
                quill::Frontend::create_or_get_sink<quill::ConsoleSink>("sink_id_1"),
                quill::PatternFormatterOptions {"[%(time)] [PID=%(process_id)] [TID=%(thread_id)] [LOG_%(log_level:<4)] %(message)", "%Y-%m-%d %H:%M:%S.%Qns", quill::Timezone::GmtTime }
            );
        }();
        logger->set_log_level(quill::LogLevel::Debug);
        return logger;
    }
}

#define LOG(level, message, ...) \
    do { \
        quill::Logger* logger = initialize_logger(); \
        level(logger, message, ##__VA_ARGS__); \
    } while (0)

#define LOG_INFO(message, ...)    LOG(QUILL_LOG_INFO, message, ##__VA_ARGS__)
#define LOG_ERROR(message, ...)   LOG(QUILL_LOG_ERROR, message, ##__VA_ARGS__)
#define LOG_WARNING(message, ...) LOG(QUILL_LOG_WARNING, message, ##__VA_ARGS__)
#define LOG_DEBUG(message, ...)   LOG(QUILL_LOG_DEBUG, message, ##__VA_ARGS__)
#define LOG_CRITICAL(message, ...) LOG(QUILL_LOG_CRITICAL, message, ##__VA_ARGS__)
