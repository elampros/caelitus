#pragma once

/// @file
/// The errors services report to their callers.
/// @ingroup core

#include <cstdint>
#include <stdexcept>
#include <string>

namespace caelitus {

/// Base of the errors raised by services for the caller to act on.
///
/// Each has a stable machine-readable code(); the message is for humans. The
/// API layer turns them into JSON-RPC errors (see api::JsonRpcHandler).
/// Services never let database exceptions escape: repositories translate the
/// ones with domain meaning (duplicate ISBN, author still has books) into
/// these.
class DomainError : public std::runtime_error {
public:
    /// @param code     Machine-readable code, e.g. "not_found".
    /// @param message  Human-readable description.
    DomainError(std::string code, const std::string& message) : std::runtime_error(message), code_(std::move(code)) {}
    /// The machine-readable code, stable across versions.
    const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

/// Input rejected. Code "validation_failed".
class ValidationError : public DomainError {
public:
    /// @param field    The offending input ("isbn", "authors", ...).
    /// @param message  What is wrong with it; the full message becomes "field: message".
    ValidationError(std::string field, const std::string& message)
        : DomainError("validation_failed", field + ": " + message),
          field_(std::move(field)) {}
    /// The offending input.
    const std::string& field() const noexcept { return field_; }

private:
    std::string field_;
};

/// The requested entity does not exist. Code "not_found".
class NotFoundError : public DomainError {
public:
    /// @param entity  "book", "author", ...
    /// @param id      The id that was looked up.
    NotFoundError(std::string entity, std::int64_t id)
        : DomainError("not_found", entity + " " + std::to_string(id) + " not found"),
          entity_(std::move(entity)),
          id_(id) {}
    /// For entities identified by name rather than id, e.g. a scheduled job.
    /// @param entity  "job", ...
    /// @param name    The name that was looked up.
    NotFoundError(std::string entity, const std::string& name)
        : DomainError("not_found", entity + " '" + name + "' not found"),
          entity_(std::move(entity)),
          name_(name) {}
    const std::string& entity() const noexcept { return entity_; }  ///< "book", "author", "job", ...
    std::int64_t id() const noexcept { return id_; }                ///< The id that was looked up; 0 for names.
    const std::string& name() const noexcept { return name_; }      ///< The name that was looked up; empty for ids.

private:
    std::string entity_;
    std::int64_t id_ = 0;
    std::string name_;
};

/// The request is valid but clashes with the current state: a duplicate ISBN,
/// deleting an author who still has books, a stale version, ...
///
/// code() is one of "isbn_taken", "slug_taken", "name_taken",
/// "author_has_books", "category_in_use", "version_conflict",
/// "stale_reference".
class ConflictError : public DomainError {
public:
    using DomainError::DomainError;
};

}  // namespace caelitus
