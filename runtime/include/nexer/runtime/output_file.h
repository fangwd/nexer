#ifndef NEXER_OUTPUT_FILE_H_
#define NEXER_OUTPUT_FILE_H_

#include "nexer/core/non_copyable.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace nexer {

// Append-only output file with byte-based rotation: once a write would push
// the file past max_bytes it is renamed to <path>.1 (shifting older copies up
// to <path>.<max_files>) and a fresh file is started. Writes are synchronous
// and never buffered; a disk failure drops the data and is reported through
// error() until a later write succeeds.
class OutputFile : NonCopyable {
  private:
    std::string path_;
    uint64_t max_bytes_;
    int max_files_;
    int fd_ = -1;
    uint64_t size_ = 0;
    std::string error_;

    bool open();
    void rotate();
    bool write_all(const char *data, size_t len);

  public:
    OutputFile(std::string path, uint64_t max_bytes, int max_files);
    ~OutputFile();

    void write(const char *data, size_t len);

    inline const std::string &path() const { return path_; }
    inline const std::string &error() const { return error_; }
    inline uint64_t size() const { return size_; }
};

} // namespace nexer

#endif // NEXER_OUTPUT_FILE_H_
