/// @file
/// Checks on generated artifacts and data committed to the repository.
/// @ingroup tests

#include "CatalogFakes.hpp"
#include "TestHarness.hpp"

#include "caelitus/catalog/mariadb/CatalogMigrations.hpp"
#include "caelitus/catalog/service/AuthorService.hpp"
#include "caelitus/catalog/service/BookService.hpp"
#include "caelitus/catalog/service/CategoryService.hpp"
#include "caelitus/db/Migrations.hpp"
#include "caelitus/json/JsonTypes.hpp"

#include <fstream>
#include <map>
#include <sstream>

using namespace caelitus;
using namespace caelitus::catalog;

namespace {

std::string readSourceFile(const std::string& path) {
    std::ifstream in(std::string(CAELITUS_SOURCE_DIR) + "/" + path);
    if (!in) throw test::Failure{path + " is missing"};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

TEST(committed_schema_sql_is_current) {
    std::ifstream in(std::string(CAELITUS_SOURCE_DIR) + "/db/schema.sql");
    if (!in) throw test::Failure{"db/schema.sql is missing; run: cmake --build <build-dir> --target schema"};
    std::stringstream committed;
    committed << in.rdbuf();
    const std::string expected = db::MigrationRunner::toSql(catalog::mariadb::catalogMigrations());
    if (committed.str().find(expected) == std::string::npos)
        throw test::Failure{"db/schema.sql is out of date; run: cmake --build <build-dir> --target schema"};
}

TEST(schema_sql_records_every_migration) {
    const auto migrations = catalog::mariadb::catalogMigrations();
    const std::string sql = db::MigrationRunner::toSql(migrations);
    for (const auto& m : migrations) {
        const std::string row = "VALUES (" + std::to_string(m.version) + ", ";
        if (sql.find(row) == std::string::npos) throw test::Failure{"no schema_migrations row for " + m.name};
        if (sql.find(db::MigrationRunner::checksum(m)) == std::string::npos) throw test::Failure{"checksum missing"};
    }
}

TEST(sample_catalog_sql_has_the_current_schema) {
    const std::string sql = readSourceFile("db/sample/catalog.sql");
    if (sql.find(db::MigrationRunner::toSql(catalog::mariadb::catalogMigrations())) == std::string::npos)
        throw test::Failure{"db/sample/catalog.sql has an old schema; run: db/sample/build.sh"};
}

// Every sample book must pass the services' rules, so the seed never fails
// half-way and catalog.sql can always be rebuilt.
TEST(sample_catalog_json_passes_the_service_rules) {
    const Json data = Json::parse(readSourceFile("db/sample/catalog.json"));
    fakes::Store store;
    auto tx = std::make_shared<fakes::FakeTx>(store);
    auto categoryRepo = std::make_shared<fakes::FakeCategories>(store);
    auto authorRepo = std::make_shared<fakes::FakeAuthors>(store);
    CategoryService categories(categoryRepo);
    AuthorService authors(authorRepo, tx);
    BookService books(std::make_shared<fakes::FakeBooks>(store), authorRepo, categoryRepo,
                      std::make_shared<fakes::FakeTags>(store), tx);

    std::map<std::string, CategoryId> categoryIds;
    for (const Json& c : data.at("categories"))
        categoryIds[c.at("slug")] = categories.create(c.at("name"), c.at("slug").get<std::string>()).id;
    std::map<std::string, AuthorId> authorIds;
    std::size_t count = 0;
    for (const Json& b : data.at("books")) {
        BookInput in;
        in.title = b.at("title");
        in.publishedOn = Date(b.at("year").get<int>(), 1, 1);
        in.language = b.at("language");
        in.category = categoryIds.at(b.at("category"));
        in.tags = b.at("tags").get<std::vector<std::string>>();
        for (const std::string name : b.at("authors")) {
            auto it = authorIds.find(name);
            if (it == authorIds.end()) it = authorIds.emplace(name, authors.create({name, {}, {}}).id).first;
            in.authors.push_back(it->second);
        }
        try {
            books.create(in);
        } catch (const std::exception& e) {
            throw test::Failure{in.title + ": " + e.what()};
        }
        ++count;
    }
    CHECK(count >= 300);  // "a few hundred books"
}

/// Runs every test case of this file (see TestHarness.hpp).
int main() {
    log::LogConfig quiet;
    quiet.console = false;
    log::init(quiet);
    return test::runAll();
}
