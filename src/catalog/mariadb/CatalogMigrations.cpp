/// @file
/// The catalog's schema migrations, in order. Applied migrations are never
/// edited; changes are new migrations appended at the end.
/// @ingroup catalog_mariadb

#include "caelitus/catalog/mariadb/CatalogMigrations.hpp"

namespace caelitus::catalog::mariadb {

// Never edit a migration once it has been applied anywhere; append a new one.
// Constraint names are referenced by the repositories' error translation.
std::vector<db::Migration> catalogMigrations() {
    const std::string table = " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";
    return {
        {1,
         "catalog: initial schema",
         {
             "CREATE TABLE categories ("
             " id BIGINT AUTO_INCREMENT PRIMARY KEY,"
             " name VARCHAR(100) NOT NULL,"
             " slug VARCHAR(100) NOT NULL,"
             " CONSTRAINT uq_categories_name UNIQUE (name),"
             " CONSTRAINT uq_categories_slug UNIQUE (slug))" +
                 table,

             "CREATE TABLE authors ("
             " id BIGINT AUTO_INCREMENT PRIMARY KEY,"
             " name VARCHAR(200) NOT NULL,"
             " bio TEXT NULL,"
             " birth_date DATE NULL,"
             " version INT NOT NULL DEFAULT 1,"
             " created_at DATETIME(6) NOT NULL,"
             " updated_at DATETIME(6) NOT NULL,"
             " INDEX ix_authors_name (name))" +
                 table,

             "CREATE TABLE books ("
             " id BIGINT AUTO_INCREMENT PRIMARY KEY,"
             " title VARCHAR(300) NOT NULL,"
             " isbn CHAR(13) NULL,"
             " description TEXT NULL,"
             " published_on DATE NOT NULL,"
             " language CHAR(2) NOT NULL,"
             " page_count INT NULL,"
             " category_id BIGINT NOT NULL,"
             " rating_count INT NOT NULL DEFAULT 0,"
             " rating_sum INT NOT NULL DEFAULT 0,"
             " version INT NOT NULL DEFAULT 1,"
             " created_at DATETIME(6) NOT NULL,"
             " updated_at DATETIME(6) NOT NULL,"
             " CONSTRAINT uq_books_isbn UNIQUE (isbn),"
             " CONSTRAINT fk_books_category FOREIGN KEY (category_id) REFERENCES categories(id),"
             " INDEX ix_books_category_published (category_id, published_on),"
             " INDEX ix_books_published (published_on),"
             " INDEX ix_books_created (created_at),"
             " INDEX ix_books_title (title))" +
                 table,

             "CREATE TABLE book_authors ("
             " book_id BIGINT NOT NULL,"
             " author_id BIGINT NOT NULL,"
             " position SMALLINT NOT NULL,"
             " PRIMARY KEY (book_id, author_id),"
             " CONSTRAINT fk_book_authors_book FOREIGN KEY (book_id) REFERENCES books(id) ON DELETE CASCADE,"
             " CONSTRAINT fk_book_authors_author FOREIGN KEY (author_id) REFERENCES authors(id),"
             " INDEX ix_book_authors_author (author_id))" +
                 table,

             "CREATE TABLE tags ("
             " id BIGINT AUTO_INCREMENT PRIMARY KEY,"
             " name VARCHAR(50) NOT NULL,"
             " CONSTRAINT uq_tags_name UNIQUE (name))" +
                 table,

             "CREATE TABLE book_tags ("
             " book_id BIGINT NOT NULL,"
             " tag_id BIGINT NOT NULL,"
             " PRIMARY KEY (book_id, tag_id),"
             " CONSTRAINT fk_book_tags_book FOREIGN KEY (book_id) REFERENCES books(id) ON DELETE CASCADE,"
             " CONSTRAINT fk_book_tags_tag FOREIGN KEY (tag_id) REFERENCES tags(id),"
             " INDEX ix_book_tags_tag (tag_id))" +
                 table,

             "CREATE TABLE reviews ("
             " id BIGINT AUTO_INCREMENT PRIMARY KEY,"
             " book_id BIGINT NOT NULL,"
             " reviewer_name VARCHAR(100) NOT NULL,"
             " rating TINYINT NOT NULL,"
             " title VARCHAR(200) NULL,"
             " body TEXT NOT NULL,"
             " created_at DATETIME(6) NOT NULL,"
             " updated_at DATETIME(6) NOT NULL,"
             " CONSTRAINT ck_reviews_rating CHECK (rating BETWEEN 1 AND 5),"
             " CONSTRAINT fk_reviews_book FOREIGN KEY (book_id) REFERENCES books(id) ON DELETE CASCADE,"
             " INDEX ix_reviews_book_created (book_id, created_at))" +
                 table,
         }},
        {2,
         "catalog: likes and dislikes",
         {
             // All-time totals on the book (cheap all-time ranking through the
             // indexed generated score); per-day counts for the periods.
             "ALTER TABLE books"
             " ADD COLUMN likes INT NOT NULL DEFAULT 0,"
             " ADD COLUMN dislikes INT NOT NULL DEFAULT 0,"
             " ADD COLUMN reaction_score INT AS (likes - dislikes) PERSISTENT,"
             " ADD INDEX ix_books_reaction_score (reaction_score)",

             "CREATE TABLE book_reactions_daily ("
             " book_id BIGINT NOT NULL,"
             " day DATE NOT NULL,"
             " likes INT NOT NULL DEFAULT 0,"
             " dislikes INT NOT NULL DEFAULT 0,"
             " PRIMARY KEY (book_id, day),"
             " CONSTRAINT fk_reactions_book FOREIGN KEY (book_id) REFERENCES books(id) ON DELETE CASCADE,"
             " INDEX ix_reactions_day (day))" +
                 table,
         }},
        {3,
         "catalog: per-book switch for likes and dislikes",
         {
             "ALTER TABLE books ADD COLUMN reactions_enabled BOOLEAN NOT NULL DEFAULT FALSE",
         }},
    };
}

}  // namespace caelitus::catalog::mariadb
