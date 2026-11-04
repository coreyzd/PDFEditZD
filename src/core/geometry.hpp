// SPDX-License-Identifier: GPL-3.0-only
// Points, rectangles and the mapping between a page's PDF coordinates and how it's shown.
#pragma once

#include <algorithm>

namespace pdfz {

struct PointF {
  double x = 0;
  double y = 0;
  friend bool operator==(const PointF&, const PointF&) = default;
};

struct RectF {
  double left = 0;
  double top = 0;
  double right = 0;
  double bottom = 0;  // y grows downwards: top < bottom

  double width() const { return right - left; }
  double height() const { return bottom - top; }
  bool empty() const { return right <= left || bottom <= top; }
  bool contains(double x, double y) const { return x >= left && x <= right && y >= top && y <= bottom; }
  RectF united(const RectF& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    return {std::min(left, o.left), std::min(top, o.top), std::max(right, o.right), std::max(bottom, o.bottom)};
  }
  RectF translated(double dx, double dy) const { return {left + dx, top + dy, right + dx, bottom + dy}; }
  RectF normalized() const {
    return {std::min(left, right), std::min(top, bottom), std::max(left, right), std::max(top, bottom)};
  }
  friend bool operator==(const RectF&, const RectF&) = default;
};

// How a page's PDF coordinates (user space: points, origin bottom-left, y up, before /Rotate)
// map to the display frame (points, origin top-left of the page as shown, y down, /Rotate
// applied): the frame Document::pageSize() and render() use.
struct PageFrame {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the page's bounding box (crop box within media box)
  int rotation = 0;                        // 0, 90, 180 or 270

  double uprightWidth() const { return x1 - x0; }
  double uprightHeight() const { return y1 - y0; }

  PointF toDisplay(PointF user) const {
    const double u = user.x - x0;  // upright frame: unrotated, top-left origin
    const double v = y1 - user.y;
    const double w = uprightWidth(), h = uprightHeight();
    switch (rotation) {
      case 90: return {h - v, u};
      case 180: return {w - u, h - v};
      case 270: return {v, w - u};
      default: return {u, v};
    }
  }
  PointF toUser(PointF d) const {
    const double w = uprightWidth(), h = uprightHeight();
    double u, v;
    switch (rotation) {
      case 90: u = d.y; v = h - d.x; break;
      case 180: u = w - d.x; v = h - d.y; break;
      case 270: u = w - d.y; v = d.x; break;
      default: u = d.x; v = d.y; break;
    }
    return {u + x0, y1 - v};
  }
  // Rectangles stay axis-aligned under quarter turns. User rects use top > bottom (PDF style)
  // in the RectF fields as given: left, top = max y, right, bottom = min y.
  RectF toDisplay(const RectF& userRect) const {
    const PointF a = toDisplay(PointF{userRect.left, userRect.top});
    const PointF b = toDisplay(PointF{userRect.right, userRect.bottom});
    return RectF{a.x, a.y, b.x, b.y}.normalized();
  }
  // The user-space rectangle as {left, top (max y), right, bottom (min y)}.
  RectF toUser(const RectF& displayRect) const {
    const PointF a = toUser(PointF{displayRect.left, displayRect.top});
    const PointF b = toUser(PointF{displayRect.right, displayRect.bottom});
    return {std::min(a.x, b.x), std::max(a.y, b.y), std::max(a.x, b.x), std::min(a.y, b.y)};
  }
};

}  // namespace pdfz
