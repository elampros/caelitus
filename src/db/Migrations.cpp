#include "caelitus/db/Migrations.hpp"

#include "caelitus/core/DateTime.hpp"
#include "caelitus/log/Log.hpp"

#include <cstdio>
#include <map>

namespace caelitus::db {

namespace {

std::string pad(int version) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%03d", version);
    return buf;
}

}  // namespace

MigrationRunner::MigrationRunner(std::shared_ptr<ConnectionPool> pool, std::string lockName)
    : pool_(std::move(pool)),
      lockName_(std::move(lockName)) {
    if (!pool_) throw std::invalid_argument("MigrationRunner: pool is null");
}

std::string MigrationRunner::checksum(const Migration& migration) {
    // FNV-1a 64: enough to notice an edited migration; not a security measure.
    std::uint64_t hash = 14695981039346656037ULL;
    auto feed = [&](const std::string& s) {
        for (char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        hash ^= 0xff;  // separator, so ["ab","c"] != ["a","bc"]
        hash *= 1099511628211ULL;
    };
    for (const auto& statement : migration.statements) feed(statement);
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(hash));
    return buf;
}

namespace {
constexpr const char* kSchemaMigrationsTable =
    "CREATE TABLE IF NOT EXISTS schema_migrations ("
    " version INT PRIMARY KEY,"
    " name VARCHAR(200) NOT NULL,"
    " checksum CHAR(16) NOT NULL,"
    " applied_at DATETIME(6) NOT NULL)";

std::string sqlString(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'' || c == '\\') out.push_back(c);
        out.push_back(c);
    }
    return out + "'";
}
}  // namespace

std::string MigrationRunner::toSql(const std::vector<Migration>& migrations) {
    std::string out;
    for (const auto& m : migrations) {
        out += "-- Migration " + pad(m.version) + ": " + m.name + "\n";
        for (const auto& statement : m.statements) out += statement + ";\n\n";
    }
    out += "-- Migration bookkeeping: the server skips migrations listed here.\n";
    out += std::string(kSchemaMigrationsTable) + ";\n\n";
    for (const auto& m : migrations)
        out += "INSERT INTO schema_migrations (version, name, checksum, applied_at) VALUES (" +
               std::to_string(m.version) + ", " + sqlString(m.name) + ", " + sqlString(checksum(m)) + ", NOW(6));\n";
    return out;
}

int MigrationRunner::migrate(const std::vector<Migration>& migrations) {
    auto log = log::get("db.migrations");

    for (std::size_t i = 0; i < migrations.size(); ++i) {
        if (migrations[i].version <= 0) throw MigrationError("Migration versions must be > 0");
        if (i > 0 && migrations[i].version <= migrations[i - 1].version)
            throw MigrationError("Migrations must be listed in strictly increasing version order");
    }

    // One connection throughout: GET_LOCK belongs to the session.
    PooledConnection conn = pool_->acquire();
    conn->execute(kSchemaMigrationsTable, {});

    auto locked = conn->query("SELECT GET_LOCK(?, ?) AS ok", {lockName_, 60});
    if (locked.empty() || locked[0].getOptional<int>("ok").value_or(0) != 1)
        throw MigrationError("Could not acquire the migration lock '" + lockName_ + "' within 60 s");

    struct Release {
        IConnection& c;
        const std::string& name;
        ~Release() {
            try {
                c.query("SELECT RELEASE_LOCK(?)", {name});
            } catch (...) {
            }
        }
    } release{*conn, lockName_};

    std::map<int, std::pair<std::string, std::string>> applied;  // version -> (name, checksum)
    for (const auto& row : conn->query("SELECT version, name, checksum FROM schema_migrations", {}))
        applied.emplace(row.get<int>("version"),
                        std::make_pair(row.get<std::string>("name"), row.get<std::string>("checksum")));

    std::map<int, const Migration*> known;
    for (const auto& m : migrations) known.emplace(m.version, &m);

    for (const auto& [version, info] : applied) {
        auto it = known.find(version);
        if (it == known.end())
            throw MigrationError("Database has migration " + pad(version) + " (" + info.first +
                                 ") which this build does not know; is this an older build?");
        if (checksum(*it->second) != info.second)
            throw MigrationError("Migration " + pad(version) + " (" + info.first +
                                 ") was changed after it was applied; add a new migration instead");
    }

    int count = 0;
    for (const auto& m : migrations) {
        if (applied.count(m.version)) continue;
        log->info("Applying migration {} ({})", pad(m.version), m.name);
        for (std::size_t i = 0; i < m.statements.size(); ++i) {
            try {
                conn->execute(m.statements[i], {});
            } catch (const DatabaseError& e) {
                throw MigrationError("Migration " + pad(m.version) + " (" + m.name + ") failed at statement " +
                                         std::to_string(i + 1) + " of " + std::to_string(m.statements.size()) +
                                         "; earlier statements are already applied: " + e.what(),
                                     e.code(), e.sqlState());
            }
        }
        conn->execute("INSERT INTO schema_migrations (version, name, checksum, applied_at) VALUES (?, ?, ?, ?)",
                      {m.version, m.name, checksum(m), nowUtc()});
        ++count;
    }

    const int current = migrations.empty() ? 0 : migrations.back().version;
    if (count > 0) log->info("Database schema at version {} ({} migration(s) applied)", pad(current), count);
    else log->debug("Database schema up to date at version {}", pad(current));
    return count;
}

}  // namespace caelitus::db
