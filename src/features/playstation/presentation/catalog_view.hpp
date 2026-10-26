#pragma once
#include "app/presentation/ui.hpp"
#include <nlohmann/json.hpp>

namespace fog::playstation {
// Synthetic/public catalog data only. Status is based on presence in the public MTS listing.
void drawCatalog(ui::Ui& ui, const nlohmann::json& computers, std::size_t selected,
                 bool focused, bool loading);
void drawConfirmation(ui::Ui& ui, const nlohmann::json& computer);
}
