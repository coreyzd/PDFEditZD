// SPDX-License-Identifier: GPL-3.0-only
#include "Markup.hpp"

#include "core/document.hpp"

#include <QDateTime>
#include <QFont>
#include <QFontMetricsF>
#include <QLocale>
#include <QObject>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QSettings>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <cstdio>

void AnnotationCache::setDocument(std::shared_ptr<pdfz::Document> doc) {
  doc_ = std::move(doc);
  cache_.clear();
}

std::shared_ptr<const AnnotationList> AnnotationCache::get(int page) {
  if (!doc_ || page < 0 || page >= doc_->pageCount()) return nullptr;
  if (auto* hit = cache_.object(page)) return *hit;
  auto list = std::make_shared<const AnnotationList>(doc_->annotations(page));
  cache_.insert(page, new std::shared_ptr<const AnnotationList>(list));
  return list;
}

void AnnotationCache::invalidate(int page) { cache_.remove(page); }

namespace {

double distToSegment(double px, double py, pdfz::PointF a, pdfz::PointF b) {
  const double dx = b.x - a.x, dy = b.y - a.y;
  const double len2 = dx * dx + dy * dy;
  double t = len2 > 0 ? ((px - a.x) * dx + (py - a.y) * dy) / len2 : 0;
  t = std::clamp(t, 0.0, 1.0);
  const double cx = a.x + t * dx - px, cy = a.y + t * dy - py;
  return std::sqrt(cx * cx + cy * cy);
}

bool hit(const pdfz::Annotation& a, double x, double y, double tol) {
  using K = pdfz::AnnotKind;
  if (a.hidden) return false;  // e.g. the text box being typed in
  if (pdfz::isTextMarkup(a.kind)) {
    for (const pdfz::RectF& q : a.quads)
      if (x >= q.left - tol && x <= q.right + tol && y >= q.top - tol && y <= q.bottom + tol) return true;
    return false;
  }
  const pdfz::RectF& r = a.rect;
  const bool inRect = x >= r.left - tol && x <= r.right + tol && y >= r.top - tol && y <= r.bottom + tol;
  if (!inRect) return false;
  const double edge = a.borderWidth / 2 + tol;
  switch (a.kind) {
    case K::Ink:
    case K::Line:
      for (const auto& s : a.ink) {
        if (s.size() == 1 && std::hypot(s[0].x - x, s[0].y - y) <= edge) return true;
        for (std::size_t i = 1; i < s.size(); ++i)
          if (distToSegment(x, y, s[i - 1], s[i]) <= edge) return true;
      }
      return false;
    case K::Square:
      if (a.hasFill) return true;
      // An empty rectangle is picked by its outline, so the text inside stays selectable.
      return std::min({std::abs(x - r.left), std::abs(x - r.right), std::abs(y - r.top), std::abs(y - r.bottom)}) <= edge;
    case K::Circle: {
      if (a.hasFill) return true;
      const double rx = r.width() / 2, ry = r.height() / 2;
      if (rx <= 0 || ry <= 0) return false;
      const double nx = (x - (r.left + rx)) / rx, ny = (y - (r.top + ry)) / ry;
      const double d = std::sqrt(nx * nx + ny * ny);  // 1 on the ellipse
      return std::abs(d - 1) * std::min(rx, ry) <= edge;
    }
    default:
      return true;
  }
}

}  // namespace

int annotationAt(const AnnotationList& list, double x, double y, double tolerance) {
  for (int i = int(list.size()) - 1; i >= 0; --i)  // the last drawn is on top
    if (hit(list[std::size_t(i)], x, y, tolerance)) return i;
  return -1;
}

bool canMove(const pdfz::Annotation& a) { return !pdfz::isTextMarkup(a.kind); }

bool canResize(const pdfz::Annotation& a) {
  return a.kind == pdfz::AnnotKind::Square || a.kind == pdfz::AnnotKind::Circle || a.kind == pdfz::AnnotKind::FreeText ||
         a.isPdfzStamp();
}

QString toolName(MarkupTool t) {
  switch (t) {
    case MarkupTool::Highlight: return QObject::tr("Highlight");
    case MarkupTool::Underline: return QObject::tr("Underline");
    case MarkupTool::StrikeOut: return QObject::tr("Strikethrough");
    case MarkupTool::Note: return QObject::tr("Sticky Note");
    case MarkupTool::TextBox: return QObject::tr("Text Box");
    case MarkupTool::Pen: return QObject::tr("Pen");
    case MarkupTool::Rectangle: return QObject::tr("Rectangle");
    case MarkupTool::Ellipse: return QObject::tr("Ellipse");
    case MarkupTool::Line: return QObject::tr("Line");
    case MarkupTool::Arrow: return QObject::tr("Arrow");
    case MarkupTool::FillText: return QObject::tr("Add Text");
    case MarkupTool::Check: return QObject::tr("Checkmark");
    case MarkupTool::Cross: return QObject::tr("Cross");
    case MarkupTool::Dot: return QObject::tr("Dot");
    case MarkupTool::Date: return QObject::tr("Date");
  }
  return {};
}

QString toolKey(MarkupTool t) {
  static const char* const keys[] = {"highlight", "underline", "strikeout", "note", "textbox",
                                     "pen", "rectangle", "ellipse", "line", "arrow",
                                     "filltext", "check", "cross", "dot", "date"};
  return QLatin1String(keys[int(t)]);
}

MarkupStyle defaultStyle(MarkupTool t) {
  MarkupStyle s;
  const QColor red(0xE0, 0x1B, 0x24);
  s.color = red;
  s.textColor = red;
  s.width = 2;
  switch (t) {
    case MarkupTool::Highlight: s.color = QColor(0xFF, 0xE5, 0x00); s.width = 1; break;
    case MarkupTool::Underline: s.color = QColor(0x00, 0x9E, 0x3A); s.width = 1; break;
    case MarkupTool::StrikeOut: s.width = 1; break;
    case MarkupTool::Note: s.color = QColor(0xFF, 0xD4, 0x00); s.width = 1; break;
    case MarkupTool::TextBox: s.width = 1; break;  // red text in a thin red box, like Acrobat's
    case MarkupTool::Pen: s.color = QColor(0x1A, 0x5F, 0xD0); break;
    // Fill & Sign: black, and text as if typed on the form (no box).
    case MarkupTool::FillText:
    case MarkupTool::Date:
      s.color = s.textColor = Qt::black;
      s.width = 0;
      s.fontSize = 11;
      break;
    case MarkupTool::Check:
    case MarkupTool::Cross:
    case MarkupTool::Dot: s.color = Qt::black; break;
    default: break;  // shapes and lines
  }
  return s;
}

MarkupStyle loadStyle(MarkupTool t) {
  QSettings s;
  const MarkupStyle d = defaultStyle(t);
  const QString k = "markup/" + toolKey(t) + "/";
  auto color = [&](const char* key, const QColor& def) {
    const QString v = s.value(k + key, def.isValid() ? def.name() : QString()).toString();
    return v.isEmpty() ? QColor() : QColor(v);
  };
  MarkupStyle out;
  out.color = color("color", d.color);
  if (!out.color.isValid()) out.color = d.color;
  out.fill = color("fill", d.fill);
  out.textColor = color("textColor", d.textColor);
  if (!out.textColor.isValid()) out.textColor = d.textColor;
  out.opacity = std::clamp(s.value(k + "opacity", d.opacity).toDouble(), 0.05, 1.0);
  out.width = s.value(k + "width", d.width).toDouble();
  out.fontSize = s.value(k + "fontSize", d.fontSize).toDouble();
  out.fontName = s.value(k + "fontName", d.fontName).toString();
  out.align = std::clamp(s.value(k + "align", d.align).toInt(), 0, 2);
  return out;
}

void saveStyle(MarkupTool t, const MarkupStyle& st) {
  QSettings s;
  const QString k = "markup/" + toolKey(t) + "/";
  s.setValue(k + "color", st.color.name());
  s.setValue(k + "fill", st.fill.isValid() ? st.fill.name() : QString());
  s.setValue(k + "textColor", st.textColor.isValid() ? st.textColor.name() : QString());
  s.setValue(k + "opacity", st.opacity);
  s.setValue(k + "width", st.width);
  s.setValue(k + "fontSize", st.fontSize);
  s.setValue(k + "fontName", st.fontName);
  s.setValue(k + "align", st.align);
}

void applyStyle(pdfz::Annotation& a, const MarkupStyle& s) {
  a.color = fromQColor(s.color);
  a.opacity = s.opacity;
  a.borderWidth = s.width;
  a.hasFill = s.fill.isValid() && usesFill(a);
  if (a.hasFill) a.fill = fromQColor(s.fill);
  if (a.kind == pdfz::AnnotKind::FreeText) {
    a.fontSize = s.fontSize;
    a.fontName = s.fontName.toStdString();
    a.textColor = fromQColor(s.textColor.isValid() ? s.textColor : s.color);
    a.align = s.align;
  }
}

void applyStyleChange(pdfz::Annotation& a, const StyleChange& c) {
  if (c.color) a.color = fromQColor(*c.color);
  if (c.fill) {
    a.hasFill = c.fill->isValid();
    if (a.hasFill) a.fill = fromQColor(*c.fill);
  }
  if (c.opacity) a.opacity = std::clamp(*c.opacity, 0.05, 1.0);
  if (c.width) a.borderWidth = *c.width;
  if (c.fontSize) a.fontSize = *c.fontSize;
  if (c.fontName) a.fontName = c.fontName->toStdString();
  if (c.textColor) a.textColor = fromQColor(*c.textColor);
  if (c.align) a.align = std::clamp(*c.align, 0, 2);
  if (c.width && isArrow(a) && !a.ink.empty() && a.ink[0].size() >= 2)  // the head follows the width
    a.ink = arrowStrokes(a.ink[0].front(), a.ink[0].back(), a.borderWidth);
}

void applyStyleChange(MarkupStyle& s, const StyleChange& c) {
  if (c.color) s.color = *c.color;
  if (c.fill) s.fill = *c.fill;
  if (c.opacity) s.opacity = std::clamp(*c.opacity, 0.05, 1.0);
  if (c.width) s.width = *c.width;
  if (c.fontSize) s.fontSize = *c.fontSize;
  if (c.fontName) s.fontName = *c.fontName;
  if (c.textColor) s.textColor = *c.textColor;
  if (c.align) s.align = std::clamp(*c.align, 0, 2);
}

MarkupStyle styleOf(const pdfz::Annotation& a) {
  MarkupStyle s;
  s.color = toQColor(a.color);
  if (a.hasFill) s.fill = toQColor(a.fill);
  s.opacity = a.opacity;
  s.width = a.borderWidth;
  s.fontSize = a.fontSize;
  s.fontName = QString::fromStdString(a.fontName);
  s.textColor = toQColor(a.textColor);
  s.align = a.align;
  return s;
}

QString styleChangeLabel(const StyleChange& c) {
  const int n = int(c.color.has_value()) + int(c.fill.has_value()) + int(c.opacity.has_value()) +
                int(c.width.has_value()) + int(c.fontSize.has_value()) + int(c.fontName.has_value()) +
                int(c.textColor.has_value()) + int(c.align.has_value());
  if (n != 1) return QObject::tr("Change Style");
  if (c.color) return QObject::tr("Change Color");
  if (c.fill) return c.fill->isValid() ? QObject::tr("Change Fill") : QObject::tr("Remove Fill");
  if (c.opacity) return QObject::tr("Change Opacity");
  if (c.width) return QObject::tr("Change Line Width");
  if (c.fontSize) return QObject::tr("Change Font Size");
  if (c.fontName) return QObject::tr("Change Font");
  if (c.textColor) return QObject::tr("Change Text Color");
  return QObject::tr("Change Alignment");
}

bool usesFill(MarkupTool t) { return t == MarkupTool::Rectangle || t == MarkupTool::Ellipse || t == MarkupTool::TextBox; }
bool usesWidth(MarkupTool t) {
  return t == MarkupTool::Pen || t == MarkupTool::Rectangle || t == MarkupTool::Ellipse || t == MarkupTool::Line ||
         t == MarkupTool::Arrow || t == MarkupTool::TextBox;
}
bool usesText(MarkupTool t) { return t == MarkupTool::TextBox || t == MarkupTool::FillText || t == MarkupTool::Date; }
bool usesOpacity(MarkupTool t) { return !markSubject(t); }
bool usesOpacity(const pdfz::Annotation& a) { return a.kind != pdfz::AnnotKind::Stamp; }

bool isMark(const pdfz::Annotation& a) {
  return a.kind == pdfz::AnnotKind::Stamp && (a.subject == "Check" || a.subject == "Cross" || a.subject == "Dot");
}

const char* markSubject(MarkupTool t) {
  switch (t) {
    case MarkupTool::Check: return "Check";
    case MarkupTool::Cross: return "Cross";
    case MarkupTool::Dot: return "Dot";
    default: return nullptr;
  }
}

double markSize(MarkupTool t) { return t == MarkupTool::Dot ? 7 : 14; }

pdfz::StampDrawing::Path stampPath(const QPainterPath& path, const QColor& color) {
  using Op = pdfz::StampDrawing::Op;
  pdfz::StampDrawing::Path out;
  out.color = fromQColor(color);
  for (int i = 0; i < path.elementCount(); ++i) {
    const QPainterPath::Element e = path.elementAt(i);
    Op op;
    if (e.isMoveTo()) {
      op.kind = Op::MoveTo;
      op.p[0] = {e.x, e.y};
    } else if (e.isLineTo()) {
      op.kind = Op::LineTo;
      op.p[0] = {e.x, e.y};
    } else if (e.isCurveTo() && i + 2 < path.elementCount()) {
      op.kind = Op::CubicTo;
      const QPainterPath::Element c2 = path.elementAt(i + 1), end = path.elementAt(i + 2);
      op.p[0] = {e.x, e.y};
      op.p[1] = {c2.x, c2.y};
      op.p[2] = {end.x, end.y};
      i += 2;
    } else {
      continue;
    }
    out.ops.push_back(op);
  }
  return out;
}

pdfz::StampDrawing markDrawing(const std::string& subject, const QColor& color) {
  // In a 100 x 100 box; strokes become outlines, so they scale with the mark.
  QPainterPath shape;
  if (subject == "Dot") {
    shape.addEllipse(QPointF(50, 50), 44, 44);
  } else {
    QPainterPath line;
    if (subject == "Check") {
      line.moveTo(10, 55);
      line.lineTo(38, 84);
      line.lineTo(90, 14);
    } else {  // Cross
      line.moveTo(16, 16);
      line.lineTo(84, 84);
      line.moveTo(84, 16);
      line.lineTo(16, 84);
    }
    QPainterPathStroker stroker;
    stroker.setWidth(14);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    shape = stroker.createStroke(line);
  }
  pdfz::StampDrawing d;
  d.width = d.height = 100;
  d.paths.push_back(stampPath(shape, color));
  return d;
}
bool usesFill(const pdfz::Annotation& a) {
  return a.kind == pdfz::AnnotKind::Square || a.kind == pdfz::AnnotKind::Circle || a.kind == pdfz::AnnotKind::FreeText;
}
bool usesWidth(const pdfz::Annotation& a) {
  return a.kind == pdfz::AnnotKind::Ink || usesFill(a);
}
bool usesText(const pdfz::Annotation& a) { return a.kind == pdfz::AnnotKind::FreeText; }

bool isArrow(const pdfz::Annotation& a) { return a.kind == pdfz::AnnotKind::Ink && a.subject == "Arrow"; }

std::vector<std::vector<pdfz::PointF>> arrowStrokes(pdfz::PointF tail, pdfz::PointF tip, double width) {
  // Arms about six times the line width (plus a little, for thin lines), 30 degrees off the
  // shaft: at 8 pt that's a head over 50 pt long, still clearly an arrow.
  const double len = std::hypot(tip.x - tail.x, tip.y - tail.y);
  const double head = std::min(6 + 6 * width, std::max(len * 0.9, 6.0));
  const double ang = std::atan2(tip.y - tail.y, tip.x - tail.x);
  const double spread = 30 * 3.14159265358979 / 180;
  const pdfz::PointF h1{tip.x - head * std::cos(ang - spread), tip.y - head * std::sin(ang - spread)};
  const pdfz::PointF h2{tip.x - head * std::cos(ang + spread), tip.y - head * std::sin(ang + spread)};
  return {{tail, tip}, {h1, tip, h2}};
}

QFont fontFor(const std::string& name, double pixelSize) {
  QFont f;
  if (name == "TiRo") {
    f.setFamily(QStringLiteral("Times New Roman"));
    f.setStyleHint(QFont::Serif);
  } else if (name == "Cour") {
    f.setFamily(QStringLiteral("Courier New"));
    f.setStyleHint(QFont::TypeWriter);
  } else {
    f.setFamily(QStringLiteral("Helvetica"));  // Arial on Windows
    f.setStyleHint(QFont::SansSerif);
  }
  f.setPixelSize(std::max(1, int(std::lround(pixelSize))));
  f.setHintingPreference(QFont::PreferNoHinting);
  return f;
}

double textBoxPadding(const pdfz::Annotation& a) { return 2 + std::max(0.0, a.borderWidth); }

QString commentAuthor() {
  const QString set = QSettings().value("comments/author").toString().trimmed();
  if (!set.isEmpty()) return set;
  QString user = qEnvironmentVariable("USERNAME");
  if (user.isEmpty()) user = qEnvironmentVariable("USER");
  return user;
}

void setCommentAuthor(const QString& name) { QSettings().setValue("comments/author", name.trimmed()); }

QColor toQColor(pdfz::Color c) { return QColor(c.r, c.g, c.b); }
pdfz::Color fromQColor(const QColor& c) {
  return {std::uint8_t(c.red()), std::uint8_t(c.green()), std::uint8_t(c.blue())};
}

namespace {

std::string num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  std::string s = buf;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  if (s == "-0") s = "0";
  return s;
}

std::string colorOp(pdfz::Color c, const char* op) {
  return num(c.r / 255.0) + " " + num(c.g / 255.0) + " " + num(c.b / 255.0) + " " + op + "\n";
}

}  // namespace

std::string freeTextAppearance(const pdfz::Annotation& a, const pdfz::PageFrame& f) {
  const pdfz::RectF box = a.rect.normalized();
  const double pad = textBoxPadding(a);
  const double fontSize = std::max(1.0, a.fontSize);
  // Laid out at a large size for precise outlines, then scaled down to the font size.
  constexpr double kLayoutPx = 1000;
  const double k = fontSize / kLayoutPx;
  const QFont font = fontFor(a.fontName, kLayoutPx);

  QString text = QString::fromStdString(a.contents);
  text.replace(QLatin1String("\r\n"), QString(QChar::LineSeparator));
  text.replace(QLatin1Char('\n'), QChar::LineSeparator);
  text.replace(QLatin1Char('\r'), QChar::LineSeparator);
  QTextLayout layout(text, font);
  QTextOption opt;
  opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  layout.setTextOption(opt);
  const double availW = std::max(1.0, box.width() - 2 * pad) / k;
  double y = 0;
  layout.beginLayout();
  for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
    line.setLineWidth(availW);
    line.setPosition(QPointF(0, y));
    y += line.height();
  }
  layout.endLayout();

  QPainterPath path;
  for (int i = 0; i < layout.lineCount(); ++i) {
    const QTextLine line = layout.lineAt(i);
    QString t = text.mid(line.textStart(), line.textLength());
    t.remove(QChar::LineSeparator);
    while (t.endsWith(QLatin1Char(' '))) t.chop(1);
    if (t.isEmpty()) continue;
    double x = 0;  // aligned within the box
    const double w = QFontMetricsF(font).horizontalAdvance(t);
    if (a.align == 1) x = (availW - w) / 2;
    else if (a.align == 2) x = availW - w;
    path.addText(QPointF(x, line.y() + line.ascent()), font, t);
  }

  auto toUser = [&](QPointF p) {  // layout units -> display points -> PDF user space
    return f.toUser(pdfz::PointF{box.left + pad + p.x() * k, box.top + pad + p.y() * k});
  };
  auto rectOp = [&](const pdfz::RectF& displayRect) {
    const pdfz::RectF u = f.toUser(displayRect);
    return num(u.left) + " " + num(u.bottom) + " " + num(u.right - u.left) + " " + num(u.top - u.bottom) + " re";
  };
  std::string out;
  if (a.opacity < 0.999) out += "/GS gs\n";  // PDFium supplies /GS from the annotation's /CA
  out += "q\n";
  out += rectOp(box) + " W n\n";  // text that doesn't fit doesn't spill out of the box
  if (a.hasFill) out += colorOp(a.fill, "rg") + rectOp(box) + " f\n";
  if (a.borderWidth > 0) {
    const double h = a.borderWidth / 2;
    out += num(a.borderWidth) + " w\n" + colorOp(a.color, "RG");
    out += rectOp(pdfz::RectF{box.left + h, box.top + h, box.right - h, box.bottom - h}) + " S\n";
  }
  out += colorOp(a.textColor, "rg");
  for (int i = 0; i < path.elementCount(); ++i) {
    const QPainterPath::Element e = path.elementAt(i);
    if (e.isMoveTo()) {
      const pdfz::PointF p = toUser(e);
      out += num(p.x) + " " + num(p.y) + " m\n";
    } else if (e.isLineTo()) {
      const pdfz::PointF p = toUser(e);
      out += num(p.x) + " " + num(p.y) + " l\n";
    } else if (e.isCurveTo() && i + 2 < path.elementCount()) {
      const pdfz::PointF c1 = toUser(e), c2 = toUser(path.elementAt(i + 1)), p = toUser(path.elementAt(i + 2));
      out += num(c1.x) + " " + num(c1.y) + " " + num(c2.x) + " " + num(c2.y) + " " + num(p.x) + " " + num(p.y) + " c\n";
      i += 2;
    }
  }
  if (!path.isEmpty()) out += "f\n";  // glyph outlines fill with the nonzero rule
  out += "Q\n";
  return out;
}

QString formatPdfDate(const std::string& d) {
  // D:YYYYMMDDHHmmSS, the rest optional; the zone is ignored (shown as written).
  std::string s = d.rfind("D:", 0) == 0 ? d.substr(2) : d;
  if (s.size() < 8) return {};
  s.resize(std::min<std::size_t>(s.size(), 14));
  while (s.size() < 14) s += "00";
  const QDateTime t = QDateTime::fromString(QString::fromStdString(s), QStringLiteral("yyyyMMddHHmmss"));
  if (!t.isValid()) return {};
  return QLocale().toString(t, QLocale::ShortFormat);
}
