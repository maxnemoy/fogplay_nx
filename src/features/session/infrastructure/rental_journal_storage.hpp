#pragma once
#include "features/session/application/rental_journal.hpp"
#include <memory>
#include <string>

namespace fog {
// MTS v1 journals are preserved byte-for-field; legacy hashes never grant ownership.
std::unique_ptr<SessionJournal> makeMtsSessionJournal(std::string path);
std::unique_ptr<CheckoutJournal> makeMtsCheckoutJournal(std::string path);
}
