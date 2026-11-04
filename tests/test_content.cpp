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
  CHECK(para->text == "The quick brown fox jumps over the lazy dog, again and again End.");
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
