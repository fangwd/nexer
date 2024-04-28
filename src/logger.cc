/* Copyright (c) Weidong Fang. All rights reserved. */

#include "logger.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

Logger::Logger(const char *filename, Logger::Level level) {
    open(filename, level);
}

int Logger::open(const char *filename, Logger::Level level) {
    int error = 0;
    if (filename) {
        if ((fd_ = ::open(filename, O_APPEND | O_WRONLY | O_CREAT, 0644)) == -1) {
            fd_ = 2;
            error = 1;
        }
    } else {
        fd_ = 1;
    }
    level_ = level;
    return error;
}

Logger::~Logger() {
    close(fd_);
}

void Logger::trace(const char *fmt, ...) {
    va_list args;
    if (level_ <= Level::TRACE) {
        va_start(args, fmt);
        vprint(Level::TRACE, fmt, args);
        va_end(args);
    }
}

void Logger::debug(const char *fmt, ...) {
    va_list args;
    if (level_ <= Level::DEBUG) {
        va_start(args, fmt);
        vprint(Level::DEBUG, fmt, args);
        va_end(args);
    }
}

void Logger::info(const char *fmt, ...) {
    va_list args;
    if (level_ <= Level::INFO) {
        va_start(args, fmt);
        vprint(Level::INFO, fmt, args);
        va_end(args);
    }
}

void Logger::warn(const char *fmt, ...) {
    va_list args;
    if (level_ <= Level::WARN) {
        va_start(args, fmt);
        vprint(Level::WARN, fmt, args);
        va_end(args);
    }
}

void Logger::error(const char *fmt, ...) {
    va_list args;
    if (level_ <= Level::ERROR) {
        va_start(args, fmt);
        vprint(Level::ERROR, fmt, args);
        va_end(args);
    }
}

void Logger::fatal(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(Level::FATAL, fmt, args);
    va_end(args);
}

void Logger::critical(const char *fmt, ...) {
    va_list args;
    if (level_ <= Level::TRACE) {
        va_start(args, fmt);
        vprint(Level::CRITICAL, fmt, args);
        va_end(args);
    }
}

static const char *level_names[] = {"TRACE ", "DEBUG ", "INFO ",    "WARN ",
                                    "ERROR ", "FATAL ", "CRITICAL "
                                   };
static size_t level_name_lengths[] = {6, 6, 5, 5, 6, 6, 9};

void Logger::vprint(Level level, const char *fmt, va_list args) {
    time_t t = time(0);
    struct tm *tm = localtime(&t);

    std::lock_guard<std::mutex> guard(mut_);

    strftime(buf_, sizeof(buf_), "%Y-%m-%d %H:%M:%S ", tm);

    memcpy(buf_ + 20, level_names[(int)level], level_name_lengths[(int)level]);

    int offset = 20 + level_name_lengths[(int)level];

    va_list cp;
    va_copy(cp, args);
    int len = vsnprintf(buf_ + offset, sizeof(buf_) - offset, fmt, cp);
    va_end(cp);

    if (len > 0) {
        if (offset + len < sizeof(buf_)) {
            buf_[offset + len] = '\n';
            write(fd_, buf_, offset + len + 1);
        } else {
            va_list cp;
            va_copy(cp, args);
            fprintf(stderr, "%.*s%s", 20, buf_, level_names[(int)level]);
            vfprintf(stderr, fmt, cp);
            fprintf(stderr, "\n");
            fflush(stderr);
            va_end(cp);
            memcpy(buf_ + sizeof(buf_) - 4, "...\n", 4);
            write(fd_, buf_, sizeof(buf_));
        }
    }
}

Logger default_logger;

bool Logger::ParseLevel(Level &level, const char *name) {
    size_t name_length = strlen(name);
    for (size_t i = 0; i < sizeof(level_names) / sizeof(level_names[0]); i++) {
        if (name_length == level_name_lengths[i] - 1 && strncmp(name, level_names[i], name_length) == 0) {
            level = Level(i);
            return true;
        }
    }
    return false;
}
