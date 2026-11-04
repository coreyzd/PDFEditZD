// SPDX-License-Identifier: GPL-3.0-only
// Editing a page's own content (Edit PDF): its text, gathered into blocks the way it reads
// (lines, and lines into paragraphs), and its pictures. Blocks can be moved, deleted, resized
// (pictures) and their text replaced; new text and pictures can be added. Every change is
// written into the page's content stream.
#pragma once

#include "core/annotations.hpp"
#include "core/geometry.hpp"

#include <string>
#include <vector>

namespace pdfz {

struct ContentBlock {
  enum class Kind { Text, Image };
  Kind kind = Kind::Text;
  RectF rect;                // display frame
  std::vector<int> objects;  // the page objects it's made of (indices on the page)
  bool editable = false;     // text that runs straight across (not turned or slanted)

  // Text.
  std::string text;          // UTF-8; lines that end a paragraph or a short line end with \n
  std::string fontName;      // the PDF font's name, e.g. "Arial-BoldMT" (subset tag removed)
  std::string family;        // a family name guessed from it: "Arial", "Helvetica"
  bool bold = false;
  bool italic = false;
  double fontSize = 12;      // points, as it appears on the page
  Color color{0, 0, 0};
  double lineSpacing = 0;    // baseline to baseline, points (0: one line)
  // Where the first line starts, in the display frame: its left end on its baseline.
  PointF origin;
};

// A font for new text: one of PDF's standard fonts (needs nothing embedded, Latin text only),
// or a TrueType/OpenType font file, embedded in the PDF.
struct TextFont {
  std::string standard;              // "Helvetica", "Times-Bold", "Courier-Oblique"...
  std::vector<std::uint8_t> file;    // or the font file
  std::string key;                   // what to remember an embedded font by ("Calibri Bold")
};

// Text to put on the page: lines already wrapped, each a baseline apart, the first one's left
// end on its baseline at `origin` (display frame).
struct TextLines {
  std::vector<std::string> lines;  // UTF-8
  PointF origin;
  double lineSpacing = 14;
  double fontSize = 12;
  Color color{0, 0, 0};
  TextFont font;
};

// Whether a standard font can show this text (the Windows-1252 characters).
bool standardFontCanShow(const std::string& utf8);

}  // namespace pdfz
