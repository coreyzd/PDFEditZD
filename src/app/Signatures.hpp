// SPDX-License-Identifier: GPL-3.0-only
// Signatures and initials for Fill & Sign: drawn, typed in a handwriting font, from a drawing
// of one (SVG or PDF, kept as shapes), or from a picture of one. They're placed on the page as stamps (as outlines, or the picture with its
// background made clear), and can be saved to be used again.
//
// These are signatures you can see, like one signed on paper: not certificate-based digital
// signatures.
#pragma once

#include "VectorArt.hpp"

#include "core/annotations.hpp"

#include <QColor>
#include <QImage>
#include <QList>
#include <QPainterPath>
#include <QPolygonF>
#include <QString>
#include <QStringList>

struct Signature {
  enum class Kind { Drawn, Typed, Image, Vector };
  QString id;              // in the saved list
  bool initials = false;   // initials rather than a full signature
  Kind kind = Kind::Drawn;
  QColor color = Qt::black;     // drawn and typed ones
  QList<QPolygonF> strokes;     // drawn: the pen's path, in the pad's pixels
  double penWidth = 3;          // drawn: in the same pixels
  QString text;                 // typed
  QString family;               // typed: the font
  QImage image;                 // a picture: ARGB32, background already clear
  VectorArt art;                // a drawing from a file, in its own colors

  bool isEmpty() const;
  // The outlines of a drawn or typed one (strokes smoothed and given width), at their size.
  QPainterPath outline() const;
  // Its extent: the outline's bounds, or the picture's size.
  QSizeF size() const;
  // As shapes in its colors (all but a picture).
  VectorArt vectorArt() const;
  // The size it's placed on a page at, in points.
  QSizeF placedSize() const;
  // What goes in the stamp.
  pdfz::StampDrawing drawing() const;
  // A picture of it fitting `box` (for menus and lists), on a clear background.
  QImage preview(const QSize& box, qreal dpr = 1) const;
};

// The saved signatures, in a JSON file in the app's data folder.
namespace SignatureStore {
QList<Signature> load();
bool save(const QList<Signature>& all);
void add(Signature s);  // gives it an id
void remove(const QString& id);
}  // namespace SignatureStore

// Writes it to an SVG or PDF file (by the name's extension), at the size it's placed on a page:
// shapes, so it stays sharp; a picture one holds its picture.
bool exportSignature(const Signature& s, const QString& file, QString* error);

// Handwriting fonts found on this computer, best first (Windows has Segoe Script, Ink Free...).
QStringList handwritingFonts();

// A photo or scan of a signature, ready to place: the paper made clear (pixels lighter than
// `threshold`, 0-255, fade out), trimmed to the ink and scaled down if it's very large.
QImage cleanSignatureImage(const QImage& source, int threshold);
