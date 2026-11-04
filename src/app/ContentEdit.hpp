// SPDX-License-Identifier: GPL-3.0-only
// Edit PDF in the app: the per-page cache of content blocks, and what's known about the text
// being typed in.
#pragma once

#include "Fonts.hpp"

#include "core/content.hpp"

#include <QCache>
#include <QColor>
#include <QString>

#include <memory>
#include <vector>

namespace pdfz { class Document; }

using BlockList = std::vector<pdfz::ContentBlock>;

class ContentCache {
public:
  void setDocument(std::shared_ptr<pdfz::Document> doc);
  std::shared_ptr<const BlockList> get(int page);
  void invalidate(int page) { cache_.remove(page); }

private:
  std::shared_ptr<pdfz::Document> doc_;
  QCache<int, std::shared_ptr<const BlockList>> cache_{16};
};

// The block under a display point (the smallest, if they overlap); -1 if none.
int blockAt(const BlockList& list, double x, double y, double tolerance);

// Text being typed in, in Edit PDF: an existing block's (hidden on the page meanwhile) or new.
struct TextEditing {
  int page = -1;
  bool isNew = false;
  pdfz::ContentBlock block;  // the original (objects to replace); for new text, only where
  QString original;          // the text it started with
  EditFont font;
  double size = 12;
  QColor color = Qt::black;
  double lineSpacing = 14;   // baseline to baseline, points
  pdfz::PointF origin;       // the first line's start on its baseline (display frame)
  double width = 0;          // wrapping width in points; 0: lines grow instead
};
