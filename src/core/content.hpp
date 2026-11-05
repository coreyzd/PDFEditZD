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

// A stretch of a block's text in one font, size and color.
struct TextRun {
  std::string text;          // UTF-8, with the block's spaces and line breaks
  std::string fontName;      // the PDF font's name, e.g. "Arial-BoldMT" (subset tag removed)
  std::string family;        // a family name guessed from it: "Arial", "Helvetica"
  bool bold = false;
  bool italic = false;
  double size = 12;          // points
  Color color{0, 0, 0};
  double rise = 0;           // its baseline above the line's, points (superscripts > 0)
  friend bool operator==(const TextRun&, const TextRun&) = default;
};

enum class TextAlign { Left, Center, Right, Justify };

struct ContentBlock {
  // Graphics: drawn shapes (paths), shadings and form XObjects, such as a logo or a line.
  enum class Kind { Text, Image, Graphic };
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
  // The text in its fonts (font, size, color above are the first run's). Joined, the runs'
  // text is `text`.
  std::vector<TextRun> runs;
  TextAlign align = TextAlign::Left;  // as the lines sit (one line: left)
  // Where text after a tab ('\t' in the text) starts: points from the first line's start.
  std::vector<double> tabStops;
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

// Text laid out for the page, in several fonts: each line a baseline apart (`advance` after
// the first, at `origin`), and in pieces, each in one font.
struct TextSpan {
  std::string text;   // UTF-8
  double x = 0;       // from the origin, points
  int font = 0;       // in PlacedText::fonts
  double size = 12;
  Color color{0, 0, 0};
  double rise = 0;    // above the line's baseline, points
};
struct PlacedLine {
  std::vector<TextSpan> spans;
  double advance = 0;  // from the line before's baseline to this one's (the first: 0)
};
struct PlacedText {
  std::vector<PlacedLine> lines;
  PointF origin;       // the first line's baseline, at the left of the box (display frame)
  std::vector<TextFont> fonts;
};

// An affine map of the display frame: x' = a x + c y + e, y' = b x + d y + f.
struct Affine {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  PointF map(PointF p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
  // This, then `next`.
  Affine then(const Affine& n) const {
    return {n.a * a + n.c * b, n.b * a + n.d * b, n.a * c + n.c * d, n.b * c + n.d * d, n.a * e + n.c * f + n.e,
            n.b * e + n.d * f + n.f};
  }
  static Affine translate(double dx, double dy) { return {1, 0, 0, 1, dx, dy}; }
  // Turning by `degrees` clockwise as shown (y grows downwards), about `center`.
  static Affine rotate(double degrees, PointF center);
  // Scaling about `anchor`.
  static Affine scale(double sx, double sy, PointF anchor) {
    return {sx, 0, 0, sy, anchor.x - sx * anchor.x, anchor.y - sy * anchor.y};
  }
};

// A drawn shape of a page, for using the drawing elsewhere (a stamp or a signature from a
// PDF): its outline in the display frame, and how it's painted. Text comes as the outlines of
// its letters.
struct PageShape {
  std::vector<StampDrawing::Op> ops;
  bool fill = false;
  bool evenOdd = false;
  Color fillColor{0, 0, 0};
  bool stroke = false;
  Color strokeColor{0, 0, 0};
  double strokeWidth = 1;   // display points
  int lineCap = 0;          // 0 butt, 1 round, 2 square
  int lineJoin = 0;         // 0 miter, 1 round, 2 bevel
};

// Draw order: to the top or bottom of everything, or one step past what overlaps it.
enum class Arrange { ToFront, ToBack, Forward, Backward };

// Whether a standard font can show this text (the Windows-1252 characters).
bool standardFontCanShow(const std::string& utf8);

}  // namespace pdfz
