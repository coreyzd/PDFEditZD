// SPDX-License-Identifier: GPL-3.0-only
#include "core/document.hpp"

#include "core/compact.hpp"
#include "core/engine.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <set>
#include <system_error>

#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_flatten.h>
#include <fpdf_formfill.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdfview.h>

namespace pdfz {

namespace detail {
PageFrame frameOfPage(void* page);
}  // namespace detail

namespace {

PageFrame pageFrameOf(FPDF_PAGE page) { return detail::frameOfPage(page); }

FPDF_DOCUMENT asDoc(void* p) { return static_cast<FPDF_DOCUMENT>(p); }

// A page from the document's cache (not closed here).
struct PageHandle {
  FPDF_PAGE page = nullptr;
  explicit PageHandle(void* p) : page(static_cast<FPDF_PAGE>(p)) {}
  explicit operator bool() const { return page != nullptr; }
};

// Whether a comment (anything but a field, a link or a pop-up) overlaps a form field on the
// page. The form layer draws fields over everything, so those pages need drawing with care.
bool annotationsOverFields(FPDF_PAGE page) {
  std::vector<FS_RECTF> fields, others;
  const int n = FPDFPage_GetAnnotCount(page);
  for (int i = 0; i < n; ++i) {
    FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
    if (!a) continue;
    const int type = FPDFAnnot_GetSubtype(a);
    FS_RECTF r{};
    const bool visible = !(FPDFAnnot_GetFlags(a) & FPDF_ANNOT_FLAG_HIDDEN);
    if (visible && FPDFAnnot_GetRect(a, &r) && type != FPDF_ANNOT_LINK && type != FPDF_ANNOT_POPUP) {
      const FS_RECTF norm{std::min(r.left, r.right), std::max(r.top, r.bottom), std::max(r.left, r.right),
                          std::min(r.top, r.bottom)};
      (type == FPDF_ANNOT_WIDGET ? fields : others).push_back(norm);
    }
    FPDFPage_CloseAnnot(a);
  }
  for (const FS_RECTF& f : fields)
    for (const FS_RECTF& o : others)
      if (o.left < f.right && f.left < o.right && o.bottom < f.top && f.bottom < o.top) return true;
  return false;
}

constexpr std::size_t kCachedPages = 4;

OpenError fromPdfiumError(unsigned long e) {
  switch (e) {
    case FPDF_ERR_SUCCESS: return OpenError::None;
    case FPDF_ERR_FILE: return OpenError::File;
    case FPDF_ERR_FORMAT: return OpenError::Format;
    case FPDF_ERR_PASSWORD: return OpenError::Password;
    case FPDF_ERR_SECURITY: return OpenError::Security;
    default: return OpenError::Unknown;
  }
}

// UTF-16LE code units (as PDFium returns text) to UTF-8.
std::string utf16ToUtf8(const unsigned short* s, std::size_t n) {
  std::string out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::uint32_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;  // lone surrogate
    }
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

// PDFium's save callback, collecting into a byte vector.
struct VectorWriter : FPDF_FILEWRITE {
  std::vector<std::uint8_t>* out;
  explicit VectorWriter(std::vector<std::uint8_t>* o) : FPDF_FILEWRITE{}, out(o) {
    version = 1;
    WriteBlock = &VectorWriter::write;
  }
  static int write(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
    auto* w = static_cast<VectorWriter*>(self);
    const auto* p = static_cast<const std::uint8_t*>(data);
    w->out->insert(w->out->end(), p, p + size);
    return 1;
  }
};

int pdfiumFlags(unsigned flags) {
  int f = 0;
  if (flags & RenderAnnotations) f |= FPDF_ANNOT;
  if (flags & RenderForPrinting) f |= FPDF_PRINTING;
  if (flags & RenderGrayscale) f |= FPDF_GRAYSCALE;
  return f;
}

}  // namespace

std::uint32_t Bitmap::pixel(int x, int y) const {
  assert(x >= 0 && y >= 0 && x < width && y < height);
  const std::uint8_t* p = pixels.data() + static_cast<std::size_t>(y) * stride + x * 4;
  return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[1]) << 8) | p[0];
}

const char* describe(OpenError e) {
  switch (e) {
    case OpenError::None: return "no error";
    case OpenError::File: return "the file couldn't be read";
    case OpenError::Format: return "it isn't a PDF, or it is too damaged to open";
    case OpenError::Password: return "it needs a password";
    case OpenError::Security: return "it is encrypted in a way that isn't supported";
    case OpenError::Unknown: break;
  }
  return "an unknown error";
}

OpenResult Document::open(const std::filesystem::path& path, const std::string& password) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {nullptr, OpenError::File};
  std::vector<std::uint8_t> bytes;
  std::error_code ec;
  if (auto size = std::filesystem::file_size(path, ec); !ec) bytes.reserve(static_cast<std::size_t>(size));
  bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (in.bad()) return {nullptr, OpenError::File};
  return openMemory(std::move(bytes), password);
}

OpenResult Document::openMemory(std::vector<std::uint8_t> bytes, const std::string& password) {
  return openBytes(std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)), password);
}

OpenResult Document::openBytes(Bytes bytes, const std::string& password) {
  assert(Engine::running());
  if (!bytes) return {nullptr, OpenError::Format};
  std::unique_ptr<Document> d(new Document);
  d->bytes_ = std::move(bytes);
  d->password_ = password;
  std::lock_guard lock(engineMutex());
  d->doc_ = FPDF_LoadMemDocument64(d->bytes_->data(), d->bytes_->size(),
                                   password.empty() ? nullptr : password.c_str());
  if (!d->doc_) return {nullptr, fromPdfiumError(FPDF_GetLastError())};
  d->refreshPageSizes();
  d->initForms();
  return {std::move(d), OpenError::None};
}

std::unique_ptr<Document> Document::create() {
  assert(Engine::running());
  std::unique_ptr<Document> d(new Document);
  std::lock_guard lock(engineMutex());
  d->doc_ = FPDF_CreateNewDocument();
  if (!d->doc_) return nullptr;
  d->initForms();
  return d;
}

Document::~Document() {
  if (doc_) {
    std::lock_guard lock(engineMutex());
    dropCachedPages();
    exitForms();
    for (auto& [key, font] : fonts_) FPDFFont_Close(static_cast<FPDF_FONT>(font));
    FPDF_CloseDocument(asDoc(doc_));
  }
}

void* Document::cachedPage(int index) const {
  for (std::size_t i = 0; i < pages_.size(); ++i)
    if (pages_[i].first == index) {
      if (i > 0) std::rotate(pages_.begin(), pages_.begin() + std::ptrdiff_t(i), pages_.begin() + std::ptrdiff_t(i) + 1);
      return pages_.front().second;
    }
  FPDF_PAGE page = FPDF_LoadPage(asDoc(doc_), index);
  if (!page) return nullptr;
  if (form_) FORM_OnAfterLoadPage(page, static_cast<FPDF_FORMHANDLE>(form_));
  if (pages_.size() >= kCachedPages) {
    closePage(pages_.back().second);
    pages_.pop_back();
  }
  pages_.insert(pages_.begin(), {index, page});
  return page;
}

void Document::closePage(void* page) const {
  renderHints_.erase(page);
  if (form_) FORM_OnBeforeClosePage(static_cast<FPDF_PAGE>(page), static_cast<FPDF_FORMHANDLE>(form_));
  FPDF_ClosePage(static_cast<FPDF_PAGE>(page));
}

void Document::dropCachedPage(int index) const {
  for (auto it = pages_.begin(); it != pages_.end(); ++it)
    if (it->first == index) {
      closePage(it->second);
      pages_.erase(it);
      return;
    }
}

void Document::dropCachedPages() const {
  for (auto& [index, page] : pages_) closePage(page);
  pages_.clear();
}

void Document::initForms() {
  // No callbacks: pdfz has no JavaScript, timers or pop-up menus for PDFium to drive. Field
  // values and their appearances are all it needs.
  auto* info = new FPDF_FORMFILLINFO{};
  info->version = 1;
  formInfo_ = info;
  form_ = FPDFDOC_InitFormFillEnvironment(asDoc(doc_), info);
  hasForm_ = FPDF_GetFormType(asDoc(doc_)) != FORMTYPE_NONE;
}

void Document::exitForms() {
  if (form_) FPDFDOC_ExitFormFillEnvironment(static_cast<FPDF_FORMHANDLE>(form_));
  form_ = nullptr;
  delete static_cast<FPDF_FORMFILLINFO*>(formInfo_);
  formInfo_ = nullptr;
}

void Document::refreshPageSizes() {
  const int n = FPDF_GetPageCount(asDoc(doc_));
  std::vector<SizeF> sizes(static_cast<std::size_t>(std::max(n, 0)));
  for (int i = 0; i < n; ++i) {
    FS_SIZEF s{};
    if (FPDF_GetPageSizeByIndexF(asDoc(doc_), i, &s)) sizes[i] = {s.width, s.height};
  }
  std::unique_lock lock(sizesMutex_);
  sizes_ = std::move(sizes);
}

int Document::pageCount() const {
  std::shared_lock lock(sizesMutex_);
  return static_cast<int>(sizes_.size());
}

SizeF Document::pageSize(int index) const {
  std::shared_lock lock(sizesMutex_);
  if (index < 0 || index >= static_cast<int>(sizes_.size())) return {};
  return sizes_[static_cast<std::size_t>(index)];
}

int Document::pageRotation(int index) const {
  if (index < 0 || index >= pageCount()) return 0;
  std::lock_guard lock(engineMutex());
  PageHandle page(cachedPage(index));
  return page ? FPDFPage_GetRotation(page.page) * 90 : 0;
}

std::string Document::metadata(const std::string& key) const {
  std::lock_guard lock(engineMutex());
  const unsigned long bytes = FPDF_GetMetaText(asDoc(doc_), key.c_str(), nullptr, 0);
  if (bytes <= 2) return {};
  std::vector<unsigned short> buf(bytes / 2);
  FPDF_GetMetaText(asDoc(doc_), key.c_str(), buf.data(), bytes);
  return utf16ToUtf8(buf.data(), buf.size() - 1);  // without the terminator
}

int Document::fileVersion() const {
  std::lock_guard lock(engineMutex());
  int v = 0;
  return FPDF_GetFileVersion(asDoc(doc_), &v) ? v : 0;
}

namespace {

// The page's objects (and those in its forms), for renderHints.
void gatherHints(FPDF_PAGEOBJECT obj, double formScale, double& density, int& hairlineFills, int depth) {
  const int type = FPDFPageObj_GetType(obj);
  if (type == FPDF_PAGEOBJ_FORM) {
    if (depth > 8) return;
    FS_MATRIX m{};
    FPDFPageObj_GetMatrix(obj, &m);
    const double s = formScale * std::sqrt(std::abs(double(m.a) * m.d - double(m.b) * m.c));
    for (int i = 0, n = FPDFFormObj_CountObjects(obj); i < n; ++i)
      gatherHints(FPDFFormObj_GetObject(obj, i), s, density, hairlineFills, depth + 1);
  } else if (type == FPDF_PAGEOBJ_IMAGE) {
    unsigned int w = 0, h = 0;
    FS_MATRIX m{};
    if (!FPDFImageObj_GetImagePixelSize(obj, &w, &h) || !FPDFPageObj_GetMatrix(obj, &m)) return;
    const double wPt = std::hypot(m.a, m.b) * formScale, hPt = std::hypot(m.c, m.d) * formScale;
    if (wPt * hPt < 400 || wPt <= 0 || hPt <= 0) return;  // an icon: never mind
    density = std::max({density, w / wPt, h / hPt});
  } else if (type == FPDF_PAGEOBJ_PATH) {
    int fill = 0;
    FPDF_BOOL stroke = false;
    float width = 1;
    if (FPDFPath_GetDrawMode(obj, &fill, &stroke) && fill != FPDF_FILLMODE_NONE && stroke &&
        FPDFPageObj_GetStrokeWidth(obj, &width) && width * formScale < 0.05)
      ++hairlineFills;
  }
}

// `big`, k times the size, scaled down into `out`: each pixel mostly the average of its k x k
// block, a little toward the block's darkest, so thin lines keep some of their weight.
void downsample(const Bitmap& big, int k, Bitmap& out) {
  constexpr int kDarkest = 90;  // of 256
  for (int y = 0; y < out.height; ++y) {
    std::uint8_t* o = out.pixels.data() + std::size_t(y) * std::size_t(out.stride);
    for (int x = 0; x < out.width; ++x) {
      for (int c = 0; c < 4; ++c) {
        int sum = 0, darkest = 255;
        for (int dy = 0; dy < k; ++dy) {
          const std::uint8_t* row = big.pixels.data() + std::size_t(y * k + dy) * std::size_t(big.stride);
          for (int dx = 0; dx < k; ++dx) {
            const int v = row[std::size_t(x * k + dx) * 4 + std::size_t(c)];
            sum += v;
            darkest = std::min(darkest, v);
          }
        }
        const int avg = (sum + k * k / 2) / (k * k);
        o[std::size_t(x) * 4 + std::size_t(c)] = std::uint8_t((avg * (256 - kDarkest) + darkest * kDarkest + 128) >> 8);
      }
    }
  }
}

}  // namespace

Document::RenderHints Document::renderHints(void* page) const {
  if (auto it = renderHints_.find(page); it != renderHints_.end()) return it->second;
  RenderHints h;
  auto p = static_cast<FPDF_PAGE>(page);
  for (int i = 0, n = FPDFPage_CountObjects(p); i < n; ++i)
    gatherHints(FPDFPage_GetObject(p, i), 1.0, h.imageDensity, h.hairlineFills, 0);
  renderHints_[page] = h;
  return h;
}

Bitmap Document::render(int index, double scale, PixelRect region, unsigned flags) const {
  if (index < 0 || index >= pageCount() || scale <= 0 || region.width <= 0 || region.height <= 0) return {};
  // Keep a single bitmap under 2^31 bytes; callers tile anything bigger.
  if (static_cast<double>(region.width) * region.height * 4 > 2'000'000'000.0) return {};
  std::lock_guard lock(engineMutex());
  PageHandle page(cachedPage(index));
  if (!page) return {};
  int k = 1;
  if (!(flags & RenderForPrinting)) {
    const RenderHints h = renderHints(page.page);
    const double ratio = h.imageDensity / scale;  // picture pixels to screen pixels
    if (ratio > 1.25) k = ratio > 2.5 ? 3 : 2;
    if (h.hairlineFills >= 200 && scale < 3) k = std::max(k, 2);
    const double bigPixels = double(region.width) * region.height * k * k;
    if (bigPixels * 4 > 1'500'000'000.0) k = 1;
  }
  if (k == 1) return renderAt(page.page, index, scale, region, flags);
  const Bitmap big = renderAt(page.page, index, scale * k,
                              PixelRect{region.x * k, region.y * k, region.width * k, region.height * k}, flags);
  if (big.width != region.width * k || big.height != region.height * k) return renderAt(page.page, index, scale, region, flags);
  Bitmap out;
  out.width = region.width;
  out.height = region.height;
  out.stride = region.width * 4;
  out.pixels.assign(static_cast<std::size_t>(out.stride) * out.height, 0);
  downsample(big, k, out);
  return out;
}

Bitmap Document::renderAt(void* pageHandle, int index, double scale, PixelRect region, unsigned flags) const {
  Bitmap out;
  out.width = region.width;
  out.height = region.height;
  out.stride = region.width * 4;
  out.pixels.assign(static_cast<std::size_t>(out.stride) * out.height, 0);
  PageHandle page(pageHandle);
  FPDF_BITMAP bmp = FPDFBitmap_CreateEx(out.width, out.height, FPDFBitmap_BGRA, out.pixels.data(), out.stride);
  if (!bmp) return {};
  FPDFBitmap_FillRect(bmp, 0, 0, out.width, out.height, 0xFFFFFFFF);
  // PDFium first maps the page to its displayed orientation at 1 unit per point (top-left
  // origin, /Rotate applied), then applies this matrix: scale, then shift the region to 0,0.
  const FS_MATRIX m{static_cast<float>(scale), 0, 0, static_cast<float>(scale),
                    static_cast<float>(-region.x), static_cast<float>(-region.y)};
  const FS_RECTF clip{0, 0, static_cast<float>(out.width), static_cast<float>(out.height)};
  FPDF_RenderPageBitmapWithMatrix(bmp, page.page, &m, &clip, pdfiumFlags(flags));
  // Form fields aren't drawn with the page: PDFium's form layer draws them, at the same place
  // (it takes the page's position and size rather than a matrix), over everything else.
  if (form_ && hasForm_ && (flags & RenderAnnotations)) {
    const SizeF s = pageSize(index);
    const int pageW = static_cast<int>(std::lround(s.width * scale));
    const int pageH = static_cast<int>(std::lround(s.height * scale));
    auto drawFields = [&](FPDF_BITMAP target) {
      FPDF_FFLDraw(static_cast<FPDF_FORMHANDLE>(form_), target, page.page, -region.x, -region.y, pageW, pageH, 0,
                   pdfiumFlags(flags));
    };
    if (!annotationsOverFields(page.page)) {
      drawFields(bmp);
    } else {
      // A comment or signature on a field must stay on top of it, as it is in the file. The
      // page is drawn again without comments, with the fields: wherever the comments changed
      // nothing, that's what shows.
      std::vector<std::uint8_t> plain(out.pixels.size()), fields(out.pixels.size());
      FPDF_BITMAP p = FPDFBitmap_CreateEx(out.width, out.height, FPDFBitmap_BGRA, plain.data(), out.stride);
      if (p) {
        FPDFBitmap_FillRect(p, 0, 0, out.width, out.height, 0xFFFFFFFF);
        FPDF_RenderPageBitmapWithMatrix(p, page.page, &m, &clip, pdfiumFlags(flags) & ~FPDF_ANNOT);
        FPDFBitmap_Destroy(p);
        fields = plain;
        FPDF_BITMAP f = FPDFBitmap_CreateEx(out.width, out.height, FPDFBitmap_BGRA, fields.data(), out.stride);
        if (f) {
          drawFields(f);
          FPDFBitmap_Destroy(f);
        }
        auto* px = reinterpret_cast<std::uint32_t*>(out.pixels.data());
        const auto* pl = reinterpret_cast<const std::uint32_t*>(plain.data());
        const auto* fl = reinterpret_cast<const std::uint32_t*>(fields.data());
        for (std::size_t i = 0, n = out.pixels.size() / 4; i < n; ++i)
          if (px[i] == pl[i]) px[i] = fl[i];
      }
    }
  }
  FPDFBitmap_Destroy(bmp);
  return out;
}

Bitmap Document::renderFit(int index, int maxWidth, int maxHeight, unsigned flags) const {
  const SizeF s = pageSize(index);
  if (s.width <= 0 || s.height <= 0 || maxWidth <= 0 || maxHeight <= 0) return {};
  const double scale = std::min(maxWidth / s.width, maxHeight / s.height);
  const int w = std::max(1, static_cast<int>(std::lround(s.width * scale)));
  const int h = std::max(1, static_cast<int>(std::lround(s.height * scale)));
  return render(index, scale, {0, 0, w, h}, flags);
}

bool Document::deletePage(int index) {
  if (index < 0 || index >= pageCount()) return false;
  std::lock_guard lock(engineMutex());
  flushAppearances();
  dropCachedPages();
  FPDFPage_Delete(asDoc(doc_), index);
  refreshPageSizes();
  exitForms();  // no pages are loaded now
  initForms();
  markEdited();
  return true;
}

bool Document::deletePages(std::vector<int> pages) {
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  const int n = pageCount();
  if (pages.empty() || int(pages.size()) >= n || pages.front() < 0 || pages.back() >= n) return false;
  std::lock_guard lock(engineMutex());
  flushAppearances();
  dropCachedPages();
  for (auto it = pages.rbegin(); it != pages.rend(); ++it) FPDFPage_Delete(asDoc(doc_), *it);
  refreshPageSizes();
  exitForms();  // no pages are loaded now
  initForms();
  markEdited();
  return true;
}

std::unique_ptr<Document> Document::extract(const std::vector<int>& pages) const {
  const int n = pageCount();
  if (pages.empty() || std::set<int>(pages.begin(), pages.end()).size() != pages.size()) return nullptr;
  for (int p : pages)
    if (p < 0 || p >= n) return nullptr;
  const Bytes bytes = snapshot();
  if (!bytes) return nullptr;
  auto [copy, err] = openBytes(bytes, password_);
  if (!copy) return nullptr;
  // The pages not wanted go; then the rest are put in the order asked for.
  std::vector<int> others;
  const std::set<int> keep(pages.begin(), pages.end());
  for (int i = 0; i < n; ++i)
    if (!keep.count(i)) others.push_back(i);
  if (!others.empty() && !copy->deletePages(others)) return nullptr;
  std::vector<int> sorted = pages;
  std::sort(sorted.begin(), sorted.end());
  if (sorted != pages) {
    // Where each wanted page is now (its rank among the kept ones), in the order wanted.
    std::vector<int> now;
    for (int p : pages) now.push_back(int(std::lower_bound(sorted.begin(), sorted.end(), p) - sorted.begin()));
    if (!copy->movePages(now, 0)) return nullptr;
  }
  return std::move(copy);
}

bool Document::insertBlankPage(int index, SizeF size) {
  if (index < 0 || index > pageCount() || size.width <= 0 || size.height <= 0) return false;
  std::lock_guard lock(engineMutex());
  flushAppearances();
  dropCachedPages();
  FPDF_PAGE page = FPDFPage_New(asDoc(doc_), index, size.width, size.height);
  if (!page) return false;
  FPDF_ClosePage(page);
  refreshPageSizes();
  exitForms();  // no pages are loaded now
  initForms();
  markEdited();
  return true;
}

namespace {

// PDFium reads a JPEG through this, from memory.
struct MemoryAccess : FPDF_FILEACCESS {
  const std::vector<std::uint8_t>* data = nullptr;
  static int read(void* param, unsigned long pos, unsigned char* buf, unsigned long size) {
    const auto* self = static_cast<const MemoryAccess*>(param);
    if (pos + size > self->data->size()) return 0;
    std::copy_n(self->data->data() + pos, size, buf);
    return 1;
  }
};

}  // namespace

namespace {

// An image object of `image` (not yet placed or on a page), or null.
FPDF_PAGEOBJECT makeImageObject(FPDF_DOCUMENT doc, FPDF_PAGE page, const Document::PageImage& image) {
  FPDF_PAGEOBJECT img = FPDFPageObj_NewImageObj(doc);
  if (!img) return nullptr;
  bool ok = false;
  FPDF_PAGE pages[] = {page};
  if (!image.jpeg.empty()) {
    MemoryAccess access;
    access.m_FileLen = static_cast<unsigned long>(image.jpeg.size());
    access.m_GetBlock = &MemoryAccess::read;
    access.m_Param = &access;
    access.data = &image.jpeg;
    // Inline: the data is copied in now, so nothing has to outlive this call.
    ok = FPDFImageObj_LoadJpegFileInline(pages, 1, img, &access);
  } else if (FPDF_BITMAP bmp = FPDFBitmap_Create(image.width, image.height, 0)) {
    auto* px = static_cast<std::uint8_t*>(FPDFBitmap_GetBuffer(bmp));
    const int stride = FPDFBitmap_GetStride(bmp);
    for (int y = 0; y < image.height; ++y)
      std::copy_n(image.bgra.data() + std::size_t(y) * std::size_t(image.width) * 4, std::size_t(image.width) * 4,
                  px + std::ptrdiff_t(y) * stride);
    ok = FPDFImageObj_SetBitmap(pages, 1, img, bmp);
    FPDFBitmap_Destroy(bmp);
  }
  if (!ok) {
    FPDFPageObj_Destroy(img);
    return nullptr;
  }
  return img;
}

bool validImage(const Document::PageImage& image) {
  return image.width > 0 && image.height > 0 &&
         (!image.jpeg.empty() || image.bgra.size() >= std::size_t(image.width) * std::size_t(image.height) * 4);
}

}  // namespace

bool Document::insertImagePage(int index, SizeF size, const PageImage& image, double margin) {
  if (index < 0 || index > pageCount() || size.width <= 0 || size.height <= 0 || !validImage(image)) return false;
  std::lock_guard lock(engineMutex());
  flushAppearances();
  dropCachedPages();
  FPDF_DOCUMENT doc = asDoc(doc_);
  FPDF_PAGE page = FPDFPage_New(doc, index, size.width, size.height);
  if (!page) return false;
  FPDF_PAGEOBJECT img = makeImageObject(doc, page, image);
  bool ok = img != nullptr;
  if (ok) {
    // As big as fits inside the margins, keeping its shape, in the middle.
    const double availW = std::max(1.0, size.width - 2 * margin), availH = std::max(1.0, size.height - 2 * margin);
    const double k = std::min(availW / image.width, availH / image.height);
    const double w = image.width * k, h = image.height * k;
    FPDFImageObj_SetMatrix(img, w, 0, 0, h, (size.width - w) / 2, (size.height - h) / 2);
    FPDFPage_InsertObject(page, img);
    ok = FPDFPage_GenerateContent(page);
  }
  FPDF_ClosePage(page);
  if (!ok) FPDFPage_Delete(doc, index);
  refreshPageSizes();
  exitForms();
  initForms();
  if (ok) markEdited();
  return ok;
}

bool Document::addContentImage(int pageIndex, const PageImage& image, const RectF& rect) {
  const RectF r = rect.normalized();
  if (pageIndex < 0 || pageIndex >= pageCount() || !validImage(image) || r.width() <= 0 || r.height() <= 0) return false;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return false;
  FPDF_PAGEOBJECT img = makeImageObject(asDoc(doc_), page, image);
  if (!img) return false;
  // The image's unit square onto the rect, upright on the screen whatever the page's /Rotate.
  const PageFrame f = pageFrameOf(page);
  const PointF bl = f.toUser(PointF{r.left, r.bottom}), br = f.toUser(PointF{r.right, r.bottom}),
               tl = f.toUser(PointF{r.left, r.top});
  FPDFImageObj_SetMatrix(img, br.x - bl.x, br.y - bl.y, tl.x - bl.x, tl.y - bl.y, bl.x, bl.y);
  FPDFPage_InsertObject(page, img);
  if (!FPDFPage_GenerateContent(page)) return false;
  markEdited();
  return true;
}

bool Document::importPages(const Document& source, const std::vector<int>& pages, int index) {
  if (&source == this || pages.empty() || index < 0 || index > pageCount()) return false;
  for (int p : pages)
    if (p < 0 || p >= source.pageCount()) return false;
  std::lock_guard lock(engineMutex());
  flushAppearances();
  dropCachedPages();
  source.flushAppearances();
  if (!FPDF_ImportPagesByIndex(asDoc(doc_), asDoc(source.doc_), pages.data(),
                               static_cast<unsigned long>(pages.size()), index))
    return false;
  refreshPageSizes();
  exitForms();  // no pages are loaded now
  initForms();
  markEdited();
  return true;
}

bool Document::movePages(const std::vector<int>& pages, int destIndex) {
  const int n = pageCount();
  const int count = static_cast<int>(pages.size());
  if (pages.empty() || destIndex < 0 || destIndex + count > n) return false;
  if (std::set<int>(pages.begin(), pages.end()).size() != pages.size()) return false;
  for (int p : pages)
    if (p < 0 || p >= n) return false;
  std::lock_guard lock(engineMutex());
  flushAppearances();
  dropCachedPages();
  // Checked above, so PDFium's "may be left in an indeterminate state" failure can't happen.
  const bool ok = FPDF_MovePages(asDoc(doc_), pages.data(), static_cast<unsigned long>(count), destIndex);
  refreshPageSizes();
  exitForms();  // no pages are loaded now
  initForms();
  markEdited();
  return ok;
}

bool Document::setPageRotation(int index, int degrees) {
  if (index < 0 || index >= pageCount()) return false;
  const int quarter = ((static_cast<int>(std::lround(degrees / 90.0)) % 4) + 4) % 4;
  std::lock_guard lock(engineMutex());
  PageHandle page(cachedPage(index));
  if (!page) return false;
  FPDFPage_SetRotation(page.page, quarter);
  dropCachedPages();  // a parsed page keeps the size it had
  refreshPageSizes();
  markEdited();
  return true;
}

bool Document::writeTo(std::vector<std::uint8_t>& out) const {
  flushAppearances();
  out.clear();
  VectorWriter writer(&out);
  if (FPDF_SaveAsCopy(asDoc(doc_), &writer, FPDF_NO_INCREMENTAL) == 0) return false;
  out = withoutUnusedObjects(out);  // PDFium keeps what edits left behind
  return true;
}

std::vector<std::uint8_t> Document::saveToMemory() const {
  std::vector<std::uint8_t> out;
  std::lock_guard lock(engineMutex());
  if (!writeTo(out)) out.clear();
  return out;
}

Bytes Document::snapshot() const {
  if (!edited_ && bytes_) return bytes_;
  auto out = std::make_shared<std::vector<std::uint8_t>>();
  std::lock_guard lock(engineMutex());
  if (!writeTo(*out)) return nullptr;
  return out;
}

namespace {

// Writes `bytes` to a temporary file next to `path`, then puts it in place of `path`.
bool replaceFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::filesystem::path tmp = path;
  tmp += ".pdfz-saving";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
      std::error_code ignored;
      std::filesystem::remove(tmp, ignored);
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);  // replaces `path` (MoveFileExW on Windows)
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

}  // namespace

bool Document::save(const std::filesystem::path& path) const {
  std::vector<std::uint8_t> bytes;
  {
    std::lock_guard lock(engineMutex());
    if (!writeTo(bytes)) return false;
  }
  if (!replaceFile(path, bytes)) return false;
  modified_ = false;
  return true;
}

bool Document::saveFlattened(const std::filesystem::path& path) const {
  std::vector<std::uint8_t> bytes;
  {
    std::lock_guard lock(engineMutex());
    std::vector<std::uint8_t> copy;
    if (!writeTo(copy)) return false;
    // Flattened in a copy opened from those bytes, so this document keeps everything editable.
    FPDF_DOCUMENT flat = FPDF_LoadMemDocument64(copy.data(), copy.size(), password_.empty() ? nullptr : password_.c_str());
    if (!flat) return false;
    bool ok = true;
    const int n = FPDF_GetPageCount(flat);
    for (int i = 0; i < n && ok; ++i) {
      FPDF_PAGE page = FPDF_LoadPage(flat, i);
      if (!page) {
        ok = false;
        break;
      }
      if (FPDFPage_Flatten(page, FLAT_NORMALDISPLAY) == FLATTEN_FAIL) ok = false;
      FPDF_ClosePage(page);
    }
    if (ok) {
      VectorWriter writer(&bytes);
      ok = FPDF_SaveAsCopy(flat, &writer, FPDF_NO_INCREMENTAL) != 0;
      if (ok) bytes = withoutUnusedObjects(bytes);
    }
    FPDF_CloseDocument(flat);
    if (!ok) return false;
  }
  return replaceFile(path, bytes);
}

}  // namespace pdfz
