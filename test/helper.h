#ifndef NEXER_TEST_HELPER_H_
#define NEXER_TEST_HELPER_H_

#include <sqlite3.h>

#include "non_copyable.h"
#include "string_buffer.h"
#include <string>
#include <map>

#define DEFAULT_EXIT_CODE 127

#define NEXER_TEST_DB "nexer-test.db"

namespace nexer {
namespace test {

struct ExecInfo {
    int sleep_time;
    std::string message;
    int exit_code;
};

int RunHelper(const char *name, int argc, char **argv);
void SetScenario(const char *name);

namespace db {

class Database : NonCopyable {
  private:
    sqlite3 *db_;

    int CreateTables();
    int table_count();

    int GetProcessStep(const std::string&);
    int Exec(const std::string&);

    static std::map<std::string, std::string> scenarios_;

  public:
    Database();
    ~Database();

    void SetScenario(const char *);

    void OpenProcess(const char *name, ExecInfo&);
    void CloseProcess(const char *name);
};

}

}  // namespace test
}  // namespace nexer

#endif  // NEXER_TEST_HELPER_H_
