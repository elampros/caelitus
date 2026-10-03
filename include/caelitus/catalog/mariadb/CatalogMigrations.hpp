#pragma once

/// @file
/// The catalog's database schema, as migrations.
/// @ingroup catalog_mariadb

#include "caelitus/db/Migrations.hpp"

#include <vector>

namespace caelitus::catalog::mariadb {

/// Schema of the book catalog, in version order.
///
/// Applied at startup by db::MigrationRunner; `caelitus --schema` prints them
/// as one script (db/schema.sql). To change the schema, append a migration;
/// never edit one that has been released.
std::vector<db::Migration> catalogMigrations();

}  // namespace caelitus::catalog::mariadb
