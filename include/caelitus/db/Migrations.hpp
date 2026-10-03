#pragma once

/// @file
/// Versioned schema changes.
/// @ingroup db

#include "caelitus/db/ConnectionPool.hpp"
#include "caelitus/db/DbErrors.hpp"

#include <memory>
#include <string>
#include <vector>

namespace caelitus::db {

/// One schema change.
///
/// Statements run one by one (the connector does not run multi-statement
/// strings). Never edit a migration that has been applied anywhere: add a new
/// one. MigrationRunner detects edits through a checksum.
struct Migration {
    int version;                          ///< > 0, strictly increasing.
    std::string name;                     ///< Short description, e.g. "catalog: likes and dislikes".
    std::vector<std::string> statements;  ///< SQL statements, run in order.
};

/// The schema cannot be brought up to date: an applied migration was edited,
/// the database is newer than this build, or a statement failed.
class MigrationError : public DatabaseError {
public:
    using DatabaseError::DatabaseError;
};

/// Brings the schema up to date at startup.
///
/// - Applied migrations are recorded in the `schema_migrations` table.
/// - A named lock (GET_LOCK) keeps two instances from migrating at once.
/// - Refuses to run if an applied migration was changed, or if the database
///   has a migration this build does not know (it was built from older code).
///
/// MariaDB commits DDL implicitly, so a migration that fails half-way leaves
/// its earlier statements applied and is **not** recorded; fix it by hand.
/// Keep migrations small to make that rare and easy.
///
/// The SQL is MariaDB/MySQL specific (GET_LOCK, the `schema_migrations` DDL).
class MigrationRunner {
public:
    /// @param pool      Where to migrate.
    /// @param lockName  Name of the GET_LOCK lock that serializes instances.
    explicit MigrationRunner(std::shared_ptr<ConnectionPool> pool, std::string lockName = "caelitus_migrations");

    /// Applies pending migrations in version order.
    /// @return How many were applied (0 if the schema was up to date).
    /// @throws MigrationError, or another DatabaseError if a statement fails.
    int migrate(const std::vector<Migration>& migrations);

    /// Checksum recorded for an applied migration (FNV-1a over its statements).
    static std::string checksum(const Migration& migration);

    /// The whole schema as one SQL script.
    ///
    /// Every migration's statements, then the `schema_migrations` table with a
    /// row per migration, so a database built from the script is recognized
    /// as up to date. `caelitus --schema` prints it (db/schema.sql).
    static std::string toSql(const std::vector<Migration>& migrations);

private:
    std::shared_ptr<ConnectionPool> pool_;
    std::string lockName_;
};

}  // namespace caelitus::db
