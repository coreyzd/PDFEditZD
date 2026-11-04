// SPDX-License-Identifier: GPL-3.0-only
#include "core/text.hpp"

#include "core/document.hpp"
#include "core/engine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

namespace pdfz {

bool isLineBreak(char32_t c) { return c == U'\r' || c == U'\n'; }

namespace {

bool isSpace(char32_t c) { return c == U' ' || c == U'\t' || c == 0xA0 || isLineBreak(c) || c == 0x3000; }

// Letters and digits in any script count as word characters, as do apostrophes inside words.
// Only ASCII punctuation and spaces separate words.
bool isWordChar(char32_t c) {
  if (isSpace(c)) return false;
  if (c < 0x80) return (c >= U'0' && c <= U'9') || (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z') ||
                       c == U'\'' || c == U'_';
  switch (c) {  // the common typographic punctuation outside ASCII
    case 0x2013: case 0x2014: case 0x2026: case 0x201C: case 0x201D: case 0x2018:
    case 0x00AB: case 0x00BB: case 0x00BF: case 0x00A1:
      return false;
    default:
      return true;  // letters of any script, CJK, the typographic apostrophe U+2019...
  }
}

void appendUtf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out += char(c);
  } else if (c < 0x800) {
    out += char(0xC0 | (c >> 6));
    out += char(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += char(0xE0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  } else {
    out += char(0xF0 | (c >> 18));
    out += char(0x80 | ((c >> 12) & 0x3F));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  }
}

// Two boxes are on the same line when their vertical extents overlap by at least half the
// smaller one.
bool sameLine(const RectF& a, const RectF& b) {
  const double overlap = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
  return overlap >= 0.5 * std::min(a.height(), b.height());
}

}  // namespace

RectF PageText::toDisplay(const RectF& r) const {
  switch (rotation) {
    case 90: return {height - r.bottom, r.left, height - r.top, r.right};
    case 180: return {width - r.right, height - r.bottom, width - r.left, height - r.top};
    case 270: return {r.top, width - r.right, r.bottom, width - r.left};
    default: return r;
  }
}

void PageText::toUpright(double x, double y, double& u, double& v) const {
  switch (rotation) {
    case 90: u = y; v = height - x; break;
    case 180: u = width - x; v = height - y; break;
    case 270: u = width - y; v = x; break;
    default: u = x; v = y; break;
  }
}

int PageText::charAt(double x, double y, double tolerance) const {
  double u, v;
  toUpright(x, y, u, v);
  for (int i = 0; i < size(); ++i) {
    const RectF& b = boxes[i];
    if (b.empty()) continue;
    if (u >= b.left - tolerance && u <= b.right + tolerance && v >= b.top - tolerance && v <= b.bottom + tolerance)
      return i;
  }
  return -1;
}

int PageText::caretAt(double x, double y) const {
  double u, v;
  toUpright(x, y, u, v);
  int best = -1;
  double bestScore = std::numeric_limits<double>::max();
  for (int i = 0; i < size(); ++i) {
    const RectF& b = boxes[i];
    if (b.empty()) continue;
    const double dy = v < b.top ? b.top - v : (v > b.bottom ? v - b.bottom : 0);
    const double dx = u < b.left ? b.left - u : (u > b.right ? u - b.right : 0);
    // Being on the line matters far more than being near the character along it.
    const double score = dy * 1000 + dx;
    if (score < bestScore) {
      bestScore = score;
      best = i;
    }
  }
  if (best < 0) return -1;
  const RectF& b = boxes[best];
  return u > (b.left + b.right) / 2 ? best + 1 : best;
}

std::pair<int, int> PageText::wordAt(int index) const {
  if (index < 0 || index >= size()) return {0, 0};
  if (!isWordChar(chars[index])) return {index, index + 1};
  int s = index;
  int e = index + 1;
  while (s > 0 && isWordChar(chars[s - 1])) --s;
  while (e < size() && isWordChar(chars[e])) ++e;
  return {s, e};
}

std::pair<int, int> PageText::lineAt(int index) const {
  if (index < 0 || index >= size()) return {0, 0};
  int s = index;
  int e = index;
  while (s > 0 && !isLineBreak(chars[s - 1])) --s;
  while (e < size() && !isLineBreak(chars[e])) ++e;
  return {s, e};
}

std::vector<RectF> PageText::selectionRects(int start, int end) const {
  std::vector<RectF> out;
  start = std::max(start, 0);
  end = std::min(end, size());
  RectF run;
  for (int i = start; i < end; ++i) {
    const RectF& b = boxes[i];
    if (isLineBreak(chars[i])) {
      if (!run.empty()) out.push_back(toDisplay(run));
      run = {};
      continue;
    }
    if (b.empty()) continue;
    // A new run where the text jumps to another line, or back to the left (a column break).
    if (!run.empty() && (!sameLine(run, b) || b.right < run.left)) {
      out.push_back(toDisplay(run));
      run = {};
    }
    run = run.united(b);
  }
  if (!run.empty()) out.push_back(toDisplay(run));
  return out;
}

std::string PageText::textUtf8(int start, int end) const {
  std::string out;
  start = std::max(start, 0);
  end = std::min(end, size());
  for (int i = start; i < end; ++i) {
    char32_t c = chars[i];
    if (c == U'\r') continue;  // PDFium's "\r\n": keep the "\n"
    // Some builds give characters outside the BMP as two UTF-16 halves: put them back together.
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < end && chars[i + 1] >= 0xDC00 && chars[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (chars[i + 1] - 0xDC00);
      ++i;
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
    if (c == 0) continue;
    appendUtf8(out, c);
  }
  return out;
}

PageText Document::extractText(int index) const {
  PageText out;
  if (index < 0 || index >= pageCount()) return out;
  std::lock_guard lock(engineMutex());
  FPDF_PAGE page = static_cast<FPDF_PAGE>(cachedPage(index));
  if (!page) return out;
  FS_RECTF bbox{};
  if (!FPDF_GetPageBoundingBox(page, &bbox)) return out;
  // FS_RECTF for a page box: top is the larger y (PDF y grows upwards).
  const double x0 = std::min(bbox.left, bbox.right);
  const double y1 = std::max(bbox.top, bbox.bottom);
  out.width = std::abs(bbox.right - bbox.left);
  out.height = std::abs(bbox.top - bbox.bottom);
  out.rotation = FPDFPage_GetRotation(page) * 90;

  if (FPDF_TEXTPAGE tp = FPDFText_LoadPage(page)) {
    const int n = std::max(0, FPDFText_CountChars(tp));
    out.chars.resize(std::size_t(n));
    out.boxes.resize(std::size_t(n));
    for (int i = 0; i < n; ++i) {
      out.chars[i] = char32_t(FPDFText_GetUnicode(tp, i));
      FS_RECTF r{};
      if (FPDFText_IsGenerated(tp, i) == 1 || !FPDFText_GetLooseCharBox(tp, i, &r)) continue;
      RectF box{r.left - x0, y1 - std::max(r.top, r.bottom), r.right - x0, y1 - std::min(r.top, r.bottom)};
      if (box.left > box.right) std::swap(box.left, box.right);
      out.boxes[i] = box;
    }
    FPDFText_ClosePage(tp);
  }
  return out;
}

}  // namespace pdfz
