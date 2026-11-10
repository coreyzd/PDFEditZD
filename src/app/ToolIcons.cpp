// SPDX-License-Identifier: GPL-3.0-only
#include "ToolIcons.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>

#include <QImage>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace {

QPixmap draw(MarkupTool tool, const QColor& ink, qreal dpr) {
  const int size = 24;
  QPixmap pm(int(size * dpr), int(size * dpr));
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  QPen pen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  QFont f = p.font();
  f.setPixelSize(13);
  f.setBold(true);
  p.setFont(f);
  const QColor yellow(0xFF, 0xD8, 0x2A), green(0x1E, 0xA0, 0x4A), red(0xE0, 0x30, 0x30);
  switch (tool) {
    case MarkupTool::Highlight:
      p.fillRect(QRectF(3, 6, 18, 12), yellow);
      p.drawText(QRectF(3, 4, 18, 16), Qt::AlignCenter, QStringLiteral("T"));
      break;
    case MarkupTool::Underline:
      p.drawText(QRectF(3, 1, 18, 16), Qt::AlignCenter, QStringLiteral("U"));
      p.setPen(QPen(green, 2.2, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(QPointF(5, 20), QPointF(19, 20));
      break;
    case MarkupTool::StrikeOut:
      p.drawText(QRectF(3, 3, 18, 16), Qt::AlignCenter, QStringLiteral("S"));
      p.setPen(QPen(red, 2.2, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(QPointF(4, 12), QPointF(20, 12));
      break;
    case MarkupTool::Note: {
      QPainterPath path;
      path.moveTo(4, 4);
      path.lineTo(20, 4);
      path.lineTo(20, 15);
      path.lineTo(15, 20);
      path.lineTo(4, 20);
      path.closeSubpath();
      p.setBrush(yellow);
      p.drawPath(path);
      p.setBrush(Qt::NoBrush);
      p.drawLine(QPointF(7.5, 9), QPointF(16.5, 9));
      p.drawLine(QPointF(7.5, 13), QPointF(13.5, 13));
      break;
    }
    case MarkupTool::TextBox:
      p.setPen(QPen(ink, 1.2, Qt::DashLine));
      p.drawRect(QRectF(3, 4, 18, 16));
      p.setPen(pen);
      p.drawText(QRectF(3, 4, 18, 16), Qt::AlignCenter, QStringLiteral("T"));
      break;
    case MarkupTool::Pen: {
      QPainterPath s;
      s.moveTo(3, 17);
      s.cubicTo(7, 6, 10, 22, 14, 12);
      s.cubicTo(16, 7, 19, 9, 21, 6);
      p.setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p.drawPath(s);
      break;
    }
    case MarkupTool::Rectangle: p.drawRect(QRectF(4, 6, 16, 12)); break;
    case MarkupTool::Ellipse: p.drawEllipse(QRectF(3, 6, 18, 12)); break;
    case MarkupTool::Line: p.drawLine(QPointF(4, 19), QPointF(20, 5)); break;
    case MarkupTool::Arrow:
      p.drawLine(QPointF(4, 19), QPointF(20, 5));
      p.drawLine(QPointF(20, 5), QPointF(13, 6.5));
      p.drawLine(QPointF(20, 5), QPointF(18.5, 12));
      break;
    case MarkupTool::FillText: {  // "Ab" and a text cursor
      QFont tiny = f;
      tiny.setPixelSize(12);
      p.setFont(tiny);
      p.drawText(QRectF(1, 3, 17, 18), Qt::AlignCenter, QStringLiteral("Ab"));
      p.setPen(QPen(ink, 1.3));
      p.drawLine(QPointF(20, 5), QPointF(20, 19));
      break;
    }
    case MarkupTool::Check:
      p.setPen(QPen(ink, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p.drawPolyline(QPolygonF({QPointF(5, 12.5), QPointF(10, 18), QPointF(19.5, 6)}));
      break;
    case MarkupTool::Cross:
      p.setPen(QPen(ink, 2.4, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(QPointF(6, 6), QPointF(18, 18));
      p.drawLine(QPointF(18, 6), QPointF(6, 18));
      break;
    case MarkupTool::Dot:
      p.setPen(Qt::NoPen);
      p.setBrush(ink);
      p.drawEllipse(QPointF(12, 12), 4.5, 4.5);
      break;
    case MarkupTool::Date: {  // a calendar page
      p.drawRoundedRect(QRectF(4, 5, 16, 15), 2, 2);
      p.fillRect(QRectF(4, 5, 16, 4), ink);
      QFont tiny = f;
      tiny.setPixelSize(8);
      p.setFont(tiny);
      p.drawText(QRectF(4, 9, 16, 11), Qt::AlignCenter, QStringLiteral("31"));
      break;
    }
  }
  return pm;
}

}  // namespace

QIcon toolIcon(MarkupTool tool, const QPalette& palette) {
  QIcon icon;
  for (qreal dpr : {1.0, 2.0}) {
    icon.addPixmap(draw(tool, palette.color(QPalette::Active, QPalette::ButtonText), dpr), QIcon::Normal);
    icon.addPixmap(draw(tool, palette.color(QPalette::Disabled, QPalette::ButtonText), dpr), QIcon::Disabled);
  }
  return icon;
}

namespace {

// Draws an icon at 1x and 2x, normal and disabled.
template <typename Fn>
QIcon drawnIcon(const QPalette& palette, int size, Fn&& paint) {
  QIcon icon;
  for (qreal dpr : {1.0, 2.0})
    for (auto [mode, group] : {std::pair{QIcon::Normal, QPalette::Active}, std::pair{QIcon::Disabled, QPalette::Disabled}}) {
      QPixmap pm(int(size * dpr), int(size * dpr));
      pm.setDevicePixelRatio(dpr);
      pm.fill(Qt::transparent);
      QPainter p(&pm);
      p.setRenderHint(QPainter::Antialiasing);
      paint(p, palette.color(group, QPalette::ButtonText), mode == QIcon::Disabled);
      p.end();
      icon.addPixmap(pm, mode);
    }
  return icon;
}

void drawNoColor(QPainter& p, const QRectF& r) {
  p.setPen(QPen(QColor(0, 0, 0, 110), 1));
  p.setBrush(Qt::white);
  p.drawRoundedRect(r, 3, 3);
  p.setPen(QPen(QColor(0xE0, 0x1B, 0x24), 1.6));
  p.drawLine(r.bottomLeft() + QPointF(2, -2), r.topRight() + QPointF(-2, 2));
}

}  // namespace

QIcon selectToolIcon(const QPalette& palette) {
  return drawnIcon(palette, 24, [](QPainter& p, const QColor& ink, bool) {
    QPainterPath arrow;  // the usual pointer, tip at the top left
    arrow.moveTo(7, 3);
    arrow.lineTo(7, 19);
    arrow.lineTo(10.8, 15.4);
    arrow.lineTo(13.6, 21.2);
    arrow.lineTo(16.2, 20);
    arrow.lineTo(13.4, 14.4);
    arrow.lineTo(18.6, 14.2);
    arrow.closeSubpath();
    const bool dark = ink.lightness() > 128;  // a dark theme: a light pointer
    p.setPen(QPen(dark ? QColor(0x20, 0x20, 0x20) : ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(dark ? ink : QColor(Qt::white));
    p.drawPath(arrow);
  });
}

QIcon handToolIcon(const QPalette& palette) {
  return drawnIcon(palette, 24, [](QPainter& p, const QColor& ink, bool) {
    // An open hand: four fingers, a thumb and the palm, drawn as one outline.
    QPainterPath hand;
    hand.setFillRule(Qt::WindingFill);
    const double fingers[4][3] = {{6.6, 6.5, 2.6}, {9.6, 3.6, 2.6}, {12.6, 3.2, 2.6}, {15.6, 5.2, 2.6}};
    for (const auto& f : fingers) hand.addRoundedRect(QRectF(f[0], f[1], f[2], 11), 1.3, 1.3);
    QPainterPath palm;
    palm.moveTo(6.6, 11);
    palm.lineTo(18.2, 11);
    palm.lineTo(18.2, 15);
    palm.cubicTo(18.2, 19, 16, 21.4, 12.6, 21.4);
    palm.lineTo(11.4, 21.4);
    palm.cubicTo(9.4, 21.4, 8.4, 20.6, 7.2, 19);
    palm.lineTo(3.6, 14);
    palm.cubicTo(2.8, 12.8, 4.4, 11.4, 5.6, 12.6);
    palm.lineTo(6.6, 13.6);
    palm.closeSubpath();
    hand = hand.united(palm).simplified();
    p.setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(ink.lightness() > 128 ? QColor(0x30, 0x30, 0x30) : QColor(Qt::white));
    p.drawPath(hand);
  });
}

QIcon signIcon(const QPalette& palette) {
  return drawnIcon(palette, 24, [](QPainter& p, const QColor& ink, bool) {
    // A pen nib over a scribbled signature.
    QPainterPath scribble;
    scribble.moveTo(2, 19);
    scribble.cubicTo(5, 12, 7, 22, 9, 17);
    scribble.cubicTo(10.5, 13.5, 12, 20, 14, 18);
    p.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(scribble);
    QPainterPath nib;
    nib.moveTo(13, 14);
    nib.lineTo(20, 3);
    nib.lineTo(23, 5);
    nib.lineTo(16, 16);
    nib.lineTo(12.5, 17.5);
    nib.closeSubpath();
    p.setBrush(ink.lightness() > 128 ? QColor(0x30, 0x30, 0x30) : QColor(Qt::white));
    p.drawPath(nib);
  });
}

QIcon stampIcon(const QPalette& palette) {
  return drawnIcon(palette, 24, [](QPainter& p, const QColor& ink, bool) {
    // A rubber stamp: handle, neck, base, and the line it prints.
    p.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(ink.lightness() > 128 ? QColor(0x30, 0x30, 0x30) : QColor(Qt::white));
    p.drawEllipse(QRectF(8.5, 2.5, 7, 6));
    p.drawRect(QRectF(10.5, 8.5, 3, 5));
    p.drawRoundedRect(QRectF(4.5, 13.5, 15, 4), 1.2, 1.2);
    p.setPen(QPen(QColor(0xC0, 0x1C, 0x28), 2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(4, 21), QPointF(20, 21));
  });
}

QIcon editPdfIcon(const QPalette& palette) {
  return drawnIcon(palette, 24, [](QPainter& p, const QColor& ink, bool) {
    // A page with lines of text, and a pencil across its corner.
    const QColor paper = ink.lightness() > 128 ? QColor(0x30, 0x30, 0x30) : QColor(Qt::white);
    p.setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(paper);
    QPainterPath page;
    page.moveTo(4.5, 2.5);
    page.lineTo(14, 2.5);
    page.lineTo(18.5, 7);
    page.lineTo(18.5, 21.5);
    page.lineTo(4.5, 21.5);
    page.closeSubpath();
    p.drawPath(page);
    p.drawLine(QPointF(14, 2.5), QPointF(14, 7));
    p.drawLine(QPointF(14, 7), QPointF(18.5, 7));
    for (double y : {10.0, 13.0, 16.0}) p.drawLine(QPointF(7, y), QPointF(y < 15 ? 15.5 : 11.5, y));
    QPainterPath pencil;
    pencil.moveTo(12, 21);
    pencil.lineTo(13, 17.5);
    pencil.lineTo(21, 9.5);
    pencil.lineTo(23.5, 12);
    pencil.lineTo(15.5, 20);
    pencil.closeSubpath();
    p.setBrush(QColor(0xE6, 0xA1, 0x17));
    p.setPen(QPen(ink, 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(pencil);
  });
}

QIcon addTextIcon(const QPalette& palette) {
  return drawnIcon(palette, 24, [](QPainter& p, const QColor& ink, bool) {
    p.setPen(QPen(ink, 1, Qt::DotLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(2.5, 2.5, 19, 19));
    p.setPen(QPen(ink, 2.2, Qt::SolidLine, Qt::FlatCap));
    p.drawLine(QPointF(7, 7), QPointF(17, 7));
    p.drawLine(QPointF(12, 7), QPointF(12, 18));
  });
}

QIcon addImageIcon(const QPalette& palette) {
  return drawnIcon(palette, 24, [](QPainter& p, const QColor& ink, bool) {
    p.setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(2.5, 4.5, 19, 15), 2, 2);
    QPainterPath hills;
    hills.moveTo(3.5, 18.5);
    hills.lineTo(9, 11);
    hills.lineTo(13, 15.5);
    hills.lineTo(15.5, 13);
    hills.lineTo(20.5, 18.5);
    hills.closeSubpath();
    p.setBrush(QColor(0x2E, 0x8B, 0x57));
    p.drawPath(hills);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xE6, 0xA1, 0x17));
    p.drawEllipse(QPointF(16.5, 8.5), 2, 2);
  });
}

QIcon strokeColorIcon(const QColor& color, const QPalette& palette) {
  return drawnIcon(palette, 20, [&](QPainter& p, const QColor& ink, bool disabled) {
    const QColor c = color.isValid() && !disabled ? color : QColor(ink.red(), ink.green(), ink.blue(), 90);
    p.setPen(QPen(QColor(0, 0, 0, 110), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(2.5, 2.5, 15, 15), 3, 3);
    p.drawRoundedRect(QRectF(7.5, 7.5, 5, 5), 1, 1);
    p.setPen(QPen(c, 4));
    p.drawRect(QRectF(5, 5, 10, 10));
  });
}

QIcon fillColorIcon(const QColor& color, const QPalette& palette) {
  return drawnIcon(palette, 20, [&](QPainter& p, const QColor& ink, bool disabled) {
    if (!color.isValid() && !disabled) {
      drawNoColor(p, QRectF(2.5, 2.5, 15, 15));
      return;
    }
    p.setPen(QPen(QColor(0, 0, 0, 110), 1));
    p.setBrush(disabled ? QColor(ink.red(), ink.green(), ink.blue(), 70) : color);
    p.drawRoundedRect(QRectF(2.5, 2.5, 15, 15), 3, 3);
  });
}

QIcon textColorIcon(const QColor& color, const QPalette& palette) {
  return drawnIcon(palette, 20, [&](QPainter& p, const QColor& ink, bool disabled) {
    QFont f = p.font();
    f.setPixelSize(13);
    f.setBold(true);
    p.setFont(f);
    p.setPen(ink);
    p.drawText(QRectF(0, 0, 20, 15), Qt::AlignCenter, QStringLiteral("A"));
    p.fillRect(QRectF(3, 15, 14, 3.5), color.isValid() && !disabled ? color : QColor(ink.red(), ink.green(), ink.blue(), 90));
  });
}

QIcon alignIcon(int align, const QPalette& palette) {
  return drawnIcon(palette, 20, [&](QPainter& p, const QColor& ink, bool) {
    p.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap));
    const double widths[] = {14, 9, 14, 7};
    for (int i = 0; i < 4; ++i) {
      const double w = align == 3 && i < 3 ? 14 : widths[i], y = 4.5 + i * 3.7;  // 3: justified
      const double x = align == 0 || align == 3 ? 3 : align == 1 ? 10 - w / 2 : 17 - w;
      p.drawLine(QPointF(x, y), QPointF(x + w, y));
    }
  });
}

QIcon rotateIcon(bool clockwise, const QPalette& palette) {
  return drawnIcon(palette, 20, [&](QPainter& p, const QColor& ink, bool) {
    // A three-quarter arc with an arrowhead, mirrored for the other way.
    if (!clockwise) {
      p.translate(20, 0);
      p.scale(-1, 1);
    }
    p.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(4, 4, 12, 12), 90 * 16, -270 * 16);
    QPainterPath head;
    head.moveTo(10, 1.5);
    head.lineTo(13.5, 4);
    head.lineTo(10, 6.5);
    p.drawPath(head);
  });
}

QIcon flipIcon(bool horizontal, const QPalette& palette) {
  return drawnIcon(palette, 20, [&](QPainter& p, const QColor& ink, bool) {
    if (!horizontal) {  // the same, turned a quarter
      p.translate(20, 0);
      p.rotate(90);
    }
    p.setPen(QPen(ink, 1.3, Qt::DashLine));
    p.drawLine(QPointF(10, 2), QPointF(10, 18));
    p.setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPolygonF left;
    left << QPointF(7.5, 5) << QPointF(7.5, 15) << QPointF(2, 15);
    p.setBrush(ink);
    p.drawPolygon(left);
    QPolygonF right;
    right << QPointF(12.5, 5) << QPointF(12.5, 15) << QPointF(18, 15);
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(right);
  });
}

QIcon sizeIcon(bool larger, const QPalette& palette) {
  return drawnIcon(palette, 20, [&](QPainter& p, const QColor& ink, bool) {
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(larger ? 16 : 10);
    p.setFont(f);
    p.setPen(ink);
    p.drawText(QRectF(0, larger ? 1 : 7, 14, larger ? 18 : 12), Qt::AlignCenter, QStringLiteral("A"));
    QPolygonF arrow;
    if (larger) arrow << QPointF(14, 9) << QPointF(19.5, 9) << QPointF(16.75, 4.5);
    else arrow << QPointF(14, 11) << QPointF(19.5, 11) << QPointF(16.75, 15.5);
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawPolygon(arrow);
  });
}

QIcon fitWidthIcon(const QPalette& palette) {
  // Thin lines, like the icon theme's Fit Page beside it.
  return drawnIcon(palette, 20, [&](QPainter& p, const QColor& ink, bool) {
    p.setPen(QPen(ink, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(2.5, 4), QPointF(2.5, 16));    // the page's edges
    p.drawLine(QPointF(17.5, 4), QPointF(17.5, 16));
    p.drawLine(QPointF(5.5, 10), QPointF(14.5, 10));  // the arrow, out to both
    QPolygonF left, right;
    left << QPointF(8.5, 7) << QPointF(5.5, 10) << QPointF(8.5, 13);
    right << QPointF(11.5, 7) << QPointF(14.5, 10) << QPointF(11.5, 13);
    p.drawPolyline(left);
    p.drawPolyline(right);
  });
}

QCursor crosshairCursor(qreal dpr) {
  static std::map<int, QCursor> cache;
  const int key = int(std::lround(dpr * 100));
  if (auto it = cache.find(key); it != cache.end()) return it->second;
  // Drawn pixel by pixel, so the lines are sharp: an odd size, the hot spot in the middle. Qt
  // keeps the hot spot in whole logical pixels, so the arm is a length that's whole in both.
  int hot = 11;
  for (int k : {12, 10, 8, 14})
    if (std::abs(k * dpr - std::round(k * dpr)) < 1e-6) {
      hot = k;
      break;
    }
  const int arm = int(std::lround(hot * dpr));
  const int n = 2 * arm + 1;
  const int t = std::max(1, int(std::floor(dpr + 0.01)));  // the line's thickness: whole pixels
  const int edge = t;                                         // the light outline round it
  QImage img(n, n, QImage::Format_ARGB32_Premultiplied);
  img.fill(Qt::transparent);
  const int lo = arm - t / 2;  // the line covers pixels lo to lo + t - 1
  {
    QPainter p(&img);
    const QColor light(255, 255, 255, 230), dark(0, 0, 0);
    p.fillRect(QRect(lo - edge, 0, t + 2 * edge, n), light);
    p.fillRect(QRect(0, lo - edge, n, t + 2 * edge), light);
    p.fillRect(QRect(lo, edge, t, n - 2 * edge), dark);
    p.fillRect(QRect(edge, lo, n - 2 * edge, t), dark);
  }
  QPixmap pm = QPixmap::fromImage(img);
  pm.setDevicePixelRatio(dpr);
  const QCursor c(pm, hot, hot);
  cache.emplace(key, c);
  return c;
}

QIcon colorIcon(const QColor& color) {
  if (!color.isValid()) {
    QIcon icon;
    for (qreal dpr : {1.0, 2.0}) {
      QPixmap pm(int(20 * dpr), int(20 * dpr));
      pm.setDevicePixelRatio(dpr);
      pm.fill(Qt::transparent);
      QPainter p(&pm);
      p.setRenderHint(QPainter::Antialiasing);
      drawNoColor(p, QRectF(2.5, 2.5, 15, 15));
      p.end();
      icon.addPixmap(pm);
    }
    return icon;
  }
  QIcon icon;
  for (qreal dpr : {1.0, 2.0}) {
    QPixmap pm(int(20 * dpr), int(20 * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0, 0, 0, 110), 1));
    p.setBrush(color);
    p.drawRoundedRect(QRectF(2.5, 2.5, 15, 15), 3, 3);
    icon.addPixmap(pm);
  }
  return icon;
}
