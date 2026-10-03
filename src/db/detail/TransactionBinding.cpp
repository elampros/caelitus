/// @file
/// The per-thread record of the open transaction (private to the db module).
/// @ingroup db

#include "db/detail/TransactionBinding.hpp"

#include <algorithm>
#include <vector>

namespace caelitus::db::detail {

namespace {
// Usually zero or one entry (one per pool with an open transaction).
thread_local std::vector<TransactionBinding> t_bindings;
}  // namespace

TransactionBinding* currentTransaction(const void* owner) noexcept {
    for (auto& b : t_bindings)
        if (b.owner == owner) return &b;
    return nullptr;
}

BindingGuard::BindingGuard(const void* owner, IConnection* conn, const TransactionOptions& options) : owner_(owner) {
    t_bindings.push_back({owner, conn, options});
}

BindingGuard::~BindingGuard() {
    t_bindings.erase(std::remove_if(t_bindings.begin(), t_bindings.end(),
                                    [this](const TransactionBinding& b) { return b.owner == owner_; }),
                     t_bindings.end());
}

TransactionBinding& BindingGuard::binding() noexcept { return *currentTransaction(owner_); }

}  // namespace caelitus::db::detail
