// SPDX-License-Identifier: GPL-3.0-only
#include "Forms.hpp"

#include "core/document.hpp"

void FieldCache::setDocument(std::shared_ptr<pdfz::Document> doc) {
  doc_ = std::move(doc);
  cache_.clear();
}

bool FieldCache::hasForm() const { return doc_ && doc_->hasForm(); }

std::shared_ptr<const FieldList> FieldCache::get(int page) {
  static const auto none = std::make_shared<const FieldList>();
  if (!doc_ || !doc_->hasForm() || page < 0 || page >= doc_->pageCount()) return none;
  if (auto* hit = cache_.object(page)) return *hit;
  auto list = std::make_shared<const FieldList>(doc_->formFields(page));
  cache_.insert(page, new std::shared_ptr<const FieldList>(list));
  return list;
}

std::vector<int> FieldCache::pagesWithField(const std::string& name) const {
  std::vector<int> out;
  for (int page : cache_.keys()) {
    const auto* list = cache_.object(page);
    for (const pdfz::FormField& f : **list)
      if (f.name == name) {
        out.push_back(page);
        break;
      }
  }
  return out;
}

bool isFillable(const pdfz::FormField& f) {
  return f.type != pdfz::FieldType::PushButton && f.type != pdfz::FieldType::Unknown;
}

int fieldAt(const FieldList& list, double x, double y) {
  for (int i = int(list.size()) - 1; i >= 0; --i) {
    const pdfz::FormField& f = list[std::size_t(i)];
    if (!isFillable(f)) continue;
    if (x >= f.rect.left && x <= f.rect.right && y >= f.rect.top && y <= f.rect.bottom) return i;
  }
  return -1;
}

QString fieldLabel(const pdfz::FormField& f) {
  const QString tip = QString::fromStdString(f.tooltip).trimmed();
  return tip.isEmpty() ? QString::fromStdString(f.name) : tip;
}
