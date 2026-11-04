// SPDX-License-Identifier: GPL-3.0-only
#include <doctest.h>

#include "core/document.hpp"
#include "core/engine.hpp"
#include "core/text.hpp"

#include <cmath>
#include <string>

#include <fpdf_edit.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>

using pdfz::Document;
using pdfz::PageText;
using pdfz::RectF;

namespace {

// A 300 x 400 pt page whose media box starts at (100, 200), not the origin, with two lines of
// 20 pt Helvetica: "Hello world" with its baseline at y = 560 and "Second line" at y = 520
// (upright: 40 and 80 points from the top), both starting at x = 120 (20 from the left).
std::unique_ptr<Document> textDoc() {
  auto doc = Document::create();
  REQUIRE(doc->insertBlankPage(0, {300, 400}));
  {
    std::lock_guard lock(pdfz::engineMutex());
    auto* fdoc = static_cast<FPDF_DOCUMENT>(doc->handle());
    FPDF_PAGE page = FPDF_LoadPage(fdoc, 0);
    REQUIRE(page);
    FPDFPage_SetMediaBox(page, 100, 200, 400, 600);
    FPDF_FONT font = FPDFText_LoadStandardFont(fdoc, "Helvetica");
    REQUIRE(font);
    auto addLine = [&](const std::u16string& s, double y) {
      FPDF_PAGEOBJECT t = FPDFPageObj_CreateTextObj(fdoc, font, 20.0f);
      std::u16string z = s + u'\0';
      FPDFText_SetText(t, reinterpret_cast<FPDF_WIDESTRING>(z.c_str()));
      FPDFPageObj_Transform(t, 1, 0, 0, 1, 120, y);
      FPDFPage_InsertObject(page, t);
    };
    addLine(u"Hello world", 560);
    addLine(u"Second line", 520);
    REQUIRE(FPDFPage_GenerateContent(page));
    FPDF_ClosePage(page);
    FPDFFont_Close(font);
  }
  auto [opened, err] = Document::openMemory(doc->saveToMemory());
  REQUIRE(opened);
  return std::move(opened);
}

int indexOf(const PageText& t, char32_t c, int from = 0) {
  for (int i = from; i < t.size(); ++i)
    if (t.chars[i] == c) return i;
  return -1;
}

bool dark(std::uint32_t argb) { return ((argb >> 8) & 0xFF) < 160; }  // green channel

}  // namespace

TEST_CASE("extracting text and where it is") {
  auto doc = textDoc();
  const PageText t = doc->extractText(0);
  REQUIRE_FALSE(t.empty());
  CHECK(t.width == doctest::Approx(300));
  CHECK(t.height == doctest::Approx(400));
  CHECK(t.rotation == 0);
  CHECK(t.textUtf8(0, t.size()) == "Hello world\nSecond line");
  REQUIRE(t.boxes.size() == t.chars.size());

  const RectF h = t.boxes[0];  // "H"
  CHECK(h.left == doctest::Approx(20).epsilon(0.05));
  CHECK(h.top < 40);     // the box reaches above the baseline...
  CHECK(h.top > 15);
  CHECK(h.bottom >= 40);  // ...and down to the font's descent
  CHECK(h.bottom < 50);

  SUBCASE("hit testing") {
    CHECK(t.charAt(h.left + 1, 35) == 0);
    CHECK(t.charAt(5, 5) == -1);
    CHECK(t.charAt(h.left - 2, 35, 3) == 0);  // within the tolerance
    CHECK(t.caretAt(h.left + 1, 35) == 0);    // left half of "H": before it
    CHECK(t.caretAt(h.right - 1, 35) == 1);   // right half: after it
    CHECK(t.caretAt(0, 35) == 0);             // left of the line: its start
    const int s2 = indexOf(t, U'S');
    CHECK(t.caretAt(5, 75) == s2);            // left of the second line
    CHECK(t.caretAt(290, 33) == indexOf(t, U'd') + 1);  // right of the first line: its end
  }

  SUBCASE("words and lines") {
    const int w = indexOf(t, U'w');
    auto [ws, we] = t.wordAt(w + 2);
    CHECK(t.textUtf8(ws, we) == "world");
    auto [hs, he] = t.wordAt(1);
    CHECK(t.textUtf8(hs, he) == "Hello");
    auto [ss, se] = t.wordAt(5);  // the space
    CHECK(se - ss == 1);
    auto [ls, le] = t.lineAt(indexOf(t, U'S') + 3);
    CHECK(t.textUtf8(ls, le) == "Second line");
  }

  SUBCASE("selection rectangles: one per line") {
    auto rects = t.selectionRects(0, t.size());
    REQUIRE(rects.size() == 2);
    CHECK(rects[0].bottom <= rects[1].top + 1);
    CHECK(rects[0].left == doctest::Approx(20).epsilon(0.05));
    auto part = t.selectionRects(2, 4);  // "ll"
    REQUIRE(part.size() == 1);
    CHECK(part[0].left > rects[0].left);
    CHECK(part[0].right < rects[0].right);
    CHECK(t.selectionRects(3, 3).empty());
  }
}

TEST_CASE("text boxes line up with the rendered text at every rotation") {
  for (int rot : {0, 90, 180, 270}) {
    CAPTURE(rot);
    auto doc = textDoc();
    REQUIRE(doc->setPageRotation(0, rot));
    const PageText t = doc->extractText(0);
    CHECK(t.rotation == rot);
    const pdfz::SizeF size = doc->pageSize(0);
    const RectF shown = t.toDisplay({0, 0, t.width, t.height});
    CHECK(shown.width() == doctest::Approx(size.width));
    CHECK(shown.height() == doctest::Approx(size.height));

    const auto rects = t.selectionRects(0, t.size());
    REQUIRE(rects.size() == 2);
    const auto bmp = doc->render(0, 1.0, {0, 0, int(size.width), int(size.height)});
    int inside = 0, outside = 0;
    for (int y = 0; y < bmp.height; ++y)
      for (int x = 0; x < bmp.width; ++x) {
        if (!dark(bmp.pixel(x, y))) continue;
        bool in = false;
        for (const RectF& r : rects)
          if (x + 0.5 >= r.left - 1 && x + 0.5 <= r.right + 1 && y + 0.5 >= r.top - 1 && y + 0.5 <= r.bottom + 1)
            in = true;
        (in ? inside : outside)++;
      }
    CHECK(inside > 200);
    CHECK(outside == 0);

    // A display point inside the first character finds it, whatever the rotation.
    const RectF h = t.toDisplay(t.boxes[0]);
    CHECK(t.charAt((h.left + h.right) / 2, (h.top + h.bottom) / 2) == 0);
    // A round trip through the two frames.
    double u, v;
    t.toUpright((h.left + h.right) / 2, (h.top + h.bottom) / 2, u, v);
    CHECK(u == doctest::Approx((t.boxes[0].left + t.boxes[0].right) / 2));
    CHECK(v == doctest::Approx((t.boxes[0].top + t.boxes[0].bottom) / 2));
  }
}

TEST_CASE("pages without text") {
  auto doc = Document::create();
  REQUIRE(doc->insertBlankPage(0, pdfz::kLetter));
  const PageText t = doc->extractText(0);
  CHECK(t.empty());
  CHECK(t.caretAt(10, 10) == -1);
  CHECK(t.selectionRects(0, 5).empty());
  CHECK(doc->extractText(3).empty());
}

TEST_CASE("UTF-8 output joins split surrogate pairs") {
  PageText t;
  t.chars = {U'a', char32_t(0xD83D), char32_t(0xDE00), U'\r', U'\n', U'b', char32_t(0xD800)};
  t.boxes.resize(t.chars.size());
  CHECK(t.textUtf8(0, t.size()) == "a\xF0\x9F\x98\x80\nb\xEF\xBF\xBD");
}
