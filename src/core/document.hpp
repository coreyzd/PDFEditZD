// SPDX-License-Identifier: GPL-3.0-only
// A PDF document: open, inspect, render, rearrange pages, save. A thin, thread-safe layer over
// PDFium with no Qt in it, so it can be tested on its own and used by command-line tools.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
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
struct PageText;
struct PageFrame;
struct Annotation;
struct FormField;
struct StampDrawing;

// The complete bytes of a PDF file, shared without copying (an open document keeps its file's
// bytes; undo snapshots are these too).
using Bytes = std::shared_ptr<const std::vector<std::uint8_t>>;

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
  static OpenResult openBytes(Bytes bytes, const std::string& password = {});
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

  // The text of a page with where each character is (see text.hpp). Empty for a page with no
  // text, or a scanned page without a text layer.
  PageText extractText(int index) const;

  // How the page's PDF coordinates map to the display frame.
  PageFrame pageFrame(int index) const;

  // Comments and markups (annotations.hpp). annotations() lists everything on the page except
  // links, form fields and the pop-ups that belong to notes.
  std::vector<Annotation> annotations(int page) const;
  // Adds `a` to the page and returns its index, or -1. The author, contents and dates are
  // written as given (modified and name are filled in when empty).
  int addAnnotation(int page, const Annotation& a);
  // Rewrites annotation `index` from `a`: geometry, colors, opacity, border, strings,
  // appearance. Returns its index afterwards (or -1): the same one, unless taking away a fill
  // meant making the annotation anew (PDFium can't remove a color entry), which puts it last.
  int updateAnnotation(int page, int index, const Annotation& a);
  // Moves an annotation by (dx, dy) display points, keeping its appearance.
  bool moveAnnotation(int page, int index, double dx, double dy);
  // Changes only the comment text, keeping the appearance (any kind, even ones pdfz didn't make).
  bool setAnnotationContents(int page, int index, const std::string& contents);
  // Removes an annotation, and its pop-up if it has one.
  bool removeAnnotation(int page, int index);
  // Hides or shows an annotation (its Hidden flag), e.g. while it's being edited in place.
  // Not an edit: snapshot() ignores it while the document is otherwise unedited.
  bool setAnnotationHidden(int page, int index, bool hidden);
  // A stamp (a signature, initials, a check mark...): `a` gives its rect, subject, author and
  // contents; `drawing` what it shows, scaled to the rect. Returns its index, or -1. Moving and
  // resizing it afterwards (moveAnnotation, updateAnnotation with a new rect) keeps the
  // drawing, scaled to fit.
  int addStamp(int page, const Annotation& a, const StampDrawing& drawing);

  // Form fields (forms.hpp): the widgets on a page, and filling them in. The setters refuse
  // read-only fields. A field's value is shared by all its widgets (a radio group's buttons,
  // or a field shown on several pages).
  bool hasForm() const;
  std::vector<FormField> formFields(int page) const;
  bool setFieldText(int page, int index, const std::string& text);
  // A check box on or off; a radio button on (it turns its group's others off).
  bool setFieldChecked(int page, int index, bool checked);
  // Selects option `option` of a combo box or list box (for a multiple-selection list, only it).
  bool setFieldChoice(int page, int index, int option);

  // Page editing. Indices are 0-based; each returns false (and changes nothing) on bad input.
  bool deletePage(int index);
  // Deletes several pages at once (distinct; not all of them).
  bool deletePages(std::vector<int> pages);
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

  // A new document of just `pages` of this one, in that order (extracting, splitting). Made
  // from a copy of the whole file with the other pages taken out, so form fields, bookmarks'
  // targets on those pages and the document's settings come along. Null on bad input.
  std::unique_ptr<Document> extract(const std::vector<int>& pages) const;

  // Writes a complete new copy of the document. Saves to a temporary file next to `path` and
  // then replaces `path`, so a failed save never leaves a half-written file behind.
  bool save(const std::filesystem::path& path) const;
  std::vector<std::uint8_t> saveToMemory() const;
  // Saves a copy in which comments, signatures and filled-in form fields are part of the page
  // content: they can't be moved or changed any more. This document stays as it is.
  bool saveFlattened(const std::filesystem::path& path) const;
  // The document as it is now, as the bytes of a PDF: the bytes it was opened from while it is
  // unedited (no copy), otherwise a fresh save. Empty if saving fails.
  Bytes snapshot() const;
  // The password the document was opened with (needed to open its snapshots again).
  const std::string& password() const { return password_; }

  // True after any page edit, until the next successful save().
  bool modified() const { return modified_; }

  // The raw handle, for code that adds features on top of pdfz (annotations, forms). Lock
  // engineMutex() around every use of it.
  void* handle() const { return doc_; }

private:
  Document() = default;
  void refreshPageSizes();  // caller holds the engine lock
  bool writeTo(std::vector<std::uint8_t>& out) const;  // caller holds the engine lock

  void markEdited() { modified_ = true; edited_ = true; }
  // The caller holds the engine lock for all of these.
  // A parsed page, from a small cache: parsing a page's content is the slow part of anything
  // done with it (a detailed drawing can take a tenth of a second), so the pages in use stay
  // parsed. Owned by the cache; valid until the next call that changes the page list.
  void* cachedPage(int index) const;
  void dropCachedPages() const;
  // PDFium's form-fill environment, which form fields need to be drawn and filled in. Made
  // again after the page list changes, so it knows the fields of imported pages.
  void initForms();
  void exitForms();
  void closePage(void* page) const;
  // Has PDFium store appearances for the annotations changed since the last save.
  void flushAppearances() const;

  void* doc_ = nullptr;  // FPDF_DOCUMENT
  void* form_ = nullptr;  // FPDF_FORMHANDLE
  void* formInfo_ = nullptr;  // FPDF_FORMFILLINFO, which must outlive form_
  bool hasForm_ = false;
  Bytes bytes_;  // must outlive doc_: PDFium reads from it lazily
  std::string password_;
  bool edited_ = false;  // changed since it was opened (unlike modified_, saving doesn't reset it)
  mutable std::vector<std::pair<int, void*>> pages_;  // page index, FPDF_PAGE; most recent first
  mutable std::set<int> needsAppearances_;            // pages with changed annotations
  // Page sizes, read without the engine lock (the UI lays pages out while a worker renders).
  mutable std::shared_mutex sizesMutex_;
  std::vector<SizeF> sizes_;
  mutable bool modified_ = false;
};

}  // namespace pdfz
