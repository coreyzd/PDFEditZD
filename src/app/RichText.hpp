// SPDX-License-Identifier: GPL-3.0-only
// Edit PDF's text in several fonts: a block's runs as the editor shows them (a QTextDocument
// whose character formats carry each run's font, size in points, color and rise), back again
// when the typing ends, and laid out for the page, line by line, in its alignment.
#pragma once

#include "Fonts.hpp"

#include "core/content.hpp"

#include <QColor>
#include <QString>
#include <QTextCharFormat>

#include <optional>
#include <vector>

class QTextDocument;
class QTextCursor;

// A stretch of text in one style. Line breaks are '\n' in the text.
struct RichRun {
  QString text;
  EditFont font;
  double size = 12;   // points
  QColor color = Qt::black;
  double rise = 0;    // above the baseline, points
  bool sameStyle(const RichRun& o) const;
};
using RichText = std::vector<RichRun>;

RichText richFromBlock(const pdfz::ContentBlock& block);
QString plainText(const RichText& text);
bool sameText(const RichText& a, const RichText& b);  // same text and styles
// The style most of the text is in (raised text aside).
RichRun mainStyle(const RichText& text);

// A change to the style of text: whatever is set.
struct TextStyleChange {
  std::optional<QString> family = {};
  std::optional<bool> bold = {};
  std::optional<bool> italic = {};
  std::optional<double> size = {};     // points
  std::optional<QColor> color = {};
  std::optional<pdfz::TextAlign> align = {};
  bool empty() const { return !family && !bold && !italic && !size && !color && !align; }
};
// `run` restyled. A size given scales a run proportionally when `relativeTo` > 0 (the size the
// whole block's text is being changed from), so raised text stays in proportion.
void applyStyle(RichRun& run, const TextStyleChange& change, double relativeTo = 0);

// The editor's document. `pxPerPt` is the zoom; `dpiY` the screen's logical dpi.
void fillDocument(QTextDocument* doc, const RichText& text, double pxPerPt, double dpiY);
RichText richFromDocument(const QTextDocument* doc);
// The character format of a run, at a zoom.
QTextCharFormat charFormat(const RichRun& run, double pxPerPt, double dpiY);
RichRun runOf(const QTextCharFormat& f);  // its style (text empty)
// After zooming: every run's font sized again.
void rescaleDocument(QTextDocument* doc, double pxPerPt, double dpiY);
// Restyles the cursor's selection (each run in it keeping what the change doesn't set).
void restyleSelection(QTextCursor& cursor, const TextStyleChange& change, double pxPerPt, double dpiY);
// The paragraphs' alignment in the editor.
void setDocumentAlignment(QTextDocument* doc, pdfz::TextAlign align);
// Tab stops (points from the text's left), then every half inch, at `pxPerPt` pixels a point.
void setTabStops(QTextOption& option, const std::vector<double>& stops, double pxPerPt);

// The text laid out for the page: wrapped to `width` points (0: no wrapping) and aligned in
// it, the first baseline at `origin` (the box's left), lines `lineSpacing` apart (more where a
// line has larger text than `baseSize`). `complete` is false if some characters are in no
// font that can go in the PDF. Fails (empty fonts) if no font can show the text.
pdfz::PlacedText layoutRich(const RichText& text, pdfz::TextAlign align, double width, double lineSpacing,
                            double baseSize, pdfz::PointF origin, const std::vector<double>& tabs, bool* complete);
// How far apart two lines with this much text are.
double lineAdvance(double lineSpacing, double baseSize, double largestSize);
