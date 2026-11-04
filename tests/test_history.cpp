// SPDX-License-Identifier: GPL-3.0-only
#include <doctest.h>

#include "core/document.hpp"
#include "core/history.hpp"

#include <cmath>

using pdfz::Document;
using pdfz::EditHistory;

namespace {

std::unique_ptr<Document> numbered(int n) {
  auto doc = Document::create();
  for (int i = 0; i < n; ++i) REQUIRE(doc->insertBlankPage(i, {100.0 * (i + 1), 500}));
  // Through bytes, like a document opened from a file.
  auto [opened, err] = Document::openMemory(doc->saveToMemory());
  REQUIRE(opened);
  return std::move(opened);
}

std::vector<int> widths(const Document& doc) {
  std::vector<int> out;
  for (int i = 0; i < doc.pageCount(); ++i) out.push_back(int(std::lround(doc.pageSize(i).width)));
  return out;
}

// Does an edit the way the app does: snapshot, change, push.
template <class F>
void edit(EditHistory& h, Document& doc, const char* label, int page, F&& change) {
  auto before = EditHistory::capture(doc, page);
  REQUIRE(before.bytes);
  REQUIRE(change(doc));
  h.push(std::move(before), label);
}

}  // namespace

TEST_CASE("an unedited document's snapshot is the bytes it was opened from") {
  auto doc = numbered(2);
  auto a = doc->snapshot();
  auto b = doc->snapshot();
  CHECK(a.get() == b.get());  // shared, not copied
  REQUIRE(doc->deletePage(0));
  auto c = doc->snapshot();
  CHECK(c.get() != a.get());
  CHECK(c->size() > 0);
}

TEST_CASE("undo and redo page edits") {
  auto doc = numbered(4);
  EditHistory h;
  CHECK_FALSE(h.canUndo());
  CHECK_FALSE(h.isModified());

  edit(h, *doc, "Delete Pages", 1, [](Document& d) { return d.deletePage(1); });
  edit(h, *doc, "Rotate Pages", 0, [](Document& d) { return d.setPageRotation(0, 90); });
  edit(h, *doc, "Move Pages", 2, [](Document& d) { return d.movePages({2}, 0); });
  REQUIRE(widths(*doc) == std::vector<int>{400, 500, 300});  // rotated page 1 is 500 wide now
  CHECK(h.undoLabel() == "Move Pages");
  CHECK(h.isModified());

  std::unique_ptr<Document> cur = std::move(doc);
  auto step = [&](bool undo, int page) {
    auto r = undo ? h.undo(*cur, page) : h.redo(*cur, page);
    REQUIRE(r.document);
    cur = std::move(r.document);
    return r.page;
  };

  CHECK(step(true, 0) == 2);  // back to where the move was made
  CHECK(widths(*cur) == std::vector<int>{500, 300, 400});
  CHECK(h.redoLabel() == "Move Pages");
  step(true, 0);
  CHECK(widths(*cur) == std::vector<int>{100, 300, 400});
  CHECK(cur->pageRotation(0) == 0);
  step(true, 0);
  CHECK(widths(*cur) == std::vector<int>{100, 200, 300, 400});
  CHECK_FALSE(h.canUndo());
  CHECK_FALSE(h.isModified());  // back to the file as opened
  CHECK(h.undo(*cur, 0).document == nullptr);

  step(false, 0);
  step(false, 0);
  CHECK(widths(*cur) == std::vector<int>{500, 300, 400});
  CHECK(h.isModified());
  CHECK(h.undoCount() == 2);
  CHECK(h.redoCount() == 1);

  SUBCASE("a new edit drops what could be redone") {
    edit(h, *cur, "Insert Blank Page", 0, [](Document& d) { return d.insertBlankPage(0, pdfz::kLetter); });
    CHECK_FALSE(h.canRedo());
    CHECK(h.undoCount() == 3);
  }
  SUBCASE("redo all the way") {
    step(false, 0);
    CHECK(widths(*cur) == std::vector<int>{400, 500, 300});
    CHECK_FALSE(h.canRedo());
  }
}

TEST_CASE("modified tracking follows the saved state") {
  auto doc = numbered(3);
  EditHistory h;
  edit(h, *doc, "Delete Pages", 0, [](Document& d) { return d.deletePage(0); });
  h.markClean();  // saved here
  CHECK_FALSE(h.isModified());

  auto r = h.undo(*doc, 0);
  REQUIRE(r.document);
  CHECK(h.isModified());  // differs from the saved file
  auto r2 = h.redo(*r.document, 0);
  REQUIRE(r2.document);
  CHECK_FALSE(h.isModified());  // back to the saved state

  // Undo, then a different edit: the saved state can't be reached any more.
  auto r3 = h.undo(*r2.document, 0);
  REQUIRE(r3.document);
  edit(h, *r3.document, "Rotate Pages", 0, [](Document& d) { return d.setPageRotation(0, 90); });
  CHECK(h.isModified());
  auto r4 = h.undo(*r3.document, 0);
  REQUIRE(r4.document);
  CHECK(h.isModified());  // the original file, not the saved one
}

TEST_CASE("old snapshots are dropped past the memory limit") {
  auto doc = numbered(3);
  const std::size_t one = doc->snapshot()->size();
  EditHistory h(one * 2 + one / 2);  // room for about two snapshots
  for (int i = 0; i < 5; ++i)
    edit(h, *doc, "Rotate Pages", 0, [&](Document& d) { return d.setPageRotation(0, 90 * (i + 1)); });
  CHECK(h.undoCount() >= 1);
  CHECK(h.undoCount() <= 3);
  CHECK(h.bytesUsed() <= one * 3);
  // The newest step is always there.
  auto r = h.undo(*doc, 0);
  REQUIRE(r.document);
  CHECK(r.document->pageRotation(0) == 0);  // 360 -> undo -> 4 * 90 = 0
}

TEST_CASE("password-protected snapshots open with the same password") {
  // PDFium keeps a document's encryption when it saves, so undo must reopen with the password.
  // Simulated: a document opened with a password it doesn't need still records it.
  auto doc = numbered(2);
  auto [withPw, err] = Document::openBytes(doc->snapshot(), "secret");
  REQUIRE(withPw);
  CHECK(withPw->password() == "secret");
  EditHistory h;
  edit(h, *withPw, "Delete Pages", 0, [](Document& d) { return d.deletePage(0); });
  auto r = h.undo(*withPw, 0);
  REQUIRE(r.document);
  CHECK(r.document->password() == "secret");
  CHECK(r.document->pageCount() == 2);
}
