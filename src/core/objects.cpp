// SPDX-License-Identifier: GPL-3.0-only
// The page's objects as a whole: their draw order, copying and pasting them (also between
// documents), and a page's drawing as shapes (for stamps and signatures made from a PDF).
#include "core/content.hpp"
#include "core/document.hpp"
#include "core/engine.hpp"

#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace pdfz {

namespace detail {
PageFrame frameOfPage(void* page);
void* pageXObject(void* dest, void* src, int index, void* loaded);
bool formToPage(void* loaded, void* formObject, FS_MATRIX* out);
}  // namespace detail

namespace {

struct Writer : FPDF_FILEWRITE {
  std::vector<std::uint8_t>* out;
  explicit Writer(std::vector<std::uint8_t>* o) : FPDF_FILEWRITE{}, out(o) {
    version = 1;
    WriteBlock = &Writer::write;
  }
  static int write(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
    auto* w = static_cast<Writer*>(self);
    const auto* p = static_cast<const std::uint8_t*>(data);
    w->out->insert(w->out->end(), p, p + size);
    return 1;
  }
};

struct Box {
  double l = 0, b = 0, r = 0, t = 0;
  bool valid = false;
  void add(FPDF_PAGEOBJECT o) {
    float ol = 0, ob = 0, orr = 0, ot = 0;
    if (!FPDFPageObj_GetBounds(o, &ol, &ob, &orr, &ot)) return;
    if (!valid) {
      l = ol, b = ob, r = orr, t = ot;
      valid = true;
      return;
    }
    l = std::min(l, double(ol));
    b = std::min(b, double(ob));
    r = std::max(r, double(orr));
    t = std::max(t, double(ot));
  }
  bool overlaps(const Box& o) const { return valid && o.valid && l < o.r && o.l < r && b < o.t && o.b < t; }
};

// The affine map taking user space points p to q(p), from three points.
FS_MATRIX matrixOf(PointF o, PointF x, PointF y) {
  return FS_MATRIX{float(x.x - o.x), float(x.y - o.y), float(y.x - o.x), float(y.y - o.y), float(o.x), float(o.y)};
}

FS_MATRIX multiply(const FS_MATRIX& m, const FS_MATRIX& n) {  // m, then n
  return FS_MATRIX{m.a * n.a + m.b * n.c, m.a * n.b + m.b * n.d, m.c * n.a + m.d * n.c,
                   m.c * n.b + m.d * n.d, m.e * n.a + m.f * n.c + n.e, m.e * n.b + m.f * n.d + n.f};
}

bool isIdentity(const FS_MATRIX& m) { return m.a == 1 && m.b == 0 && m.c == 0 && m.d == 1 && m.e == 0 && m.f == 0; }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Draw order

std::vector<int> Document::arrangeContent(int pageIndex, const std::vector<int>& objects, Arrange how) {
  if (pageIndex < 0 || pageIndex >= pageCount() || objects.empty()) return {};
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(pageForContentEdit(pageIndex));
  if (!page) return {};
  const int n = FPDFPage_CountObjects(page);
  std::set<int> sel;
  for (int i : objects)
    if (i >= 0 && i < n) sel.insert(i);
  if (sel.empty()) return {};
  std::vector<FPDF_PAGEOBJECT> all(std::size_t(n), nullptr);
  for (int i = 0; i < n; ++i) all[std::size_t(i)] = FPDFPage_GetObject(page, i);
  Box selBox;
  for (int i : sel) selBox.add(all[std::size_t(i)]);

  // The new order, as old indices.
  std::vector<int> order, selected(sel.begin(), sel.end()), others;
  for (int i = 0; i < n; ++i)
    if (!sel.count(i)) others.push_back(i);
  auto overlapping = [&](int i) {
    Box b;
    b.add(all[std::size_t(i)]);
    return b.overlaps(selBox);
  };
  switch (how) {
    case Arrange::ToFront:
      order = others;
      order.insert(order.end(), selected.begin(), selected.end());
      break;
    case Arrange::ToBack:
      order = selected;
      order.insert(order.end(), others.begin(), others.end());
      break;
    case Arrange::Forward: {
      // Just over the first thing above it that overlaps it (or the next thing, if none does).
      const int top = *sel.rbegin();
      int past = -1;
      for (int i = top + 1; i < n && past < 0; ++i)
        if (!sel.count(i) && overlapping(i)) past = i;
      if (past < 0)
        for (int i = top + 1; i < n && past < 0; ++i)
          if (!sel.count(i)) past = i;
      if (past < 0) return selected;  // on top already
      for (int i : others)
        if (i <= past) order.push_back(i);
      order.insert(order.end(), selected.begin(), selected.end());
      for (int i : others)
        if (i > past) order.push_back(i);
      break;
    }
    case Arrange::Backward: {
      const int bottom = *sel.begin();
      int under = -1;
      for (int i = bottom - 1; i >= 0 && under < 0; --i)
        if (!sel.count(i) && overlapping(i)) under = i;
      if (under < 0)
        for (int i = bottom - 1; i >= 0 && under < 0; --i)
          if (!sel.count(i)) under = i;
      if (under < 0) return selected;  // at the bottom already
      for (int i : others)
        if (i < under) order.push_back(i);
      order.insert(order.end(), selected.begin(), selected.end());
      for (int i : others)
        if (i >= under) order.push_back(i);
      break;
    }
  }
  // All taken out and put back in the new order. (They're in one content stream, which
  // pageForContentEdit saw to, so the stream is written in this order.)
  for (int i = n - 1; i >= 0; --i) FPDFPage_RemoveObject(page, all[std::size_t(i)]);
  std::vector<int> out;
  for (std::size_t k = 0; k < order.size(); ++k) {
    FPDFPage_InsertObject(page, all[std::size_t(order[k])]);
    if (sel.count(order[k])) out.push_back(int(k));
  }
  if (!FPDFPage_GenerateContent(page)) return {};
  markEdited();
  return out;
}

// ---------------------------------------------------------------------------------------------
// Copy and paste

Bytes Document::copyContent(int pageIndex, const std::vector<int>& objects) const {
  if (pageIndex < 0 || pageIndex >= pageCount() || objects.empty()) return nullptr;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return nullptr;
  const int n = FPDFPage_CountObjects(page);
  std::set<int> sel;
  for (int i : objects)
    if (i >= 0 && i < n) sel.insert(i);
  if (sel.empty()) return nullptr;
  FPDF_DOCUMENT src = static_cast<FPDF_DOCUMENT>(doc_);

  // A new document, its page the same box and turn as this one, holding copies of the objects:
  // the page's content read whole (as a form, so content split over several streams is read
  // right), and the others thrown away.
  FPDF_DOCUMENT out = FPDF_CreateNewDocument();
  if (!out) return nullptr;
  const PageFrame f = detail::frameOfPage(page);
  Bytes result;
  FPDF_PAGE np = FPDFPage_New(out, 0, f.uprightWidth(), f.uprightHeight());
  auto xobj = np ? static_cast<FPDF_XOBJECT>(detail::pageXObject(out, src, pageIndex, page)) : nullptr;
  FPDF_PAGEOBJECT form = xobj ? FPDF_NewFormObjectFromXObject(xobj) : nullptr;
  if (xobj) FPDF_CloseXObject(xobj);
  FS_MATRIX m{};
  if (form && FPDFFormObj_CountObjects(form) == n && detail::formToPage(page, form, &m)) {
    FPDFPage_SetMediaBox(np, float(f.x0), float(f.y0), float(f.x1), float(f.y1));
    FPDFPage_SetRotation(np, f.rotation / 90);
    for (int i = 0; FPDFFormObj_CountObjects(form) > 0; ++i) {
      FPDF_PAGEOBJECT child = FPDFFormObj_GetObject(form, 0);
      if (!child || !FPDFFormObj_RemoveObject(form, child)) break;
      if (!sel.count(i)) {
        FPDFPageObj_Destroy(child);
        continue;
      }
      if (!isIdentity(m)) {
        FPDFPageObj_TransformF(child, &m);
        FPDFPageObj_TransformClipPath(child, m.a, m.b, m.c, m.d, m.e, m.f);
      }
      FPDFPageObj_SetIsActive(child, true);
      FPDFPage_InsertObject(np, child);
    }
    if (FPDFPage_CountObjects(np) > 0 && FPDFPage_GenerateContent(np)) {
      auto bytes = std::make_shared<std::vector<std::uint8_t>>();
      Writer w(bytes.get());
      if (FPDF_SaveAsCopy(out, &w, FPDF_NO_INCREMENTAL)) result = bytes;
    }
  }
  if (form) FPDFPageObj_Destroy(form);
  if (np) FPDF_ClosePage(np);
  FPDF_CloseDocument(out);
  return result;
}

std::vector<int> Document::pasteContent(int pageIndex, const Bytes& copied) {
  if (pageIndex < 0 || pageIndex >= pageCount() || !copied || copied->empty()) return {};
  std::lock_guard lock(engineMutex());
  // Kept alive while the copied document is open.
  const Bytes keep = copied;
  FPDF_DOCUMENT src = FPDF_LoadMemDocument64(keep->data(), keep->size(), nullptr);
  if (!src) return {};
  std::vector<int> out;
  FPDF_PAGE sp = FPDF_LoadPage(src, 0);
  auto page = sp ? static_cast<FPDF_PAGE>(pageForContentEdit(pageIndex)) : nullptr;
  if (sp && page) {
    const PageFrame from = detail::frameOfPage(sp), to = detail::frameOfPage(page);
    FPDF_DOCUMENT dst = static_cast<FPDF_DOCUMENT>(doc_);
    auto xobj = static_cast<FPDF_XOBJECT>(detail::pageXObject(dst, src, 0, sp));
    FPDF_PAGEOBJECT form = xobj ? FPDF_NewFormObjectFromXObject(xobj) : nullptr;
    if (xobj) FPDF_CloseXObject(xobj);
    FS_MATRIX fm{1, 0, 0, 1, 0, 0};
    if (form && detail::formToPage(sp, form, &fm)) {
      // Where they were on their page as shown (from its top-left) is where they go on this one.
      auto map = [&](PointF p) { return to.toUser(from.toDisplay(p)); };
      FS_MATRIX place = matrixOf(map({0, 0}), map({1, 0}), map({0, 1}));
      FS_MATRIX m = multiply(fm, place);
      std::vector<FPDF_PAGEOBJECT> children;
      while (FPDFFormObj_CountObjects(form) > 0) {
        FPDF_PAGEOBJECT child = FPDFFormObj_GetObject(form, 0);
        if (!child || !FPDFFormObj_RemoveObject(form, child)) break;
        FPDFPageObj_TransformF(child, &m);
        FPDFPageObj_TransformClipPath(child, m.a, m.b, m.c, m.d, m.e, m.f);
        children.push_back(child);
      }
      // Off this page (a smaller one): to its middle instead.
      Box box;
      for (FPDF_PAGEOBJECT c : children) box.add(c);
      Box pageBox{to.x0, to.y0, to.x1, to.y1, true};
      if (box.valid && !box.overlaps(pageBox)) {
        const double dx = (to.x0 + to.x1) / 2 - (box.l + box.r) / 2, dy = (to.y0 + to.y1) / 2 - (box.b + box.t) / 2;
        for (FPDF_PAGEOBJECT c : children) {
          FPDFPageObj_Transform(c, 1, 0, 0, 1, dx, dy);
          FPDFPageObj_TransformClipPath(c, 1, 0, 0, 1, dx, dy);
        }
      }
      const int first = FPDFPage_CountObjects(page);
      for (FPDF_PAGEOBJECT c : children) FPDFPage_InsertObject(page, c);
      if (!children.empty() && FPDFPage_GenerateContent(page)) {
        for (int i = 0; i < int(children.size()); ++i) out.push_back(first + i);
        markEdited();
      }
    }
    if (form) FPDFPageObj_Destroy(form);
  }
  if (sp) FPDF_ClosePage(sp);
  FPDF_CloseDocument(src);
  return out;
}

// ---------------------------------------------------------------------------------------------
// Shapes

namespace {

struct ShapeWalker {
  PageFrame frame;
  FPDF_TEXTPAGE textPage = nullptr;
  std::map<FPDF_PAGEOBJECT, std::vector<int>> charsOf;  // a text object's characters
  std::vector<PageShape> out;
  int pictures = 0;

  PointF toDisplay(const FS_MATRIX& m, double x, double y) const {
    return frame.toDisplay(PointF{m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f});
  }

  // A path's segments, mapped by `m` (user space) into the display frame.
  template <typename Count, typename Get>
  bool addSegments(std::vector<StampDrawing::Op>& ops, const FS_MATRIX& m, Count count, Get get) const {
    const int n = count();
    std::vector<PointF> pending;  // Bézier points waiting for the third
    for (int i = 0; i < n; ++i) {
      FPDF_PATHSEGMENT seg = get(i);
      float x = 0, y = 0;
      if (!seg || !FPDFPathSegment_GetPoint(seg, &x, &y)) continue;
      const PointF p = toDisplay(m, x, y);
      StampDrawing::Op op;
      switch (FPDFPathSegment_GetType(seg)) {
        case FPDF_SEGMENT_MOVETO:
          pending.clear();
          op.kind = StampDrawing::Op::MoveTo;
          op.p[0] = p;
          ops.push_back(op);
          break;
        case FPDF_SEGMENT_LINETO:
          pending.clear();
          op.kind = StampDrawing::Op::LineTo;
          op.p[0] = p;
          ops.push_back(op);
          break;
        case FPDF_SEGMENT_BEZIERTO:
          pending.push_back(p);
          if (pending.size() == 3) {
            op.kind = StampDrawing::Op::CubicTo;
            op.p[0] = pending[0];
            op.p[1] = pending[1];
            op.p[2] = pending[2];
            ops.push_back(op);
            pending.clear();
          }
          break;
        default: continue;
      }
      if (FPDFPathSegment_GetClose(seg)) {
        StampDrawing::Op close;
        close.kind = StampDrawing::Op::Close;
        ops.push_back(close);
      }
    }
    return !ops.empty() && ops.front().kind == StampDrawing::Op::MoveTo;
  }

  static Color colorOf(unsigned r, unsigned g, unsigned b) { return Color{std::uint8_t(r), std::uint8_t(g), std::uint8_t(b)}; }

  void path(FPDF_PAGEOBJECT obj, const FS_MATRIX& m) {
    int fillMode = 0;
    FPDF_BOOL stroke = false;
    if (!FPDFPath_GetDrawMode(obj, &fillMode, &stroke)) return;
    PageShape s;
    unsigned r = 0, g = 0, b = 0, a = 255;
    if (fillMode != FPDF_FILLMODE_NONE && FPDFPageObj_GetFillColor(obj, &r, &g, &b, &a) && a > 0) {
      s.fill = true;
      s.evenOdd = fillMode == FPDF_FILLMODE_ALTERNATE;
      s.fillColor = colorOf(r, g, b);
    }
    if (stroke && FPDFPageObj_GetStrokeColor(obj, &r, &g, &b, &a) && a > 0) {
      float w = 1;
      FPDFPageObj_GetStrokeWidth(obj, &w);
      s.stroke = true;
      s.strokeColor = colorOf(r, g, b);
      // The width scales with the drawing (an average for uneven scaling); a zero width is
      // the thinnest line the device can draw: a quarter point here.
      const double k = std::sqrt(std::abs(m.a * m.d - m.b * m.c));
      s.strokeWidth = std::max(0.25, double(w) * (k > 0 ? k : 1));
      s.lineCap = std::max(0, FPDFPageObj_GetLineCap(obj));
      s.lineJoin = std::max(0, FPDFPageObj_GetLineJoin(obj));
    }
    if (!s.fill && !s.stroke) return;
    if (!addSegments(s.ops, m, [&] { return FPDFPath_CountSegments(obj); }, [&](int i) { return FPDFPath_GetPathSegment(obj, i); }))
      return;
    out.push_back(std::move(s));
  }

  // Each letter's outline, from the font, where the text page put the letter.
  void text(FPDF_PAGEOBJECT obj) {
    if (FPDFTextObj_GetTextRenderMode(obj) == FPDF_TEXTRENDERMODE_INVISIBLE) return;
    auto it = charsOf.find(obj);
    FPDF_FONT font = FPDFTextObj_GetFont(obj);
    if (it == charsOf.end() || !font) return;
    unsigned r = 0, g = 0, b = 0, a = 255;
    if (!FPDFPageObj_GetFillColor(obj, &r, &g, &b, &a) || a == 0) return;
    float size = 0;
    FPDFTextObj_GetFontSize(obj, &size);
    if (size <= 0) size = 1;
    PageShape s;
    s.fill = true;
    s.fillColor = colorOf(r, g, b);
    for (int c : it->second) {
      const unsigned code = FPDFText_GetUnicode(textPage, c);
      if (code == 0 || code == ' ' || code == '\r' || code == '\n') continue;
      double ox = 0, oy = 0;
      FS_MATRIX cm{};
      if (!FPDFText_GetCharOrigin(textPage, c, &ox, &oy) || !FPDFText_GetMatrix(textPage, c, &cm)) continue;
      FPDF_GLYPHPATH glyph = FPDFFont_GetGlyphPath(font, code, size);
      if (!glyph) continue;
      // The glyph is in text space at the font size: the character's matrix (without its own
      // position) turns and scales it, and it sits at the character's origin.
      const FS_MATRIX gm{cm.a, cm.b, cm.c, cm.d, float(ox), float(oy)};
      addSegments(s.ops, gm, [&] { return FPDFGlyphPath_CountGlyphSegments(glyph); },
                  [&](int i) { return FPDFGlyphPath_GetGlyphPathSegment(glyph, i); });
    }
    if (!s.ops.empty() && s.ops.front().kind == StampDrawing::Op::MoveTo) out.push_back(std::move(s));
  }

  void walk(FPDF_PAGEOBJECT obj, const FS_MATRIX& outer) {
    FPDF_BOOL active = true;
    if (FPDFPageObj_GetIsActive(obj, &active) && !active) return;
    switch (FPDFPageObj_GetType(obj)) {
      case FPDF_PAGEOBJ_PATH: {
        FS_MATRIX m{1, 0, 0, 1, 0, 0};
        FPDFPageObj_GetMatrix(obj, &m);
        path(obj, multiply(m, outer));
        break;
      }
      case FPDF_PAGEOBJ_TEXT: text(obj); break;
      case FPDF_PAGEOBJ_IMAGE:
      case FPDF_PAGEOBJ_SHADING: ++pictures; break;
      case FPDF_PAGEOBJ_FORM: {
        FS_MATRIX m{1, 0, 0, 1, 0, 0};
        FPDFPageObj_GetMatrix(obj, &m);
        const FS_MATRIX inner = multiply(m, outer);
        const int n = FPDFFormObj_CountObjects(obj);
        for (int i = 0; i < n; ++i)
          if (FPDF_PAGEOBJECT child = FPDFFormObj_GetObject(obj, unsigned(i))) walk(child, inner);
        break;
      }
      default: break;
    }
  }
};

}  // namespace

std::vector<PageShape> Document::pageShapes(int pageIndex, int* pictures) const {
  if (pictures) *pictures = 0;
  if (pageIndex < 0 || pageIndex >= pageCount()) return {};
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return {};
  ShapeWalker w;
  w.frame = detail::frameOfPage(page);
  w.textPage = FPDFText_LoadPage(page);
  if (w.textPage) {
    const int count = FPDFText_CountChars(w.textPage);
    for (int c = 0; c < count; ++c)
      if (FPDF_PAGEOBJECT o = FPDFText_GetTextObject(w.textPage, c)) w.charsOf[o].push_back(c);
  }
  const FS_MATRIX identity{1, 0, 0, 1, 0, 0};
  const int n = FPDFPage_CountObjects(page);
  for (int i = 0; i < n; ++i)
    if (FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i)) w.walk(obj, identity);
  if (w.textPage) FPDFText_ClosePage(w.textPage);
  if (pictures) *pictures = w.pictures;
  return std::move(w.out);
}

}  // namespace pdfz
