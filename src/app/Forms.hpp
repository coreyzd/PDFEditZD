// SPDX-License-Identifier: GPL-3.0-only
// Form fields in the app: the per-page cache of fields, and finding them under the mouse.
#pragma once

#include "core/forms.hpp"

#include <QCache>
#include <QString>

#include <memory>
#include <vector>

namespace pdfz { class Document; }

using FieldList = std::vector<pdfz::FormField>;

class FieldCache {
public:
  void setDocument(std::shared_ptr<pdfz::Document> doc);
  bool hasForm() const;
  std::shared_ptr<const FieldList> get(int page);  // empty for a page without fields
  void invalidate() { cache_.clear(); }
  // The pages, among those already looked at, with a widget of the field `name`.
  std::vector<int> pagesWithField(const std::string& name) const;

private:
  std::shared_ptr<pdfz::Document> doc_;
  QCache<int, std::shared_ptr<const FieldList>> cache_{256};
};

// The field under a display point, topmost (last) first; -1 if none. Push buttons and hidden
// widgets don't count. Returns the position in `list`.
int fieldAt(const FieldList& list, double x, double y);
// Fields that can be filled in by clicking (everything but push buttons).
bool isFillable(const pdfz::FormField& f);
// What to call a field in the UI: its tooltip, or else its name.
QString fieldLabel(const pdfz::FormField& f);
