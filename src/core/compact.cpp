// SPDX-License-Identifier: GPL-3.0-only
// Dropping the objects nothing uses from a PDF that PDFium wrote.
//
// PDFium writes every object it has ever held, used or not. Edits leave objects behind (Edit
// PDF reads a page's content through a copy of its resources: the fonts and pictures the page
// used before the edit stay in the file), so a file grew with every edit. PDFium writes plain
// files (every object on its own, a classic cross-reference table, no object streams), which
// makes this easy: keep what can be reached from the trailer, leave out the rest, and write a
// new cross-reference table. Anything unexpected and the file is kept as it is.
//
// On the way, annotations that came without an appearance are written without the one PDFium
// made for them (see withoutGeneratedAppearance).
#include "core/compact.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace pdfz {

namespace {

bool isWhite(unsigned char c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
bool isDelim(unsigned char c) {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
         c == '%';
}

// A token reader over the bytes: numbers, keywords, names, strings and brackets, one by one.
struct Lexer {
  const std::uint8_t* d;
  std::size_t n, pos;

  void skip() {
    while (pos < n) {
      if (isWhite(d[pos])) {
        ++pos;
      } else if (d[pos] == '%') {
        while (pos < n && d[pos] != '\n' && d[pos] != '\r') ++pos;
      } else {
        break;
      }
    }
  }

  // The next token; empty at the end. Strings come back whole: "(...)" or "<...>".
  std::string_view next() {
    skip();
    if (pos >= n) return {};
    const std::size_t start = pos;
    const unsigned char c = d[pos];
    auto view = [&] { return std::string_view(reinterpret_cast<const char*>(d + start), pos - start); };
    if (c == '(') {  // a literal string: nested brackets, backslash escapes
      int depth = 0;
      while (pos < n) {
        const unsigned char k = d[pos++];
        if (k == '\\') ++pos;
        else if (k == '(') ++depth;
        else if (k == ')' && --depth == 0) break;
      }
      return view();
    }
    if (c == '<' || c == '>') {
      if (pos + 1 < n && d[pos + 1] == c) {  // << or >>
        pos += 2;
        return view();
      }
      if (c == '<') {  // a hex string
        while (pos < n && d[pos] != '>') ++pos;
        if (pos < n) ++pos;
        return view();
      }
      ++pos;
      return view();
    }
    if (c == '[' || c == ']' || c == '{' || c == '}' || c == ')') {
      ++pos;
      return view();
    }
    if (c == '/') ++pos;
    while (pos < n && !isWhite(d[pos]) && !isDelim(d[pos])) ++pos;
    if (pos == start) ++pos;  // a stray character: take it, so reading goes on
    return view();
  }
};

bool isUInt(std::string_view t) {
  return !t.empty() && t.size() < 11 && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; });
}
long toLong(std::string_view t) { return std::strtol(std::string(t).c_str(), nullptr, 10); }

// The objects referred to ("12 0 R") from `pos` up to "stream" or "endobj" (or the end of the
// trailer's dictionary, if `trailer`).
bool referencesFrom(const std::vector<std::uint8_t>& pdf, std::size_t pos, bool trailer, std::vector<long>& refs) {
  Lexer lx{pdf.data(), pdf.size(), pos};
  std::string_view a, b;  // the two tokens before this one
  int depth = 0;
  for (int guard = 0; guard < 50'000'000; ++guard) {
    const std::string_view t = lx.next();
    if (t.empty()) return trailer;  // the end without endobj: only all right after a trailer
    if (t == "stream" || t == "endobj") return !trailer;
    if (t == "R" && isUInt(a) && isUInt(b)) refs.push_back(toLong(a));
    if (t == "<<") ++depth;
    if (t == ">>" && --depth == 0 && trailer) return true;
    a = b;
    b = t;
  }
  return false;
}

// An annotation's dictionary without the appearance PDFium made for it ("/AP" and the
// "/PDFIUM_HasGeneratedAP true" that marks it), for one that came without: AutoCAD's text
// comments (an invisible /Square for each piece of SHX text, so it can be searched) have none,
// and Acrobat, finding one, shows each with a comment icon. Not for pdfz's own annotations
// (they have /PDFZStyle, or an /NM of "pdfz-..."), whose appearance PDFium may well have made.
// `body`: the object's bytes from just after "obj". Empty if there's nothing to take out.
std::string withoutGeneratedAppearance(std::string_view body) {
  if (body.find("PDFIUM_HasGeneratedAP") == std::string_view::npos) return {};
  const auto* d = reinterpret_cast<const std::uint8_t*>(body.data());
  Lexer lx{d, body.size(), 0};
  if (lx.next() != "<<") return {};
  struct Entry {
    std::string_view key;
    std::size_t start, end;  // the key and its value, in `body`
    std::string_view first;  // the value's first token
  };
  std::vector<Entry> entries;
  std::size_t close = 0;
  for (int guard = 0; guard < 10'000; ++guard) {
    lx.skip();
    const std::size_t start = lx.pos;
    const std::string_view key = lx.next();
    if (key == ">>") {
      close = lx.pos;
      break;
    }
    if (key.empty() || key[0] != '/') return {};
    const std::string_view first = lx.next();
    if (first.empty() || first == ">>") return {};
    if (first == "<<" || first == "[") {  // to its matching end
      int depth = 1;
      while (depth > 0) {
        const std::string_view t = lx.next();
        if (t.empty()) return {};
        if (t == "<<" || t == "[") ++depth;
        if (t == ">>" || t == "]") --depth;
      }
    } else if (isUInt(first)) {  // a number, or "12 0 R"
      const std::size_t back = lx.pos;
      const std::string_view gen = lx.next(), r = lx.next();
      if (!(isUInt(gen) && r == "R")) lx.pos = back;
    }
    entries.push_back({key, start, lx.pos, first});
  }
  if (!close) return {};
  bool generated = false, ours = false;
  for (const Entry& e : entries) {
    if (e.key == "/PDFIUM_HasGeneratedAP" && e.first == "true") generated = true;
    if (e.key == "/PDFZStyle") ours = true;
    if (e.key == "/NM" && e.first.substr(0, 6) == "(pdfz-") ours = true;
    if (e.key == "/Subtype" && e.first == "/Widget") ours = true;  // form fields: never touched here
  }
  if (!generated || ours) return {};
  std::string out = "<<";
  for (const Entry& e : entries)
    if (e.key != "/AP" && e.key != "/PDFIUM_HasGeneratedAP") out.append(body.data() + e.start, e.end - e.start);
  out += ">>";
  out.append(body.data() + close, body.size() - close);
  return out;
}

}  // namespace

std::vector<std::uint8_t> withoutUnusedObjects(const std::vector<std::uint8_t>& pdf) {
  // startxref <offset> %%EOF, near the end.
  const std::string_view all(reinterpret_cast<const char*>(pdf.data()), pdf.size());
  const std::size_t sx = all.rfind("startxref");
  if (sx == std::string_view::npos) return pdf;
  Lexer lx{pdf.data(), pdf.size(), sx + 9};
  const std::string_view xrefTok = lx.next();
  if (!isUInt(xrefTok)) return pdf;
  const std::size_t xrefAt = std::size_t(toLong(xrefTok));
  if (xrefAt >= pdf.size()) return pdf;

  // The classic cross-reference table: subsections of "offset generation n|f".
  lx.pos = xrefAt;
  if (lx.next() != "xref") return pdf;  // a cross-reference stream: not PDFium's way, left alone
  std::map<long, std::pair<std::size_t, long>> used;  // object -> (offset, generation)
  std::size_t trailerAt = 0;
  for (;;) {
    const std::string_view first = lx.next();
    if (first == "trailer") {
      trailerAt = lx.pos;
      break;
    }
    const std::string_view count = lx.next();
    if (!isUInt(first) || !isUInt(count)) return pdf;
    const long start = toLong(first), cnt = toLong(count);
    for (long i = 0; i < cnt; ++i) {
      const std::string_view off = lx.next(), gen = lx.next(), type = lx.next();
      if (!isUInt(off) || !isUInt(gen) || (type != "n" && type != "f")) return pdf;
      if (type == "n" && toLong(off) > 0) used[start + i] = {std::size_t(toLong(off)), toLong(gen)};
    }
  }
  // The trailer's dictionary, as written, and what it refers to.
  Lexer tl{pdf.data(), pdf.size(), trailerAt};
  tl.skip();
  const std::size_t dictStart = tl.pos;
  std::vector<long> roots;
  if (!referencesFrom(pdf, dictStart, true, roots)) return pdf;
  long size = 0;  // the table's length: the trailer's /Size, or more if objects go further
  {  // the dictionary's end: where referencesFrom stopped, found again
    int depth = 0;
    std::string_view before;
    for (;;) {
      const std::string_view t = tl.next();
      if (t.empty()) return pdf;
      if (t == "/Prev") return pdf;  // an incremental update: left alone
      if (before == "/Size" && depth == 1 && isUInt(t)) size = toLong(t);
      if (t == "<<") ++depth;
      if (t == ">>" && --depth == 0) break;
      before = t;
    }
  }
  const std::size_t dictEnd = tl.pos;
  for (const auto& [num, _] : used) size = std::max(size, num + 1);

  // Each object's bytes: from its offset to the next object's (or the table's).
  std::vector<std::pair<std::size_t, long>> order;  // (offset, object)
  for (const auto& [num, og] : used) order.emplace_back(og.first, num);
  std::sort(order.begin(), order.end());
  if (order.empty() || order.front().first > xrefAt) return pdf;
  std::map<long, std::pair<std::size_t, std::size_t>> span;  // object -> [offset, end)
  for (std::size_t i = 0; i < order.size(); ++i) {
    const auto [off, num] = order[i];
    const std::size_t end = i + 1 < order.size() ? order[i + 1].first : xrefAt;
    if (end < off || end > pdf.size()) return pdf;
    span[num] = {off, end};
  }

  // Annotations given back the appearance they came without (see withoutGeneratedAppearance):
  // "num gen obj" and the object again.
  std::map<long, std::vector<std::uint8_t>> rewritten;
  for (const auto& [num, se] : span) {
    const std::string_view obj(all.data() + se.first, se.second - se.first);
    if (obj.find("PDFIUM_HasGeneratedAP") == std::string_view::npos) continue;
    Lexer ol{pdf.data(), se.second, se.first};
    if (ol.next() != std::to_string(num)) continue;
    ol.next();
    if (ol.next() != "obj") continue;
    const std::size_t bodyAt = ol.pos;
    const std::string body = withoutGeneratedAppearance(obj.substr(bodyAt - se.first));
    if (body.empty()) continue;
    std::vector<std::uint8_t> bytes(pdf.begin() + std::ptrdiff_t(se.first), pdf.begin() + std::ptrdiff_t(bodyAt));
    bytes.insert(bytes.end(), body.begin(), body.end());
    rewritten[num] = std::move(bytes);
  }

  // What can be reached from the trailer.
  std::set<long> keep;
  std::vector<long> todo = roots;
  while (!todo.empty()) {
    const long num = todo.back();
    todo.pop_back();
    if (keep.count(num)) continue;
    auto it = used.find(num);
    if (it == used.end()) continue;  // a reference to nothing: ignored, as readers do
    keep.insert(num);
    // "num gen obj", then its value.
    const auto rw = rewritten.find(num);
    const std::vector<std::uint8_t>& src = rw != rewritten.end() ? rw->second : pdf;
    Lexer ol{src.data(), src.size(), rw != rewritten.end() ? 0 : it->second.first};
    if (ol.next() != std::to_string(num)) return pdf;
    ol.next();
    if (ol.next() != "obj") return pdf;
    std::vector<long> refs;
    if (!referencesFrom(src, ol.pos, false, refs)) return pdf;
    todo.insert(todo.end(), refs.begin(), refs.end());
  }
  if (keep.size() == used.size() && rewritten.empty()) return pdf;  // nothing to change

  std::vector<std::uint8_t> out(pdf.begin(), pdf.begin() + std::ptrdiff_t(order.front().first));  // the header
  std::map<long, std::size_t> offsets;
  for (const auto& [off, num] : order) {
    if (!keep.count(num)) continue;
    offsets[num] = out.size();
    if (const auto rw = rewritten.find(num); rw != rewritten.end()) {
      out.insert(out.end(), rw->second.begin(), rw->second.end());
      continue;
    }
    const std::size_t end = span[num].second;
    out.insert(out.end(), pdf.begin() + std::ptrdiff_t(off), pdf.begin() + std::ptrdiff_t(end));
  }
  if (out.back() != '\n' && out.back() != '\r') out.push_back('\n');

  // A new table: the kept objects where they now are; the others free, in one chain.
  const std::size_t newXref = out.size();
  std::string table = "xref\n0 " + std::to_string(size) + "\n";
  std::vector<long> freed;
  for (long i = 1; i < size; ++i)
    if (!offsets.count(i)) freed.push_back(i);
  char entry[48];
  std::snprintf(entry, sizeof entry, "%010ld 65535 f\r\n", freed.empty() ? 0L : freed.front());
  table += entry;
  std::size_t f = 0;
  for (long i = 1; i < size; ++i) {
    if (auto it = offsets.find(i); it != offsets.end()) {
      std::snprintf(entry, sizeof entry, "%010zu %05ld n\r\n", it->second, used[i].second);
    } else {
      const long next = f + 1 < freed.size() ? freed[f + 1] : 0;
      const long gen = used.count(i) ? std::min(used[i].second + 1, 65535L) : 0L;
      std::snprintf(entry, sizeof entry, "%010ld %05ld f\r\n", next, gen);
      ++f;
    }
    table += entry;
  }
  table += "trailer\n";
  table.append(reinterpret_cast<const char*>(pdf.data()) + dictStart, dictEnd - dictStart);
  table += "\nstartxref\n" + std::to_string(newXref) + "\n%%EOF\n";
  out.insert(out.end(), table.begin(), table.end());
  return out;
}

}  // namespace pdfz
