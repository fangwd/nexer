/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef _NEXER_LOGGER_H_
#define _NEXER_LOGGER_H_

#include <stdarg.h>
#include <mutex>

/**
 * Usage:
 *
 * Logger logger("my.log", Logger::Level::Debug);
 *
 * logger.info("value=%d", 100);
 *
 * Note: Logger has a fixed line width of 4095. If a log entry is too long,
 * a truncated line (with "..." at the end) will be written to the specified file,
 * and the whole content of that entry will be written to stderr.
 */

class Logger {
  public:
    enum class Level { TRACE = 0, DEBUG, INFO, WARN, ERROR, FATAL, CRITICAL };
    static bool ParseLevel(Level&, const char *);

    Logger(const char *filename = nullptr, Level level = Level::INFO);
    ~Logger();

    int open(const char *filename = nullptr, Level level = Level::INFO);

    void trace(const char *fmt, ...);
    void debug(const char *fmt, ...);
    void info(const char *fmt, ...);
    void warn(const char *fmt, ...);
    void error(const char *fmt, ...);
    void fatal(const char *fmt, ...);
    void critical(const char *fmt, ...);

    inline void set_level(Level level) {
        level_ = level;
    }

  private:
    int fd_;
    Level level_;
    char buf_[4096];

    std::mutex mut_;
    void vprint(Level level, const char *fmt, va_list args);
};

extern Logger default_logger;

#define log_trace(fmt,...) default_logger.trace(fmt,##__VA_ARGS__)
#define log_debug(fmt,...) default_logger.debug(fmt,##__VA_ARGS__)
#define log_info(fmt,...) default_logger.info(fmt,##__VA_ARGS__)
#define log_warn(fmt,...) default_logger.warn(fmt,##__VA_ARGS__)
#define log_error(fmt,...) default_logger.error(fmt,##__VA_ARGS__)
#define log_fatal(fmt,...) default_logger.fatal(fmt,##__VA_ARGS__)
#define log_critical(fmt,...) default_logger.critical(fmt,##__VA_ARGS__)
#define log_set_level(level) default_logger.set_level(level)

#endif
