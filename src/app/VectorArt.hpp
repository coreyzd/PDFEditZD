// SPDX-License-Identifier: GPL-3.0-only
// Drawings made of filled shapes, each in one color: what an SVG or PDF file is turned into
// for a stamp, a signature or initials, so they stay sharp at any size. Lines are made the
// shapes they cover; text the outlines of its letters. Saved, and exported again, as SVG (or
// PDF).
#pragma once

#include "core/annotations.hpp"

#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QList>
#include <QPainterPath>
#include <QString>

class QPainter;

struct VectorShape {
  QPainterPath path;  // filled by its fill rule
  QColor color;
};

struct VectorArt {
  QList<VectorShape> shapes;

  bool isEmpty() const { return shapes.isEmpty(); }
  QRectF bounds() const;
  // Moved so its bounds start at (0, 0).
  VectorArt normalized() const;
  // Drawn into `target`, keeping its shape (centered).
  void paint(QPainter& p, const QRectF& target) const;
  QImage preview(const QSize& box, qreal dpr = 1) const;
  // For a stamp: its units, the top-left at (0, 0).
  pdfz::StampDrawing drawing() const;

  // Saved as path data and colors.
  QJsonArray toJson() const;
  static VectorArt fromJson(const QJsonArray& a);
  // A standalone SVG document, `sizePt` points big (the drawing scaled to fit).
  QByteArray toSvg(QSizeF sizePt) const;
};

// The files that can be read as drawings.
QStringList vectorPatterns();  // *.svg, *.svgz, *.pdf, *.ai
bool isVectorFile(const QString& path);

// Reads an SVG (through Qt SVG) or the first page of a PDF (or an Illustrator file saved
// with PDF content). Pictures inside the file aren't part of the drawing: `pictures` says
// whether there were any. Empty if there's nothing drawn in it (a scan saved as PDF); then
// renderVectorFile() gives it as a picture instead.
VectorArt readVectorFile(const QString& path, QString* error, bool* pictures = nullptr);
QImage renderVectorFile(const QString& path, int maxSide);

// SVG path data ("M 1 2 L 3 4 C ... Z") to and from a path.
QString pathData(const QPainterPath& path);
QPainterPath parsePathData(const QString& d);

// Writes `art`, `sizePt` big, as a PDF of one page that size (vector, through Qt's PDF writer),
// or a picture in one (`image`, if art is empty).
bool writeVectorPdf(const QString& file, const VectorArt& art, QSizeF sizePt, const QImage& image = {});
// An SVG of a picture (embedded as PNG), `sizePt` big.
QByteArray imageSvg(const QImage& image, QSizeF sizePt);
