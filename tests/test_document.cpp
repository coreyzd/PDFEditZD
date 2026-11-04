// SPDX-License-Identifier: GPL-3.0-only
#include <doctest.h>

#include "core/document.hpp"
#include "core/engine.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <fpdf_edit.h>
#include <fpdfview.h>

using pdfz::Document;
using pdfz::SizeF;

namespace {

constexpr std::uint32_t kWhite = 0xFFFFFFFF;
constexpr std::uint32_t kBlack = 0xFF000000;

// Appends a page of `size` with a black rectangle at (x, y, w, h), in PDF coordinates (origin
// bottom-left).
void addPageWithRect(Document& doc, SizeF size, double x, double y, double w, double h) {
  const int index = doc.pageCount();
  REQUIRE(doc.insertBlankPage(index, size));
  std::lock_guard lock(pdfz::engineMutex());
  auto* fdoc = static_cast<FPDF_DOCUMENT>(doc.handle());
  FPDF_PAGE page = FPDF_LoadPage(fdoc, index);
  REQUIRE(page);
  FPDF_PAGEOBJECT rect = FPDFPageObj_CreateNewRect(float(x), float(y), float(w), float(h));
  FPDFPageObj_SetFillColor(rect, 0, 0, 0, 255);
  FPDFPath_SetDrawMode(rect, FPDF_FILLMODE_ALTERNATE, 0);
  FPDFPage_InsertObject(page, rect);
  REQUIRE(FPDFPage_GenerateContent(page));
  FPDF_ClosePage(page);
}

// A document whose pages are told apart by width: 100, 200, 300, ... points wide.
std::unique_ptr<Document> numberedPages(int n) {
  auto doc = Document::create();
  REQUIRE(doc);
  for (int i = 0; i < n; ++i) REQUIRE(doc->insertBlankPage(i, {100.0 * (i + 1), 500}));
  return doc;
}

std::vector<int> widths(const Document& doc) {
  std::vector<int> out;
  for (int i = 0; i < doc.pageCount(); ++i) out.push_back(int(std::lround(doc.pageSize(i).width)));
  return out;
}

std::filesystem::path tempPath(const std::string& name) {
  return std::filesystem::temp_directory_path() / ("pdfz-test-" + name);
}

std::vector<std::uint8_t> bytesOf(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace

TEST_CASE("engine is running for the tests") {
  CHECK(pdfz::Engine::running());
  CHECK(pdfz::Engine::version() != "unknown");
}

TEST_CASE("a new document is empty and gains blank pages") {
  auto doc = Document::create();
  REQUIRE(doc);
  CHECK(doc->pageCount() == 0);
  CHECK_FALSE(doc->modified());
  CHECK(doc->insertBlankPage(0, pdfz::kLetter));
  CHECK(doc->insertBlankPage(1, pdfz::kA4));
  CHECK(doc->pageCount() == 2);
  CHECK(doc->pageSize(0) == pdfz::kLetter);
  CHECK(doc->pageSize(1).width == doctest::Approx(pdfz::kA4.width).epsilon(1e-4));
  CHECK(doc->modified());
  CHECK(doc->pageSize(5) == SizeF{});
}

TEST_CASE("rendering a whole page and a region of it") {
  auto doc = Document::create();
  addPageWithRect(*doc, {200, 100}, 20, 10, 40, 30);  // shown at x 20..60, y 60..90 from the top

  SUBCASE("whole page at 1 px/pt") {
    auto bmp = doc->render(0, 1.0, {0, 0, 200, 100});
    REQUIRE(bmp.width == 200);
    REQUIRE(bmp.height == 100);
    CHECK(bmp.stride == 800);
    CHECK(bmp.pixel(40, 75) == kBlack);
    CHECK(bmp.pixel(40, 20) == kWhite);
    CHECK(bmp.pixel(100, 75) == kWhite);
    CHECK(bmp.pixel(0, 0) == kWhite);
  }

  SUBCASE("a tile matches the same pixels of the whole page") {
    const double scale = 2.0;
    auto whole = doc->render(0, scale, {0, 0, 400, 200});
    auto tile = doc->render(0, scale, {60, 100, 64, 48});
    REQUIRE(tile.width == 64);
    REQUIRE(tile.height == 48);
    int differing = 0;
    for (int y = 0; y < tile.height; ++y)
      for (int x = 0; x < tile.width; ++x)
        if (tile.pixel(x, y) != whole.pixel(x + 60, y + 100)) ++differing;
    CHECK(differing == 0);
    CHECK(tile.pixel(20, 40) == kBlack);  // page point (40, 70) from the top
  }

  SUBCASE("renderFit keeps the aspect ratio") {
    auto bmp = doc->renderFit(0, 50, 50);
    CHECK(bmp.width == 50);
    CHECK(bmp.height == 25);
  }

  SUBCASE("bad requests give an empty bitmap") {
    CHECK(doc->render(1, 1.0, {0, 0, 10, 10}).empty());
    CHECK(doc->render(0, 0.0, {0, 0, 10, 10}).empty());
    CHECK(doc->render(0, 1.0, {0, 0, 0, 10}).empty());
  }
}

TEST_CASE("rotated pages are sized and rendered as displayed") {
  auto doc = Document::create();
  addPageWithRect(*doc, {200, 100}, 20, 10, 40, 30);
  CHECK(doc->pageRotation(0) == 0);
  REQUIRE(doc->setPageRotation(0, 90));
  CHECK(doc->pageRotation(0) == 90);
  CHECK(doc->pageSize(0) == SizeF{100, 200});

  // Turned a quarter clockwise, the rectangle (PDF x 20..60, y 10..40) is shown at
  // x 10..40, y 20..60 from the top-left.
  auto bmp = doc->render(0, 1.0, {0, 0, 100, 200});
  REQUIRE(bmp.width == 100);
  CHECK(bmp.pixel(25, 40) == kBlack);
  CHECK(bmp.pixel(75, 40) == kWhite);
  CHECK(bmp.pixel(25, 150) == kWhite);

  CHECK(doc->setPageRotation(0, -90));
  CHECK(doc->pageRotation(0) == 270);
  CHECK(doc->setPageRotation(0, 360));
  CHECK(doc->pageRotation(0) == 0);
}

TEST_CASE("deleting, inserting and moving pages") {
  auto doc = numberedPages(4);
  REQUIRE(widths(*doc) == std::vector<int>{100, 200, 300, 400});

  SUBCASE("delete") {
    CHECK(doc->deletePage(1));
    CHECK(widths(*doc) == std::vector<int>{100, 300, 400});
    CHECK_FALSE(doc->deletePage(3));
    CHECK_FALSE(doc->deletePage(-1));
  }
  SUBCASE("insert at the start, middle and end") {
    CHECK(doc->insertBlankPage(0, {50, 500}));
    CHECK(doc->insertBlankPage(2, {60, 500}));
    CHECK(doc->insertBlankPage(doc->pageCount(), {70, 500}));
    CHECK(widths(*doc) == std::vector<int>{50, 100, 60, 200, 300, 400, 70});
    CHECK_FALSE(doc->insertBlankPage(99, pdfz::kLetter));
    CHECK_FALSE(doc->insertBlankPage(0, {0, 100}));
  }
  SUBCASE("move: PDFium's own example, [A B C D] -> [A D C B]") {
    CHECK(doc->movePages({3, 2}, 1));
    CHECK(widths(*doc) == std::vector<int>{100, 400, 300, 200});
  }
  SUBCASE("move to the end") {
    CHECK(doc->movePages({0}, 3));
    CHECK(widths(*doc) == std::vector<int>{200, 300, 400, 100});
  }
  SUBCASE("bad moves change nothing") {
    CHECK_FALSE(doc->movePages({2, 2}, 0));
    CHECK_FALSE(doc->movePages({0, 4}, 0));
    CHECK_FALSE(doc->movePages({0, 1}, 3));
    CHECK_FALSE(doc->movePages({}, 0));
    CHECK(widths(*doc) == std::vector<int>{100, 200, 300, 400});
  }
}

TEST_CASE("importing pages from another document") {
  auto doc = numberedPages(2);
  auto other = Document::create();
  REQUIRE(other->insertBlankPage(0, {777, 500}));
  REQUIRE(other->insertBlankPage(1, {888, 500}));

  CHECK(doc->importPages(*other, {1, 0}, 1));
  CHECK(widths(*doc) == std::vector<int>{100, 888, 777, 200});
  CHECK(other->pageCount() == 2);  // the source is untouched
  CHECK_FALSE(doc->importPages(*doc, {0}, 0));
  CHECK_FALSE(doc->importPages(*other, {2}, 0));
}

TEST_CASE("saving and opening again") {
  auto doc = Document::create();
  addPageWithRect(*doc, {200, 100}, 20, 10, 40, 30);
  REQUIRE(doc->insertBlankPage(1, pdfz::kLetter));

  SUBCASE("through memory") {
    auto bytes = doc->saveToMemory();
    REQUIRE(bytes.size() > 100);
    CHECK(std::string(bytes.begin(), bytes.begin() + 5) == "%PDF-");
    auto [again, err] = Document::openMemory(std::move(bytes));
    REQUIRE(again);
    CHECK(err == pdfz::OpenError::None);
    CHECK(again->pageCount() == 2);
    CHECK(again->pageSize(1) == pdfz::kLetter);
    CHECK(again->render(0, 1.0, {0, 0, 200, 100}).pixel(40, 75) == kBlack);
  }

  SUBCASE("through a file, including over the file that is open") {
    const auto path = tempPath("save.pdf");
    REQUIRE(doc->save(path));
    CHECK_FALSE(doc->modified());
    CHECK_FALSE(std::filesystem::exists(path.string() + ".pdfz-saving"));

    auto [opened, err] = Document::open(path);
    REQUIRE(opened);
    REQUIRE(opened->deletePage(1));
    // The opened document is in memory, so its own file can be replaced (on Windows too).
    REQUIRE(opened->save(path));
    auto [third, err3] = Document::open(path);
    REQUIRE(third);
    CHECK(third->pageCount() == 1);
    third.reset();
    opened.reset();
    std::filesystem::remove(path);
  }
}

TEST_CASE("opening things that aren't usable PDFs") {
  SUBCASE("a missing file") {
    auto [doc, err] = Document::open(tempPath("does-not-exist.pdf"));
    CHECK_FALSE(doc);
    CHECK(err == pdfz::OpenError::File);
  }
  SUBCASE("not a PDF") {
    auto [doc, err] = Document::openMemory(bytesOf("hello, this is plain text and not a PDF at all"));
    CHECK_FALSE(doc);
    CHECK(err == pdfz::OpenError::Format);
  }
  SUBCASE("every error has a description") {
    for (auto e : {pdfz::OpenError::None, pdfz::OpenError::File, pdfz::OpenError::Format,
                   pdfz::OpenError::Password, pdfz::OpenError::Security, pdfz::OpenError::Unknown})
      CHECK(std::string(pdfz::describe(e)).size() > 3);
  }
}

TEST_CASE("metadata comes back as UTF-8") {
  // A hand-written PDF (no valid xref: PDFium rebuilds it, as it does for damaged files) with
  // a UTF-16 title: "Hi" and U+1F600, which needs a surrogate pair.
  const std::string pdf =
      "%PDF-1.4\n"
      "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
      "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
      "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >> endobj\n"
      "4 0 obj << /Title <FEFF00480069D83DDE00> /Author (Corey) >> endobj\n"
      "trailer << /Root 1 0 R /Info 4 0 R >>\n"
      "%%EOF\n";
  auto [doc, err] = Document::openMemory(bytesOf(pdf));
  REQUIRE(doc);
  CHECK(doc->pageCount() == 1);
  CHECK(doc->pageSize(0) == SizeF{300, 400});
  CHECK(doc->fileVersion() == 14);
  CHECK(doc->metadata("Title") == "Hi\xF0\x9F\x98\x80");
  CHECK(doc->metadata("Author") == "Corey");
  CHECK(doc->metadata("Subject").empty());
}

TEST_CASE("extracting pages, and deleting several at once") {
  auto doc = pdfz::Document::create();
  // Pages told apart by their widths.
  for (int i = 0; i < 6; ++i) REQUIRE(doc->insertBlankPage(i, {100.0 + 10 * i, 200}));
  auto [d, err] = pdfz::Document::openMemory(doc->saveToMemory());
  REQUIRE(d);
  auto part = d->extract({4, 1, 2});
  REQUIRE(part);
  REQUIRE(part->pageCount() == 3);
  CHECK(part->pageSize(0).width == doctest::Approx(140));
  CHECK(part->pageSize(1).width == doctest::Approx(110));
  CHECK(part->pageSize(2).width == doctest::Approx(120));
  CHECK(d->pageCount() == 6);  // the original keeps its pages
  CHECK(!d->extract({}));
  CHECK(!d->extract({1, 1}));
  CHECK(!d->extract({9}));
  auto all = d->extract({0, 1, 2, 3, 4, 5});
  REQUIRE(all);
  CHECK(all->pageCount() == 6);

  REQUIRE(d->deletePages({5, 0, 2}));
  REQUIRE(d->pageCount() == 3);
  CHECK(d->pageSize(0).width == doctest::Approx(110));
  CHECK(d->pageSize(1).width == doctest::Approx(130));
  CHECK(d->pageSize(2).width == doctest::Approx(140));
  CHECK(!d->deletePages({0, 1, 2}));  // not every page
}

TEST_CASE("pages made from pictures") {
  auto doc = pdfz::Document::create();
  REQUIRE(doc->insertBlankPage(0, {612, 792}));
  // A 40 x 20 picture, red: fitted into a Letter page inside 18 pt margins, it is 576 x 288.
  pdfz::Document::PageImage img;
  img.width = 40;
  img.height = 20;
  img.bgra.resize(40 * 20 * 4);
  for (std::size_t i = 0; i < img.bgra.size(); i += 4) {
    img.bgra[i + 2] = 255;
    img.bgra[i + 3] = 255;
  }
  REQUIRE(doc->insertImagePage(1, {612, 792}, img));
  REQUIRE(doc->pageCount() == 2);
  CHECK(doc->pageSize(1).width == doctest::Approx(612));
  auto [saved, err] = pdfz::Document::openMemory(doc->saveToMemory());
  REQUIRE(saved);
  const auto bmp = saved->render(1, 0.5, {0, 0, 306, 396});
  CHECK((bmp.pixel(153, 198) & 0xFFFFFF) == 0xFF0000u);  // the middle: red
  CHECK((bmp.pixel(153, 20) & 0xFFFFFF) == 0xFFFFFFu);   // above it: white
  CHECK((bmp.pixel(4, 198) & 0xFFFFFF) == 0xFFFFFFu);    // the margin

  // Bad input changes nothing.
  CHECK(!doc->insertImagePage(5, {612, 792}, img));
  pdfz::Document::PageImage empty;
  CHECK(!doc->insertImagePage(0, {612, 792}, empty));
  CHECK(doc->pageCount() == 2);
}
