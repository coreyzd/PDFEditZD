// SPDX-License-Identifier: GPL-3.0-only
#include "ContentEdit.hpp"

#include "core/document.hpp"

void ContentCache::setDocument(std::shared_ptr<pdfz::Document> doc) {
  doc_ = std::move(doc);
  cache_.clear();
}

std::shared_ptr<const BlockList> ContentCache::get(int page) {
  static const auto none = std::make_shared<const BlockList>();
  if (!doc_ || page < 0 || page >= doc_->pageCount()) return none;
  if (auto* hit = cache_.object(page)) return *hit;
  auto list = std::make_shared<const BlockList>(doc_->contentBlocks(page));
  cache_.insert(page, new std::shared_ptr<const BlockList>(list));
  return list;
}

int blockAt(const BlockList& list, double x, double y, double tol) {
  int best = -1;
  double bestArea = 0;
  for (int i = 0; i < int(list.size()); ++i) {
    const pdfz::RectF& r = list[std::size_t(i)].rect;
    if (x < r.left - tol || x > r.right + tol || y < r.top - tol || y > r.bottom + tol) continue;
    const double area = r.width() * r.height();
    if (best < 0 || area < bestArea) {
      best = i;
      bestArea = area;
    }
  }
  return best;
}
