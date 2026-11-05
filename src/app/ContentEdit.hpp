// SPDX-License-Identifier: GPL-3.0-only
// Edit PDF in the app: the per-page cache of content blocks, and what's known about the text
// being typed in.
#pragma once

#include "Fonts.hpp"
#include "RichText.hpp"

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

// The block under a display point (text and pictures before shapes, then the smallest); -1
// if none.
int blockAt(const BlockList& list, double x, double y, double tolerance);
// The blocks wholly inside a display rect.
std::vector<int> blocksIn(const BlockList& list, const pdfz::RectF& area);

// Text being typed in, in Edit PDF: an existing block's (hidden on the page meanwhile) or new.
struct TextEditing {
  int page = -1;
  bool isNew = false;
  pdfz::ContentBlock block;  // the original (objects to replace); for new text, only where
  RichText original;         // the text it started with
  RichRun style;             // its main style: what new text is typed in (no text)
  pdfz::TextAlign align = pdfz::TextAlign::Left;
  double lineSpacing = 14;   // baseline to baseline, points, for text in style.size
  pdfz::PointF origin;       // the first line's baseline at the box's left (display frame)
  double width = 0;          // wrapping width in points; 0: lines grow instead
  std::vector<double> tabs;  // tab stops, points from origin.x
};
