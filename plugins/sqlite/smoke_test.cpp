// End-to-end smoke test for the SQLite DB plugin.
//
// Loads shield_db_sqlite.dll directly via the dynamic_library helper,
// exercises the full C ABI (connect, execute, query, transaction,
// ping, disconnect), and validates the plugin contract. This is NOT
// linked into the regular ctest suite - run manually after building
// the plugin to verify the wiring.

#include "shield/data/db_plugin.h"
#include "dynamic_library.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using shield::data::detail::DynamicLibrary;
using shield::data::detail::host_executable_directory;

#define CHECK(cond, msg)                                          \
    do {                                                          \
        if (!(cond)) {                                            \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg,       \
                         __FILE__, __LINE__);                     \
            return 1;                                             \
        }                                                         \
    } while (0)

int main() {
    // 1. Load the plugin from the directory containing this exe.
    auto exe_dir = host_executable_directory();
    CHECK(!exe_dir.empty(), "could not locate host executable dir");

    auto path = exe_dir + "shield_db_sqlite.dll";
    std::string err;
    auto lib = DynamicLibrary::load(path, err);
    CHECK(lib.is_loaded(), err.c_str());

    // 2. Resolve the entry point.
    auto api_fn = reinterpret_cast<const shield_db_plugin* (*)()>(
        lib.resolve("shield_db_plugin_api"));
    CHECK(api_fn != nullptr, "missing shield_db_plugin_api export");

    const shield_db_plugin* plugin = api_fn();
    CHECK(plugin != nullptr, "plugin returned NULL api");
    CHECK(plugin->abi_version == SHIELD_DB_ABI_VERSION, "abi mismatch");
    CHECK(std::strcmp(plugin->name, "sqlite") == 0,
          "unexpected plugin name");

    std::printf("OK loaded plugin '%s' v%s abi=%u\n",
                plugin->name, plugin->version, plugin->abi_version);

    // 3. Connect to an in-memory database.
    shield_db_connect_args args{};
    args.host = nullptr;
    args.port = 0;
    args.user = nullptr;
    args.password = nullptr;
    args.database = ":memory:";
    args.connect_timeout_ms = 3000;
    args.query_timeout_ms = 3000;

    char err_buf[256] = {};
    shield_db_conn* conn = plugin->connect(&args, err_buf, sizeof(err_buf));
    CHECK(conn != nullptr, err_buf[0] ? err_buf : "connect returned NULL");

    // 4. Ping.
    CHECK(plugin->ping(conn) == 1, "ping failed on fresh connection");

    // 5. CREATE TABLE.
    shield_db_result r{};
    int rc = plugin->execute(conn,
        "CREATE TABLE players (id INTEGER PRIMARY KEY, name TEXT)", nullptr, 0, &r);
    CHECK(rc == 0, "execute transport error");
    CHECK(r.success == 1, "CREATE TABLE failed");
    plugin->free_result(&r);

    // 6. INSERT with parameters.
    const char* params1[] = { "neo" };
    rc = plugin->execute(conn, "INSERT INTO players (name) VALUES (?)",
                         params1, 1, &r);
    CHECK(rc == 0, "execute transport error");
    CHECK(r.success == 1, "INSERT failed");
    CHECK(r.affected_rows == 1, "expected 1 affected row");
    CHECK(r.last_insert_id == 1, "expected last_insert_id == 1");
    plugin->free_result(&r);

    const char* params2[] = { "trinity" };
    rc = plugin->execute(conn, "INSERT INTO players (name) VALUES (?)",
                         params2, 1, &r);
    CHECK(rc == 0 && r.success == 1, "second INSERT failed");
    CHECK(r.last_insert_id == 2, "expected last_insert_id == 2");
    plugin->free_result(&r);

    // 7. SELECT and read rows back.
    rc = plugin->query(conn, "SELECT id, name FROM players ORDER BY id",
                       nullptr, 0, &r);
    CHECK(rc == 0, "query transport error");
    CHECK(r.success == 1, "SELECT failed");
    CHECK(r.row_count == 2, "expected 2 rows");
    CHECK(r.col_count == 2, "expected 2 columns");

    // Row 0: id=1, name=neo
    CHECK(std::strcmp(r.cells[0], "1") == 0, "row0 id");
    CHECK(std::strcmp(r.cells[1], "neo") == 0, "row0 name");
    // Row 1: id=2, name=trinity
    CHECK(std::strcmp(r.cells[2], "2") == 0, "row1 id");
    CHECK(std::strcmp(r.cells[3], "trinity") == 0, "row1 name");
    plugin->free_result(&r);

    // 8. Transaction commit.
    rc = plugin->begin(conn, &r);
    CHECK(rc == 0 && r.success == 1, "BEGIN failed");
    plugin->free_result(&r);

    const char* params3[] = { "morpheus" };
    rc = plugin->execute(conn, "INSERT INTO players (name) VALUES (?)",
                         params3, 1, &r);
    CHECK(rc == 0 && r.success == 1, "txn INSERT failed");
    plugin->free_result(&r);

    rc = plugin->commit(conn, &r);
    CHECK(rc == 0 && r.success == 1, "COMMIT failed");
    plugin->free_result(&r);

    // 9. Transaction rollback.
    rc = plugin->begin(conn, &r);
    CHECK(rc == 0 && r.success == 1, "BEGIN failed");
    plugin->free_result(&r);

    const char* params4[] = { "cypher" };
    rc = plugin->execute(conn, "INSERT INTO players (name) VALUES (?)",
                         params4, 1, &r);
    CHECK(rc == 0 && r.success == 1, "txn INSERT failed");
    plugin->free_result(&r);

    rc = plugin->rollback(conn, &r);
    CHECK(rc == 0 && r.success == 1, "ROLLBACK failed");
    plugin->free_result(&r);

    // 10. Verify rollback worked: still 3 rows.
    rc = plugin->query(conn, "SELECT COUNT(*) FROM players", nullptr, 0, &r);
    CHECK(rc == 0 && r.success == 1, "COUNT failed");
    CHECK(r.row_count == 1 && r.col_count == 1, "bad COUNT shape");
    CHECK(std::strcmp(r.cells[0], "3") == 0, "expected 3 rows after rollback");
    plugin->free_result(&r);

    // 11. Error handling: bad SQL produces a soft failure.
    rc = plugin->query(conn, "SELECT * FROM nonexistent_table", nullptr, 0, &r);
    CHECK(rc == 0, "transport error on bad SQL");
    CHECK(r.success == 0, "expected soft failure on bad SQL");
    CHECK(r.error_msg != nullptr && r.error_msg[0] != '\0',
          "expected error message");
    CHECK(r.error_code != nullptr, "expected stable error code");
    std::printf("OK error path: '%s' [%s]\n", r.error_msg, r.error_code);
    plugin->free_result(&r);

    // 12. Cleanup.
    plugin->disconnect(conn);

    std::printf("\nAll SQLite plugin smoke checks passed.\n");
    return 0;
}
