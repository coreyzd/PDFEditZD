// SPDX-License-Identifier: GPL-3.0-only
#include "Stamps.hpp"

#include "Markup.hpp"

#include <QDate>
#include <QDateTime>
#include <QTransform>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

QList<StampStyle> standardStamps() {
  const QColor green(0x1E, 0x7B, 0x34), red(0xC0, 0x1C, 0x28), blue(0x1A, 0x4F, 0xA8);
  return {
      {QObject::tr("APPROVED"), green, true},     {QObject::tr("REVIEWED"), blue, true},
      {QObject::tr("RECEIVED"), blue, true},      {QObject::tr("REVISED"), blue, true},
      // An office date stamp: department, date, name.
      {QObject::tr("DRAFTING"), QColor(0xD8, 0x2A, 0x3C), true, StampStyle::Layout::DateBox},
      {QObject::tr("NOT APPROVED"), red, false},  {QObject::tr("PRELIMINARY"), blue, false},
      {QObject::tr("CONFIDENTIAL"), red, false},  {QObject::tr("FOR COMMENT"), blue, false},
      {QObject::tr("INFORMATION ONLY"), blue, false}, {QObject::tr("REFERENCE"), red, false},
      {QObject::tr("OBSOLETE"), red, false},      {QObject::tr("VOID"), red, false},
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

namespace {

// `text` as outlines, its capitals `cap` high and `xscale` of its width (less than 1 only to
// fit), centered on `centerX` with its baseline at `baseline`.
QPainterPath fitted(const QString& text, bool bold, double cap, double xscale, double centerX, double baseline) {
  QFont f = stampFont(100, bold);
  const double capRatio = QFontMetricsF(f).capHeight() / 100;
  f = stampFont(cap / (capRatio > 0.3 ? capRatio : 0.716), bold);
  QPainterPath p;
  p.addText(0, 0, f, text);
  const double w = QFontMetricsF(f).horizontalAdvance(text) * xscale;
  QTransform t;
  t.translate(centerX - w / 2, baseline);
  t.scale(xscale, 1);
  return t.map(p);
}

// Like the date stamps on office desks: a thin frame, the title (a department) at the top, the
// date in a box in the middle, and the name at the bottom. 113 x 68 points, laid out in tenths
// of a point.
MadeStamp makeDateStamp(const StampStyle& style, const QString& author) {
  const double W = 1130, H = 680, line = 8;
  const QDate today = QDate::currentDate();
  QPainterPath all;
  all.setFillRule(Qt::WindingFill);
  QPainterPath frames;
  frames.addRect(QRectF(line / 2, line / 2, W - line, H - line));
  frames.addRect(QRectF(108 + line / 2, 144 + line / 2, 900 - line, 392 - line));
  QPainterPathStroker stroker;
  stroker.setWidth(line);
  stroker.setJoinStyle(Qt::MiterJoin);
  all.addPath(stroker.createStroke(frames));
  auto add = [&](const QString& text, bool bold, double cap, double xscale, double cx, double base, double maxW) {
    QFont f = stampFont(100, bold);
    const double natural = QFontMetricsF(f).horizontalAdvance(text) * cap / std::max(1.0, QFontMetricsF(f).capHeight());
    if (natural * xscale > maxW && natural > 0) xscale = maxW / natural;  // a long name: narrower, to fit
    all.addPath(fitted(text, bold, cap, xscale, cx, base));
  };
  // Helvetica throughout, as the other stamps; the title and the name the same size and as far
  // from the date's box (2 pt), and as far from the frame.
  add(style.title, false, 78, 1.0, W / 2, 124, W - 120);
  const QString date = QLocale().toString(today, QStringLiteral("MMM")).toUpper() + QLatin1Char(' ') +
                       today.toString(QStringLiteral("dd yyyy"));
  add(date, false, 90, 1.0, W / 2, 340 + 45, 900 - 100);  // in the middle of the box
  const QString name = (author.isEmpty() ? QObject::tr("(unknown)") : author).toUpper();
  add(name, false, 78, 1.0, W / 2, 634, W - 100);

  MadeStamp out;
  out.drawing.width = W;
  out.drawing.height = H;
  out.drawing.paths.push_back(stampPath(all, style.color));
  out.width = W / 10;
  out.height = H / 10;
  out.preview = previewOf(all, style.color, QSizeF(160, 160 * H / W));
  return out;
}

}  // namespace

MadeStamp makeStamp(const StampStyle& style, const QString& author) {
  if (style.layout == StampStyle::Layout::DateBox) return makeDateStamp(style, author);
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

MadeStamp makeVectorStamp(const VectorArt& art) {
  MadeStamp out;
  out.drawing = art.drawing();
  const double w = out.drawing.width, h = out.drawing.height;
  const double k = 108 / std::max(w, h);  // an inch and a half across, the longer way
  out.width = w * k;
  out.height = h * k;
  out.preview = art.preview(QSize(160, std::max(16, int(std::lround(160 * h / w)))), 2);
  return out;
}

namespace {
QString stampDir() {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/stamps");
  QDir().mkpath(dir);
  return dir;
}
}  // namespace

QString CustomStamps::Entry::key() const { return QStringLiteral("custom:") + QFileInfo(path).fileName(); }

MadeStamp CustomStamps::Entry::made() const { return art.isEmpty() ? makeImageStamp(image) : makeVectorStamp(art); }

QList<CustomStamps::Entry> CustomStamps::load() {
  QList<Entry> out;
  const QDir dir(stampDir());
  for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.png"), QStringLiteral("*.svg")}, QDir::Files,
                                               QDir::Time | QDir::Reversed)) {
    Entry e;
    e.path = fi.absoluteFilePath();
    if (fi.suffix().compare(QLatin1String("svg"), Qt::CaseInsensitive) == 0) e.art = readVectorFile(e.path, nullptr);
    else e.image = QImage(e.path);
    if (!e.image.isNull() || !e.art.isEmpty()) out.push_back(e);
  }
  return out;
}

bool CustomStamps::add(const QImage& image) {
  QImage img = image;
  if (img.width() > 1600 || img.height() > 1600) img = img.scaled(1600, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  return img.save(stampDir() + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".png", "PNG");
}

bool CustomStamps::add(const VectorArt& art) {
  if (art.isEmpty()) return false;
  const QRectF b = art.bounds();
  QFile f(stampDir() + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".svg");
  if (!f.open(QIODevice::WriteOnly)) return false;
  return f.write(art.toSvg(b.size())) > 0;
}

void CustomStamps::remove(const QString& path) {
  if (QFileInfo(path).absolutePath() == QFileInfo(stampDir()).absoluteFilePath()) {
    StampFavorites::set(QStringLiteral("custom:") + QFileInfo(path).fileName(), false);
    QFile::remove(path);
  }
}

QString StampFavorites::standardKey(const StampStyle& style) { return QStringLiteral("std:") + style.title; }

QStringList StampFavorites::load() {
  const QSettings s;
  if (!s.contains("stamps/favorites"))
    return {QStringLiteral("std:APPROVED"), QStringLiteral("std:REVIEWED"), QStringLiteral("std:PRELIMINARY"),
            QStringLiteral("std:CONFIDENTIAL")};
  return s.value("stamps/favorites").toStringList();
}

bool StampFavorites::contains(const QString& key) { return load().contains(key); }

void StampFavorites::set(const QString& key, bool favorite) {
  QStringList all = load();
  if (favorite && !all.contains(key)) all << key;
  if (!favorite) all.removeAll(key);
  QSettings().setValue("stamps/favorites", all);
}
