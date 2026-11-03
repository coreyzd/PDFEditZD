// SPDX-License-Identifier: GPL-3.0-only
// A PDF document: open, inspect, render, rearrange pages, save. A thin, thread-safe layer over
// PDFium with no Qt in it, so it can be tested on its own and used by command-line tools.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace pdfz {

// Sizes and positions on a page are in PDF points (1/72 inch), as the page is displayed: a
// page with /Rotate 90 reports its width and height swapped.
struct SizeF {
  double width = 0;
  double height = 0;
  friend bool operator==(const SizeF&, const SizeF&) = default;
};

// Letter and A4, the sizes a new blank page usually wants.
inline constexpr SizeF kLetter{612, 792};
inline constexpr SizeF kA4{595.276, 841.89};

// A rendered region: 32-bit pixels, rows top to bottom, bytes B,G,R,A (alpha always 255; the
// page background is painted white). On little-endian machines this is QImage::Format_RGB32.
struct Bitmap {
  int width = 0;
  int height = 0;
  int stride = 0;  // bytes per row
  std::vector<std::uint8_t> pixels;

  bool empty() const { return width <= 0 || height <= 0; }
  // The pixel at (x, y) as 0xAARRGGBB.
  std::uint32_t pixel(int x, int y) const;
};

// A rectangle in device pixels of a page rendered at some scale; (0, 0) is the page's top-left.
struct PixelRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

enum RenderFlag : unsigned {
  RenderAnnotations = 1u << 0,  // draw annotations and form fields (what a viewer wants)
  RenderForPrinting = 1u << 1,  // annotation print/no-print flags, print-quality images
  RenderGrayscale = 1u << 2,
};

enum class OpenError {
  None,
  File,      // missing or unreadable
  Format,    // not a PDF, or too damaged to open
  Password,  // needs a password, or the one given is wrong
  Security,  // encrypted in a way PDFium doesn't support
  Unknown,
};

const char* describe(OpenError e);

class Document;

struct OpenResult {
  std::unique_ptr<Document> document;
  OpenError error = OpenError::None;
};

class Document {
public:
  // Reads the whole file into memory and opens it from there, so the file itself isn't held
  // open (on Windows an open file can't be replaced, and "Save" writes over the original).
  static OpenResult open(const std::filesystem::path& path, const std::string& password = {});
  static OpenResult openMemory(std::vector<std::uint8_t> bytes, const std::string& password = {});
  // A new document with no pages.
  static std::unique_ptr<Document> create();

  ~Document();
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;

  int pageCount() const;
  // Displayed size of a page (see SizeF), from a cache: cheap, no PDFium call.
  SizeF pageSize(int index) const;
  // The /Rotate of a page: 0, 90, 180 or 270.
  int pageRotation(int index) const;
  // Info dictionary entries ("Title", "Author", ...), empty if missing.
  std::string metadata(const std::string& key) const;
  // The PDF version, e.g. 17 for 1.7; 0 for a document made with create().
  int fileVersion() const;

  // Renders part of a page at `scale` device pixels per point. `region` is in the pixels of
  // the whole page at that scale; a region of {0, 0, ceil(w*scale), ceil(h*scale)} is the
  // whole page. Returns an empty Bitmap if the page can't be loaded.
  Bitmap render(int index, double scale, PixelRect region, unsigned flags = RenderAnnotations) const;
  // The whole page, scaled to fit inside maxWidth x maxHeight pixels.
  Bitmap renderFit(int index, int maxWidth, int maxHeight, unsigned flags = RenderAnnotations) const;

  // Page editing. Indices are 0-based; each returns false (and changes nothing) on bad input.
  bool deletePage(int index);
  // A blank page that becomes page `index` (index == pageCount() appends).
  bool insertBlankPage(int index, SizeF size);
  // Copies `pages` of `source` (in that order) so they start at `index` here. `source` may not
  // be this document.
  bool importPages(const Document& source, const std::vector<int>& pages, int index);
  // Moves `pages` (distinct, any order) so that, in the order given, they start at `destIndex`
  // of the resulting document. Example: pages {4, 1} with destIndex 0 makes the old page 4
  // first and the old page 1 second.
  bool movePages(const std::vector<int>& pages, int destIndex);
  // Sets /Rotate (0, 90, 180 or 270, anything else rounds to those).
  bool setPageRotation(int index, int degrees);

  // Writes a complete new copy of the document. Saves to a temporary file next to `path` and
  // then replaces `path`, so a failed save never leaves a half-written file behind.
  bool save(const std::filesystem::path& path) const;
  std::vector<std::uint8_t> saveToMemory() const;

  // True after any page edit, until the next successful save().
  bool modified() const { return modified_; }

  // The raw handle, for code that adds features on top of pdfz (annotations, forms). Lock
  // engineMutex() around every use of it.
  void* handle() const { return doc_; }

private:
  Document() = default;
  void refreshPageSizes();  // caller holds the engine lock
  bool writeTo(std::vector<std::uint8_t>& out) const;  // caller holds the engine lock

  void* doc_ = nullptr;  // FPDF_DOCUMENT
  std::vector<std::uint8_t> bytes_;  // must outlive doc_: PDFium reads from it lazily
  // Page sizes, read without the engine lock (the UI lays pages out while a worker renders).
  mutable std::shared_mutex sizesMutex_;
  std::vector<SizeF> sizes_;
  mutable bool modified_ = false;
};

}  // namespace pdfz
