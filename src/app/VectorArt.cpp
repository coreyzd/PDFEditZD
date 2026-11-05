// SPDX-License-Identifier: GPL-3.0-only
#include "VectorArt.hpp"

#include "ImageUtil.hpp"
#include "Markup.hpp"

#include "core/content.hpp"
#include "core/document.hpp"

#include <QBuffer>
#include <QFileInfo>
#include <QJsonObject>
#include <QPageLayout>
#include <QPageSize>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPdfWriter>
#include <QSvgRenderer>
#include <QTextItem>

#include <algorithm>
#include <cmath>
#include <filesystem>

// ---------------------------------------------------------------------------------------------
// The drawing itself

QRectF VectorArt::bounds() const {
  QRectF r;
  for (const VectorShape& s : shapes) r = r.united(s.path.boundingRect());
  return r;
}

VectorArt VectorArt::normalized() const {
  const QRectF b = bounds();
  VectorArt out = *this;
  for (VectorShape& s : out.shapes) s.path.translate(-b.topLeft());
  return out;
}

void VectorArt::paint(QPainter& p, const QRectF& target) const {
  const QRectF b = bounds();
  if (b.isEmpty() || target.isEmpty()) return;
  const double k = std::min(target.width() / b.width(), target.height() / b.height());
  p.save();
  p.translate(target.center().x() - b.width() * k / 2, target.center().y() - b.height() * k / 2);
  p.scale(k, k);
  p.translate(-b.topLeft());
  p.setPen(Qt::NoPen);
  for (const VectorShape& s : shapes) p.fillPath(s.path, s.color);
  p.restore();
}

QImage VectorArt::preview(const QSize& box, qreal dpr) const {
  QImage out(QSize(int(box.width() * dpr), int(box.height() * dpr)), QImage::Format_ARGB32_Premultiplied);
  out.setDevicePixelRatio(dpr);
  out.fill(Qt::transparent);
  QPainter p(&out);
  p.setRenderHint(QPainter::Antialiasing);
  paint(p, QRectF(2, 2, box.width() - 4, box.height() - 4));
  return out;
}

pdfz::StampDrawing VectorArt::drawing() const {
  pdfz::StampDrawing d;
  const VectorArt art = normalized();
  const QRectF b = art.bounds();
  d.width = std::max(1.0, b.width());
  d.height = std::max(1.0, b.height());
  for (const VectorShape& s : art.shapes) d.paths.push_back(stampPath(s.path, s.color));
  return d;
}

// ---------------------------------------------------------------------------------------------
// Path data

namespace {

QString num(double v) {
  QString s = QString::number(std::round(v * 1000) / 1000, 'f', 3);
  while (s.contains('.') && (s.endsWith('0') || s.endsWith('.'))) s.chop(1);
  return s == QLatin1String("-0") ? QStringLiteral("0") : s;
}

}  // namespace

QString pathData(const QPainterPath& path) {
  QString d;
  QPointF start;
  for (int i = 0; i < path.elementCount(); ++i) {
    const QPainterPath::Element e = path.elementAt(i);
    if (e.isMoveTo()) {
      // A subpath that came back to its start is closed.
      if (i > 0 && path.elementAt(i - 1).x == start.x() && path.elementAt(i - 1).y == start.y()) d += QStringLiteral("Z ");
      d += QStringLiteral("M%1 %2 ").arg(num(e.x), num(e.y));
      start = QPointF(e.x, e.y);
    } else if (e.isLineTo()) {
      d += QStringLiteral("L%1 %2 ").arg(num(e.x), num(e.y));
    } else if (e.isCurveTo() && i + 2 < path.elementCount()) {
      const QPainterPath::Element c2 = path.elementAt(i + 1), end = path.elementAt(i + 2);
      d += QStringLiteral("C%1 %2 %3 %4 %5 %6 ").arg(num(e.x), num(e.y), num(c2.x), num(c2.y), num(end.x), num(end.y));
      i += 2;
    }
  }
  const int n = path.elementCount();
  if (n > 1 && path.elementAt(n - 1).x == start.x() && path.elementAt(n - 1).y == start.y()) d += QStringLiteral("Z");
  return d.trimmed();
}

QPainterPath parsePathData(const QString& d) {
  QPainterPath path;
  // Tokens: command letters and numbers.
  QList<QString> tokens;
  QString cur;
  auto flush = [&] {
    if (!cur.isEmpty()) tokens << cur;
    cur.clear();
  };
  for (QChar c : d) {
    if (c.isLetter() && c != 'e' && c != 'E') {
      flush();
      tokens << QString(c);
    } else if (c.isSpace() || c == ',') {
      flush();
    } else if (c == '-' && !cur.isEmpty() && !cur.endsWith('e') && !cur.endsWith('E')) {
      flush();
      cur += c;
    } else {
      cur += c;
    }
  }
  flush();
  QChar cmd;
  qsizetype i = 0;
  auto next = [&](double& v) {
    if (i >= tokens.size()) return false;
    bool ok = false;
    v = tokens[i].toDouble(&ok);
    if (!ok) return false;
    ++i;
    return true;
  };
  QPointF last;
  while (i < tokens.size()) {
    if (tokens[i].size() == 1 && tokens[i][0].isLetter()) cmd = tokens[i++][0];
    double a, b, c, e, f, g;
    switch (cmd.unicode()) {
      case 'M':
        if (!next(a) || !next(b)) return path;
        path.moveTo(a, b);
        last = {a, b};
        cmd = 'L';  // more pairs after a moveto are linetos
        break;
      case 'L':
        if (!next(a) || !next(b)) return path;
        path.lineTo(a, b);
        last = {a, b};
        break;
      case 'H':
        if (!next(a)) return path;
        path.lineTo(a, last.y());
        last.setX(a);
        break;
      case 'V':
        if (!next(a)) return path;
        path.lineTo(last.x(), a);
        last.setY(a);
        break;
      case 'C':
        if (!next(a) || !next(b) || !next(c) || !next(e) || !next(f) || !next(g)) return path;
        path.cubicTo(a, b, c, e, f, g);
        last = {f, g};
        break;
      case 'Q':
        if (!next(a) || !next(b) || !next(c) || !next(e)) return path;
        path.quadTo(a, b, c, e);
        last = {c, e};
        break;
      case 'Z':
      case 'z':
        path.closeSubpath();
        last = path.currentPosition();
        break;
      default:
        ++i;  // not ours: skipped
    }
  }
  return path;
}

QJsonArray VectorArt::toJson() const {
  QJsonArray out;
  for (const VectorShape& s : shapes) {
    QJsonObject o;
    o["d"] = pathData(s.path);
    o["c"] = s.color.name();
    if (s.path.fillRule() == Qt::OddEvenFill) o["eo"] = true;
    out.append(o);
  }
  return out;
}

VectorArt VectorArt::fromJson(const QJsonArray& a) {
  VectorArt out;
  for (const QJsonValue& v : a) {
    const QJsonObject o = v.toObject();
    VectorShape s;
    s.path = parsePathData(o.value("d").toString());
    s.path.setFillRule(o.value("eo").toBool() ? Qt::OddEvenFill : Qt::WindingFill);
    s.color = QColor(o.value("c").toString("#000000"));
    if (!s.path.isEmpty()) out.shapes << s;
  }
  return out;
}

QByteArray VectorArt::toSvg(QSizeF size) const {
  const QRectF b = bounds();
  QString svg = QStringLiteral(
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%1pt\" height=\"%2pt\" viewBox=\"0 0 %1 %2\">\n")
                    .arg(num(size.width()), num(size.height()));
  if (!b.isEmpty()) {
    const double k = std::min(size.width() / b.width(), size.height() / b.height());
    const double tx = (size.width() - b.width() * k) / 2 - b.left() * k, ty = (size.height() - b.height() * k) / 2 - b.top() * k;
    svg += QStringLiteral("<g transform=\"matrix(%1 0 0 %1 %2 %3)\">\n").arg(num(k), num(tx), num(ty));
    for (const VectorShape& s : shapes)
      svg += QStringLiteral("<path fill=\"%1\"%2 d=\"%3\"/>\n")
                 .arg(s.color.name(), s.path.fillRule() == Qt::OddEvenFill ? QStringLiteral(" fill-rule=\"evenodd\"") : QString(),
                      pathData(s.path));
    svg += QStringLiteral("</g>\n");
  }
  svg += QStringLiteral("</svg>\n");
  return svg.toUtf8();
}

QByteArray imageSvg(const QImage& image, QSizeF size) {
  QByteArray png;
  QBuffer buf(&png);
  buf.open(QIODevice::WriteOnly);
  image.save(&buf, "PNG");
  return QStringLiteral(
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"%1pt\" "
             "height=\"%2pt\" viewBox=\"0 0 %1 %2\">\n"
             "<image width=\"%1\" height=\"%2\" preserveAspectRatio=\"xMidYMid meet\" "
             "xlink:href=\"data:image/png;base64,%3\"/>\n</svg>\n")
      .arg(num(size.width()), num(size.height()), QString::fromLatin1(png.toBase64()))
      .toUtf8();
}

bool writeVectorPdf(const QString& file, const VectorArt& art, QSizeF size, const QImage& image) {
  QPdfWriter writer(file);
  writer.setCreator(QStringLiteral("PDFEditZD"));
  writer.setResolution(720);  // 10 units a point
  QPageLayout layout(QPageSize(size, QPageSize::Point, QString(), QPageSize::ExactMatch), QPageLayout::Portrait,
                     QMarginsF(0, 0, 0, 0));
  layout.setMode(QPageLayout::FullPageMode);
  if (!writer.setPageLayout(layout)) return false;
  QPainter p;
  if (!p.begin(&writer)) return false;
  p.scale(10, 10);
  const QRectF target(0, 0, size.width(), size.height());
  if (!art.isEmpty()) {
    art.paint(p, target);
  } else if (!image.isNull()) {
    const QSizeF s = QSizeF(image.size()).scaled(target.size(), Qt::KeepAspectRatio);
    p.drawImage(QRectF(target.center() - QPointF(s.width() / 2, s.height() / 2), s), image);
  }
  return p.end();
}

// ---------------------------------------------------------------------------------------------
// Reading SVG: Qt SVG draws it onto a device that keeps what's drawn as shapes.

namespace {

QColor brushColor(const QBrush& b) {
  if (const QGradient* g = b.gradient()) {
    // A gradient: the average of its colors.
    double r = 0, gr = 0, bl = 0, a = 0;
    const QGradientStops stops = g->stops();
    for (const auto& s : stops) {
      r += s.second.redF();
      gr += s.second.greenF();
      bl += s.second.blueF();
      a += s.second.alphaF();
    }
    const double n = double(std::max<qsizetype>(1, stops.size()));
    return QColor::fromRgbF(float(r / n), float(gr / n), float(bl / n), float(a / n));
  }
  return b.color();
}

class RecordingEngine : public QPaintEngine {
public:
  RecordingEngine() : QPaintEngine(QPaintEngine::AllFeatures) {}
  VectorArt art;
  int pictures = 0;

  bool begin(QPaintDevice*) override { return true; }
  bool end() override { return true; }
  void updateState(const QPaintEngineState&) override {}  // read from the painter when drawing
  Type type() const override { return QPaintEngine::User; }

  void drawPath(const QPainterPath& path) override { add(path, true, true); }
  void drawPolygon(const QPointF* points, int count, PolygonDrawMode mode) override {
    QPainterPath path;
    if (count < 2) return;
    path.moveTo(points[0]);
    for (int i = 1; i < count; ++i) path.lineTo(points[i]);
    if (mode != PolylineMode) path.closeSubpath();
    path.setFillRule(mode == OddEvenMode ? Qt::OddEvenFill : Qt::WindingFill);
    add(path, mode != PolylineMode, true);
  }
  void drawRects(const QRectF* rects, int count) override {
    for (int i = 0; i < count; ++i) {
      QPainterPath path;
      path.addRect(rects[i]);
      add(path, true, true);
    }
  }
  void drawLines(const QLineF* lines, int count) override {
    for (int i = 0; i < count; ++i) {
      QPainterPath path;
      path.moveTo(lines[i].p1());
      path.lineTo(lines[i].p2());
      add(path, false, true);
    }
  }
  void drawEllipse(const QRectF& r) override {
    QPainterPath path;
    path.addEllipse(r);
    add(path, true, true);
  }
  void drawTextItem(const QPointF& p, const QTextItem& item) override {
    QPainterPath path;
    path.addText(p, item.font(), item.text());
    fill(path, painter()->pen().brush());
  }
  void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override { ++pictures; }
  void drawImage(const QRectF&, const QImage&, const QRectF&, Qt::ImageConversionFlags) override { ++pictures; }
  void drawTiledPixmap(const QRectF&, const QPixmap&, const QPointF&) override { ++pictures; }

private:
  void fill(const QPainterPath& path, const QBrush& brush) {
    if (brush.style() == Qt::NoBrush) return;
    QColor c = brushColor(brush);
    const double alpha = c.alphaF() * painter()->opacity();
    if (alpha < 0.05) return;
    c.setAlpha(255);
    QPainterPath shape = painter()->combinedTransform().map(path);
    shape.setFillRule(path.fillRule());
    if (painter()->hasClipping()) {
      const QPainterPath clip = painter()->combinedTransform().map(painter()->clipPath());
      if (!clip.boundingRect().contains(shape.boundingRect())) shape = shape.intersected(clip);
    }
    if (shape.isEmpty()) return;
    art.shapes << VectorShape{shape, c};
  }

  void add(const QPainterPath& path, bool withFill, bool withStroke) {
    if (withFill) fill(path, painter()->brush());
    const QPen pen = painter()->pen();
    if (!withStroke || pen.style() == Qt::NoPen) return;
    QPainterPathStroker stroker;
    stroker.setWidth(pen.widthF() > 0 ? pen.widthF() : 0.5);
    stroker.setCapStyle(pen.capStyle());
    stroker.setJoinStyle(pen.joinStyle());
    stroker.setMiterLimit(pen.miterLimit());
    if (pen.style() != Qt::SolidLine) {
      stroker.setDashPattern(pen.dashPattern());
      stroker.setDashOffset(pen.dashOffset());
    }
    QPainterPath outline = stroker.createStroke(path);
    outline.setFillRule(Qt::WindingFill);
    fill(outline, pen.brush());
  }
};

class RecordingDevice : public QPaintDevice {
public:
  explicit RecordingDevice(QSize size) : size_(size) {}
  RecordingEngine engine;
  QPaintEngine* paintEngine() const override { return const_cast<RecordingEngine*>(&engine); }

protected:
  int metric(PaintDeviceMetric m) const override {
    switch (m) {
      case PdmWidth: return size_.width();
      case PdmHeight: return size_.height();
      case PdmWidthMM: return int(size_.width() * 25.4 / 72);
      case PdmHeightMM: return int(size_.height() * 25.4 / 72);
      case PdmNumColors: return INT_MAX;
      case PdmDepth: return 32;
      case PdmDpiX:
      case PdmDpiY:
      case PdmPhysicalDpiX:
      case PdmPhysicalDpiY: return 72;
      case PdmDevicePixelRatio: return 1;
      case PdmDevicePixelRatioScaled: return int(devicePixelRatioFScale());
      default: return 0;
    }
  }

private:
  QSize size_;
};

QPainterPath toPath(const std::vector<pdfz::StampDrawing::Op>& ops) {
  QPainterPath path;
  for (const auto& op : ops) {
    switch (op.kind) {
      case pdfz::StampDrawing::Op::MoveTo: path.moveTo(op.p[0].x, op.p[0].y); break;
      case pdfz::StampDrawing::Op::LineTo: path.lineTo(op.p[0].x, op.p[0].y); break;
      case pdfz::StampDrawing::Op::CubicTo: path.cubicTo(op.p[0].x, op.p[0].y, op.p[1].x, op.p[1].y, op.p[2].x, op.p[2].y); break;
      case pdfz::StampDrawing::Op::Close: path.closeSubpath(); break;
    }
  }
  return path;
}

bool isSvg(const QString& path) {
  const QString s = QFileInfo(path).suffix().toLower();
  return s == "svg" || s == "svgz";
}

}  // namespace

QStringList vectorPatterns() { return {"*.svg", "*.svgz", "*.pdf", "*.ai"}; }

bool isVectorFile(const QString& path) {
  return vectorPatterns().contains("*." + QFileInfo(path).suffix().toLower());
}

VectorArt readVectorFile(const QString& file, QString* error, bool* pictures) {
  if (error) error->clear();
  if (pictures) *pictures = false;
  if (isSvg(file)) {
    QSvgRenderer svg(file);
    if (!svg.isValid()) {
      if (error) *error = QObject::tr("That SVG file couldn't be read.");
      return {};
    }
    QSize size = svg.defaultSize();
    if (size.isEmpty()) size = QSize(500, 500);
    RecordingDevice dev(size);
    QPainter p(&dev);
    svg.render(&p, QRectF(QPointF(0, 0), size));
    p.end();
    if (pictures) *pictures = dev.engine.pictures > 0;
    return dev.engine.art;
  }
  auto [doc, err] = pdfz::Document::open(std::filesystem::path(file.toStdU16String()));
  if (!doc || doc->pageCount() < 1) {
    if (error)
      *error = err == pdfz::OpenError::Password ? QObject::tr("That PDF has a password.")
                                                : QObject::tr("That file couldn't be read as a PDF.");
    return {};
  }
  int pics = 0;
  VectorArt art;
  for (const pdfz::PageShape& s : doc->pageShapes(0, &pics)) {
    QPainterPath path = toPath(s.ops);
    if (s.fill) {
      QPainterPath filled = path;
      filled.setFillRule(s.evenOdd ? Qt::OddEvenFill : Qt::WindingFill);
      art.shapes << VectorShape{filled, toQColor(s.fillColor)};
    }
    if (s.stroke) {
      QPainterPathStroker stroker;
      stroker.setWidth(s.strokeWidth);
      stroker.setCapStyle(s.lineCap == 1 ? Qt::RoundCap : s.lineCap == 2 ? Qt::SquareCap : Qt::FlatCap);
      stroker.setJoinStyle(s.lineJoin == 1 ? Qt::RoundJoin : s.lineJoin == 2 ? Qt::BevelJoin : Qt::MiterJoin);
      QPainterPath outline = stroker.createStroke(path);
      outline.setFillRule(Qt::WindingFill);
      art.shapes << VectorShape{outline, toQColor(s.strokeColor)};
    }
  }
  if (pictures) *pictures = pics > 0;
  return art;
}

QImage renderVectorFile(const QString& file, int maxSide) {
  if (isSvg(file)) {
    QSvgRenderer svg(file);
    if (!svg.isValid()) return {};
    QSizeF size = svg.defaultSize();
    if (size.isEmpty()) size = QSizeF(500, 500);
    size.scale(maxSide, maxSide, Qt::KeepAspectRatio);
    QImage img(size.toSize(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    svg.render(&p);
    return img;
  }
  auto [doc, err] = pdfz::Document::open(std::filesystem::path(file.toStdU16String()));
  if (!doc || doc->pageCount() < 1) return {};
  return toQImage(doc->renderFit(0, maxSide, maxSide)).copy();
}
