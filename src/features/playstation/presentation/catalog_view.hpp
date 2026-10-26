#pragma once
#include "app/presentation/ui.hpp"
#include "features/catalog/domain/models.hpp"

namespace fog::playstation {
// Synthetic/public catalog data only. Status is based on presence in the public MTS listing.
void drawCatalog(ui::Ui& ui, const std::vector<Computer>& computers, std::size_t selected,
                 bool focused, bool loading);
void drawConfirmation(ui::Ui& ui, const Computer& computer);
}
