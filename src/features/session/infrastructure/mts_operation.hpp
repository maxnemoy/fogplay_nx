#pragma once
#include "core/domain/repository_error.hpp"
#include <utility>
#include <nlohmann/json.hpp>
namespace fog {
template<class Operation> decltype(auto) mtsOperation(Operation&& operation) {
    try { return std::forward<Operation>(operation)(); }
    catch (const RepositoryError&) { throw; }
    catch (const nlohmann::json::exception&) { throw RepositoryError(RepositoryErrorKind::InvalidResponse,"Invalid MTS response"); }
    catch (const std::logic_error& error) { throw RepositoryError(RepositoryErrorKind::InvalidRequest,error.what()); }
    catch (const std::runtime_error& error) { throw RepositoryError(RepositoryErrorKind::InvalidResponse,error.what()); }
}
}
