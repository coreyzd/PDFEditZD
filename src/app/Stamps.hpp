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
  // Banner: the title in a rounded frame (Acrobat's). DateBox: like an office rubber date
  // stamp: the title over a box with today's date (the day in bold), the name under it.
  enum class Layout { Banner, DateBox };
  Layout layout = Layout::Banner;
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
  QString name;   // as given (empty if none was)
  QString label;  // the name, or "Stamp 2" for one without
  QString key() const;  // for favorites
  MadeStamp made() const;
};
QList<Entry> load();
// Saved with its name: the stamp's path, or empty if it couldn't be saved.
QString add(const QImage& image, const QString& name);
QString add(const VectorArt& art, const QString& name);
void rename(const QString& path, const QString& name);
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
