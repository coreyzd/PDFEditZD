// SPDX-License-Identifier: GPL-3.0-only
#include <doctest.h>

#include "core/annotations.hpp"
#include "core/document.hpp"
#include "core/geometry.hpp"

#include <cmath>
#include <string>

#include <fpdf_annot.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>

#include "core/engine.hpp"

using pdfz::AnnotKind;
using pdfz::Annotation;
using pdfz::Color;
using pdfz::Document;
using pdfz::PageFrame;
using pdfz::PointF;
using pdfz::RectF;

namespace {

std::unique_ptr<Document> blank(double w = 300, double h = 400, int rotation = 0, bool offsetBox = false) {
  auto doc = Document::create();
  REQUIRE(doc->insertBlankPage(0, {w, h}));
  if (offsetBox) {
    std::lock_guard lock(pdfz::engineMutex());
    FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(doc->handle()), 0);
    FPDFPage_SetMediaBox(page, 100, 200, float(100 + w), float(200 + h));
    FPDF_ClosePage(page);
  }
  if (rotation) REQUIRE(doc->setPageRotation(0, rotation));
  auto [opened, err] = Document::openMemory(doc->saveToMemory());
  REQUIRE(opened);
  return std::move(opened);
}

std::unique_ptr<Document> reopen(const Document& d) {
  auto [opened, err] = Document::openMemory(d.saveToMemory());
  REQUIRE(opened);
  return std::move(opened);
}

// Pixels of a color (roughly) within a display rect, rendered at 1 px per point.
int countColor(const Document& doc, const RectF& r, Color want, int tolerance = 60) {
  const pdfz::SizeF s = doc.pageSize(0);
  const auto bmp = doc.render(0, 1.0, {0, 0, int(s.width), int(s.height)});
  int n = 0;
  for (int y = std::max(0, int(r.top)); y < std::min(bmp.height, int(std::ceil(r.bottom))); ++y)
    for (int x = std::max(0, int(r.left)); x < std::min(bmp.width, int(std::ceil(r.right))); ++x) {
      const std::uint32_t p = bmp.pixel(x, y);
      const int pr = (p >> 16) & 0xFF, pg = (p >> 8) & 0xFF, pb = p & 0xFF;
      if (std::abs(pr - want.r) <= tolerance && std::abs(pg - want.g) <= tolerance && std::abs(pb - want.b) <= tolerance)
        ++n;
    }
  return n;
}

bool nearRect(const RectF& a, const RectF& b, double eps = 0.6) {
  return std::abs(a.left - b.left) < eps && std::abs(a.top - b.top) < eps && std::abs(a.right - b.right) < eps &&
         std::abs(a.bottom - b.bottom) < eps;
}

}  // namespace

TEST_CASE("page frames map user space to display and back") {
  for (int rot : {0, 90, 180, 270}) {
    CAPTURE(rot);
    PageFrame f{100, 200, 400, 600, rot};  // 300 x 400 upright, box not at the origin
    const PointF corners[] = {{100, 600}, {400, 600}, {100, 200}, {400, 200}};
    for (PointF c : corners) {
      const PointF d = f.toDisplay(c);
      const PointF back = f.toUser(d);
      CHECK(back.x == doctest::Approx(c.x));
      CHECK(back.y == doctest::Approx(c.y));
      const double w = (rot == 90 || rot == 270) ? 400 : 300;
      const double h = (rot == 90 || rot == 270) ? 300 : 400;
      CHECK(d.x >= -1e-9);
      CHECK(d.x <= w + 1e-9);
      CHECK(d.y >= -1e-9);
      CHECK(d.y <= h + 1e-9);
    }
    const PointF tl = f.toDisplay(PointF{100, 600});  // the upright top-left corner
    if (rot == 0) CHECK(tl == PointF{0, 0});
    if (rot == 90) CHECK(tl == PointF{400, 0});  // turned clockwise: top-left goes top-right
  }
}

TEST_CASE("adding and reading back each kind") {
  auto doc = blank(300, 400, 0, true);
  CHECK(doc->annotations(0).empty());

  Annotation hl;
  hl.kind = AnnotKind::Highlight;
  hl.color = {255, 220, 0};
  hl.quads = {{20, 20, 200, 40}, {20, 45, 120, 65}};
  hl.author = "Corey";
  hl.contents = "Check this – é";
  CHECK(doc->addAnnotation(0, hl) == 0);

  Annotation note;
  note.kind = AnnotKind::Note;
  note.rect = {250, 20, 270, 40};
  note.color = {255, 200, 0};
  note.contents = "A sticky note";
  CHECK(doc->addAnnotation(0, note) == 1);

  Annotation ink;
  ink.kind = AnnotKind::Ink;
  ink.color = {0, 0, 255};
  ink.borderWidth = 3;
  ink.ink = {{{30, 100}, {80, 150}, {130, 100}}};
  CHECK(doc->addAnnotation(0, ink) == 2);

  Annotation sq;
  sq.kind = AnnotKind::Square;
  sq.rect = {150, 200, 250, 300};
  sq.color = {200, 0, 0};
  sq.borderWidth = 4;
  CHECK(doc->addAnnotation(0, sq) == 3);

  Annotation circ;
  circ.kind = AnnotKind::Circle;
  circ.rect = {20, 300, 120, 380};
  circ.color = {0, 150, 0};
  circ.hasFill = true;
  circ.fill = {200, 255, 200};
  CHECK(doc->addAnnotation(0, circ) == 4);

  Annotation ft;
  ft.kind = AnnotKind::FreeText;
  ft.rect = {150, 320, 290, 360};
  ft.textColor = {0, 0, 200};
  ft.color = {0, 0, 200};
  ft.fontSize = 14;
  ft.borderWidth = 0;
  ft.contents = "Text box";
  ft.appearance = "0 0 .8 rg 360 250 20 20 re f";  // stands in for the app's laid-out text (user space)
  CHECK(doc->addAnnotation(0, ft) == 5);

  Annotation bad;
  bad.kind = AnnotKind::Highlight;  // no quads
  CHECK(doc->addAnnotation(0, bad) == -1);
  bad.kind = AnnotKind::Stamp;
  CHECK(doc->addAnnotation(0, bad) == -1);

  auto check = [&](const Document& d) {
    const auto all = d.annotations(0);
    REQUIRE(all.size() == 6);
    CHECK(all[0].kind == AnnotKind::Highlight);
    CHECK(all[0].author == "Corey");
    CHECK(all[0].contents == "Check this – é");
    REQUIRE(all[0].quads.size() == 2);
    CHECK(nearRect(all[0].quads[0], hl.quads[0]));
    CHECK(nearRect(all[0].quads[1], hl.quads[1]));
    CHECK(nearRect(all[0].rect, {20, 20, 200, 65}));
    CHECK(all[0].color == Color{255, 220, 0});
    CHECK(all[0].name.rfind("pdfz-", 0) == 0);
    CHECK(all[0].modified.rfind("D:20", 0) == 0);

    CHECK(all[1].kind == AnnotKind::Note);
    CHECK(nearRect(all[1].rect, note.rect));
    CHECK(all[1].contents == "A sticky note");

    CHECK(all[2].kind == AnnotKind::Ink);
    REQUIRE(all[2].ink.size() == 1);
    REQUIRE(all[2].ink[0].size() == 3);
    CHECK(all[2].ink[0][1].x == doctest::Approx(80).epsilon(0.01));
    CHECK(all[2].ink[0][1].y == doctest::Approx(150).epsilon(0.01));
    CHECK(all[2].borderWidth == doctest::Approx(3));
    CHECK(all[2].color == Color{0, 0, 255});

    CHECK(all[3].kind == AnnotKind::Square);
    CHECK(nearRect(all[3].rect, sq.rect));
    CHECK(all[3].color == Color{200, 0, 0});
    CHECK(all[4].kind == AnnotKind::Circle);

    CHECK(all[5].kind == AnnotKind::FreeText);
    CHECK(all[5].fontSize == doctest::Approx(14));
    CHECK(all[5].textColor == Color{0, 0, 200});  // from its /DA
    CHECK(all[5].contents == "Text box");
  };
  check(*doc);
  auto again = reopen(*doc);  // and saved: everything, appearances included, is in the file
  check(*again);

  // They're drawn where they are.
  CHECK(countColor(*again, {20, 20, 200, 40}, {255, 220, 0}) > 2000);
  CHECK(countColor(*again, {28, 98, 132, 152}, {0, 0, 255}) > 100);
  CHECK(countColor(*again, {150, 200, 250, 300}, {200, 0, 0}) > 1000);
  CHECK(countColor(*again, {20, 300, 120, 380}, {200, 255, 200}, 20) > 2000);
  CHECK(countColor(*again, {260, 330, 280, 350}, {0, 0, 204}, 20) > 300);  // the text box's own appearance
  CHECK(countColor(*again, {0, 150, 140, 290}, {200, 0, 0}) == 0);
}

TEST_CASE("markups on rotated pages are drawn over the display rects given") {
  for (int rot : {90, 180, 270}) {
    CAPTURE(rot);
    auto doc = blank(300, 400, rot, true);
    Annotation hl;
    hl.kind = AnnotKind::Highlight;
    hl.color = {255, 0, 0};
    hl.quads = {{30, 40, 130, 60}};
    REQUIRE(doc->addAnnotation(0, hl) == 0);
    const auto back = doc->annotations(0);
    REQUIRE(back.size() == 1);
    CHECK(nearRect(back[0].quads[0], hl.quads[0]));
    CHECK(countColor(*doc, {30, 40, 130, 60}, {255, 0, 0}) > 1500);
    CHECK(countColor(*doc, {140, 0, 300, 300}, {255, 0, 0}) == 0);
  }
}

TEST_CASE("moving, restyling and removing") {
  auto doc = blank();
  Annotation sq;
  sq.kind = AnnotKind::Square;
  sq.rect = {20, 20, 80, 80};
  sq.color = {255, 0, 0};
  sq.borderWidth = 3;
  REQUIRE(doc->addAnnotation(0, sq) == 0);
  Annotation ink;
  ink.kind = AnnotKind::Ink;
  ink.color = {0, 0, 255};
  ink.borderWidth = 2;
  ink.ink = {{{150, 150}, {200, 200}}};
  REQUIRE(doc->addAnnotation(0, ink) == 1);

  SUBCASE("move keeps the appearance and shifts the geometry") {
    REQUIRE(doc->moveAnnotation(0, 0, 100, 50));
    REQUIRE(doc->moveAnnotation(0, 1, -100, 0));
    const auto all = doc->annotations(0);
    CHECK(nearRect(all[0].rect, {120, 70, 180, 130}));
    CHECK(all[1].ink[0][0].x == doctest::Approx(50).epsilon(0.01));
    CHECK(countColor(*doc, {118, 68, 182, 132}, {255, 0, 0}) > 300);
    CHECK(countColor(*doc, {18, 18, 82, 68}, {255, 0, 0}) == 0);
    CHECK(countColor(*doc, {48, 148, 102, 202}, {0, 0, 255}) > 50);
    CHECK(countColor(*doc, {148, 148, 202, 202}, {0, 0, 255}) == 0);
    auto saved = reopen(*doc);
    CHECK(countColor(*saved, {48, 148, 102, 202}, {0, 0, 255}) > 50);
  }
  SUBCASE("update redraws with the new color and size") {
    Annotation changed = doc->annotations(0)[0];
    changed.color = {0, 160, 0};
    changed.rect = {20, 20, 150, 150};
    REQUIRE(doc->updateAnnotation(0, 0, changed) == 0);
    CHECK(countColor(*doc, {18, 18, 152, 152}, {0, 160, 0}) > 1000);
    CHECK(countColor(*doc, {18, 18, 152, 152}, {255, 0, 0}) == 0);
    CHECK(nearRect(doc->annotations(0)[0].rect, changed.rect));
    Annotation wrongKind = changed;
    wrongKind.kind = AnnotKind::Circle;
    CHECK(doc->updateAnnotation(0, 0, wrongKind) == -1);
  }
  SUBCASE("contents only") {
    REQUIRE(doc->setAnnotationContents(0, 1, "Freehand"));
    CHECK(doc->annotations(0)[1].contents == "Freehand");
  }
  SUBCASE("remove") {
    REQUIRE(doc->removeAnnotation(0, 0));
    const auto all = doc->annotations(0);
    REQUIRE(all.size() == 1);
    CHECK(all[0].kind == AnnotKind::Ink);
    CHECK(all[0].index == 0);
    CHECK_FALSE(doc->removeAnnotation(0, 5));
    CHECK(countColor(*doc, {18, 18, 82, 82}, {255, 0, 0}) == 0);
  }
  CHECK(doc->modified());
}

TEST_CASE("PDF dates") {
  const std::string d = pdfz::pdfDateNow();
  REQUIRE(d.size() == 23);
  CHECK(d.rfind("D:20", 0) == 0);
  CHECK((d[16] == '+' || d[16] == '-'));
  CHECK(d[19] == '\'');
}

TEST_CASE("opacity and fills, including taking a fill away") {
  auto doc = blank();
  Annotation sq;
  sq.kind = AnnotKind::Square;
  sq.rect = {20, 20, 120, 120};
  sq.color = {0, 0, 255};
  sq.borderWidth = 2;
  sq.opacity = 0.5;
  REQUIRE(doc->addAnnotation(0, sq) == 0);
  Annotation hl;
  hl.kind = AnnotKind::Highlight;
  hl.quads = {{150, 20, 250, 40}};
  hl.color = {255, 0, 0};
  REQUIRE(doc->addAnnotation(0, hl) == 1);

  auto all = doc->annotations(0);
  CHECK(all[0].opacity == doctest::Approx(0.5).epsilon(0.01));
  CHECK_FALSE(all[0].hasFill);
  CHECK(all[1].opacity == doctest::Approx(1.0));

  // Half-transparent blue over white comes out light blue, not blue.
  CHECK(countColor(*doc, {19, 19, 23, 121}, {0, 0, 255}, 40) == 0);
  CHECK(countColor(*doc, {19, 19, 23, 121}, {127, 127, 255}, 40) > 50);

  // Add a fill afterwards.
  Annotation filled = all[0];
  filled.hasFill = true;
  filled.fill = {0, 255, 0};
  filled.opacity = 1.0;
  REQUIRE(doc->updateAnnotation(0, 0, filled) == 0);
  all = doc->annotations(0);
  REQUIRE(all[0].hasFill);
  CHECK(all[0].fill == Color{0, 255, 0});
  CHECK(countColor(*doc, {40, 40, 100, 100}, {0, 255, 0}, 20) > 3000);

  // And take it away again: made anew under the same name, now last on the page.
  const std::string name = all[0].name;
  Annotation unfilled = all[0];
  unfilled.hasFill = false;
  const int idx = doc->updateAnnotation(0, 0, unfilled);
  CHECK(idx == 1);
  all = doc->annotations(0);
  REQUIRE(all.size() == 2);
  CHECK(all[1].kind == AnnotKind::Square);
  CHECK(all[1].name == name);
  CHECK_FALSE(all[1].hasFill);
  CHECK(all[0].kind == AnnotKind::Highlight);
  CHECK(countColor(*doc, {40, 40, 100, 100}, {0, 255, 0}, 20) == 0);
  auto saved = reopen(*doc);
  CHECK_FALSE(saved->annotations(0)[1].hasFill);
}

TEST_CASE("text boxes keep their border, fill, font and alignment") {
  auto doc = blank();
  Annotation ft;
  ft.kind = AnnotKind::FreeText;
  ft.rect = {20, 20, 220, 80};
  ft.contents = "Hello";
  ft.textColor = {200, 0, 0};
  ft.color = {0, 0, 200};
  ft.borderWidth = 2;
  ft.hasFill = true;
  ft.fill = {255, 255, 0};
  ft.fontName = "TiRo";
  ft.fontSize = 18;
  ft.align = 1;
  ft.opacity = 0.75;
  ft.appearance = "/GS gs 1 1 0 rg 20 312 200 60 re f";
  REQUIRE(doc->addAnnotation(0, ft) == 0);
  auto saved = reopen(*doc);
  for (const Document* d : {doc.get(), saved.get()}) {
    const auto a = d->annotations(0).at(0);
    CHECK(a.textColor == Color{200, 0, 0});
    CHECK(a.color == Color{0, 0, 200});
    CHECK(a.hasFill);
    CHECK(a.fill == Color{255, 255, 0});
    CHECK(a.fontName == "TiRo");
    CHECK(a.fontSize == doctest::Approx(18));
    CHECK(a.align == 1);
    CHECK(a.borderWidth == doctest::Approx(2));
    CHECK(a.opacity == doctest::Approx(0.75).epsilon(0.01));
  }
  // The appearance's /GS gs found its transparency state: yellow at 75% over white.
  CHECK(countColor(*doc, {30, 30, 200, 70}, {255, 255, 64}, 30) > 3000);
}

TEST_CASE("restyling a text box after it's made: opacity and fill stick") {
  auto doc = blank();
  Annotation ft;
  ft.kind = AnnotKind::FreeText;
  ft.rect = {20, 20, 220, 80};
  ft.contents = "Hello";
  ft.color = {0, 0, 200};
  ft.borderWidth = 1;
  ft.appearance = "0 0 1 RG 20 312 200 60 re S";
  REQUIRE(doc->addAnnotation(0, ft) == 0);

  // Opacity alone, with no fill (the text box already has an appearance by now).
  Annotation a = doc->annotations(0).at(0);
  a.opacity = 0.5;
  a.appearance = "/GS gs 0 0 1 RG 20 312 200 60 re S";
  REQUIRE(doc->updateAnnotation(0, 0, a) == 0);
  CHECK(doc->annotations(0).at(0).opacity == doctest::Approx(0.5).epsilon(0.01));

  // Then a fill and another opacity.
  a = doc->annotations(0).at(0);
  a.hasFill = true;
  a.fill = {255, 255, 0};
  a.opacity = 0.75;
  a.appearance = "/GS gs 1 1 0 rg 20 312 200 60 re f";
  REQUIRE(doc->updateAnnotation(0, 0, a) == 0);
  auto saved = reopen(*doc);
  for (const Document* d : {doc.get(), saved.get()}) {
    const auto got = d->annotations(0).at(0);
    CHECK(got.opacity == doctest::Approx(0.75).epsilon(0.01));
    CHECK(got.hasFill);
    CHECK(got.fill == Color{255, 255, 0});
  }
  // Drawn at 75%: yellow over white, not solid yellow.
  CHECK(countColor(*doc, {30, 30, 200, 70}, {255, 255, 64}, 30) > 3000);
  CHECK(countColor(*saved, {30, 30, 200, 70}, {255, 255, 64}, 30) > 3000);
}

namespace {
// A drawing 100 x 50: a filled square at its right end (75..95 x 15..35), and an image of the
// whole box that is clear except for a blue middle third.
pdfz::StampDrawing testDrawing() {
  pdfz::StampDrawing d;
  d.width = 100;
  d.height = 50;
  pdfz::StampDrawing::Path sq;
  sq.color = {0, 160, 0};
  using Op = pdfz::StampDrawing::Op;
  sq.ops = {{Op::MoveTo, {{75, 15}}}, {Op::LineTo, {{95, 15}}}, {Op::LineTo, {{95, 35}}}, {Op::LineTo, {{75, 35}}},
            {Op::Close, {}}};
  d.paths.push_back(sq);
  d.imageWidth = 30;
  d.imageHeight = 15;
  d.imageBgra.assign(std::size_t(30 * 15 * 4), 0);
  for (int y = 0; y < 15; ++y)
    for (int x = 10; x < 20; ++x) {
      std::uint8_t* px = &d.imageBgra[std::size_t((y * 30 + x) * 4)];
      px[0] = 255;
      px[3] = 255;
    }
  return d;
}

// The test drawing shown in `r`: the square, the blue band, and nothing (clear image) on the left.
void checkTestDrawing(const Document& dd, const RectF& r) {
  auto at = [&](double fx, double fy) { return RectF{r.left + fx * r.width() - 2, r.top + fy * r.height() - 2,
                                                     r.left + fx * r.width() + 2, r.top + fy * r.height() + 2}; };
  CHECK(countColor(dd, at(0.85, 0.5), {0, 160, 0}, 40) >= 9);
  CHECK(countColor(dd, at(0.5, 0.5), {0, 0, 255}, 40) >= 9);
  CHECK(countColor(dd, at(0.15, 0.5), {255, 255, 255}, 10) >= 9);
  CHECK(countColor(dd, at(0.85, 0.15), {255, 255, 255}, 10) >= 9);  // above the square: clear
}
}  // namespace

TEST_CASE("stamps: drawn outlines and an image, moved and resized") {
  auto doc = blank(300, 400);
  const pdfz::StampDrawing d = testDrawing();
  Annotation a;
  a.kind = AnnotKind::Stamp;
  a.subject = "Signature";
  a.author = "Corey";
  a.rect = {50, 100, 150, 150};
  const int index = doc->addStamp(0, a, d);
  REQUIRE(index == 0);
  auto check = [](const Document& dd, const RectF& r) { checkTestDrawing(dd, r); };
  check(*doc, a.rect);
  auto saved = reopen(*doc);
  check(*saved, a.rect);
  const auto listed = saved->annotations(0).at(0);
  CHECK(listed.kind == AnnotKind::Stamp);
  CHECK(listed.subject == "Signature");
  CHECK(listed.isPdfzStamp());
  CHECK(nearRect(listed.rect, a.rect));

  // Moved, then resized to twice the size: the drawing goes along and scales.
  REQUIRE(saved->moveAnnotation(0, 0, 20, 30));
  Annotation b = saved->annotations(0).at(0);
  CHECK(nearRect(b.rect, {70, 130, 170, 180}));
  b.rect = {70, 130, 270, 230};
  REQUIRE(saved->updateAnnotation(0, 0, b) == 0);
  check(*saved, b.rect);
  check(*reopen(*saved), b.rect);
}

TEST_CASE("stamps: the drawing read back, after a move and resize, copies to another document") {
  for (int rot : {0, 90, 270}) {
    CAPTURE(rot);
    auto doc = blank(300, 400, rot, true);
    Annotation a;
    a.kind = AnnotKind::Stamp;
    a.subject = "APPROVED";
    a.author = "Corey";
    a.color = {30, 123, 52};
    a.rect = {40, 60, 140, 110};
    REQUIRE(doc->addStamp(0, a, testDrawing()) == 0);
    auto saved = reopen(*doc);
    REQUIRE(saved->moveAnnotation(0, 0, 15, 25));
    Annotation b = saved->annotations(0).at(0);
    b.rect = {b.rect.left, b.rect.top, b.rect.left + 150, b.rect.top + 75};  // 1.5 times the size
    REQUIRE(saved->updateAnnotation(0, 0, b) == 0);
    saved = reopen(*saved);
    b = saved->annotations(0).at(0);
    CHECK(nearRect(b.rect, {55, 85, 205, 160}));

    pdfz::StampDrawing got;
    REQUIRE(saved->stampDrawing(0, 0, got));
    CHECK(std::abs(got.width / got.height - 2.0) < 0.01);
    CHECK(got.paths.size() == 1);
    CHECK(got.paths.at(0).color == Color{0, 160, 0});
    CHECK(got.imageWidth == 30);
    CHECK(got.imageHeight == 15);

    // Pasted on an upright page of another document, at its size and place: the same look.
    for (int to : {0, 90}) {
      CAPTURE(to);
      auto other = blank(300, 400, to);
      REQUIRE(other->addStamp(0, b, got) == 0);
      checkTestDrawing(*other, b.rect);
      auto again = reopen(*other);
      checkTestDrawing(*again, b.rect);
      CHECK(nearRect(again->annotations(0).at(0).rect, b.rect));
      CHECK(again->annotations(0).at(0).subject == "APPROVED");
      // And copied again from there: still the same.
      pdfz::StampDrawing twice;
      REQUIRE(again->stampDrawing(0, 0, twice));
      CHECK(twice.paths.size() == 1);
      CHECK(std::abs(twice.width / twice.height - 2.0) < 0.01);
    }
  }
}

TEST_CASE("stamps: one drawn some other way isn't read back") {
  auto doc = blank();
  Annotation sq;
  sq.kind = AnnotKind::Square;
  sq.rect = {20, 20, 120, 120};
  REQUIRE(doc->addAnnotation(0, sq) == 0);
  pdfz::StampDrawing got;
  CHECK_FALSE(doc->stampDrawing(0, 0, got));
  CHECK_FALSE(doc->stampDrawing(0, 5, got));
}

TEST_CASE("hiding an annotation while it's edited") {
  auto doc = blank();
  Annotation sq;
  sq.kind = AnnotKind::Square;
  sq.rect = {20, 20, 120, 120};
  sq.color = {255, 0, 0};
  sq.borderWidth = 4;
  REQUIRE(doc->addAnnotation(0, sq) == 0);
  CHECK(countColor(*doc, {18, 18, 122, 122}, {255, 0, 0}) > 300);
  REQUIRE(doc->setAnnotationHidden(0, 0, true));
  CHECK(doc->annotations(0)[0].hidden);
  CHECK(countColor(*doc, {18, 18, 122, 122}, {255, 0, 0}) == 0);
  REQUIRE(doc->setAnnotationHidden(0, 0, false));
  CHECK(countColor(*doc, {18, 18, 122, 122}, {255, 0, 0}) > 300);
}

TEST_CASE("AutoCAD's text comments are saved as they came, without an appearance") {
  // AutoCAD puts an invisible /Square over each piece of SHX text, with the text, so it can be
  // searched, and no appearance. PDFium makes one for it (an empty box) when the page is
  // drawn; saved with that, Acrobat showed every one with a comment icon.
  std::string pdf = "%PDF-1.7\n";
  std::vector<std::size_t> offs;
  auto obj = [&](const std::string& body) {
    offs.push_back(pdf.size());
    pdf += std::to_string(offs.size()) + " 0 obj\n" + body + "\nendobj\n";
  };
  const std::string draw = "0 0 0 RG 1 w 90 690 m 300 690 l S";
  obj("<< /Type /Catalog /Pages 2 0 R >>");
  obj("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 800] /Contents 4 0 R /Annots [5 0 R] >>");
  obj("<< /Length " + std::to_string(draw.size()) + " >>\nstream\n" + draw + "\nendstream");
  obj("<< /Type /Annot /Subtype /Square /Rect [90 680 300 710] /Border [0 0 0] /C [] /F 4 "
      "/T (AutoCAD SHX Text) /Contents (10'-6\" LONG SPOOL) >>");
  const std::size_t xref = pdf.size();
  pdf += "xref\n0 " + std::to_string(offs.size() + 1) + "\n0000000000 65535 f \n";
  for (std::size_t o : offs) {
    char line[24];
    std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
    pdf += line;
  }
  pdf += "trailer\n<< /Size " + std::to_string(offs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) +
         "\n%%EOF\n";
  auto [doc, err] = Document::openMemory(std::vector<std::uint8_t>(pdf.begin(), pdf.end()));
  REQUIRE(doc);
  // Drawn (PDFium makes the appearance then), and a comment of pdfz's own added.
  doc->render(0, 1.0, {0, 0, 400, 800});
  Annotation sq;
  sq.kind = AnnotKind::Square;
  sq.rect = {20, 20, 120, 120};
  sq.color = {255, 0, 0};
  REQUIRE(doc->addAnnotation(0, sq) >= 0);
  doc->render(0, 1.0, {0, 0, 400, 800});
  auto again = reopen(*doc);
  std::lock_guard lock(pdfz::engineMutex());
  FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(again->handle()), 0);
  REQUIRE(page);
  REQUIRE(FPDFPage_GetAnnotCount(page) == 2);
  int withoutAp = 0, withAp = 0;
  for (int i = 0; i < 2; ++i) {
    FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
    REQUIRE(a);
    if (FPDFAnnot_HasKey(a, "NM")) {  // pdfz's: its appearance stays
      CHECK(FPDFAnnot_HasKey(a, "AP"));
      ++withAp;
    } else {  // AutoCAD's: as it came
      CHECK_FALSE(FPDFAnnot_HasKey(a, "AP"));
      CHECK_FALSE(FPDFAnnot_HasKey(a, "PDFIUM_HasGeneratedAP"));
      CHECK(FPDFAnnot_HasKey(a, "Contents"));
      ++withoutAp;
    }
    FPDFPage_CloseAnnot(a);
  }
  FPDF_ClosePage(page);
  CHECK(withAp == 1);
  CHECK(withoutAp == 1);
}
