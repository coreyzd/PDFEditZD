// SPDX-License-Identifier: GPL-3.0-only
// The text on a page and where each character is: what text selection, copying and search are
// built on. Extracted once from PDFium (Document::extractText), then used without it, so the UI
// can hit-test on every mouse move without waiting for the renderer's hold on PDFium.
#pragma once

#include "core/geometry.hpp"

#include <string>
#include <utility>
#include <vector>

namespace pdfz {

// Two frames are used for positions on a page, both in points with the origin top-left:
//  - display: the page as shown, with its /Rotate applied (what Document::pageSize() and
//    render() use);
//  - upright: the page before /Rotate. Text is laid out (and hit-tested) here, where lines of
//    ordinary text run left to right whatever the page's rotation.
struct PageText {
  // One entry per character as PDFium numbers them. PDFium adds characters of its own: spaces
  // between words and "\r\n" at line ends where the PDF has none; those have empty boxes.
  std::u32string chars;
  std::vector<RectF> boxes;  // upright frame
  double width = 0;          // upright page size
  double height = 0;
  int rotation = 0;          // 0, 90, 180 or 270

  int size() const { return int(chars.size()); }
  bool empty() const { return chars.empty(); }

  RectF toDisplay(const RectF& upright) const;
  void toUpright(double x, double y, double& u, double& v) const;

  // The character whose box (grown by `tolerance` points) holds the display point, or -1.
  int charAt(double x, double y, double tolerance = 0) const;
  // The caret position (0..size()) nearest the display point: before or after the closest
  // character on the closest line. -1 if the page has no positioned text.
  int caretAt(double x, double y) const;
  // The word around character `index`, as [start, end). A space or punctuation mark is a
  // "word" of one character.
  std::pair<int, int> wordAt(int index) const;
  // The line around character `index`, as [start, end), without its line break.
  std::pair<int, int> lineAt(int index) const;
  // The highlight for characters [start, end): one rectangle per run on a line, display frame.
  std::vector<RectF> selectionRects(int start, int end) const;
  // Characters [start, end) as UTF-8, with PDFium's "\r\n" line breaks turned into "\n".
  std::string textUtf8(int start, int end) const;
};

bool isLineBreak(char32_t c);

}  // namespace pdfz
