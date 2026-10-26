#pragma once
#include "features/auth/domain/auth_repository.hpp"
#include "features/account/domain/account_repository.hpp"
#include "features/auth/infrastructure/foghub_cookie_storage.hpp"
#include "features/providers/infrastructure/foghub_transport.hpp"
#include "features/providers/infrastructure/foghub_html.hpp"
#include <atomic>
#include <memory>
#include <map>

namespace fog::foghub {
// One serialized worker owns the session, its repositories and cookie store.
class Session {
public:
    Session(WebTransport& transport, CookieStore& storage);
    AuthData auth() const { return auth_; }
    bool authenticated() const noexcept { return authenticated_.load(); }
    void validateLogin(const LoginRequest&) const;
    AuthResult login(const LoginRequest&);
    RestoreAuthResult restore();
    void forget();
    std::string storageError() const { return storageError_; }
    UserProfile profile();
    std::vector<HistoryEntry> history();
    std::string readPublic(const std::string& path);
    void cancel() noexcept { transport_.cancel(); }
private:
    friend class Rentals;
    // Raw page evidence stays private to infrastructure. Rental commands use
    // the same complete/account-verified history reader as the read adapter.
    struct OwnHistory {
        AuthData owner;
        std::vector<HistoryRow> rows;
        std::vector<std::string> pages;
        std::map<std::string,std::size_t> rowPages;
    };
    OwnHistory ownHistory();
    std::string readRentalPage(const std::string& path);
    WebResponse submitPairing(const std::string& computer, const Query& fields, bool freshPair = false);
    WebResponse closeRental(const std::string& rental);
    WebResponse get(const std::string& path, bool requiresAuth);
    UserProfile verifiedProfile();
    void invalidate(bool removeSaved);
    void persist();
    void accept(const UserProfile& profile);
    WebTransport& transport_;
    CookieStore& storage_;
    AuthData auth_{{ProviderId::FogHub,{}}, AuthState::SignedOut, 0};
    std::atomic_bool authenticated_{false};
    std::string storageError_;
};

class Auth final : public AuthRepository {
public:
    explicit Auth(Session& session) : session_(session) {}
    AuthData auth() const override { return session_.auth(); }
    bool authenticated() const noexcept override { return session_.authenticated(); }
    AuthCapabilities capabilities() const noexcept override { return {LoginMethod::Credentials}; }
    void validateLogin(const LoginRequest& request) const override { session_.validateLogin(request); }
    AuthResult begin(const LoginRequest& request) override { return session_.login(request); }
    AuthResult answer(const AuthAnswer&) override;
    RestoreAuthResult restore() override { return session_.restore(); }
    void forget() override { session_.forget(); }
    std::string storageError() const override { return session_.storageError(); }
private:
    Session& session_;
};
class Account final : public AccountRepository {
public:
    explicit Account(Session& session) : session_(session) {}
    UserProfile profile() override { return session_.profile(); }
    std::vector<HistoryEntry> history() override { return session_.history(); }
private:
    Session& session_;
};
class Catalog final : public CatalogRepository {
public:
    explicit Catalog(Session& session) : session_(session) {}
    Page<Game> games(const GameQuery&) override;
    Game game(const EntityId&) override;
    Page<Computer> computers(const ComputerQuery&) override;
    AutomaticComputers recommend(const ComputerQuery&) override;
    std::vector<Computer> playStationComputers(const CatalogProgress& = {}, const CatalogCancellation& = {}) override;
    void cancel() noexcept override { session_.cancel(); }
private:
    Session& session_;
};

// Isolated public catalog with in-memory cookies and no account storage.
std::shared_ptr<CatalogRepository> makePublicCatalog();

// Owns the read adapters; composition attaches rental and streaming separately.
class ReadRepositories {
public:
    explicit ReadRepositories(const std::string& cookiePath);
    AuthRepository& auth() { return auth_; }
    AccountRepository& account() { return account_; }
    CatalogRepository& catalog() { return catalog_; }
    std::shared_ptr<CatalogRepository> publicCatalog();
    void cancel() noexcept { session_.cancel(); }
private:
    std::unique_ptr<WebTransport> transport_;
    std::unique_ptr<CookieStore> storage_;
    Session session_;
    Auth auth_;
    Account account_;
    Catalog catalog_;
};
}
