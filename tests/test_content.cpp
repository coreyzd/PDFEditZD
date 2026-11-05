// SPDX-License-Identifier: GPL-3.0-only
#include <doctest.h>

#include "core/content.hpp"
#include "core/document.hpp"

#include <algorithm>

using pdfz::ContentBlock;
using pdfz::Document;

namespace {

std::unique_ptr<Document> page(double w = 400, double h = 300) {
  auto doc = Document::create();
  REQUIRE(doc->insertBlankPage(0, {w, h}));
  auto [d, err] = Document::openMemory(doc->saveToMemory());
  REQUIRE(d);
  return std::move(d);
}

std::unique_ptr<Document> reopen(const Document& d) {
  auto [r, err] = Document::openMemory(d.saveToMemory());
  REQUIRE(r);
  return std::move(r);
}

pdfz::TextLines lines(std::vector<std::string> l, double x, double y, double size = 12) {
  pdfz::TextLines t;
  t.lines = std::move(l);
  t.origin = {x, y};
  t.fontSize = size;
  t.lineSpacing = size * 1.2;
  t.font.standard = "Helvetica";
  return t;
}

// Dark pixels in a display rect at 1 px per point.
int ink(const Document& d, const pdfz::RectF& r) {
  const pdfz::SizeF s = d.pageSize(0);
  const auto bmp = d.render(0, 1.0, {0, 0, int(s.width), int(s.height)});
  int n = 0;
  for (int y = std::max(0, int(r.top)); y < std::min(bmp.height, int(r.bottom)); ++y)
    for (int x = std::max(0, int(r.left)); x < std::min(bmp.width, int(r.right)); ++x)
      if ((bmp.pixel(x, y) & 0xFF) < 128) ++n;
  return n;
}

const ContentBlock* textBlock(const std::vector<ContentBlock>& bs, const std::string& starts) {
  for (const ContentBlock& b : bs)
    if (b.kind == ContentBlock::Kind::Text && b.text.rfind(starts, 0) == 0) return &b;
  return nullptr;
}

}  // namespace

TEST_CASE("text is gathered into blocks: lines, and lines into paragraphs") {
  auto doc = page();
  // A paragraph of three lines (the first two full, so they run on), and a label far off.
  REQUIRE(doc->replaceText(0, {}, lines({"The quick brown fox jumps over", "the lazy dog, again and again", "End."}, 20, 40)));
  REQUIRE(doc->replaceText(0, {}, lines({"Total:"}, 300, 200)));
  auto saved = reopen(*doc);
  const auto blocks = saved->contentBlocks(0);
  REQUIRE(blocks.size() == 2);
  const ContentBlock* para = textBlock(blocks, "The quick");
  REQUIRE(para);
  // Full lines run on (they wrapped); the paragraph reads as one.
  CHECK(para->text == "The quick brown fox jumps over the lazy dog, again and again\nEnd.");
  CHECK(para->objects.size() == 3);
  CHECK(para->fontName == "Helvetica");
  CHECK(para->family == "Helvetica");
  CHECK(para->fontSize == doctest::Approx(12));
  CHECK(para->lineSpacing == doctest::Approx(14.4).epsilon(0.01));
  CHECK(para->origin.x == doctest::Approx(20).epsilon(0.01));
  CHECK(para->origin.y == doctest::Approx(40).epsilon(0.01));
  CHECK(para->editable);
  CHECK(para->rect.top < 40);
  CHECK(para->rect.bottom > 40 + 2 * 14.4);
  const ContentBlock* label = textBlock(blocks, "Total:");
  REQUIRE(label);
  CHECK(label->objects.size() == 1);
}

TEST_CASE("moving, replacing and deleting text") {
  auto doc = page();
  REQUIRE(doc->replaceText(0, {}, lines({"Hello there"}, 20, 40, 20)));
  auto blocks = doc->contentBlocks(0);
  REQUIRE(blocks.size() == 1);
  CHECK(ink(*doc, {18, 20, 140, 46}) > 100);

  REQUIRE(doc->moveContent(0, blocks[0].objects, 100, 150));
  CHECK(ink(*doc, {18, 20, 140, 46}) == 0);
  CHECK(ink(*doc, {118, 170, 240, 196}) > 100);
  blocks = doc->contentBlocks(0);
  REQUIRE(blocks.size() == 1);
  CHECK(blocks[0].origin.x == doctest::Approx(120).epsilon(0.01));

  // Replaced: the old text goes, the new is where it's put; one undo-able document change.
  auto edited = lines({"Goodbye", "and thanks for all the fish"}, blocks[0].origin.x, blocks[0].origin.y, 20);
  REQUIRE(doc->replaceText(0, blocks[0].objects, edited));
  auto saved = reopen(*doc);
  blocks = saved->contentBlocks(0);
  REQUIRE(blocks.size() == 1);
  CHECK(blocks[0].text == "Goodbye\nand thanks for all the fish");  // a short line ends there

  // Hidden while it's edited: not drawn, but nothing changes in the file.
  REQUIRE(saved->setContentHidden(0, blocks[0].objects, true));
  CHECK(ink(*saved, {100, 140, 300, 230}) == 0);
  CHECK(saved->contentBlocks(0).empty());
  REQUIRE(saved->setContentHidden(0, blocks[0].objects, false));
  CHECK(ink(*saved, {100, 140, 300, 230}) > 100);

  REQUIRE(saved->deleteContent(0, blocks[0].objects));
  CHECK(saved->contentBlocks(0).empty());
  CHECK(reopen(*saved)->contentBlocks(0).empty());
  CHECK(!saved->deleteContent(0, {99}));  // no such object
}

TEST_CASE("text a standard font can't show") {
  CHECK(pdfz::standardFontCanShow("Caf\xc3\xa9 \xe2\x82\xac 5 \xe2\x80\x94 ok"));  // é € —
  CHECK(!pdfz::standardFontCanShow("\xce\xa9"));                                   // Greek omega
  CHECK(!pdfz::standardFontCanShow("\xe6\x97\xa5\xe6\x9c\xac"));                    // Japanese
}

TEST_CASE("pictures in the page's content: added, resized, moved") {
  auto doc = page();
  Document::PageImage img;
  img.width = 10;
  img.height = 10;
  img.bgra.assign(10 * 10 * 4, 0);  // black, opaque
  for (std::size_t i = 3; i < img.bgra.size(); i += 4) img.bgra[i] = 255;
  REQUIRE(doc->addContentImage(0, img, {50, 50, 100, 100}));
  auto blocks = doc->contentBlocks(0);
  REQUIRE(blocks.size() == 1);
  CHECK(blocks[0].kind == ContentBlock::Kind::Image);
  CHECK(blocks[0].rect.left == doctest::Approx(50));
  CHECK(blocks[0].rect.bottom == doctest::Approx(100));
  CHECK(ink(*doc, {52, 52, 98, 98}) > 2000);

  REQUIRE(doc->resizeContent(0, blocks[0].objects, blocks[0].rect, {50, 50, 150, 200}));
  blocks = reopen(*doc)->contentBlocks(0);
  REQUIRE(blocks.size() == 1);
  CHECK(blocks[0].rect.right == doctest::Approx(150));
  CHECK(blocks[0].rect.bottom == doctest::Approx(200));
  REQUIRE(doc->moveContent(0, blocks[0].objects, -40, -40));
  CHECK(ink(*doc, {12, 12, 108, 158}) > 12000);
}

namespace {

// A placed line of spans in fonts 0 (Helvetica) and 1 (Helvetica-Bold).
pdfz::PlacedText twoFonts(double x, double y) {
  pdfz::PlacedText t;
  t.origin = {x, y};
  t.fonts.resize(2);
  t.fonts[0].standard = "Helvetica";
  t.fonts[1].standard = "Helvetica-Bold";
  return t;
}

pdfz::TextSpan span(const std::string& text, double x, int font, double size = 12, double rise = 0) {
  pdfz::TextSpan s;
  s.text = text;
  s.x = x;
  s.font = font;
  s.size = size;
  s.rise = rise;
  return s;
}

// A one-page PDF whose content is in two streams, with a shape that starts in the first and
// ends in the second (as Word writes them), then text.
std::unique_ptr<Document> splitStreams() {
  const std::string s1 = "q 0.5 0 0 0.5 0 0 cm 0 0 0 rg 20 20 m 120 20 l";
  const std::string s2 = "120 120 l h f Q BT /F1 12 Tf 50 250 Td (Hello) Tj ET";
  std::string pdf = "%PDF-1.7\n";
  std::vector<std::size_t> offs;
  auto obj = [&](const std::string& body) {
    offs.push_back(pdf.size());
    pdf += std::to_string(offs.size()) + " 0 obj\n" + body + "\nendobj\n";
  };
  obj("<< /Type /Catalog /Pages 2 0 R >>");
  obj("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 300] /Contents [4 0 R 5 0 R] "
      "/Resources << /Font << /F1 6 0 R >> >> >>");
  obj("<< /Length " + std::to_string(s1.size()) + " >>\nstream\n" + s1 + "\nendstream");
  obj("<< /Length " + std::to_string(s2.size()) + " >>\nstream\n" + s2 + "\nendstream");
  obj("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  const std::size_t xref = pdf.size();
  pdf += "xref\n0 " + std::to_string(offs.size() + 1) + "\n0000000000 65535 f \n";
  for (std::size_t o : offs) {
    char line[24];
    std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
    pdf += line;
  }
  pdf += "trailer\n<< /Size " + std::to_string(offs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) +
         "\n%%EOF\n";
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(pdf.begin(), pdf.end());
  auto [d, err] = Document::openBytes(bytes);
  REQUIRE(d);
  return std::move(d);
}

}  // namespace

TEST_CASE("text in several fonts, superscripts and alignment") {
  auto doc = page();
  // "Total: 5 jobs" with "5" bold, and "16th" with "th" raised and smaller.
  auto t = twoFonts(20, 40);
  t.lines.push_back({{span("Total: ", 0, 0), span("5", 33.4, 1), span(" jobs", 40.1, 0)}, 0});
  REQUIRE(doc->replaceText(0, {}, t));
  auto t2 = twoFonts(20, 80);
  t2.lines.push_back({{span("Oct. 16", 0, 0), span("th", 40.0, 0, 7, 4)}, 0});
  REQUIRE(doc->replaceText(0, {}, t2));
  // Three centered lines.
  auto t3 = twoFonts(0, 150);
  t3.fonts[0].standard = "Courier";  // 0.6 em a character: widths known
  for (const char* line : {"A centered title line", "short", "and a third one here"}) {
    const double w = double(std::string(line).size()) * 7.2;
    t3.lines.push_back({{span(line, 200 - w / 2, 0)}, t3.lines.empty() ? 0.0 : 15.0});
  }
  REQUIRE(doc->replaceText(0, {}, t3));
  // A numbered item: the number, then (well apart) its text, at a tab stop.
  auto t4 = twoFonts(20, 250);
  t4.lines.push_back({{span("1.)", 0, 0), span("First item", 25, 0)}, 0});
  REQUIRE(doc->replaceText(0, {}, t4));

  const auto blocks = reopen(*doc)->contentBlocks(0);
  const ContentBlock* total = textBlock(blocks, "Total");
  REQUIRE(total);
  CHECK(total->text == "Total: 5 jobs");
  REQUIRE(total->runs.size() == 3);
  CHECK(total->runs[0].text == "Total: ");
  CHECK(!total->runs[0].bold);
  CHECK(total->runs[1].text.rfind("5", 0) == 0);
  CHECK(total->runs[1].bold);
  CHECK(total->runs[2].text.find("jobs") != std::string::npos);
  CHECK(!total->bold);  // the block's own style: most of its text
  CHECK(total->tabStops.empty());

  const ContentBlock* oct = textBlock(blocks, "Oct");
  REQUIRE(oct);
  CHECK(oct->text == "Oct. 16th");  // one block, the "th" in it
  REQUIRE(oct->runs.size() == 2);
  CHECK(oct->runs[1].text == "th");
  CHECK(oct->runs[1].size == doctest::Approx(7));
  CHECK(oct->runs[1].rise == doctest::Approx(4).epsilon(0.05));

  const ContentBlock* item = textBlock(blocks, "1.)");
  REQUIRE(item);
  CHECK(item->text == "1.)\tFirst item");
  REQUIRE(item->tabStops.size() == 1);
  CHECK(item->tabStops[0] == doctest::Approx(25));

  const ContentBlock* title = textBlock(blocks, "A centered");
  REQUIRE(title);
  CHECK(title->align == pdfz::TextAlign::Center);
  CHECK(title->text == "A centered title line\nshort\nand a third one here");
}

TEST_CASE("pictures and shapes moved, turned and flipped") {
  auto doc = page();
  Document::PageImage img;
  img.width = 20;
  img.height = 10;
  img.bgra.assign(20 * 10 * 4, 0);
  for (std::size_t i = 3; i < img.bgra.size(); i += 4) img.bgra[i] = 255;
  REQUIRE(doc->addContentImage(0, img, {100, 100, 200, 150}));
  auto blocks = doc->contentBlocks(0);
  REQUIRE(blocks.size() == 1);
  // A quarter turn about its middle (150, 125): 50 wide, 100 high.
  REQUIRE(doc->transformContent(0, blocks[0].objects, pdfz::Affine::rotate(90, {150, 125})));
  blocks = reopen(*doc)->contentBlocks(0);
  REQUIRE(blocks.size() == 1);
  CHECK(blocks[0].rect.left == doctest::Approx(125));
  CHECK(blocks[0].rect.right == doctest::Approx(175));
  CHECK(blocks[0].rect.top == doctest::Approx(75));
  CHECK(blocks[0].rect.bottom == doctest::Approx(175));
  CHECK(ink(*doc, {127, 77, 173, 173}) > 4000);
  // Flipped left to right about x = 150: stays where it is.
  REQUIRE(doc->transformContent(0, blocks[0].objects, pdfz::Affine::scale(-1, 1, {150, 125})));
  CHECK(ink(*doc, {127, 77, 173, 173}) > 4000);
  CHECK(!doc->transformContent(0, blocks[0].objects, pdfz::Affine::scale(0, 1, {150, 125})));  // flat: refused
}

TEST_CASE("shapes are graphics blocks; content split across streams survives an edit") {
  auto doc = splitStreams();
  auto blocks = doc->contentBlocks(0);
  const ContentBlock* shape = nullptr;
  for (const ContentBlock& b : blocks)
    if (b.kind == ContentBlock::Kind::Graphic) shape = &b;
  REQUIRE(shape);
  // The triangle, drawn at half size: (10, 10) to (60, 60) in PDF space.
  CHECK(shape->rect.left == doctest::Approx(10));
  CHECK(shape->rect.right == doctest::Approx(60));
  CHECK(shape->rect.top == doctest::Approx(240));
  const ContentBlock* hello = textBlock(blocks, "Hello");
  REQUIRE(hello);
  CHECK(hello->rect.left == doctest::Approx(50).epsilon(0.05));

  // Moving the text: the shape (in both streams) and the text stay right.
  REQUIRE(doc->moveContent(0, hello->objects, 100, 0));
  auto reopened = reopen(*doc);
  for (Document* d : {doc.get(), reopened.get()}) {
    blocks = d->contentBlocks(0);
    hello = textBlock(blocks, "Hello");
    REQUIRE(hello);
    CHECK(hello->rect.left == doctest::Approx(150).epsilon(0.05));
    CHECK(hello->rect.top == doctest::Approx(300 - 250 - 12).epsilon(0.1));
  }
  auto again = reopen(*doc);
  CHECK(ink(*again, {35, 275, 58, 288}) > 200);   // the triangle
  CHECK(ink(*again, {150, 38, 185, 52}) > 50);    // the text, moved
  CHECK(ink(*again, {50, 38, 80, 52}) == 0);      // and not where it was
}
