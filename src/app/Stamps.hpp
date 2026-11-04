// SPDX-License-Identifier: GPL-3.0-only
// Stamps like Acrobat's: Approved, Draft, Confidential... drawn as outlines in a colored frame
// (the "dynamic" ones say who stamped and when), and pictures saved as stamps of your own.
#pragma once

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

// Pictures saved as stamps, in the app's data folder.
namespace CustomStamps {
struct Entry {
  QString path;
  QImage image;
};
QList<Entry> load();
bool add(const QImage& image);  // saved as PNG
void remove(const QString& path);
}  // namespace CustomStamps
