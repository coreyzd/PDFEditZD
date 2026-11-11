// SPDX-License-Identifier: GPL-3.0-only
#include "core/content.hpp"

#include "core/document.hpp"
#include "core/engine.hpp"

#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>

namespace pdfz {

namespace detail {
std::u16string toUtf16(const std::string& s);
std::string toUtf8(const std::u16string& s);
PageFrame frameOfPage(void* page);

// A page's content as a form XObject in `dest`, its objects in the page's own space.
// FPDF_NewXObjectFromPage turns the content upright by the page's /Rotate (it's made for
// putting pages side by side), which would turn every object taken out of it a quarter or half
// round: so the rotation is off while it's made. `loaded` is the page, loaded from `src`.
void* pageXObject(void* dest, void* src, int index, void* loaded) {
  auto page = static_cast<FPDF_PAGE>(loaded);
  const int rotation = page ? FPDFPage_GetRotation(page) : 0;
  if (rotation != 0) FPDFPage_SetRotation(page, 0);
  FPDF_XOBJECT x = FPDF_NewXObjectFromPage(static_cast<FPDF_DOCUMENT>(dest), static_cast<FPDF_DOCUMENT>(src), index);
  if (rotation != 0) FPDFPage_SetRotation(page, rotation);
  return x;
}

// What turns the objects of a form made by pageXObject back into the page's space: the form
// may still move them (a page whose box doesn't start at 0, 0). Found from an object that's
// on both: `child` x the result = the object on the page. False if it can't be told.
bool formToPage(void* loaded, void* formObject, FS_MATRIX* out) {
  auto page = static_cast<FPDF_PAGE>(loaded);
  auto form = static_cast<FPDF_PAGEOBJECT>(formObject);
  *out = FS_MATRIX{1, 0, 0, 1, 0, 0};
  const int n = FPDFPage_CountObjects(page);
  if (FPDFFormObj_CountObjects(form) != n) return false;
  // The same objects in the same order, or nothing can be told from them.
  for (int i = 0; i < n; ++i)
    if (FPDFPageObj_GetType(FPDFPage_GetObject(page, i)) != FPDFPageObj_GetType(FPDFFormObj_GetObject(form, i))) return false;
  auto close = [](const FS_MATRIX& p, const FS_MATRIX& q) {
    const float s = 1e-3f * (1 + std::abs(p.e) + std::abs(p.f));
    return std::abs(p.a - q.a) < 1e-4f && std::abs(p.b - q.b) < 1e-4f && std::abs(p.c - q.c) < 1e-4f &&
           std::abs(p.d - q.d) < 1e-4f && std::abs(p.e - q.e) < s && std::abs(p.f - q.f) < s;
  };
  bool differs = false;
  for (int i = 0; i < n; ++i) {
    FS_MATRIX mp{}, mc{};
    if (!FPDFPageObj_GetMatrix(FPDFPage_GetObject(page, i), &mp) || !FPDFPageObj_GetMatrix(FPDFFormObj_GetObject(form, i), &mc))
      continue;
    if (close(mp, mc)) continue;
    differs = true;
    const double det = double(mc.a) * mc.d - double(mc.b) * mc.c;
    if (std::abs(det) < 1e-9) continue;
    // inverse(mc) x mp, in PDF's row-vector order.
    const double ia = mc.d / det, ib = -mc.b / det, ic = -mc.c / det, id = mc.a / det;
    const double ie = -(mc.e * ia + mc.f * ic), iff = -(mc.e * ib + mc.f * id);
    out->a = float(ia * mp.a + ib * mp.c);
    out->b = float(ia * mp.b + ib * mp.d);
    out->c = float(ic * mp.a + id * mp.c);
    out->d = float(ic * mp.b + id * mp.d);
    out->e = float(ie * mp.a + iff * mp.c + mp.e);
    out->f = float(ie * mp.b + iff * mp.d + mp.f);
    // It must put the others right too (checked on up to a hundred of them).
    const int step = std::max(1, n / 100);
    for (int j = 0; j < n; j += step) {
      FS_MATRIX p{}, q{};
      if (!FPDFPageObj_GetMatrix(FPDFPage_GetObject(page, j), &p) || !FPDFPageObj_GetMatrix(FPDFFormObj_GetObject(form, j), &q))
        continue;
      const FS_MATRIX r{q.a * out->a + q.b * out->c, q.a * out->b + q.b * out->d, q.c * out->a + q.d * out->c,
                        q.c * out->b + q.d * out->d, q.e * out->a + q.f * out->c + out->e, q.e * out->b + q.f * out->d + out->f};
      if (!close(p, r)) return false;
    }
    return true;
  }
  return !differs;
}
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
  std::string family;  // from the embedded font's own name table, when it has one
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

bool contains(const std::string& s, const char* what);

// What an embedded TrueType/OpenType font says it is, in its name and OS/2 tables. Printers
// (Microsoft Print to PDF) name their fonts "CIDFont+F1" and leave out the weight, so the font
// file is the only place that says it's Arial Bold.
struct FontFileNames {
  bool known = false;
  std::string family, postscript;
  bool bold = false, italic = false;
};

FontFileNames namesInFontFile(const std::vector<std::uint8_t>& d) {
  FontFileNames out;
  auto u16 = [&](std::size_t o) { return o + 2 <= d.size() ? unsigned(d[o] << 8 | d[o + 1]) : 0u; };
  auto u32 = [&](std::size_t o) { return o + 4 <= d.size() ? (std::uint32_t(u16(o)) << 16) | u16(o + 2) : 0u; };
  const std::uint32_t tag = u32(0);
  if (tag != 0x00010000 && tag != 0x4F54544F && tag != 0x74727565) return out;
  const unsigned numTables = u16(4);
  std::size_t nameOff = 0, nameLen = 0, os2Off = 0, os2Len = 0;
  for (unsigned t = 0; t < numTables; ++t) {
    const std::size_t rec = 12 + 16 * std::size_t(t);
    if (rec + 16 > d.size()) break;
    const std::uint32_t off = u32(rec + 8), len = u32(rec + 12);
    if (std::memcmp(&d[rec], "name", 4) == 0) nameOff = off, nameLen = len;
    if (std::memcmp(&d[rec], "OS/2", 4) == 0) os2Off = off, os2Len = len;
  }
  if (nameOff == 0 || nameOff + 6 > d.size()) return out;
  const unsigned count = u16(nameOff + 2), strings = u16(nameOff + 4);
  std::map<unsigned, std::pair<int, std::string>> best;  // name ID -> (rank, UTF-8)
  for (unsigned r = 0; r < count; ++r) {
    const std::size_t rec = nameOff + 6 + 12 * std::size_t(r);
    if (rec + 12 > d.size() || rec + 12 > nameOff + nameLen) break;
    const unsigned platform = u16(rec), encoding = u16(rec + 2), language = u16(rec + 4), id = u16(rec + 6);
    const unsigned len = u16(rec + 8), off = u16(rec + 10);
    if (id != 1 && id != 2 && id != 6 && id != 16 && id != 17) continue;
    const std::size_t at = nameOff + strings + off;
    if (at + len > d.size()) continue;
    std::string text;
    int rank;
    if (platform == 3 && (encoding == 0 || encoding == 1 || encoding == 10)) {
      std::u16string w;
      for (unsigned k = 0; k + 1 < len; k += 2) w += char16_t(u16(at + k));
      text = detail::toUtf8(w);
      rank = language == 0x409 ? 3 : 2;
    } else if (platform == 1 && encoding == 0) {
      text.assign(reinterpret_cast<const char*>(&d[at]), len);
      rank = language == 0 ? 1 : 0;
    } else {
      continue;
    }
    if (text.empty()) continue;
    auto it = best.find(id);
    if (it == best.end() || it->second.first < rank) best[id] = {rank, text};
  }
  if (best.count(16)) out.family = best[16].second;
  else if (best.count(1)) out.family = best[1].second;
  if (out.family.empty()) return out;
  out.known = true;
  if (best.count(6)) out.postscript = best[6].second;
  const std::string sub = best.count(17) ? best[17].second : best.count(2) ? best[2].second : "";
  int weight = 0, selection = 0;
  if (os2Off && os2Len >= 64 && os2Off + 64 <= d.size()) {
    weight = int(u16(os2Off + 4));
    selection = int(u16(os2Off + 62));
  }
  out.bold = weight >= 600 || (selection & 0x20) || contains(sub, "bold") || contains(sub, "black") || contains(sub, "heavy");
  out.italic = (selection & 1) || contains(sub, "italic") || contains(sub, "oblique");
  return out;
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

Piece pieceOf(FPDF_PAGEOBJECT obj, int index, FPDF_TEXTPAGE textPage, std::map<FPDF_FONT, FontFileNames>& embedded) {
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
    // An embedded font's own names and style win: the PDF's can be made up, and a font
    // descriptor without a weight gets one guessed from its stem width.
    auto it = embedded.find(font);
    if (it == embedded.end()) {
      FontFileNames names;
      std::size_t fontBytes = 0;
      if (FPDFFont_GetIsEmbedded(font) == 1 && FPDFFont_GetFontData(font, nullptr, 0, &fontBytes) && fontBytes > 12 &&
          fontBytes < (64u << 20)) {
        std::vector<std::uint8_t> data(fontBytes);
        if (FPDFFont_GetFontData(font, data.data(), fontBytes, &fontBytes)) names = namesInFontFile(data);
      }
      it = embedded.emplace(font, names).first;
    }
    if (it->second.known) {
      if (!it->second.postscript.empty()) p.font = it->second.postscript;
      p.family = it->second.family;
      p.bold = it->second.bold;
      p.italic = it->second.italic;
    }
  }
  unsigned int R = 0, G = 0, B = 0, A = 255;
  if (FPDFPageObj_GetFillColor(obj, &R, &G, &B, &A)) p.color = {std::uint8_t(R), std::uint8_t(G), std::uint8_t(B)};
  return p;
}

bool isSpace(char16_t c) { return c == u' ' || c == u'\t' || c == 0xA0; }

}  // namespace

namespace {

bool sameStyle(const Piece& a, const Piece& b, double riseA, double riseB) {
  return a.font == b.font && std::abs(a.size - b.size) < 0.05 && a.color == b.color && std::abs(riseA - riseB) < 0.1;
}

// Whether a line is all bold, or none of it (or mixed).
enum class Weight { None, All, Mixed };
Weight weightOf(const Line& l) {
  bool any = false, all = true;
  for (const Piece& p : l.pieces) {
    any = any || p.bold;
    all = all && p.bold;
  }
  return all ? Weight::All : any ? Weight::Mixed : Weight::None;
}

double center(const Line& l) { return (l.left + l.right) / 2; }

// Whether a line starts like an item of a list: "1.", "a)", "iv.", "2.)", a bullet.
bool startsListItem(const Line& l) {
  std::u16string t;
  for (const Piece& p : l.pieces) {
    t += p.text;
    if (t.size() > 8) break;
  }
  std::size_t i = 0;
  while (i < t.size() && isSpace(t[i])) ++i;
  if (i < t.size() && (t[i] == u'\u2022' || t[i] == u'\u25CF' || t[i] == u'\u25AA' || t[i] == u'-' || t[i] == u'\u2013'))
    return true;
  std::size_t j = i;
  while (j < t.size() && j - i < 4 && ((t[j] >= u'0' && t[j] <= u'9') || (t[j] >= u'a' && t[j] <= u'z') || (t[j] >= u'A' && t[j] <= u'Z')))
    ++j;
  return j > i && j < t.size() && (t[j] == u'.' || t[j] == u')');
}

}  // namespace

std::vector<ContentBlock> Document::contentBlocks(int pageIndex) const {
  std::vector<ContentBlock> out;
  if (pageIndex < 0 || pageIndex >= pageCount()) return out;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return out;
  const PageFrame frame = detail::frameOfPage(page);
  FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
  std::map<FPDF_FONT, FontFileNames> embeddedFonts;
  std::vector<Piece> pieces;
  std::vector<ContentBlock> graphics;
  const int n = FPDFPage_CountObjects(page);
  const double pageW = frame.uprightWidth(), pageH = frame.uprightHeight();
  for (int i = 0; i < n; ++i) {
    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
    FPDF_BOOL active = true;
    if (!obj || (FPDFPageObj_GetIsActive(obj, &active) && !active)) continue;
    const int type = FPDFPageObj_GetType(obj);
    if (type == FPDF_PAGEOBJ_TEXT) {
      Piece p = pieceOf(obj, i, textPage, embeddedFonts);
      if (std::all_of(p.text.begin(), p.text.end(), isSpace)) continue;  // nothing to see
      pieces.push_back(std::move(p));
      continue;
    }
    float l = 0, b = 0, r = 0, t = 0;
    if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &t)) continue;
    ContentBlock blk;
    blk.rect = frame.toDisplay(RectF{l, t, r, b});
    blk.objects = {i};
    blk.editable = true;
    if (type == FPDF_PAGEOBJ_IMAGE) {
      blk.kind = ContentBlock::Kind::Image;
      out.push_back(std::move(blk));
    } else if (type == FPDF_PAGEOBJ_PATH || type == FPDF_PAGEOBJ_SHADING || type == FPDF_PAGEOBJ_FORM) {
      if (type == FPDF_PAGEOBJ_PATH) {
        int fill = 0;
        FPDF_BOOL stroke = false;
        if (FPDFPath_GetDrawMode(obj, &fill, &stroke) && fill == FPDF_FILLMODE_NONE && !stroke) continue;  // invisible
      }
      // A background filling the page isn't something to pick.
      if (r - l > 0.8 * pageW && t - b > 0.8 * pageH) continue;
      if (graphics.size() >= 20000) continue;  // a drawing of countless lines: the first ones
      blk.kind = ContentBlock::Kind::Graphic;
      graphics.push_back(std::move(blk));
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
  auto extend = [](Line& l, const Piece& p) {
    l.left = std::min(l.left, p.left);
    l.right = std::max(l.right, p.right);
    l.bottom = std::min(l.bottom, p.bottom);
    l.top = std::max(l.top, p.top);
  };
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
    extend(*into, p);
    into->pieces.push_back(std::move(p));
  }
  // Superscripts and subscripts ("16th", "H2O"): smaller text a little above or below a
  // line, within or just after it, goes into that line.
  std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.size < b.size; });
  for (std::size_t i = 0; i < lines.size(); ++i) {
    Line& small = lines[i];
    if (small.pieces.empty()) continue;
    Line* best = nullptr;
    for (std::size_t j = i + 1; j < lines.size(); ++j) {
      Line& big = lines[j];
      if (big.pieces.empty() || small.size >= 0.85 * big.size) continue;
      const double rise = small.baseline - big.baseline;
      if (rise < -0.35 * big.size || rise > 0.6 * big.size || std::abs(rise) < 0.05 * big.size) continue;
      if (small.left < big.left - 0.3 * big.size || small.left > big.right + 0.6 * big.size) continue;
      if (!best || std::abs(big.baseline - small.baseline) < std::abs(best->baseline - small.baseline)) best = &big;
    }
    if (!best) continue;
    for (Piece& p : small.pieces) {
      extend(*best, p);
      best->pieces.push_back(std::move(p));
    }
    small.pieces.clear();
  }
  lines.erase(std::remove_if(lines.begin(), lines.end(), [](const Line& l) { return l.pieces.empty(); }), lines.end());
  for (Line& l : lines)
    std::sort(l.pieces.begin(), l.pieces.end(), [](const Piece& a, const Piece& b) { return a.x < b.x; });

  // Lines into paragraphs: one under another, the same size, about a line apart, and lined
  // up the same way: on the left, the right or the middle.
  enum class Edge { Unknown, Left, Right, Center };
  std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.baseline > b.baseline; });
  struct Para {
    std::vector<Line*> lines;
    Edge edge = Edge::Unknown;
  };
  std::vector<Para> paras;
  std::vector<bool> used(lines.size(), false);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (used[i]) continue;
    used[i] = true;
    Para para;
    para.lines.push_back(&lines[i]);
    for (std::size_t j = i + 1; j < lines.size(); ++j) {
      if (used[j]) continue;
      const Line& first = *para.lines.front();
      const Line& last = *para.lines.back();
      const Line& next = lines[j];
      const double drop = last.baseline - next.baseline;
      if (drop > 2.2 * last.size) break;  // sorted: everything after is further down
      const bool sameSize = std::abs(next.size - last.size) < 0.12 * last.size;
      const bool spacing = drop > 0.8 * last.size && drop < 1.9 * last.size &&
                           (para.lines.size() < 2 ||
                            std::abs(drop - (para.lines[0]->baseline - para.lines[1]->baseline)) < 0.25 * last.size);
      const bool overlaps = next.left < last.right + last.size && next.right > last.left - last.size;
      // A bold line over a plain one is a heading and its text (or a table's header and a cell).
      const Weight wl = weightOf(last), wn = weightOf(next);
      const bool sameWeight = !((wl == Weight::All && wn == Weight::None) || (wl == Weight::None && wn == Weight::All));
      if (!(sameSize && spacing && overlaps && sameWeight)) continue;
      const double tol = 0.3 * last.size;
      // Lines of a list that happen to end level aren't right-aligned text.
      const bool list = startsListItem(first) || startsListItem(next);
      const bool left = std::abs(next.left - first.left) < 1.0 * last.size;
      const bool middle = !list && std::abs(center(next) - center(first)) < tol;
      const bool right = !list && std::abs(next.right - first.right) < tol;
      Edge edge = Edge::Unknown;
      if (para.edge != Edge::Unknown) {  // as the paragraph's lines are already
        const bool fits = para.edge == Edge::Left ? left : para.edge == Edge::Center ? middle : right;
        if (fits) edge = para.edge;
      } else {
        edge = left ? Edge::Left : middle ? Edge::Center : right ? Edge::Right : Edge::Unknown;
      }
      if (edge == Edge::Unknown) continue;
      para.edge = edge;
      used[j] = true;
      para.lines.push_back(&lines[j]);
    }
    paras.push_back(std::move(para));
  }

  for (const Para& para : paras) {
    ContentBlock blk;
    blk.kind = ContentBlock::Kind::Text;
    double l = 1e9, r = -1e9, b = 1e9, t = -1e9;
    for (const Line* line : para.lines) {
      l = std::min(l, line->left);
      r = std::max(r, line->right);
      b = std::min(b, line->bottom);
      t = std::max(t, line->top);
      for (const Piece& p : line->pieces) blk.objects.push_back(p.index);
    }
    blk.rect = frame.toDisplay(RectF{l, t, r, b});
    const std::size_t count = para.lines.size();
    blk.lineSpacing = count > 1 ? (para.lines.front()->baseline - para.lines.back()->baseline) / double(count - 1) : 0;
    blk.origin = frame.toDisplay(PointF{para.lines.front()->pieces.front().x, para.lines.front()->baseline});
    blk.editable = frame.rotation == 0;
    const double size = para.lines.front()->size;
    switch (para.edge) {
      case Edge::Center: blk.align = TextAlign::Center; break;
      case Edge::Right: blk.align = TextAlign::Right; break;
      default: {
        // Lined up on both sides, apart from the last line: justified (it takes two such
        // lines to tell).
        int even = 0;
        for (std::size_t k = 0; k + 1 < count; ++k)
          if (std::abs(para.lines[k]->right - r) < 0.25 * size) ++even;
        blk.align = count >= 3 && even == int(count - 1) ? TextAlign::Justify : TextAlign::Left;
      }
    }

    // The text, in runs of one style. Pieces of a line apart by more than a little have a
    // space between them. A line ends the paragraph's line there (a hard break) if the next
    // line's first word would have fitted on it; otherwise it wrapped, and runs on with a space.
    std::vector<std::u16string> runText;
    std::vector<const Piece*> runStyle;
    std::vector<double> runRise;
    auto append = [&](const std::u16string& text, const Piece* style, double rise) {
      if (text.empty()) return;
      if (style && (runStyle.empty() || !sameStyle(*runStyle.back(), *style, runRise.back(), rise))) {
        runText.push_back({});
        runStyle.push_back(style);
        runRise.push_back(rise);
      }
      if (runText.empty()) return;
      runText.back() += text;
    };
    auto lastChar = [&]() -> char16_t {
      for (auto it = runText.rbegin(); it != runText.rend(); ++it)
        if (!it->empty()) return it->back();
      return u' ';
    };
    for (std::size_t k = 0; k < count; ++k) {
      const Line& line = *para.lines[k];
      for (std::size_t q = 0; q < line.pieces.size(); ++q) {
        const Piece& p = line.pieces[q];
        std::u16string pt = p.text;
        if (q + 1 == line.pieces.size())
          while (!pt.empty() && isSpace(pt.back())) pt.pop_back();
        if (q > 0 && !pt.empty()) {
          const Piece& prev = line.pieces[q - 1];
          const double gap = p.left - prev.right, em = std::max(p.size, prev.size);
          // A gap wider than a couple of spaces, in text lined up on the left, is a tab: the
          // text after it starts at a tab stop ("1.)" and the item's text).
          if (blk.align == TextAlign::Left && gap > 0.5 * em && std::abs(p.baseline - line.baseline) < 0.05 &&
              std::abs(prev.baseline - line.baseline) < 0.05) {
            for (auto it = runText.rbegin(); it != runText.rend(); ++it) {
              while (!it->empty() && isSpace(it->back())) it->pop_back();
              if (!it->empty()) break;
            }
            while (!pt.empty() && isSpace(pt.front())) pt.erase(pt.begin());
            append(u"\t", nullptr, 0);
            const double stop = p.x - para.lines.front()->pieces.front().x;
            if (std::none_of(blk.tabStops.begin(), blk.tabStops.end(), [&](double at) { return std::abs(at - stop) < 1; }))
              blk.tabStops.push_back(stop);
          } else if (!isSpace(lastChar()) && lastChar() != u'\t' && !isSpace(pt.front()) && gap > 0.2 * em) {
            append(u" ", nullptr, 0);
          }
        }
        if (isSpace(lastChar()) || lastChar() == u'\t')  // a space already: one is enough
          while (!pt.empty() && isSpace(pt.front())) pt.erase(pt.begin());
        append(pt, &p, p.baseline - line.baseline);
      }
      if (k + 1 == count) break;
      const Line& next = *para.lines[k + 1];
      std::u16string nextText;
      for (const Piece& p : next.pieces) nextText += p.text;
      std::size_t w = 0;
      while (w < nextText.size() && isSpace(nextText[w])) ++w;
      std::size_t we = w;
      while (we < nextText.size() && !isSpace(nextText[we])) ++we;
      const double perChar = (next.right - next.left) / double(std::max<std::size_t>(1, nextText.size()));
      const double firstWord = double(we - w) * perChar;
      // Centered or right-aligned lines end where they were broken.
      const bool wrapped = para.edge != Edge::Center && para.edge != Edge::Right && !startsListItem(next) && (line.right - para.lines.front()->left) + 0.3 * size + firstWord > (r - l) - 0.1 * size;
      append(wrapped ? u" " : u"\n", nullptr, 0);
    }
    std::u16string all;
    std::size_t mostChars = 0;
    const Piece* base = para.lines.front()->pieces.front().text.empty() ? nullptr : &para.lines.front()->pieces.front();
    for (std::size_t k = 0; k < runText.size(); ++k) {
      all += runText[k];
      const Piece& st = *runStyle[k];
      TextRun run;
      run.text = detail::toUtf8(runText[k]);
      run.fontName = st.font;
      run.family = st.family.empty() ? familyOf(st.font) : st.family;
      run.bold = st.bold;
      run.italic = st.italic;
      run.size = st.size;
      run.color = st.color;
      run.rise = runRise[k];
      blk.runs.push_back(std::move(run));
      // The block's own style: the one most of its (unraised) text is in.
      if (std::abs(runRise[k]) < 0.05 && runText[k].size() > mostChars) {
        mostChars = runText[k].size();
        base = &st;
      }
    }
    if (!base) base = &para.lines.front()->pieces.front();
    std::sort(blk.tabStops.begin(), blk.tabStops.end());
    blk.text = detail::toUtf8(all);
    blk.fontName = base->font;
    blk.family = base->family.empty() ? familyOf(base->font) : base->family;
    blk.bold = base->bold;
    blk.italic = base->italic;
    blk.fontSize = base->size;
    blk.color = base->color;
    out.push_back(std::move(blk));
  }
  // Text that's turned or slanted: one block per object, which can be moved or deleted.
  for (const Piece& p : other) {
    ContentBlock blk;
    blk.kind = ContentBlock::Kind::Text;
    blk.rect = frame.toDisplay(RectF{p.left, p.top, p.right, p.bottom});
    blk.objects = {p.index};
    blk.text = detail::toUtf8(p.text);
    blk.fontName = p.font;
    blk.family = p.family.empty() ? familyOf(p.font) : p.family;
    blk.fontSize = p.size;
    blk.color = p.color;
    blk.editable = false;
    TextRun run;
    run.text = blk.text;
    run.fontName = p.font;
    run.family = blk.family;
    run.bold = p.bold;
    run.italic = p.italic;
    run.size = p.size;
    run.color = p.color;
    blk.runs.push_back(std::move(run));
    out.push_back(std::move(blk));
  }
  // Graphics last: text and pictures come first when blocks overlap.
  for (ContentBlock& g : graphics) out.push_back(std::move(g));
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

Affine Affine::rotate(double degrees, PointF c) {
  const double rad = degrees * 3.14159265358979323846 / 180;
  double cs = std::cos(rad), sn = std::sin(rad);
  // Quarter turns exactly.
  if (std::abs(cs) < 1e-12) cs = 0;
  if (std::abs(sn) < 1e-12) sn = 0;
  // y grows downwards, so this turns clockwise on screen.
  return {cs, sn, -sn, cs, c.x - cs * c.x + sn * c.y, c.y - sn * c.x - cs * c.y};
}

namespace {

bool generateContent(FPDF_PAGE page) { return FPDFPage_GenerateContent(page); }

// `m`, a map of the display frame, as a map of the page's user space.
FS_MATRIX userMatrix(const PageFrame& f, const Affine& m) {
  auto t = [&](PointF user) { return f.toUser(m.map(f.toDisplay(user))); };
  const PointF o = t({0, 0}), x = t({1, 0}), y = t({0, 1});
  return FS_MATRIX{float(x.x - o.x), float(x.y - o.y), float(y.x - o.x), float(y.y - o.y), float(o.x), float(o.y)};
}

}  // namespace

bool Document::transformContent(int pageIndex, const std::vector<int>& objects, const Affine& m) {
  if (pageIndex < 0 || pageIndex >= pageCount()) return false;
  if (std::abs(m.a * m.d - m.b * m.c) < 1e-9) return false;  // flattened to nothing
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(pageForContentEdit(pageIndex));
  if (!page) return false;
  const auto objs = objectsOf(page, objects);
  if (objs.empty()) return false;
  const FS_MATRIX u = userMatrix(detail::frameOfPage(page), m);
  for (FPDF_PAGEOBJECT obj : objs) {
    FPDFPageObj_SetIsActive(obj, true);
    FPDFPageObj_TransformF(obj, &u);
    // Its clip (a cell or the page, often) goes with it, or it could be clipped away.
    FPDFPageObj_TransformClipPath(obj, u.a, u.b, u.c, u.d, u.e, u.f);
  }
  if (!generateContent(page)) return false;
  markEdited();
  return true;
}

bool Document::moveContent(int pageIndex, const std::vector<int>& objects, double dx, double dy) {
  return transformContent(pageIndex, objects, Affine::translate(dx, dy));
}

bool Document::resizeContent(int pageIndex, const std::vector<int>& objects, const RectF& from, const RectF& to) {
  const RectF a = from.normalized(), b = to.normalized();
  if (a.width() <= 0 || a.height() <= 0 || b.width() <= 0 || b.height() <= 0) return false;
  const double sx = b.width() / a.width(), sy = b.height() / a.height();
  return transformContent(pageIndex, objects, Affine{sx, 0, 0, sy, b.left - a.left * sx, b.top - a.top * sy});
}

bool Document::deleteContent(int pageIndex, const std::vector<int>& objects) {
  if (pageIndex < 0 || pageIndex >= pageCount()) return false;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(pageForContentEdit(pageIndex));
  if (!page) return false;
  const auto objs = objectsOf(page, objects);
  if (objs.empty()) return false;
  for (FPDF_PAGEOBJECT obj : objs)
    if (FPDFPage_RemoveObject(page, obj)) FPDFPageObj_Destroy(obj);
  if (!generateContent(page)) return false;
  markEdited();
  return true;
}

bool Document::replaceText(int pageIndex, const std::vector<int>& remove, const TextLines& text) {
  PlacedText placed;
  placed.origin = text.origin;
  placed.fonts = {text.font};
  for (std::size_t i = 0; i < text.lines.size(); ++i) {
    PlacedLine line;
    line.advance = i == 0 ? 0 : text.lineSpacing;
    if (!text.lines[i].empty()) line.spans.push_back(TextSpan{text.lines[i], 0, 0, text.fontSize, text.color, 0});
    placed.lines.push_back(std::move(line));
  }
  return replaceText(pageIndex, remove, placed);
}

bool Document::replaceText(int pageIndex, const std::vector<int>& remove, const PlacedText& text) {
  if (pageIndex < 0 || pageIndex >= pageCount() || text.fonts.empty()) return false;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(pageForContentEdit(pageIndex));
  if (!page) return false;
  FPDF_DOCUMENT doc = static_cast<FPDF_DOCUMENT>(doc_);
  std::vector<FPDF_PAGEOBJECT> old;
  if (!remove.empty()) {
    old = objectsOf(page, remove);
    if (old.empty()) return false;
  }
  // The fonts: standard ones (loaded for this), or embedded ones (loaded once per document).
  std::vector<FPDF_FONT> fonts(text.fonts.size(), nullptr);
  std::vector<FPDF_FONT> toClose;
  bool ok = true;
  for (std::size_t k = 0; k < text.fonts.size() && ok; ++k) {
    const TextFont& tf = text.fonts[k];
    if (!tf.file.empty()) {
      for (auto& [key, f] : fonts_)
        if (key == tf.key) fonts[k] = static_cast<FPDF_FONT>(f);
      if (!fonts[k]) {
        fonts[k] = FPDFText_LoadFont(doc, tf.file.data(), static_cast<uint32_t>(tf.file.size()), FPDF_FONT_TRUETYPE,
                                     /*cid=*/true);
        if (fonts[k]) fonts_.emplace_back(tf.key, fonts[k]);
      }
    } else {
      fonts[k] = FPDFText_LoadStandardFont(doc, tf.standard.empty() ? "Helvetica" : tf.standard.c_str());
      if (fonts[k]) toClose.push_back(fonts[k]);
    }
    ok = fonts[k] != nullptr;
  }
  const PageFrame f = detail::frameOfPage(page);
  std::vector<FPDF_PAGEOBJECT> added;
  double y = text.origin.y;
  for (std::size_t i = 0; i < text.lines.size() && ok; ++i) {
    const PlacedLine& line = text.lines[i];
    if (i > 0) y += line.advance;
    for (const TextSpan& span : line.spans) {
      if (span.text.empty() || span.font < 0 || span.font >= int(fonts.size()) || span.size <= 0) continue;
      FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(doc, fonts[std::size_t(span.font)], float(span.size));
      if (!obj) {
        ok = false;
        break;
      }
      const std::u16string w = detail::toUtf16(span.text);
      if (!FPDFText_SetText(obj, reinterpret_cast<FPDF_WIDESTRING>(w.c_str()))) {
        FPDFPageObj_Destroy(obj);
        ok = false;
        break;
      }
      // Its start on the baseline (display frame: y down, so a rise goes up).
      const PointF at = f.toUser(PointF{text.origin.x + span.x, y - span.rise});
      FPDFPageObj_Transform(obj, 1, 0, 0, 1, at.x, at.y);
      FPDFPageObj_SetFillColor(obj, span.color.r, span.color.g, span.color.b, 255);
      added.push_back(obj);
    }
  }
  for (FPDF_FONT font : toClose) FPDFFont_Close(font);
  if (!ok) {
    for (FPDF_PAGEOBJECT obj : added) FPDFPageObj_Destroy(obj);
    return false;
  }
  for (FPDF_PAGEOBJECT obj : old)
    if (FPDFPage_RemoveObject(page, obj)) FPDFPageObj_Destroy(obj);
  for (FPDF_PAGEOBJECT obj : added) FPDFPage_InsertObject(page, obj);
  if (!generateContent(page)) return false;
  markEdited();
  return true;
}

// Before changing a page's content: its objects are parsed afresh from its content streams
// joined into one, so they all belong to one stream. PDFium rewrites only the streams that
// hold changed objects, keeping track of the drawing state from one stream to the next as it
// first read them. Word (among others) starts a shape in one stream and ends it in the next,
// so rewriting one stream of such a page left half the shape behind, and its unfinished state
// (a scaling, say) was applied to everything drawn after it, or to text added later.
//
// So: the page is emptied of all but its first object (the streams that held only the others
// go), read again (PDFium then knows the state at each stream's end as it now is), and then
// all the objects, the first too, are put back from the joined copy: they're all in one stream,
// written in their order. Streams that held only part of a shape stay, as stray path pieces
// that draw nothing. The objects keep their order, and so their indices.
void* Document::pageForContentEdit(int pageIndex) {
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return nullptr;
  // The objects are about to change (some destroyed): what render() kept of them goes.
  renderHints_.erase(page);
  const int n = FPDFPage_CountObjects(page);
  if (n < 2) return page;
  FPDF_DOCUMENT doc = static_cast<FPDF_DOCUMENT>(doc_);
  auto xobj = static_cast<FPDF_XOBJECT>(detail::pageXObject(doc, doc, pageIndex, page));
  if (!xobj) return page;
  FPDF_PAGEOBJECT form = FPDF_NewFormObjectFromXObject(xobj);
  FPDF_CloseXObject(xobj);
  if (!form) return page;
  // Only if every object is there both ways (an edit not yet written would be lost), and they
  // can be put back where they are.
  FS_MATRIX m{};
  if (FPDFFormObj_CountObjects(form) != n || !detail::formToPage(page, form, &m)) {
    FPDFPageObj_Destroy(form);
    return page;
  }
  for (int i = n - 1; i >= 1; --i) {
    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
    if (obj && FPDFPage_RemoveObject(page, obj)) FPDFPageObj_Destroy(obj);
  }
  FPDFPage_GenerateContent(page);
  dropCachedPage(pageIndex);
  page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) {
    FPDFPageObj_Destroy(form);
    return nullptr;
  }
  // The first object goes too: all of them come from the copy, so they all belong to one
  // stream and are written in their order. (Keeping it, when it was in a later stream than the
  // others, as a picture after an AutoCAD drawing's set-up stream, left it in its own stream
  // after them: drawn on top, and first in the list but last in the content, so the next edit
  // matched the wrong objects up.)
  for (int i = FPDFPage_CountObjects(page) - 1; i >= 0; --i) {
    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
    if (obj && FPDFPage_RemoveObject(page, obj)) FPDFPageObj_Destroy(obj);
  }
  const bool identity = m.a == 1 && m.b == 0 && m.c == 0 && m.d == 1 && m.e == 0 && m.f == 0;
  while (FPDFFormObj_CountObjects(form) > 0) {
    FPDF_PAGEOBJECT child = FPDFFormObj_GetObject(form, 0);
    if (!child || !FPDFFormObj_RemoveObject(form, child)) break;
    if (!identity) {
      FPDFPageObj_TransformF(child, &m);
      FPDFPageObj_TransformClipPath(child, m.a, m.b, m.c, m.d, m.e, m.f);
    }
    FPDFPage_InsertObject(page, child);
  }
  FPDFPageObj_Destroy(form);
  return page;
}

}  // namespace pdfz
