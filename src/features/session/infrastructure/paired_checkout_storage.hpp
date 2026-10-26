#pragma once
#include "features/session/application/paired_checkout.hpp"

namespace fog {
std::unique_ptr<PairedCheckoutJournal> makePairedCheckoutJournal(std::string path);
}
