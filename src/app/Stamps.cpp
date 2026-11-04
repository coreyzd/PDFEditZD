// SPDX-License-Identifier: GPL-3.0-only
#include "Stamps.hpp"

#include "Markup.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

QList<StampStyle> standardStamps() {
  const QColor green(0x1E, 0x7B, 0x34), red(0xC0, 0x1C, 0x28), blue(0x1A, 0x4F, 0xA8);
  return {
      {QObject::tr("APPROVED"), green, true},     {QObject::tr("REVIEWED"), blue, true},
      {QObject::tr("RECEIVED"), blue, true},      {QObject::tr("REVISED"), blue, true},
      {QObject::tr("NOT APPROVED"), red, false},  {QObject::tr("DRAFT"), blue, false},
      {QObject::tr("FINAL"), green, false},       {QObject::tr("COMPLETED"), green, false},
      {QObject::tr("CONFIDENTIAL"), red, false},  {QObject::tr("FOR COMMENT"), blue, false},
      {QObject::tr("FOR PUBLIC RELEASE"), green, false}, {QObject::tr("NOT FOR PUBLIC RELEASE"), red, false},
      {QObject::tr("PRELIMINARY RESULTS"), blue, false}, {QObject::tr("INFORMATION ONLY"), blue, false},
      {QObject::tr("VOID"), red, false},
  };
}

namespace {

QFont stampFont(double px, bool bold) {
  QFont f = fontFor("Helv", px);
  f.setBold(bold);
  return f;
}

QImage previewOf(const QPainterPath& path, const QColor& color, const QSizeF& box) {
  const qreal dpr = 2;
  QImage img(QSize(int(box.width() * dpr), int(box.height() * dpr)), QImage::Format_ARGB32_Premultiplied);
  img.setDevicePixelRatio(dpr);
  img.fill(Qt::transparent);
  const QRectF b = path.boundingRect();
  if (b.isEmpty()) return img;
  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing);
  const double k = std::min((box.width() - 2) / b.width(), (box.height() - 2) / b.height());
  p.translate((box.width() - b.width() * k) / 2, (box.height() - b.height() * k) / 2);
  p.scale(k, k);
  p.translate(-b.topLeft());
  p.fillPath(path, color);
  return img;
}

}  // namespace

MadeStamp makeStamp(const StampStyle& style, const QString& author) {
  // Laid out in units of 1/10 point: a 16 pt title in a frame, and a 6 pt line under it.
  const double title = 160, subSize = 62, padX = 60, padY = 36, gap = 14, frame = 24, radius = 60;
  QPainterPath text;
  text.setFillRule(Qt::WindingFill);
  const QFont tf = stampFont(title, true);
  const QFontMetricsF tm(tf);
  double width = tm.horizontalAdvance(style.title);
  double y = padY + frame + tm.ascent();
  QString sub;
  if (style.dynamic) {
    const QDateTime now = QDateTime::currentDateTime();
    sub = QObject::tr("By %1 at %2, %3")
              .arg(author.isEmpty() ? QObject::tr("(unknown)") : author,
                   QLocale().toString(now.time(), QLocale::ShortFormat), QLocale().toString(now.date(), QLocale::ShortFormat));
    width = std::max(width, QFontMetricsF(stampFont(subSize, false)).horizontalAdvance(sub));
  }
  const double boxW = width + 2 * (padX + frame);
  text.addText((boxW - tm.horizontalAdvance(style.title)) / 2, y, tf, style.title);
  double boxH = y + tm.descent() + padY + frame;
  if (style.dynamic) {
    const QFont sf = stampFont(subSize, false);
    const QFontMetricsF sm(sf);
    y += tm.descent() + gap + sm.ascent();
    text.addText((boxW - sm.horizontalAdvance(sub)) / 2, y, sf, sub);
    boxH = y + sm.descent() + padY + frame;
  }
  // The frame: a rounded outline, made a filled shape so it scales with the stamp.
  QPainterPath outline;
  outline.addRoundedRect(QRectF(frame / 2, frame / 2, boxW - frame, boxH - frame), radius, radius);
  QPainterPathStroker stroker;
  stroker.setWidth(frame);
  QPainterPath all = stroker.createStroke(outline);
  all.setFillRule(Qt::WindingFill);
  all.addPath(text);

  MadeStamp out;
  out.drawing.width = boxW;
  out.drawing.height = boxH;
  out.drawing.paths.push_back(stampPath(all, style.color));
  out.width = boxW / 10;
  out.height = boxH / 10;
  out.preview = previewOf(all, style.color, QSizeF(160, 160 * boxH / boxW));
  return out;
}

MadeStamp makeImageStamp(const QImage& image) {
  MadeStamp out;
  const QImage img = image.convertToFormat(QImage::Format_ARGB32);
  pdfz::StampDrawing& d = out.drawing;
  d.width = img.width();
  d.height = img.height();
  d.imageWidth = img.width();
  d.imageHeight = img.height();
  d.imageBgra.resize(std::size_t(img.width()) * std::size_t(img.height()) * 4);
  for (int y = 0; y < img.height(); ++y) {
    const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
    for (int x = 0; x < img.width(); ++x) {
      std::uint8_t* px = &d.imageBgra[(std::size_t(y) * std::size_t(img.width()) + std::size_t(x)) * 4];
      px[0] = std::uint8_t(qBlue(line[x]));
      px[1] = std::uint8_t(qGreen(line[x]));
      px[2] = std::uint8_t(qRed(line[x]));
      px[3] = std::uint8_t(qAlpha(line[x]));
    }
  }
  out.width = 108;  // an inch and a half
  out.height = img.width() > 0 ? 108.0 * img.height() / img.width() : 0;
  out.preview = img.scaled(160, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  return out;
}

namespace {
QString stampDir() {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/stamps");
  QDir().mkpath(dir);
  return dir;
}
}  // namespace

QList<CustomStamps::Entry> CustomStamps::load() {
  QList<Entry> out;
  const QDir dir(stampDir());
  for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Time | QDir::Reversed)) {
    QImage img(fi.absoluteFilePath());
    if (!img.isNull()) out.push_back({fi.absoluteFilePath(), img});
  }
  return out;
}

bool CustomStamps::add(const QImage& image) {
  QImage img = image;
  if (img.width() > 1600 || img.height() > 1600) img = img.scaled(1600, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  return img.save(stampDir() + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".png", "PNG");
}

void CustomStamps::remove(const QString& path) {
  if (QFileInfo(path).absolutePath() == QFileInfo(stampDir()).absoluteFilePath()) QFile::remove(path);
}
