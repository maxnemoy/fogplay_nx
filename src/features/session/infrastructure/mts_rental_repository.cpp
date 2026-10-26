#include "features/session/infrastructure/mts_rental_repository.hpp"
#include "features/session/infrastructure/mts_operation.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace fog {
namespace {
EntityId mtsId(std::int64_t value) {
    return {ProviderId::Mts, value > 0 ? std::to_string(value) : std::string{}};
}
std::int64_t positiveInteger(const Json& object, const char* key) {
    const auto item = object.find(key);
    if (item == object.end() || !item->is_number_integer()) return 0;
    if (item->is_number_unsigned() && item->get<std::uint64_t>() >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return 0;
    const auto value = item->get<std::int64_t>();
    return value > 0 ? value : 0;
}
std::string text(const Json& object, const char* key) {
    const auto item = object.find(key);
    return item != object.end() && item->is_string() ? item->get<std::string>() : std::string{};
}
std::int64_t number(const EntityId& id, std::int64_t maximum) {
    if (id.provider != ProviderId::Mts || id.value.empty() || id.value.size() > 19)
        throw std::invalid_argument("Invalid MTS identifier");
    std::int64_t value = 0;
    for (const auto ch : id.value) {
        if (ch < '0' || ch > '9' || value > (maximum - (ch - '0')) / 10)
            throw std::invalid_argument("Invalid MTS identifier");
        value = value * 10 + (ch - '0');
    }
    if (!value) throw std::invalid_argument("Invalid MTS identifier");
    return value;
}
bool paymentLink(const std::string& url) {
    return url.size() > 8 && url.size() <= 4096 && url.rfind("https://", 0) == 0 &&
        std::all_of(url.begin(), url.end(), [](unsigned char c) { return c > 32 && c != 127; });
}
}
AuthData MtsRentalRepository::auth() const {
    return {{ProviderId::Mts, api_.accountIdentity()},
        api_.loggedIn() ? AuthState::Authenticated : AuthState::SignedOut, api_.authGeneration()};
}
std::vector<Rental> MtsRentalRepository::activeSessions(bool shutdown) {
    return mtsOperation([&] {
        if (!api_.loggedIn()) throw RepositoryError(RepositoryErrorKind::NotAuthenticated, "Sign in first");
        const auto data = api_.activeSessions(shutdown);
        if (!data.is_array() || data.size() > 10000) throw std::runtime_error("Invalid MTS active rental response");
        std::vector<Rental> rentals;
        rentals.reserve(data.size());
        for (const auto& item : data) {
            const auto hash = text(item, "hash");
            if (!item.is_object() || !validSessionHash(hash)) throw std::runtime_error("Invalid MTS active rental identity");
            const auto name = item.find("computer_name");
            if (name != item.end() && !name->is_null() && !name->is_string())
                throw std::runtime_error("Invalid MTS active rental computer name");
            Rental rental{{ProviderId::Mts, hash}, text(item, "computer_name"), std::nullopt};
            auto computer = positiveInteger(item, "computer_id");
            if (!computer) {
                const auto idText = text(item, "computer_id");
                if (!idText.empty()) {
                    try { computer = number({ProviderId::Mts, idText}, (std::numeric_limits<int>::max)()); }
                    catch (const std::invalid_argument&) {}
                }
            }
            if (computer > 0 && computer <= (std::numeric_limits<int>::max)()) rental.computerId = mtsId(computer);
            rentals.push_back(std::move(rental));
        }
        return rentals;
    });
}
bool MtsRentalRepository::finishSession(const RentalId& rental) {
    return mtsOperation([&] {
        if (rental.provider != ProviderId::Mts || !validRentalId(rental))
            throw std::invalid_argument("Invalid MTS rental identifier");
        if (!api_.loggedIn()) throw RepositoryError(RepositoryErrorKind::NotAuthenticated, "Sign in first");
        return api_.finishSession(rental.value);
    });
}
CreationReceipt MtsRentalRepository::createOrder(const StartRentalRequest& request) {
    return mtsOperation([&] {
        CreateOrderRequest dto;
        dto.computerId = static_cast<int>(number(request.computerId, (std::numeric_limits<int>::max)()));
        dto.tariffId = static_cast<int>(number(request.offerId, (std::numeric_limits<int>::max)()));
        dto.gameId = request.gameId.empty() ? 0 : static_cast<int>(number(request.gameId, (std::numeric_limits<int>::max)()));
        dto.ownSteamAccount = request.ownAccount;
        if (!api_.loggedIn()) throw RepositoryError(RepositoryErrorKind::NotAuthenticated, "Sign in first");
        Json data;
        try { data = api_.createOrder(dto); }
        catch (...) { throw RepositoryError(RepositoryErrorKind::OutcomeUnknown, "MTS rental creation outcome is unknown"); }
        if (!data.is_object()) throw RepositoryError(RepositoryErrorKind::OutcomeUnknown, "MTS rental creation outcome is unknown");
        CreationReceipt receipt;
        receipt.id = mtsId(positiveInteger(data, "orderId"));
        receipt.reportedError = !text(data, "error").empty();
        const auto link = text(data, "paymentUrl");
        if (paymentLink(link)) receipt.payment = PaymentAction{link, text(data, "paymentAmount")};
        return receipt;
    });
}
RentalPreparation MtsRentalRepository::orderStatus(const CreationId& creation) {
    return mtsOperation([&] {
        if (!api_.loggedIn()) throw RepositoryError(RepositoryErrorKind::NotAuthenticated, "Sign in first");
        const auto data = api_.orderStatus(number(creation, (std::numeric_limits<std::int64_t>::max)()));
        if (!data.is_object()) throw std::runtime_error("Invalid MTS preparation response");
        RentalPreparation result;
        result.id = mtsId(positiveInteger(data, "id"));
        const auto status = data.find("status");
        if (status != data.end() && status->is_number_integer() &&
            *status >= (std::numeric_limits<int>::min)() && *status <= (std::numeric_limits<int>::max)())
            result.diagnosticStatus = status->get<int>();
        switch (result.diagnosticStatus) {
        case 0: result.state = PreparationState::Pending; break;
        case 1: result.state = PreparationState::Preparing; break;
        case 2: result.state = PreparationState::PaymentConfirmed; break;
        case 3: result.state = PreparationState::Unpaid; break;
        default: break;
        }
        const auto cancelled = data.find("found_cancel");
        if (cancelled != data.end()) result.cancellation = !cancelled->is_boolean() ? CancellationState::Invalid :
            cancelled->get<bool>() ? CancellationState::Requested : CancellationState::NotRequested;
        const auto hash = text(data, "hash");
        result.diagnosticRentalPresent = std::any_of(hash.begin(), hash.end(), [](unsigned char c) { return c > 32 && c != 127; });
        if (validSessionHash(hash)) result.rental = {ProviderId::Mts, hash};
        return result;
    });
}
}
