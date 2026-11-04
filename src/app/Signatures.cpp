// SPDX-License-Identifier: GPL-3.0-only
#include "Signatures.hpp"

#include "Markup.hpp"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPathStroker>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace {

QFont signatureFont(const QString& family) {
  QFont f(family.isEmpty() ? QString() : family);
  f.setPixelSize(96);
  if (family.isEmpty()) f.setItalic(true);  // no handwriting font: an italic will have to do
  f.setHintingPreference(QFont::PreferNoHinting);
  return f;
}

// A pen stroke as a smooth curve: through the midpoints between the points the mouse gave,
// with the points themselves as control points.
QPainterPath smooth(const QPolygonF& pts) {
  QPainterPath path;
  if (pts.isEmpty()) return path;
  path.moveTo(pts[0]);
  if (pts.size() == 1) {
    path.lineTo(pts[0] + QPointF(0.01, 0));  // a dot: the round caps make it round
    return path;
  }
  if (pts.size() == 2) {
    path.lineTo(pts[1]);
    return path;
  }
  for (int i = 1; i + 1 < pts.size(); ++i) path.quadTo(pts[i], (pts[i] + pts[i + 1]) / 2);
  path.lineTo(pts.back());
  return path;
}

}  // namespace

bool Signature::isEmpty() const {
  switch (kind) {
    case Kind::Drawn: return strokes.isEmpty();
    case Kind::Typed: return text.trimmed().isEmpty();
    case Kind::Image: return image.isNull();
  }
  return true;
}

QPainterPath Signature::outline() const {
  QPainterPath out;
  out.setFillRule(Qt::WindingFill);
  if (kind == Kind::Drawn) {
    QPainterPathStroker stroker;
    stroker.setWidth(penWidth);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    for (const QPolygonF& s : strokes) out.addPath(stroker.createStroke(smooth(s)));
  } else if (kind == Kind::Typed) {
    out.addText(0, 0, signatureFont(family), text.trimmed());
  }
  return out;
}

QSizeF Signature::size() const {
  if (kind == Kind::Image) return image.size();
  return outline().boundingRect().size();
}

pdfz::StampDrawing Signature::drawing() const {
  pdfz::StampDrawing d;
  if (kind == Kind::Image) {
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);  // straight alpha
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
    return d;
  }
  QPainterPath path = outline();
  const QRectF b = path.boundingRect();
  path.translate(-b.topLeft());
  d.width = std::max(1.0, b.width());
  d.height = std::max(1.0, b.height());
  d.paths.push_back(stampPath(path, color));
  return d;
}

QImage Signature::preview(const QSize& box, qreal dpr) const {
  QImage out(QSize(int(box.width() * dpr), int(box.height() * dpr)), QImage::Format_ARGB32_Premultiplied);
  out.setDevicePixelRatio(dpr);
  out.fill(Qt::transparent);
  const QSizeF s = size();
  if (s.isEmpty()) return out;
  QPainter p(&out);
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  const double k = std::min((box.width() - 4) / s.width(), (box.height() - 4) / s.height());
  const QSizeF drawn = s * k;
  const QRectF target(QPointF((box.width() - drawn.width()) / 2, (box.height() - drawn.height()) / 2), drawn);
  if (kind == Kind::Image) {
    p.drawImage(target, image);
  } else {
    QPainterPath path = outline();
    const QRectF b = path.boundingRect();
    p.translate(target.topLeft());
    p.scale(k, k);
    p.translate(-b.topLeft());
    p.fillPath(path, color);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------

namespace {

QString storePath() {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir().mkpath(dir);
  return dir + QStringLiteral("/signatures.json");
}

const char* kindKey(Signature::Kind k) {
  return k == Signature::Kind::Drawn ? "drawn" : k == Signature::Kind::Typed ? "typed" : "image";
}

}  // namespace

QList<Signature> SignatureStore::load() {
  QList<Signature> out;
  QFile f(storePath());
  if (!f.open(QIODevice::ReadOnly)) return out;
  const QJsonArray all = QJsonDocument::fromJson(f.readAll()).array();
  for (const QJsonValue& v : all) {
    const QJsonObject o = v.toObject();
    Signature s;
    s.id = o.value("id").toString();
    s.initials = o.value("initials").toBool();
    const QString kind = o.value("kind").toString();
    s.kind = kind == "typed" ? Signature::Kind::Typed : kind == "image" ? Signature::Kind::Image : Signature::Kind::Drawn;
    s.color = QColor(o.value("color").toString("#000000"));
    s.penWidth = o.value("penWidth").toDouble(3);
    for (const QJsonValue& stroke : o.value("strokes").toArray()) {
      const QJsonArray xy = stroke.toArray();
      QPolygonF poly;
      for (qsizetype i = 0; i + 1 < xy.size(); i += 2) poly << QPointF(xy[i].toDouble(), xy[i + 1].toDouble());
      if (!poly.isEmpty()) s.strokes << poly;
    }
    s.text = o.value("text").toString();
    s.family = o.value("family").toString();
    if (o.contains("png")) s.image.loadFromData(QByteArray::fromBase64(o.value("png").toString().toLatin1()), "PNG");
    if (!s.isEmpty() && !s.id.isEmpty()) out << s;
  }
  return out;
}

bool SignatureStore::save(const QList<Signature>& all) {
  QJsonArray arr;
  for (const Signature& s : all) {
    QJsonObject o;
    o["id"] = s.id;
    o["initials"] = s.initials;
    o["kind"] = kindKey(s.kind);
    o["color"] = s.color.name();
    if (s.kind == Signature::Kind::Drawn) {
      o["penWidth"] = s.penWidth;
      QJsonArray strokes;
      for (const QPolygonF& poly : s.strokes) {
        QJsonArray xy;
        for (const QPointF& p : poly) {
          xy.append(std::round(p.x() * 10) / 10);
          xy.append(std::round(p.y() * 10) / 10);
        }
        strokes.append(xy);
      }
      o["strokes"] = strokes;
    } else if (s.kind == Signature::Kind::Typed) {
      o["text"] = s.text;
      o["family"] = s.family;
    } else {
      QByteArray png;
      QBuffer buf(&png);
      buf.open(QIODevice::WriteOnly);
      s.image.save(&buf, "PNG");
      o["png"] = QString::fromLatin1(png.toBase64());
    }
    arr.append(o);
  }
  QSaveFile f(storePath());
  if (!f.open(QIODevice::WriteOnly)) return false;
  f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
  return f.commit();
}

void SignatureStore::add(Signature s) {
  QList<Signature> all = load();
  s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  all.prepend(s);  // newest first: what the Sign button uses
  save(all);
}

void SignatureStore::remove(const QString& id) {
  QList<Signature> all = load();
  all.erase(std::remove_if(all.begin(), all.end(), [&](const Signature& s) { return s.id == id; }), all.end());
  save(all);
}

QStringList handwritingFonts() {
  static const char* const candidates[] = {
      "Segoe Script", "Ink Free", "Lucida Handwriting", "Segoe Print", "Brush Script MT", "Freestyle Script",
      "Mistral", "Bradley Hand ITC", "Kristen ITC", "Gabriola", "Dancing Script", "Great Vibes", "Caveat",
      "Pacifico", "Satisfy", "Homemade Apple", "Z003", "URW Chancery L", "TeX Gyre Chorus", "Comic Neue"};
  QStringList out;
  const QStringList installed = QFontDatabase::families();
  for (const char* c : candidates)
    if (installed.contains(QString::fromLatin1(c), Qt::CaseInsensitive)) out << QString::fromLatin1(c);
  return out;
}

QImage cleanSignatureImage(const QImage& source, int threshold) {
  if (source.isNull()) return {};
  QImage img = source.convertToFormat(QImage::Format_ARGB32);
  if (img.width() > 1600 || img.height() > 1600)
    img = img.scaled(1600, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  // Lighter than the threshold: clear; a little darker: fading in, for smooth edges.
  const int fade = 40;
  const int lo = std::max(0, threshold - fade);
  int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
  for (int y = 0; y < img.height(); ++y) {
    auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
    for (int x = 0; x < img.width(); ++x) {
      const QRgb c = line[x];
      const int lum = qGray(c);
      int a = lum >= threshold ? 0 : lum <= lo ? 255 : 255 * (threshold - lum) / std::max(1, threshold - lo);
      a = a * qAlpha(c) / 255;
      line[x] = qRgba(qRed(c), qGreen(c), qBlue(c), a);
      if (a > 24) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
    }
  }
  if (x1 < x0 || y1 < y0) return {};  // nothing dark enough: no ink
  const int m = 2;
  return img.copy(QRect(QPoint(std::max(0, x0 - m), std::max(0, y0 - m)),
                        QPoint(std::min(img.width() - 1, x1 + m), std::min(img.height() - 1, y1 + m))));
}
