// SPDX-License-Identifier: GPL-3.0-only
#include "core/content.hpp"

#include "core/document.hpp"
#include "core/engine.hpp"

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace pdfz {

namespace detail {
std::u16string toUtf16(const std::string& s);
std::string toUtf8(const std::u16string& s);
PageFrame frameOfPage(void* page);
}  // namespace detail

bool standardFontCanShow(const std::string& utf8) {
  // Windows-1252: Latin-1 plus the few extra characters it puts in 0x80-0x9F.
  static const char16_t extra[] = {0x20AC, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                   0x2039, 0x0152, 0x017D, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                   0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x017E, 0x0178};
  for (char16_t c : detail::toUtf16(utf8)) {
    if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) continue;
    if (std::find(std::begin(extra), std::end(extra), c) == std::end(extra)) return false;
  }
  return true;
}

namespace {

// One text object, as the grouping sees it (user space).
struct Piece {
  int index = 0;
  double left = 0, right = 0, bottom = 0, top = 0;
  double x = 0, baseline = 0;  // where it starts, on its baseline
  double size = 12;
  bool straight = false;
  std::u16string text;
  std::string font;
  bool bold = false, italic = false;
  Color color;
};

struct Line {
  std::vector<Piece> pieces;
  double left = 0, right = 0, bottom = 0, top = 0, baseline = 0, size = 0;
};

std::string cleanFontName(std::string name) {
  // "ABCDEF+Arial-BoldMT": the subset tag goes.
  if (name.size() > 7 && name[6] == '+' &&
      std::all_of(name.begin(), name.begin() + 6, [](char c) { return c >= 'A' && c <= 'Z'; }))
    name.erase(0, 7);
  return name;
}

bool contains(const std::string& s, const char* what) {
  std::string lower = s;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return lower.find(what) != std::string::npos;
}

// "Arial-BoldMT" -> "Arial", "TimesNewRomanPS-ItalicMT" -> "Times New Roman", "Helvetica" ...
std::string familyOf(const std::string& font) {
  std::string base = font.substr(0, font.find_first_of("-,"));
  for (const char* suffix : {"PSMT", "PS", "MT"})
    if (base.size() > std::strlen(suffix) && base.compare(base.size() - std::strlen(suffix), std::string::npos, suffix) == 0)
      base.resize(base.size() - std::strlen(suffix));
  // CamelCase to words: "TimesNewRoman" -> "Times New Roman".
  std::string out;
  for (std::size_t i = 0; i < base.size(); ++i) {
    if (i > 0 && std::isupper(static_cast<unsigned char>(base[i])) && std::islower(static_cast<unsigned char>(base[i - 1])))
      out += ' ';
    out += base[i];
  }
  return out;
}

Piece pieceOf(FPDF_PAGEOBJECT obj, int index, FPDF_TEXTPAGE textPage) {
  Piece p;
  p.index = index;
  float l = 0, b = 0, r = 0, t = 0;
  FPDFPageObj_GetBounds(obj, &l, &b, &r, &t);
  p.left = l;
  p.bottom = b;
  p.right = r;
  p.top = t;
  FS_MATRIX m{};
  FPDFPageObj_GetMatrix(obj, &m);
  const double scaleY = std::hypot(m.c, m.d);
  p.straight = std::abs(m.b) < 1e-3 * std::abs(m.a) && std::abs(m.c) < 1e-3 * std::abs(m.d) && m.a > 0 && m.d > 0;
  p.x = m.e;
  p.baseline = m.f;
  float size = 0;
  FPDFTextObj_GetFontSize(obj, &size);  // the Tf size: the matrix may scale it (Tf 1 with a 12x Tm)
  p.size = (size > 0 ? size : 1) * (scaleY > 0 ? scaleY : 1);
  const unsigned long bytes = FPDFTextObj_GetText(obj, textPage, nullptr, 0);
  if (bytes > 2) {
    std::u16string buf(bytes / 2, u'\0');
    FPDFTextObj_GetText(obj, textPage, reinterpret_cast<FPDF_WCHAR*>(buf.data()), bytes);
    buf.resize(bytes / 2 - 1);
    p.text = buf;
  }
  if (FPDF_FONT font = FPDFTextObj_GetFont(obj)) {
    const std::size_t n = FPDFFont_GetBaseFontName(font, nullptr, 0);
    if (n > 1) {
      std::string name(n, '\0');
      FPDFFont_GetBaseFontName(font, name.data(), n);
      name.resize(n - 1);
      p.font = cleanFontName(name);
    }
    const int weight = FPDFFont_GetWeight(font);
    const int flags = FPDFFont_GetFlags(font);
    p.bold = weight >= 600 || contains(p.font, "bold") || contains(p.font, "black") || contains(p.font, "heavy");
    p.italic = (flags > 0 && (flags & 64)) || contains(p.font, "italic") || contains(p.font, "oblique");
  }
  unsigned int R = 0, G = 0, B = 0, A = 255;
  if (FPDFPageObj_GetFillColor(obj, &R, &G, &B, &A)) p.color = {std::uint8_t(R), std::uint8_t(G), std::uint8_t(B)};
  return p;
}

bool isSpace(char16_t c) { return c == u' ' || c == u'\t' || c == 0xA0; }

}  // namespace

std::vector<ContentBlock> Document::contentBlocks(int pageIndex) const {
  std::vector<ContentBlock> out;
  if (pageIndex < 0 || pageIndex >= pageCount()) return out;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return out;
  const PageFrame frame = detail::frameOfPage(page);
  FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
  std::vector<Piece> pieces;
  const int n = FPDFPage_CountObjects(page);
  for (int i = 0; i < n; ++i) {
    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
    FPDF_BOOL active = true;
    if (!obj || (FPDFPageObj_GetIsActive(obj, &active) && !active)) continue;
    const int type = FPDFPageObj_GetType(obj);
    if (type == FPDF_PAGEOBJ_TEXT) {
      Piece p = pieceOf(obj, i, textPage);
      if (std::all_of(p.text.begin(), p.text.end(), isSpace)) continue;  // nothing to see
      pieces.push_back(std::move(p));
    } else if (type == FPDF_PAGEOBJ_IMAGE) {
      float l = 0, b = 0, r = 0, t = 0;
      if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &t)) continue;
      ContentBlock blk;
      blk.kind = ContentBlock::Kind::Image;
      blk.rect = frame.toDisplay(RectF{l, b, r, t});
      blk.objects = {i};
      blk.editable = true;
      out.push_back(std::move(blk));
    }
  }
  if (textPage) FPDFText_ClosePage(textPage);

  // Pieces into lines: the same baseline, and close enough along it (a wide gap is a new
  // block: a label and its value, or another column).
  std::vector<Piece> straight, other;
  for (Piece& p : pieces) (p.straight ? straight : other).push_back(std::move(p));
  std::sort(straight.begin(), straight.end(), [](const Piece& a, const Piece& b) {
    if (std::abs(a.baseline - b.baseline) > 0.3 * std::min(a.size, b.size)) return a.baseline > b.baseline;
    return a.x < b.x;
  });
  std::vector<Line> lines;
  for (Piece& p : straight) {
    Line* into = nullptr;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
      if (std::abs(it->baseline - p.baseline) > 0.3 * std::min(it->size, p.size)) {
        if (it->baseline - p.baseline > 2 * p.size) break;  // lines above: too far up already
        continue;
      }
      const double gap = p.left - it->right;
      if (gap < 1.2 * std::max(it->size, p.size) && gap > -0.5 * p.size && std::abs(it->size - p.size) < 0.35 * p.size) {
        into = &*it;
        break;
      }
    }
    if (!into) {
      lines.push_back({});
      into = &lines.back();
      into->left = p.left;
      into->right = p.right;
      into->bottom = p.bottom;
      into->top = p.top;
      into->baseline = p.baseline;
      into->size = p.size;
    }
    into->left = std::min(into->left, p.left);
    into->right = std::max(into->right, p.right);
    into->bottom = std::min(into->bottom, p.bottom);
    into->top = std::max(into->top, p.top);
    into->pieces.push_back(std::move(p));
  }
  for (Line& l : lines)
    std::sort(l.pieces.begin(), l.pieces.end(), [](const Piece& a, const Piece& b) { return a.x < b.x; });

  // Lines into paragraphs: one under another, starting at the same place, the same size, and
  // about a line apart.
  std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.baseline > b.baseline; });
  std::vector<std::vector<Line*>> paras;
  std::vector<bool> used(lines.size(), false);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (used[i]) continue;
    used[i] = true;
    std::vector<Line*> para{&lines[i]};
    for (std::size_t j = i + 1; j < lines.size(); ++j) {
      if (used[j]) continue;
      const Line& last = *para.back();
      const Line& next = lines[j];
      const double drop = last.baseline - next.baseline;
      if (drop > 2.2 * last.size) break;  // sorted: everything after is further down
      const bool sameSize = std::abs(next.size - last.size) < 0.2 * last.size;
      const bool aligned = std::abs(next.left - para.front()->left) < 1.0 * last.size;
      const bool spacing = drop > 0.8 * last.size &&
                           (para.size() < 2 || std::abs(drop - (para[0]->baseline - para[1]->baseline)) < 0.25 * last.size);
      const bool overlaps = next.left < last.right + last.size && next.right > last.left - last.size;
      if (sameSize && aligned && spacing && overlaps && drop < 1.9 * last.size) {
        used[j] = true;
        para.push_back(&lines[j]);
      }
    }
    paras.push_back(std::move(para));
  }

  auto lineText = [](const Line& l) {
    std::u16string t;
    for (std::size_t k = 0; k < l.pieces.size(); ++k) {
      const Piece& p = l.pieces[k];
      if (k > 0 && !t.empty() && !isSpace(t.back()) && !p.text.empty() && !isSpace(p.text.front())) {
        const double gap = p.left - l.pieces[k - 1].right;
        if (gap > 0.15 * p.size) t += u' ';
      }
      t += p.text;
    }
    while (!t.empty() && isSpace(t.back())) t.pop_back();
    return t;
  };

  for (const auto& para : paras) {
    ContentBlock blk;
    blk.kind = ContentBlock::Kind::Text;
    double l = 1e9, r = -1e9, b = 1e9, t = -1e9, widest = 0;
    for (const Line* line : para) {
      l = std::min(l, line->left);
      r = std::max(r, line->right);
      b = std::min(b, line->bottom);
      t = std::max(t, line->top);
      widest = std::max(widest, line->right - line->left);
      for (const Piece& p : line->pieces) blk.objects.push_back(p.index);
    }
    blk.rect = frame.toDisplay(RectF{l, b, r, t});
    const Piece& first = para.front()->pieces.front();
    blk.fontName = first.font;
    blk.family = familyOf(first.font);
    blk.bold = first.bold;
    blk.italic = first.italic;
    blk.fontSize = first.size;
    blk.color = first.color;
    blk.lineSpacing = para.size() > 1 ? (para.front()->baseline - para.back()->baseline) / double(para.size() - 1) : 0;
    blk.origin = frame.toDisplay(PointF{para.front()->pieces.front().x, para.front()->baseline});
    blk.editable = frame.rotation == 0;
    // The text: a line that fills most of the width runs on into the next (it wrapped); a
    // short one, or one ending a label, ends there.
    std::u16string text;
    for (std::size_t k = 0; k < para.size(); ++k) {
      const std::u16string lt = lineText(*para[k]);
      text += lt;
      if (k + 1 == para.size()) break;
      const double w = para[k]->right - para[k]->left;
      const bool full = w > 0.8 * widest;
      const char16_t last = lt.empty() ? u' ' : lt.back();
      const bool ends = last == u':' || last == u'.' || last == u'!' || last == u'?';
      text += full && !ends ? u' ' : u'\n';
    }
    blk.text = detail::toUtf8(text);
    out.push_back(std::move(blk));
  }
  // Text that's turned or slanted: one block per object, which can be moved or deleted.
  for (const Piece& p : other) {
    ContentBlock blk;
    blk.kind = ContentBlock::Kind::Text;
    blk.rect = frame.toDisplay(RectF{p.left, p.bottom, p.right, p.top});
    blk.objects = {p.index};
    blk.text = detail::toUtf8(p.text);
    blk.fontName = p.font;
    blk.family = familyOf(p.font);
    blk.fontSize = p.size;
    blk.color = p.color;
    blk.editable = false;
    out.push_back(std::move(blk));
  }
  return out;
}

namespace {

// The objects of `indices` that exist, highest index first (so removing them in that order
// keeps the others' indices right).
std::vector<FPDF_PAGEOBJECT> objectsOf(FPDF_PAGE page, std::vector<int> indices, std::vector<int>* sortedOut = nullptr) {
  std::sort(indices.begin(), indices.end(), std::greater<int>());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  std::vector<FPDF_PAGEOBJECT> out;
  const int n = FPDFPage_CountObjects(page);
  for (int i : indices) {
    if (i < 0 || i >= n) return {};
    out.push_back(FPDFPage_GetObject(page, i));
  }
  if (sortedOut) *sortedOut = indices;
  return out;
}

}  // namespace

bool Document::setContentHidden(int pageIndex, const std::vector<int>& objects, bool hidden) {
  if (pageIndex < 0 || pageIndex >= pageCount()) return false;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return false;
  const auto objs = objectsOf(page, objects);
  if (objs.empty()) return false;
  for (FPDF_PAGEOBJECT o : objs) FPDFPageObj_SetIsActive(o, !hidden);
  return true;
}

bool Document::moveContent(int pageIndex, const std::vector<int>& objects, double dx, double dy) {
  if (pageIndex < 0 || pageIndex >= pageCount()) return false;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return false;
  const auto objs = objectsOf(page, objects);
  if (objs.empty()) return false;
  const PageFrame f = detail::frameOfPage(page);
  const PointF o = f.toUser(PointF{0, 0}), d = f.toUser(PointF{dx, dy});
  for (FPDF_PAGEOBJECT obj : objs) {
    FPDFPageObj_SetIsActive(obj, true);
    FPDFPageObj_Transform(obj, 1, 0, 0, 1, d.x - o.x, d.y - o.y);
  }
  if (!FPDFPage_GenerateContent(page)) return false;
  markEdited();
  return true;
}

bool Document::resizeContent(int pageIndex, const std::vector<int>& objects, const RectF& from, const RectF& to) {
  if (pageIndex < 0 || pageIndex >= pageCount() || from.width() <= 0 || from.height() <= 0 || to.width() <= 0 ||
      to.height() <= 0)
    return false;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return false;
  const auto objs = objectsOf(page, objects);
  if (objs.empty()) return false;
  const PageFrame f = detail::frameOfPage(page);
  const RectF a = f.toUser(from.normalized()), b = f.toUser(to.normalized());
  // User space: left, top = max y, right, bottom = min y. Scale about a's corner, then move.
  const double sx = (b.right - b.left) / (a.right - a.left), sy = (b.top - b.bottom) / (a.top - a.bottom);
  for (FPDF_PAGEOBJECT obj : objs) {
    FPDFPageObj_SetIsActive(obj, true);
    FPDFPageObj_Transform(obj, sx, 0, 0, sy, b.left - a.left * sx, b.bottom - a.bottom * sy);
  }
  if (!FPDFPage_GenerateContent(page)) return false;
  markEdited();
  return true;
}

bool Document::deleteContent(int pageIndex, const std::vector<int>& objects) {
  if (pageIndex < 0 || pageIndex >= pageCount()) return false;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return false;
  const auto objs = objectsOf(page, objects);
  if (objs.empty()) return false;
  for (FPDF_PAGEOBJECT obj : objs)
    if (FPDFPage_RemoveObject(page, obj)) FPDFPageObj_Destroy(obj);
  if (!FPDFPage_GenerateContent(page)) return false;
  markEdited();
  return true;
}

bool Document::replaceText(int pageIndex, const std::vector<int>& remove, const TextLines& text) {
  if (pageIndex < 0 || pageIndex >= pageCount() || text.fontSize <= 0) return false;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return false;
  FPDF_DOCUMENT doc = static_cast<FPDF_DOCUMENT>(doc_);
  std::vector<FPDF_PAGEOBJECT> old;
  if (!remove.empty()) {
    old = objectsOf(page, remove);
    if (old.empty()) return false;
  }
  // The font: a standard one (loaded for this), or an embedded one (loaded once per document).
  FPDF_FONT font = nullptr;
  bool closeFont = false;
  if (!text.font.file.empty()) {
    for (auto& [key, f] : fonts_)
      if (key == text.font.key) font = static_cast<FPDF_FONT>(f);
    if (!font) {
      font = FPDFText_LoadFont(doc, text.font.file.data(), static_cast<uint32_t>(text.font.file.size()), FPDF_FONT_TRUETYPE,
                               /*cid=*/true);
      if (font) fonts_.emplace_back(text.font.key, font);
    }
  } else {
    font = FPDFText_LoadStandardFont(doc, text.font.standard.empty() ? "Helvetica" : text.font.standard.c_str());
    closeFont = true;
  }
  if (!font) return false;
  const PageFrame f = detail::frameOfPage(page);
  std::vector<FPDF_PAGEOBJECT> added;
  bool ok = true;
  for (std::size_t i = 0; i < text.lines.size() && ok; ++i) {
    if (text.lines[i].empty()) continue;
    FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(doc, font, float(text.fontSize));
    if (!obj) {
      ok = false;
      break;
    }
    const std::u16string w = detail::toUtf16(text.lines[i]);
    ok = FPDFText_SetText(obj, reinterpret_cast<FPDF_WIDESTRING>(w.c_str()));
    // The line's start on its baseline: down from the first by whole line spacings.
    const PointF at = f.toUser(PointF{text.origin.x, text.origin.y + double(i) * text.lineSpacing});
    FPDFPageObj_Transform(obj, 1, 0, 0, 1, at.x, at.y);
    FPDFPageObj_SetFillColor(obj, text.color.r, text.color.g, text.color.b, 255);
    if (ok) added.push_back(obj);
    else FPDFPageObj_Destroy(obj);
  }
  if (closeFont) FPDFFont_Close(font);
  if (!ok) {
    for (FPDF_PAGEOBJECT obj : added) FPDFPageObj_Destroy(obj);
    return false;
  }
  for (FPDF_PAGEOBJECT obj : old)
    if (FPDFPage_RemoveObject(page, obj)) FPDFPageObj_Destroy(obj);
  for (FPDF_PAGEOBJECT obj : added) FPDFPage_InsertObject(page, obj);
  if (!FPDFPage_GenerateContent(page)) return false;
  markEdited();
  return true;
}

}  // namespace pdfz
