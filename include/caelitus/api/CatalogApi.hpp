#pragma once

/// @file
/// The catalog's JSON-RPC methods.
/// @ingroup api

#include "caelitus/api/JsonRpc.hpp"
#include "caelitus/api/OpenRpc.hpp"
#include "caelitus/catalog/service/AuthorService.hpp"
#include "caelitus/catalog/service/BookService.hpp"
#include "caelitus/catalog/service/CategoryService.hpp"
#include "caelitus/catalog/service/ReactionService.hpp"
#include "caelitus/catalog/service/ReviewService.hpp"

#include <memory>

namespace caelitus::api {

/// The services the API methods call.
struct CatalogServices {
    std::shared_ptr<catalog::CategoryService> categories;  ///< categories.*
    std::shared_ptr<catalog::AuthorService> authors;       ///< authors.*
    std::shared_ptr<catalog::BookService> books;           ///< books.*, tags.list
    std::shared_ptr<catalog::ReviewService> reviews;       ///< reviews.*
    std::shared_ptr<catalog::ReactionService> reactions;   ///< reactions.*
};

/// Title, version and description of the catalog API, for OpenRPC.
ApiInfo catalogApiInfo();

/// Registers the catalog schemas and methods (categories.*, authors.*,
/// books.*, tags.*, reviews.*, reactions.*), system.ping and rpc.discover.
///
/// Each method only converts JSON to service calls and back; the rules live in
/// the services. Services are only used when a method runs, so null services
/// are fine for generating the OpenRPC document (`caelitus --openrpc`).
void registerCatalogApi(JsonRpcHandler& rpc, const CatalogServices& services);

}  // namespace caelitus::api
