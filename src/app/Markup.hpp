// SPDX-License-Identifier: GPL-3.0-only
// Comments and markups in the app: the per-page cache of annotations, hit-testing them, the
// tools' styles, and the appearance of text boxes.
#pragma once

#include "core/annotations.hpp"

#include <QCache>
#include <QMetaType>
#include <QColor>
#include <QFont>
#include <QPainterPath>
#include <QString>

#include <memory>
#include <optional>
#include <vector>

namespace pdfz { class Document; }

using AnnotationList = std::vector<pdfz::Annotation>;
Q_DECLARE_METATYPE(pdfz::Annotation)

class AnnotationCache {
public:
  void setDocument(std::shared_ptr<pdfz::Document> doc);
  std::shared_ptr<const AnnotationList> get(int page);  // null for a page that doesn't exist
  void invalidate(int page);

private:
  std::shared_ptr<pdfz::Document> doc_;
  QCache<int, std::shared_ptr<const AnnotationList>> cache_{64};
};

// Which annotation is under a display point (`tolerance` points of slack), topmost first; -1 if
// none. Returns the position in `list`, not the annotation's index.
int annotationAt(const AnnotationList& list, double x, double y, double tolerance);

// Annotations that can be dragged to a new place: everything but text markups (which belong to
// the text they mark).
bool canMove(const pdfz::Annotation& a);
// Ones whose size can be changed by dragging a corner.
bool canResize(const pdfz::Annotation& a);

// The tools that make markups, and their remembered styles: the comment tools, then the Fill &
// Sign ones (text without a box, check marks, crosses, dots and today's date).
enum class MarkupTool {
  Highlight, Underline, StrikeOut, Note, TextBox, Pen, Rectangle, Ellipse, Line, Arrow,
  FillText, Check, Cross, Dot, Date
};
constexpr int kMarkupToolCount = 15;
constexpr int kCommentToolCount = 10;  // the first ten
inline bool isFillSignTool(MarkupTool t) { return int(t) >= kCommentToolCount; }

struct MarkupStyle {
  QColor color;            // the stroke, markup or note color; a text box's border
  QColor fill;             // invalid for none (rectangles, ellipses, text boxes)
  double opacity = 1.0;
  double width = 2;        // line width in points (pen, shapes, lines, text box border)
  // Text boxes.
  double fontSize = 12;
  QString fontName = QStringLiteral("Helv");  // Helv, TiRo or Cour
  QColor textColor;
  int align = 0;  // 0 left, 1 center, 2 right
};

// A change to some of a comment's style; what's left empty stays as it is.
struct StyleChange {
  std::optional<QColor> color{};
  std::optional<QColor> fill{};  // an invalid color takes the fill away
  std::optional<double> opacity{};
  std::optional<double> width{};
  std::optional<double> fontSize{};
  std::optional<QString> fontName{};
  std::optional<QColor> textColor{};
  std::optional<int> align{};
};
void applyStyleChange(pdfz::Annotation& a, const StyleChange& change);
void applyStyleChange(MarkupStyle& s, const StyleChange& change);
// A new annotation gets the style of the tool that made it.
void applyStyle(pdfz::Annotation& a, const MarkupStyle& style);
// An annotation's style, as the style controls show it.
MarkupStyle styleOf(const pdfz::Annotation& a);
// "Change Color", "Change Fill"...: the undo label for a change.
QString styleChangeLabel(const StyleChange& change);

// Which style settings apply to a tool's markups, or to an annotation.
bool usesFill(MarkupTool t);
bool usesWidth(MarkupTool t);
bool usesText(MarkupTool t);
bool usesFill(const pdfz::Annotation& a);
bool usesWidth(const pdfz::Annotation& a);
bool usesText(const pdfz::Annotation& a);
bool usesOpacity(MarkupTool t);
bool usesOpacity(const pdfz::Annotation& a);

// Fill & Sign marks (check, cross, dot): stamps drawn here as outlines, by their subject.
bool isMark(const pdfz::Annotation& a);
const char* markSubject(MarkupTool t);  // "Check", "Cross", "Dot"; nullptr for other tools
pdfz::StampDrawing markDrawing(const std::string& subject, const QColor& color);
double markSize(MarkupTool t);          // its usual width and height, in points
// A Qt path as a stamp's filled outline (any curves kept).
pdfz::StampDrawing::Path stampPath(const QPainterPath& path, const QColor& color);

// The strokes of an arrow from `tail` to `tip`: the shaft, then an open head whose size grows
// with the line width, so it stays an arrow when the line is thick (as Acrobat draws them).
std::vector<std::vector<pdfz::PointF>> arrowStrokes(pdfz::PointF tail, pdfz::PointF tip, double width);
bool isArrow(const pdfz::Annotation& a);

// The Qt font standing in for a PDF standard font name, at `pixelSize`.
QFont fontFor(const std::string& pdfFontName, double pixelSize);
// Inside a text box: the space between its edge and the text, in points.
double textBoxPadding(const pdfz::Annotation& a);
// How narrow text typed on a form (Fill & Sign) starts out: room for a character or two.
double fillTextMinWidth(const pdfz::Annotation& a);

QString toolName(MarkupTool t);   // "Highlight", "Text Box"...
QString toolKey(MarkupTool t);    // for settings: "highlight", "textbox"...
MarkupStyle defaultStyle(MarkupTool t);
MarkupStyle loadStyle(MarkupTool t);
void saveStyle(MarkupTool t, const MarkupStyle& s);

// The name written on new comments: the one set in Edit > Comment Author, or the login name.
QString commentAuthor();
void setCommentAuthor(const QString& name);

QColor toQColor(pdfz::Color c);
pdfz::Color fromQColor(const QColor& c);

// A text box's appearance: its text wrapped to the box, as glyph outlines (so it needs no font
// in the file and looks the same in every reader), in PDF user space.
std::string freeTextAppearance(const pdfz::Annotation& a, const pdfz::PageFrame& frame);

// "Nov 4, 2026 9:30 AM" from a PDF date; empty if it can't be read.
QString formatPdfDate(const std::string& pdfDate);
