// SPDX-License-Identifier: GPL-3.0-only
// Stamps like Acrobat's: Approved, Draft, Confidential... drawn as outlines in a colored frame
// (the "dynamic" ones say who stamped and when), and stamps of your own: drawings (from SVG or
// PDF, kept as shapes) or pictures. Any of them can be a favorite.
#pragma once

#include "VectorArt.hpp"

#include "core/annotations.hpp"

#include <QColor>
#include <QImage>
#include <QList>
#include <QString>

struct StampStyle {
  QString title;    // "APPROVED"
  QColor color;
  bool dynamic = false;  // adds "By <name> at <time>, <date>"
};

QList<StampStyle> standardStamps();

// A stamp's drawing, its size in points when placed, and a picture of it for menus.
struct MadeStamp {
  pdfz::StampDrawing drawing;
  double width = 0, height = 0;  // points
  QImage preview;
};
MadeStamp makeStamp(const StampStyle& style, const QString& author);
MadeStamp makeImageStamp(const QImage& image);  // a picture, about 1.5 inches wide
MadeStamp makeVectorStamp(const VectorArt& art);  // a drawing, about 1.5 inches across

// Stamps of your own, in the app's data folder: drawings as SVG, pictures as PNG.
namespace CustomStamps {
struct Entry {
  QString path;
  QImage image;   // a picture
  VectorArt art;  // or a drawing
  QString key() const;  // for favorites
  MadeStamp made() const;
};
QList<Entry> load();
bool add(const QImage& image);
bool add(const VectorArt& art);
void remove(const QString& path);
}  // namespace CustomStamps

// Which stamps are favorites (the ones the Stamp menu shows first), by key: "std:APPROVED",
// or a custom stamp's key. A few standard ones until it's changed.
namespace StampFavorites {
QString standardKey(const StampStyle& style);
QStringList load();
bool contains(const QString& key);
void set(const QString& key, bool favorite);
}  // namespace StampFavorites
