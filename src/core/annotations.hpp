// SPDX-License-Identifier: GPL-3.0-only
// Comments and markups (PDF annotations): reading them, adding, moving, restyling, removing.
//
// Geometry is in the display frame (see geometry.hpp), like everything else the UI deals with.
// Appearances: for the kinds PDFium knows how to draw (highlight, underline, strikeout,
// squiggly, note, ink, square, circle) pdfz clears the appearance after a change and has PDFium
// draw a new one, which is saved with the file so other PDF readers show the same thing. A text
// box (FreeText) gets the appearance the caller builds (Annotation::appearance), because
// PDFium's own puts the whole text on one line. With an opacity below 1, that appearance
// starts with "/GS gs": PDFium adds the transparency state it names.
#pragma once

#include "core/geometry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pdfz {

enum class AnnotKind {
  Highlight,
  Underline,
  StrikeOut,
  Squiggly,
  Note,      // a sticky note (/Text)
  FreeText,  // a text box
  Ink,       // freehand, and the lines and arrows pdfz draws
  Square,
  Circle,
  Line,      // made by other programs; pdfz draws lines as Ink, which PDFium can create
  Stamp,
  FileAttachment,
  Other,
};

const char* kindName(AnnotKind kind);  // "Highlight", "Note", ...
bool isTextMarkup(AnnotKind kind);      // highlight, underline, strikeout, squiggly

struct Color {
  std::uint8_t r = 0, g = 0, b = 0;
  friend bool operator==(const Color&, const Color&) = default;
};

struct Annotation {
  int index = -1;  // position in the page's annotation list, filled in by annotations()
  AnnotKind kind = AnnotKind::Other;
  int subtype = 0;  // the raw PDFium subtype

  RectF rect;               // display frame
  Color color{255, 0, 0};   // the stroke, markup or note color; a text box's border
  bool hasFill = false;     // squares, circles and text boxes: filled with `fill`
  Color fill{255, 255, 255};
  double opacity = 1.0;     // /CA
  double borderWidth = 1;   // squares, circles, ink, text boxes; 0 for none
  bool hidden = false;      // the Hidden flag (set while a text box is edited in place)
  std::vector<RectF> quads;               // text markups: the marked text, one rect per line
  std::vector<std::vector<PointF>> ink;   // ink: the strokes
  // Text boxes.
  double fontSize = 12;
  std::string fontName = "Helv";  // the PDF standard font: Helv, TiRo (Times) or Cour (Courier)
  Color textColor{0, 0, 0};
  int align = 0;                  // 0 left, 1 center, 2 right

  std::string contents;  // the comment text (a text box's text), UTF-8
  std::string author;    // /T
  std::string subject;   // /Subj, e.g. "Arrow" for an arrow drawn as ink
  std::string modified;  // /M, a PDF date: "D:20261104093000-05'00'"
  std::string name;      // /NM, unique within the page

  // Adding or updating a text box: its appearance as a content stream in PDF user space
  // (the box's rect there is its bounding box). Not filled in by annotations().
  std::string appearance;

  // pdfz can add this kind, and move and restyle it.
  bool editable() const;
  // A stamp pdfz made (a signature, initials, a check mark...): it can be moved and resized.
  bool isPdfzStamp() const { return kind == AnnotKind::Stamp && !subject.empty(); }
};

// What a stamp shows (Document::addStamp): filled outlines and an image, drawn in a box of the
// drawing's own (`width` by `height`, y downwards) that is fitted to the annotation's rect.
// Signatures and initials are drawn strokes made into outlines, typed names as glyph outlines,
// or an image; check marks, crosses and dots are outlines.
struct StampDrawing {
  struct Op {
    enum Kind { MoveTo, LineTo, CubicTo, Close } kind = MoveTo;
    PointF p[3];  // MoveTo/LineTo: p[0]; CubicTo: the two control points, then the end
  };
  struct Path {
    std::vector<Op> ops;  // filled with the nonzero winding rule, or even-odd
    Color color{0, 0, 0};
    bool evenOdd = false;
  };
  double width = 1, height = 1;
  std::vector<Path> paths;
  // An image over the whole box (under the paths): BGRA with straight alpha, top row first.
  int imageWidth = 0, imageHeight = 0;
  std::vector<std::uint8_t> imageBgra;
};

// "D:YYYYMMDDHHmmSS+HH'mm'" for now, in local time.
std::string pdfDateNow();

}  // namespace pdfz
