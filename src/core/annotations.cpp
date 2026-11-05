// SPDX-License-Identifier: GPL-3.0-only
#include "core/annotations.hpp"

#include "core/document.hpp"
#include "core/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <random>
#include <sstream>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdfview.h>

namespace pdfz {

const char* kindName(AnnotKind kind) {
  switch (kind) {
    case AnnotKind::Highlight: return "Highlight";
    case AnnotKind::Underline: return "Underline";
    case AnnotKind::StrikeOut: return "Strikeout";
    case AnnotKind::Squiggly: return "Squiggly";
    case AnnotKind::Note: return "Note";
    case AnnotKind::FreeText: return "Text Box";
    case AnnotKind::Ink: return "Drawing";
    case AnnotKind::Square: return "Rectangle";
    case AnnotKind::Circle: return "Ellipse";
    case AnnotKind::Line: return "Line";
    case AnnotKind::Stamp: return "Stamp";
    case AnnotKind::FileAttachment: return "Attachment";
    case AnnotKind::Other: break;
  }
  return "Annotation";
}

bool isTextMarkup(AnnotKind k) {
  return k == AnnotKind::Highlight || k == AnnotKind::Underline || k == AnnotKind::StrikeOut ||
         k == AnnotKind::Squiggly;
}

bool Annotation::editable() const {
  switch (kind) {
    case AnnotKind::Highlight: case AnnotKind::Underline: case AnnotKind::StrikeOut:
    case AnnotKind::Squiggly: case AnnotKind::Note: case AnnotKind::FreeText: case AnnotKind::Ink:
    case AnnotKind::Square: case AnnotKind::Circle:
      return true;
    default:
      return false;
  }
}

std::string pdfDateNow() {
  const std::time_t t = std::time(nullptr);
  std::tm local{}, utc{};
#ifdef _WIN32
  localtime_s(&local, &t);
  gmtime_s(&utc, &t);
#else
  localtime_r(&t, &local);
  gmtime_r(&t, &utc);
#endif
  // The offset from UTC: the difference between the two broken-down times, read as local.
  utc.tm_isdst = local.tm_isdst;
  const long offset = long(std::difftime(std::mktime(&local), std::mktime(&utc)) / 60);  // minutes
  char buf[64];
  std::snprintf(buf, sizeof buf, "D:%04d%02d%02d%02d%02d%02d%c%02ld'%02ld'", local.tm_year + 1900, local.tm_mon + 1,
                local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, offset < 0 ? '-' : '+',
                std::labs(offset) / 60, std::labs(offset) % 60);
  return buf;
}

namespace {

// A page from the document's cache (not closed here).
struct Page {
  FPDF_PAGE page = nullptr;
  explicit Page(void* p) : page(static_cast<FPDF_PAGE>(p)) {}
};

const char* const kStyleKey = "PDFZStyle";

struct Annot {
  FPDF_ANNOTATION a = nullptr;
  explicit Annot(FPDF_ANNOTATION h) : a(h) {}
  ~Annot() { close(); }
  void close() {
    if (a) FPDFPage_CloseAnnot(a);
    a = nullptr;
  }
  Annot(const Annot&) = delete;
  Annot& operator=(const Annot&) = delete;
};

PageFrame frameOf(FPDF_PAGE page) {
  PageFrame f;
  FS_RECTF b{};
  if (FPDF_GetPageBoundingBox(page, &b)) {
    f.x0 = std::min(b.left, b.right);
    f.x1 = std::max(b.left, b.right);
    f.y0 = std::min(b.top, b.bottom);
    f.y1 = std::max(b.top, b.bottom);
  }
  f.rotation = FPDFPage_GetRotation(page) * 90;
  return f;
}

AnnotKind kindOf(int subtype) {
  switch (subtype) {
    case FPDF_ANNOT_HIGHLIGHT: return AnnotKind::Highlight;
    case FPDF_ANNOT_UNDERLINE: return AnnotKind::Underline;
    case FPDF_ANNOT_STRIKEOUT: return AnnotKind::StrikeOut;
    case FPDF_ANNOT_SQUIGGLY: return AnnotKind::Squiggly;
    case FPDF_ANNOT_TEXT: return AnnotKind::Note;
    case FPDF_ANNOT_FREETEXT: return AnnotKind::FreeText;
    case FPDF_ANNOT_INK: return AnnotKind::Ink;
    case FPDF_ANNOT_SQUARE: return AnnotKind::Square;
    case FPDF_ANNOT_CIRCLE: return AnnotKind::Circle;
    case FPDF_ANNOT_LINE: return AnnotKind::Line;
    case FPDF_ANNOT_STAMP: return AnnotKind::Stamp;
    case FPDF_ANNOT_FILEATTACHMENT: return AnnotKind::FileAttachment;
    default: return AnnotKind::Other;
  }
}

int subtypeFor(AnnotKind k) {
  switch (k) {
    case AnnotKind::Highlight: return FPDF_ANNOT_HIGHLIGHT;
    case AnnotKind::Underline: return FPDF_ANNOT_UNDERLINE;
    case AnnotKind::StrikeOut: return FPDF_ANNOT_STRIKEOUT;
    case AnnotKind::Squiggly: return FPDF_ANNOT_SQUIGGLY;
    case AnnotKind::Note: return FPDF_ANNOT_TEXT;
    case AnnotKind::FreeText: return FPDF_ANNOT_FREETEXT;
    case AnnotKind::Ink: case AnnotKind::Line: return FPDF_ANNOT_INK;
    case AnnotKind::Square: return FPDF_ANNOT_SQUARE;
    case AnnotKind::Circle: return FPDF_ANNOT_CIRCLE;
    default: return FPDF_ANNOT_UNKNOWN;
  }
}

// The kinds whose appearance PDFium draws itself when there is none.
bool pdfiumDraws(AnnotKind k) {
  return isTextMarkup(k) || k == AnnotKind::Note || k == AnnotKind::Ink || k == AnnotKind::Square ||
         k == AnnotKind::Circle;
}

std::u16string utf8ToUtf16(const std::string& s) {
  std::u16string out;
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    char32_t cp;
    int len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
    else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
    else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
    else { ++i; out += u'�'; continue; }
    if (i + len > s.size()) break;
    for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += std::size_t(len);
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out += char16_t(0xD800 + (cp >> 10));
      out += char16_t(0xDC00 + (cp & 0x3FF));
    } else {
      out += char16_t(cp);
    }
  }
  return out;
}

std::string utf16ToUtf8(const std::u16string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char32_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
  }
  return out;
}

std::string getString(FPDF_ANNOTATION a, const char* key) {
  const unsigned long bytes = FPDFAnnot_GetStringValue(a, key, nullptr, 0);
  if (bytes <= 2) return {};
  std::u16string buf(bytes / 2, u'\0');
  FPDFAnnot_GetStringValue(a, key, reinterpret_cast<FPDF_WCHAR*>(buf.data()), bytes);
  buf.resize(bytes / 2 - 1);  // the terminator
  return utf16ToUtf8(buf);
}

void setString(FPDF_ANNOTATION a, const char* key, const std::string& value) {
  const std::u16string w = utf8ToUtf16(value);
  FPDFAnnot_SetStringValue(a, key, reinterpret_cast<FPDF_WIDESTRING>(w.c_str()));
}

std::string getAppearance(FPDF_ANNOTATION a) {
  const unsigned long bytes = FPDFAnnot_GetAP(a, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0);
  if (bytes <= 2) return {};
  std::u16string buf(bytes / 2, u'\0');
  FPDFAnnot_GetAP(a, FPDF_ANNOT_APPEARANCEMODE_NORMAL, reinterpret_cast<FPDF_WCHAR*>(buf.data()), bytes);
  buf.resize(bytes / 2 - 1);
  return utf16ToUtf8(buf);
}

bool setAppearance(FPDF_ANNOTATION a, const std::string& content) {
  if (content.empty()) return FPDFAnnot_SetAP(a, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr);
  const std::u16string w = utf8ToUtf16(content);
  return FPDFAnnot_SetAP(a, FPDF_ANNOT_APPEARANCEMODE_NORMAL, reinterpret_cast<FPDF_WIDESTRING>(w.c_str()));
}

std::uint8_t to255(double v) { return std::uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255)); }

struct ColorOps {
  std::optional<Color> fill, stroke;
};

// The first fill and stroke colors set in a content stream. PDFium won't report an
// annotation's color once it has an appearance, and that appearance is what's shown anyway.
ColorOps colorsInContent(const std::string& content) {
  ColorOps out;
  std::istringstream in(content);
  std::vector<double> nums;
  std::string tok;
  while (in >> tok && !(out.fill && out.stroke)) {
    char* end = nullptr;
    const double v = std::strtod(tok.c_str(), &end);
    if (end && *end == '\0' && !tok.empty()) {
      nums.push_back(v);
      continue;
    }
    std::optional<Color> c;
    const std::size_t n = nums.size();
    if ((tok == "rg" || tok == "RG") && n >= 3) c = Color{to255(nums[n - 3]), to255(nums[n - 2]), to255(nums[n - 1])};
    else if ((tok == "g" || tok == "G") && n >= 1) c = Color{to255(nums[n - 1]), to255(nums[n - 1]), to255(nums[n - 1])};
    else if ((tok == "k" || tok == "K") && n >= 4) {
      const double k = nums[n - 1];
      c = Color{to255((1 - nums[n - 4]) * (1 - k)), to255((1 - nums[n - 3]) * (1 - k)), to255((1 - nums[n - 2]) * (1 - k))};
    }
    if (c) {
      const bool stroke = tok == "RG" || tok == "G" || tok == "K";
      if (stroke && !out.stroke) out.stroke = c;
      if (!stroke && !out.fill) out.fill = c;
    }
    nums.clear();
  }
  return out;
}

// "/Helv 12 Tf 0 0 1 rg 1 0 0 RG" -> font Helv, size 12, text blue, border red.
void parseDA(const std::string& da, Annotation& a, bool& hasBorder) {
  std::istringstream in(da);
  std::vector<double> nums;
  std::string name, tok;
  while (in >> tok) {
    char* end = nullptr;
    const double v = std::strtod(tok.c_str(), &end);
    if (end && *end == '\0') {
      nums.push_back(v);
      continue;
    }
    const std::size_t n = nums.size();
    if (!tok.empty() && tok[0] == '/') name = tok.substr(1);
    else if (tok == "Tf" && n >= 1) {
      a.fontSize = nums.back();
      if (!name.empty()) a.fontName = name;
    } else if (tok == "rg" && n >= 3) a.textColor = {to255(nums[n - 3]), to255(nums[n - 2]), to255(nums[n - 1])};
    else if (tok == "g" && n >= 1) a.textColor = {to255(nums[n - 1]), to255(nums[n - 1]), to255(nums[n - 1])};
    else if (tok == "RG" && n >= 3) {
      a.color = {to255(nums[n - 3]), to255(nums[n - 2]), to255(nums[n - 1])};
      hasBorder = true;
    }
    nums.clear();
  }
}

std::string formatDA(const Annotation& a) {
  char buf[160];
  const std::string font = a.fontName.empty() ? "Helv" : a.fontName;
  std::snprintf(buf, sizeof buf, "/%s %g Tf %.4g %.4g %.4g rg %.4g %.4g %.4g RG", font.c_str(), a.fontSize,
                a.textColor.r / 255.0, a.textColor.g / 255.0, a.textColor.b / 255.0, a.color.r / 255.0,
                a.color.g / 255.0, a.color.b / 255.0);
  return buf;
}

std::string uniqueName() {
  static std::mt19937_64 rng{std::random_device{}() ^
                             std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())};
  char buf[32];
  std::snprintf(buf, sizeof buf, "pdfz-%016llx", static_cast<unsigned long long>(rng()));
  return buf;
}

FS_RECTF toFS(const RectF& user) {  // user: top is the larger y
  return {float(user.left), float(user.top), float(user.right), float(user.bottom)};
}

RectF displayRectOf(FPDF_ANNOTATION a, const PageFrame& f) {
  FS_RECTF r{};
  if (!FPDFAnnot_GetRect(a, &r)) return {};
  const RectF user{std::min(r.left, r.right), std::max(r.top, r.bottom), std::max(r.left, r.right),
                   std::min(r.top, r.bottom)};
  return f.toDisplay(user);
}

FS_QUADPOINTSF quadFor(const RectF& displayRect, const PageFrame& f) {
  const RectF u = f.toUser(displayRect);
  // Corners in the order readers expect: top-left, top-right, bottom-left, bottom-right.
  return {float(u.left), float(u.top), float(u.right), float(u.top), float(u.left), float(u.bottom), float(u.right),
          float(u.bottom)};
}

RectF displayRectOfQuad(const FS_QUADPOINTSF& q, const PageFrame& f) {
  const double xs[] = {q.x1, q.x2, q.x3, q.x4};
  const double ys[] = {q.y1, q.y2, q.y3, q.y4};
  const RectF user{*std::min_element(xs, xs + 4), *std::max_element(ys, ys + 4), *std::max_element(xs, xs + 4),
                   *std::min_element(ys, ys + 4)};
  return f.toDisplay(user);
}

void setInk(FPDF_ANNOTATION a, const std::vector<std::vector<PointF>>& strokes, const PageFrame& f) {
  FPDFAnnot_RemoveInkList(a);
  for (const auto& s : strokes) {
    if (s.empty()) continue;
    std::vector<FS_POINTF> pts;
    pts.reserve(s.size());
    for (const PointF& p : s) {
      const PointF u = f.toUser(p);
      pts.push_back({float(u.x), float(u.y)});
    }
    FPDFAnnot_AddInkStroke(a, pts.data(), pts.size());
  }
}

// The rect an annotation needs: the given one, or one around its quads / strokes.
RectF boundsFor(const Annotation& a) {
  if (isTextMarkup(a.kind) && !a.quads.empty()) {
    RectF r;
    for (const RectF& q : a.quads) r = r.united(q);
    return r;
  }
  if ((a.kind == AnnotKind::Ink || a.kind == AnnotKind::Line) && !a.ink.empty()) {
    RectF r;
    bool first = true;
    for (const auto& s : a.ink)
      for (const PointF& p : s) {
        if (first) r = {p.x, p.y, p.x, p.y};
        r = {std::min(r.left, p.x), std::min(r.top, p.y), std::max(r.right, p.x), std::max(r.bottom, p.y)};
        first = false;
      }
    const double pad = std::max(1.0, a.borderWidth);
    return {r.left - pad, r.top - pad, r.right + pad, r.bottom + pad};
  }
  return a.rect.normalized();
}

std::string hexColor(Color c) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "%02x%02x%02x", c.r, c.g, c.b);
  return buf;
}

std::optional<Color> parseHex(const std::string& s) {
  if (s.size() != 6) return std::nullopt;
  char* end = nullptr;
  const unsigned long v = std::strtoul(s.c_str(), &end, 16);
  if (!end || *end) return std::nullopt;
  return Color{std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)};
}

// pdfz keeps the style it set in a private entry, "c=rrggbb;f=rrggbb|-;a=0", which other
// readers ignore: PDFium won't say an annotation's colors once it has an appearance, and a text
// box's border and alignment have no standard entry PDFium can write.
std::string formatStyle(const Annotation& a) {
  std::string s = "c=" + hexColor(a.color) + ";f=" + (a.hasFill ? hexColor(a.fill) : std::string("-"));
  if (a.kind == AnnotKind::FreeText) s += ";a=" + std::to_string(std::clamp(a.align, 0, 2));
  return s;
}

bool parseStyle(const std::string& s, Annotation& a) {
  if (s.empty()) return false;
  std::istringstream in(s);
  std::string part;
  while (std::getline(in, part, ';')) {
    const auto eq = part.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = part.substr(0, eq), v = part.substr(eq + 1);
    if (k == "c") {
      if (auto c = parseHex(v)) a.color = *c;
    } else if (k == "f") {
      if (v == "-") a.hasFill = false;
      else if (auto c = parseHex(v)) {
        a.hasFill = true;
        a.fill = *c;
      }
    } else if (k == "a") {
      a.align = std::clamp(std::atoi(v.c_str()), 0, 2);
    }
  }
  return true;
}

// Writes everything but the strings that identify the annotation (name, creation date).
void writeAnnotation(FPDF_ANNOTATION h, const Annotation& a, const PageFrame& f, bool isNew) {
  const RectF bounds = boundsFor(a);
  FS_RECTF r = toFS(f.toUser(bounds));
  FPDFAnnot_SetRect(h, &r);
  const unsigned alpha = unsigned(std::lround(std::clamp(a.opacity, 0.0, 1.0) * 255));
  if (a.kind == AnnotKind::FreeText) {
    setString(h, "DA", formatDA(a));
    // A text box's /C is its background. Without one, the opacity (/CA) still has to be set,
    // and the interior color (meaningless for a text box) is the way to set it alone.
    if (a.hasFill) FPDFAnnot_SetColor(h, FPDFANNOT_COLORTYPE_Color, a.fill.r, a.fill.g, a.fill.b, alpha);
    else FPDFAnnot_SetColor(h, FPDFANNOT_COLORTYPE_InteriorColor, a.color.r, a.color.g, a.color.b, alpha);
  } else {
    FPDFAnnot_SetColor(h, FPDFANNOT_COLORTYPE_Color, a.color.r, a.color.g, a.color.b, alpha);
  }
  if ((a.kind == AnnotKind::Square || a.kind == AnnotKind::Circle) && a.hasFill)
    FPDFAnnot_SetColor(h, FPDFANNOT_COLORTYPE_InteriorColor, a.fill.r, a.fill.g, a.fill.b, alpha);
  if (a.kind == AnnotKind::Ink || a.kind == AnnotKind::Line || a.kind == AnnotKind::Square ||
      a.kind == AnnotKind::Circle || a.kind == AnnotKind::FreeText)
    FPDFAnnot_SetBorder(h, 0, 0, float(a.borderWidth));
  if (isTextMarkup(a.kind)) {
    const std::size_t have = FPDFAnnot_CountAttachmentPoints(h);
    for (std::size_t i = 0; i < a.quads.size(); ++i) {
      const FS_QUADPOINTSF q = quadFor(a.quads[i], f);
      if (i < have) FPDFAnnot_SetAttachmentPoints(h, i, &q);
      else FPDFAnnot_AppendAttachmentPoints(h, &q);
    }
  }
  if (a.kind == AnnotKind::Ink || a.kind == AnnotKind::Line) setInk(h, a.ink, f);
  setString(h, "Contents", a.contents);
  if (!a.author.empty() || !isNew) setString(h, "T", a.author);
  if (!a.subject.empty() || !isNew) setString(h, "Subj", a.subject);
  setString(h, "M", a.modified.empty() ? pdfDateNow() : a.modified);
  setString(h, kStyleKey, formatStyle(a));
  // After /CA: PDFium gives the appearance the transparency state it names (/GS) when < 1.
  if (a.kind == AnnotKind::FreeText || !a.appearance.empty()) setAppearance(h, a.appearance);
}

// Has PDFium draw (and store) the appearances it can for annotations on the page that have
// none, by rendering it once. Saved files then look the same in any reader. The page is put far
// outside the one-pixel bitmap, so its content is all clipped away and costs next to nothing
// (rendering a detailed drawing for real, even into one pixel, takes a good tenth of a second).
void generateAppearances(FPDF_PAGE page) {
  bool missing = false;
  const int n = FPDFPage_GetAnnotCount(page);
  for (int i = 0; i < n && !missing; ++i) {
    Annot h(FPDFPage_GetAnnot(page, i));
    if (h.a && pdfiumDraws(kindOf(FPDFAnnot_GetSubtype(h.a))) &&
        FPDFAnnot_GetAP(h.a, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0) <= 2)
      missing = true;
  }
  if (!missing) return;  // the renderer has drawn them all already
  std::uint32_t px = 0;
  FPDF_BITMAP bmp = FPDFBitmap_CreateEx(1, 1, FPDFBitmap_BGRA, &px, 4);
  if (!bmp) return;
  const FS_MATRIX offscreen{1, 0, 0, 1, -1e6f, -1e6f};
  const FS_RECTF clip{0, 0, 1, 1};
  FPDF_RenderPageBitmapWithMatrix(bmp, page, &offscreen, &clip, FPDF_ANNOT);
  FPDFBitmap_Destroy(bmp);
}

bool listed(int subtype) {
  return subtype != FPDF_ANNOT_POPUP && subtype != FPDF_ANNOT_LINK && subtype != FPDF_ANNOT_WIDGET &&
         subtype != FPDF_ANNOT_UNKNOWN;
}

// The strings that identify an annotation, carried over when it has to be made anew.
struct Identity {
  std::string name, created;
};

}  // namespace

void Document::flushAppearances() const {
  for (int page : needsAppearances_)
    if (page >= 0 && page < pageCount())
      if (void* p = cachedPage(page)) generateAppearances(static_cast<FPDF_PAGE>(p));
  needsAppearances_.clear();
}

PageFrame Document::pageFrame(int index) const {
  if (index < 0 || index >= pageCount()) return {};
  std::lock_guard lock(engineMutex());
  Page p(cachedPage(index));
  return p.page ? frameOf(p.page) : PageFrame{};
}

std::vector<Annotation> Document::annotations(int pageIndex) const {
  std::vector<Annotation> out;
  if (pageIndex < 0 || pageIndex >= pageCount()) return out;
  std::lock_guard lock(engineMutex());
  Page p(cachedPage(pageIndex));
  if (!p.page) return out;
  const PageFrame f = frameOf(p.page);
  const int n = FPDFPage_GetAnnotCount(p.page);
  for (int i = 0; i < n; ++i) {
    Annot h(FPDFPage_GetAnnot(p.page, i));
    if (!h.a) continue;
    const int subtype = FPDFAnnot_GetSubtype(h.a);
    if (!listed(subtype)) continue;
    Annotation a;
    a.index = i;
    a.subtype = subtype;
    a.kind = kindOf(subtype);
    a.rect = displayRectOf(h.a, f);
    a.contents = getString(h.a, "Contents");
    a.author = getString(h.a, "T");
    a.subject = getString(h.a, "Subj");
    a.modified = getString(h.a, "M");
    a.name = getString(h.a, "NM");
    a.hidden = (FPDFAnnot_GetFlags(h.a) & FPDF_ANNOT_FLAG_HIDDEN) != 0;
    float hr = 0, vr = 0, bw = 1;
    if (FPDFAnnot_GetBorder(h.a, &hr, &vr, &bw)) a.borderWidth = bw;
    float ca = 1;
    if (FPDFAnnot_GetNumberValue(h.a, "CA", &ca)) a.opacity = std::clamp(double(ca), 0.0, 1.0);

    unsigned R, G, B, A;
    const std::string ap = getAppearance(h.a);
    if (a.kind == AnnotKind::FreeText) {
      a.textColor = {0, 0, 0};
      bool hasBorder = false;
      parseDA(getString(h.a, "DA"), a, hasBorder);
      if (!hasBorder) a.color = a.textColor;
      if (FPDFAnnot_GetColor(h.a, FPDFANNOT_COLORTYPE_Color, &R, &G, &B, &A) && FPDFAnnot_HasKey(h.a, "C")) {
        a.hasFill = true;
        a.fill = {std::uint8_t(R), std::uint8_t(G), std::uint8_t(B)};
      }
    } else if (FPDFAnnot_GetColor(h.a, FPDFANNOT_COLORTYPE_Color, &R, &G, &B, &A)) {
      a.color = {std::uint8_t(R), std::uint8_t(G), std::uint8_t(B)};
      if (FPDFAnnot_GetColor(h.a, FPDFANNOT_COLORTYPE_InteriorColor, &R, &G, &B, &A) && FPDFAnnot_HasKey(h.a, "IC")) {
        a.hasFill = true;
        a.fill = {std::uint8_t(R), std::uint8_t(G), std::uint8_t(B)};
      }
    } else {
      const ColorOps ops = colorsInContent(ap);
      const bool strokeKind = a.kind == AnnotKind::Ink || a.kind == AnnotKind::Square ||
                              a.kind == AnnotKind::Circle || a.kind == AnnotKind::Line ||
                              a.kind == AnnotKind::Underline || a.kind == AnnotKind::StrikeOut ||
                              a.kind == AnnotKind::Squiggly;
      if (auto c = strokeKind ? (ops.stroke ? ops.stroke : ops.fill) : (ops.fill ? ops.fill : ops.stroke)) a.color = *c;
      if ((a.kind == AnnotKind::Square || a.kind == AnnotKind::Circle) && FPDFAnnot_HasKey(h.a, "IC") && ops.fill) {
        a.hasFill = true;
        a.fill = *ops.fill;
      }
    }
    parseStyle(getString(h.a, kStyleKey), a);  // pdfz's own record, when there is one

    if (FPDFAnnot_HasAttachmentPoints(h.a)) {
      const std::size_t nq = FPDFAnnot_CountAttachmentPoints(h.a);
      for (std::size_t q = 0; q < nq; ++q) {
        FS_QUADPOINTSF qp{};
        if (FPDFAnnot_GetAttachmentPoints(h.a, q, &qp)) a.quads.push_back(displayRectOfQuad(qp, f));
      }
    }
    const unsigned long strokes = FPDFAnnot_GetInkListCount(h.a);
    for (unsigned long s = 0; s < strokes; ++s) {
      const unsigned long np = FPDFAnnot_GetInkListPath(h.a, s, nullptr, 0);
      std::vector<FS_POINTF> pts(np);
      FPDFAnnot_GetInkListPath(h.a, s, pts.data(), np);
      std::vector<PointF> stroke;
      stroke.reserve(np);
      for (const FS_POINTF& pt : pts) stroke.push_back(f.toDisplay(PointF{pt.x, pt.y}));
      a.ink.push_back(std::move(stroke));
    }
    out.push_back(std::move(a));
  }
  return out;
}

namespace {

int createAnnotation(FPDF_PAGE page, const Annotation& a, const PageFrame& f, const Identity* keep) {
  Annot h(FPDFPage_CreateAnnot(page, subtypeFor(a.kind)));
  if (!h.a) return -1;
  FPDFAnnot_SetFlags(h.a, FPDF_ANNOT_FLAG_PRINT);
  setString(h.a, "NM", keep ? keep->name : (a.name.empty() ? uniqueName() : a.name));
  setString(h.a, "CreationDate", keep ? keep->created : pdfDateNow());
  writeAnnotation(h.a, a, f, true);
  return FPDFPage_GetAnnotIndex(page, h.a);
}

bool removeWithPopup(FPDF_PAGE page, int index) {
  int popup = -1;
  {
    Annot h(FPDFPage_GetAnnot(page, index));
    if (!h.a) return false;
    Annot linked(FPDFAnnot_GetLinkedAnnot(h.a, "Popup"));
    if (linked.a) popup = FPDFPage_GetAnnotIndex(page, linked.a);
  }
  // The later one first, so the other's index stays right.
  if (popup > index) FPDFPage_RemoveAnnot(page, popup);
  const bool ok = FPDFPage_RemoveAnnot(page, index);
  if (popup >= 0 && popup < index) FPDFPage_RemoveAnnot(page, popup);
  return ok;
}

}  // namespace

int Document::addAnnotation(int pageIndex, const Annotation& a) {
  const int subtype = subtypeFor(a.kind);
  if (subtype == FPDF_ANNOT_UNKNOWN || pageIndex < 0 || pageIndex >= pageCount()) return -1;
  if (isTextMarkup(a.kind) && a.quads.empty()) return -1;
  if ((a.kind == AnnotKind::Ink || a.kind == AnnotKind::Line) && a.ink.empty()) return -1;
  std::lock_guard lock(engineMutex());
  Page p(cachedPage(pageIndex));
  if (!p.page) return -1;
  const int index = createAnnotation(p.page, a, frameOf(p.page), nullptr);
  if (index < 0) return -1;
  needsAppearances_.insert(pageIndex);
  markEdited();
  return index;
}

int Document::updateAnnotation(int pageIndex, int index, const Annotation& a) {
  if (pageIndex < 0 || pageIndex >= pageCount() || !(a.editable() || a.kind == AnnotKind::Stamp)) return -1;
  std::lock_guard lock(engineMutex());
  Page p(cachedPage(pageIndex));
  if (!p.page) return -1;
  const PageFrame f = frameOf(p.page);
  if (a.kind == AnnotKind::Stamp) {  // the drawing stays, fitted to the new rect
    Annot h(FPDFPage_GetAnnot(p.page, index));
    if (!h.a || FPDFAnnot_GetSubtype(h.a) != FPDF_ANNOT_STAMP) return -1;
    FS_RECTF r = toFS(f.toUser(a.rect.normalized()));
    FPDFAnnot_SetRect(h.a, &r);
    setString(h.a, "Contents", a.contents);
    setString(h.a, "M", a.modified.empty() ? pdfDateNow() : a.modified);
    const int flags = FPDFAnnot_GetFlags(h.a);
    FPDFAnnot_SetFlags(h.a, flags & ~FPDF_ANNOT_FLAG_HIDDEN);
    markEdited();
    return index;
  }
  bool remake = false;
  Identity keep;
  {
    Annot h(FPDFPage_GetAnnot(p.page, index));
    if (!h.a || kindOf(FPDFAnnot_GetSubtype(h.a)) != (a.kind == AnnotKind::Line ? AnnotKind::Ink : a.kind)) return -1;
    // A fill can't be taken away in place: PDFium can set /IC (or a text box's /C) but not
    // remove it. Then the annotation is made again, under the same name, without one.
    const bool shape = a.kind == AnnotKind::Square || a.kind == AnnotKind::Circle;
    remake = !a.hasFill && ((shape && FPDFAnnot_HasKey(h.a, "IC")) ||
                            (a.kind == AnnotKind::FreeText && FPDFAnnot_HasKey(h.a, "C")));
    if (remake) {
      keep = {getString(h.a, "NM"), getString(h.a, "CreationDate")};
      if (keep.name.empty()) keep.name = uniqueName();
      if (keep.created.empty()) keep.created = pdfDateNow();
    } else {
      // FPDFAnnot_SetColor (and with it the opacity, /CA) fails while there's an appearance,
      // so it goes first. PDFium draws a new one from the new values, or (a text box) the
      // caller's new one is set last.
      if (pdfiumDraws(a.kind) || a.kind == AnnotKind::FreeText) setAppearance(h.a, {});
      const int flags = FPDFAnnot_GetFlags(h.a);
      writeAnnotation(h.a, a, f, false);
      FPDFAnnot_SetFlags(h.a, flags & ~FPDF_ANNOT_FLAG_HIDDEN);
    }
  }
  if (remake) {
    if (!removeWithPopup(p.page, index)) return -1;
    index = createAnnotation(p.page, a, f, &keep);
  }
  needsAppearances_.insert(pageIndex);
  markEdited();
  return index;
}

bool Document::moveAnnotation(int pageIndex, int index, double dx, double dy) {
  if (pageIndex < 0 || pageIndex >= pageCount()) return false;
  std::lock_guard lock(engineMutex());
  Page p(cachedPage(pageIndex));
  if (!p.page) return false;
  const PageFrame f = frameOf(p.page);
  Annot h(FPDFPage_GetAnnot(p.page, index));
  if (!h.a) return false;
  // The same shift in user space.
  const PointF o = f.toUser(PointF{0, 0});
  const PointF d = f.toUser(PointF{dx, dy});
  const float ux = float(d.x - o.x), uy = float(d.y - o.y);

  FS_RECTF r{};
  if (!FPDFAnnot_GetRect(h.a, &r)) return false;
  r.left += ux; r.right += ux; r.top += uy; r.bottom += uy;
  // The appearance is drawn into the rect, so moving the rect moves what's shown; the points
  // below move along so the annotation stays consistent for any reader that redraws it.
  if (FPDFAnnot_HasAttachmentPoints(h.a)) {
    const std::size_t nq = FPDFAnnot_CountAttachmentPoints(h.a);
    for (std::size_t q = 0; q < nq; ++q) {
      FS_QUADPOINTSF qp{};
      if (!FPDFAnnot_GetAttachmentPoints(h.a, q, &qp)) continue;
      qp.x1 += ux; qp.x2 += ux; qp.x3 += ux; qp.x4 += ux;
      qp.y1 += uy; qp.y2 += uy; qp.y3 += uy; qp.y4 += uy;
      FPDFAnnot_SetAttachmentPoints(h.a, q, &qp);
    }
  }
  const unsigned long strokes = FPDFAnnot_GetInkListCount(h.a);
  if (strokes > 0) {
    std::vector<std::vector<FS_POINTF>> all(strokes);
    for (unsigned long s = 0; s < strokes; ++s) {
      const unsigned long np = FPDFAnnot_GetInkListPath(h.a, s, nullptr, 0);
      all[s].resize(np);
      FPDFAnnot_GetInkListPath(h.a, s, all[s].data(), np);
      for (FS_POINTF& pt : all[s]) { pt.x += ux; pt.y += uy; }
    }
    FPDFAnnot_RemoveInkList(h.a);
    for (const auto& s : all) FPDFAnnot_AddInkStroke(h.a, s.data(), s.size());
  }
  FPDFAnnot_SetRect(h.a, &r);
  setString(h.a, "M", pdfDateNow());
  needsAppearances_.insert(pageIndex);  // in case changing the ink list dropped the appearance
  markEdited();
  return true;
}

bool Document::setAnnotationContents(int pageIndex, int index, const std::string& contents) {
  if (pageIndex < 0 || pageIndex >= pageCount()) return false;
  std::lock_guard lock(engineMutex());
  Page p(cachedPage(pageIndex));
  if (!p.page) return false;
  Annot h(FPDFPage_GetAnnot(p.page, index));
  if (!h.a) return false;
  setString(h.a, "Contents", contents);
  setString(h.a, "M", pdfDateNow());
  markEdited();
  return true;
}

bool Document::removeAnnotation(int pageIndex, int index) {
  if (pageIndex < 0 || pageIndex >= pageCount()) return false;
  std::lock_guard lock(engineMutex());
  Page p(cachedPage(pageIndex));
  if (!p.page) return false;
  const bool ok = removeWithPopup(p.page, index);
  if (ok) markEdited();
  return ok;
}

bool Document::setAnnotationHidden(int pageIndex, int index, bool hidden) {
  if (pageIndex < 0 || pageIndex >= pageCount()) return false;
  std::lock_guard lock(engineMutex());
  Page p(cachedPage(pageIndex));
  if (!p.page) return false;
  Annot h(FPDFPage_GetAnnot(p.page, index));
  if (!h.a) return false;
  const int flags = FPDFAnnot_GetFlags(h.a);
  return FPDFAnnot_SetFlags(h.a, hidden ? (flags | FPDF_ANNOT_FLAG_HIDDEN) : (flags & ~FPDF_ANNOT_FLAG_HIDDEN));
}

int Document::addStamp(int pageIndex, const Annotation& a, const StampDrawing& d) {
  if (pageIndex < 0 || pageIndex >= pageCount() || d.width <= 0 || d.height <= 0) return -1;
  const RectF box = a.rect.normalized();
  if (box.width() <= 0 || box.height() <= 0) return -1;
  std::lock_guard lock(engineMutex());
  Page p(cachedPage(pageIndex));
  if (!p.page) return -1;
  const PageFrame f = frameOf(p.page);
  Annot h(FPDFPage_CreateAnnot(p.page, FPDF_ANNOT_STAMP));
  if (!h.a) return -1;
  FPDFAnnot_SetFlags(h.a, FPDF_ANNOT_FLAG_PRINT);
  FS_RECTF r = toFS(f.toUser(box));
  FPDFAnnot_SetRect(h.a, &r);
  setString(h.a, "NM", a.name.empty() ? uniqueName() : a.name);
  setString(h.a, "CreationDate", pdfDateNow());
  setString(h.a, "M", a.modified.empty() ? pdfDateNow() : a.modified);
  setString(h.a, "Contents", a.contents);
  if (!a.author.empty()) setString(h.a, "T", a.author);
  if (!a.subject.empty()) setString(h.a, "Subj", a.subject);
  setString(h.a, kStyleKey, formatStyle(a));
  FPDFAnnot_SetColor(h.a, FPDFANNOT_COLORTYPE_Color, a.color.r, a.color.g, a.color.b, 255);

  // Drawing coordinates -> display points in the rect -> user space (so it's upright on a
  // rotated page too).
  auto toUser = [&](PointF q) {
    return f.toUser(PointF{box.left + q.x / d.width * box.width(), box.top + q.y / d.height * box.height()});
  };
  FPDF_DOCUMENT doc = static_cast<FPDF_DOCUMENT>(doc_);
  bool ok = true;
  if (d.imageWidth > 0 && d.imageHeight > 0 &&
      d.imageBgra.size() >= std::size_t(d.imageWidth) * std::size_t(d.imageHeight) * 4) {
    FPDF_BITMAP bmp = FPDFBitmap_Create(d.imageWidth, d.imageHeight, 1);
    FPDF_PAGEOBJECT img = bmp ? FPDFPageObj_NewImageObj(doc) : nullptr;
    if (img) {
      auto* px = static_cast<std::uint8_t*>(FPDFBitmap_GetBuffer(bmp));
      const int stride = FPDFBitmap_GetStride(bmp);
      for (int y = 0; y < d.imageHeight; ++y)
        std::copy_n(d.imageBgra.data() + std::size_t(y) * std::size_t(d.imageWidth) * 4, std::size_t(d.imageWidth) * 4,
                    px + std::ptrdiff_t(y) * stride);
      // The image's unit square, bottom-left first, onto the box's corners in user space.
      const PointF bl = toUser({0, d.height}), br = toUser({d.width, d.height}), tl = toUser({0, 0});
      ok = FPDFImageObj_SetBitmap(nullptr, 0, img, bmp) &&
           FPDFImageObj_SetMatrix(img, br.x - bl.x, br.y - bl.y, tl.x - bl.x, tl.y - bl.y, bl.x, bl.y) &&
           FPDFAnnot_AppendObject(h.a, img);
      if (!ok) FPDFPageObj_Destroy(img);
    } else {
      ok = false;
    }
    if (bmp) FPDFBitmap_Destroy(bmp);
  }
  for (const StampDrawing::Path& path : d.paths) {
    if (!ok || path.ops.empty() || path.ops.front().kind != StampDrawing::Op::MoveTo) continue;
    const PointF start = toUser(path.ops.front().p[0]);
    FPDF_PAGEOBJECT obj = FPDFPageObj_CreateNewPath(float(start.x), float(start.y));
    if (!obj) {
      ok = false;
      break;
    }
    for (std::size_t i = 1; i < path.ops.size(); ++i) {
      const StampDrawing::Op& op = path.ops[i];
      switch (op.kind) {
        case StampDrawing::Op::MoveTo: {
          const PointF q = toUser(op.p[0]);
          FPDFPath_MoveTo(obj, float(q.x), float(q.y));
          break;
        }
        case StampDrawing::Op::LineTo: {
          const PointF q = toUser(op.p[0]);
          FPDFPath_LineTo(obj, float(q.x), float(q.y));
          break;
        }
        case StampDrawing::Op::CubicTo: {
          const PointF c1 = toUser(op.p[0]), c2 = toUser(op.p[1]), e = toUser(op.p[2]);
          FPDFPath_BezierTo(obj, float(c1.x), float(c1.y), float(c2.x), float(c2.y), float(e.x), float(e.y));
          break;
        }
        case StampDrawing::Op::Close: FPDFPath_Close(obj); break;
      }
    }
    FPDFPageObj_SetFillColor(obj, path.color.r, path.color.g, path.color.b, 255);
    FPDFPath_SetDrawMode(obj, path.evenOdd ? FPDF_FILLMODE_ALTERNATE : FPDF_FILLMODE_WINDING, 0);
    if (!FPDFAnnot_AppendObject(h.a, obj)) {
      FPDFPageObj_Destroy(obj);
      ok = false;
    }
  }
  const int index = FPDFPage_GetAnnotIndex(p.page, h.a);
  if (!ok) {
    h.close();
    FPDFPage_RemoveAnnot(p.page, index);
    return -1;
  }
  markEdited();
  return index;
}

// For the other parts of pdfz built on the same PDFium helpers (forms.cpp).
namespace detail {
std::u16string toUtf16(const std::string& s) { return utf8ToUtf16(s); }
std::string toUtf8(const std::u16string& s) { return utf16ToUtf8(s); }
PageFrame frameOfPage(void* page) { return frameOf(static_cast<FPDF_PAGE>(page)); }
}  // namespace detail

}  // namespace pdfz
