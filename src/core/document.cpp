// SPDX-License-Identifier: GPL-3.0-only
#include "core/document.hpp"

#include "core/engine.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <set>
#include <system_error>

#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdfview.h>

namespace pdfz {

namespace {

FPDF_DOCUMENT asDoc(void* p) { return static_cast<FPDF_DOCUMENT>(p); }

// FPDF_PAGE that closes itself.
struct PageHandle {
  FPDF_PAGE page = nullptr;
  PageHandle(FPDF_DOCUMENT doc, int index) : page(FPDF_LoadPage(doc, index)) {}
  ~PageHandle() {
    if (page) FPDF_ClosePage(page);
  }
  PageHandle(const PageHandle&) = delete;
  PageHandle& operator=(const PageHandle&) = delete;
  explicit operator bool() const { return page != nullptr; }
};

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
  assert(Engine::running());
  std::unique_ptr<Document> d(new Document);
  d->bytes_ = std::move(bytes);
  std::lock_guard lock(engineMutex());
  d->doc_ = FPDF_LoadMemDocument64(d->bytes_.data(), d->bytes_.size(),
                                   password.empty() ? nullptr : password.c_str());
  if (!d->doc_) return {nullptr, fromPdfiumError(FPDF_GetLastError())};
  d->refreshPageSizes();
  return {std::move(d), OpenError::None};
}

std::unique_ptr<Document> Document::create() {
  assert(Engine::running());
  std::unique_ptr<Document> d(new Document);
  std::lock_guard lock(engineMutex());
  d->doc_ = FPDF_CreateNewDocument();
  if (!d->doc_) return nullptr;
  return d;
}

Document::~Document() {
  if (doc_) {
    std::lock_guard lock(engineMutex());
    FPDF_CloseDocument(asDoc(doc_));
  }
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
  PageHandle page(asDoc(doc_), index);
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

Bitmap Document::render(int index, double scale, PixelRect region, unsigned flags) const {
  Bitmap out;
  if (index < 0 || index >= pageCount() || scale <= 0 || region.width <= 0 || region.height <= 0)
    return out;
  // Keep a single bitmap under 2^31 bytes; callers tile anything bigger.
  if (static_cast<double>(region.width) * region.height * 4 > 2'000'000'000.0) return out;

  out.width = region.width;
  out.height = region.height;
  out.stride = region.width * 4;
  out.pixels.assign(static_cast<std::size_t>(out.stride) * out.height, 0);

  std::lock_guard lock(engineMutex());
  PageHandle page(asDoc(doc_), index);
  if (!page) return {};
  FPDF_BITMAP bmp = FPDFBitmap_CreateEx(out.width, out.height, FPDFBitmap_BGRA, out.pixels.data(), out.stride);
  if (!bmp) return {};
  FPDFBitmap_FillRect(bmp, 0, 0, out.width, out.height, 0xFFFFFFFF);
  // PDFium first maps the page to its displayed orientation at 1 unit per point (top-left
  // origin, /Rotate applied), then applies this matrix: scale, then shift the region to 0,0.
  const FS_MATRIX m{static_cast<float>(scale), 0, 0, static_cast<float>(scale),
                    static_cast<float>(-region.x), static_cast<float>(-region.y)};
  const FS_RECTF clip{0, 0, static_cast<float>(out.width), static_cast<float>(out.height)};
  FPDF_RenderPageBitmapWithMatrix(bmp, page.page, &m, &clip, pdfiumFlags(flags));
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
  FPDFPage_Delete(asDoc(doc_), index);
  refreshPageSizes();
  modified_ = true;
  return true;
}

bool Document::insertBlankPage(int index, SizeF size) {
  if (index < 0 || index > pageCount() || size.width <= 0 || size.height <= 0) return false;
  std::lock_guard lock(engineMutex());
  FPDF_PAGE page = FPDFPage_New(asDoc(doc_), index, size.width, size.height);
  if (!page) return false;
  FPDF_ClosePage(page);
  refreshPageSizes();
  modified_ = true;
  return true;
}

bool Document::importPages(const Document& source, const std::vector<int>& pages, int index) {
  if (&source == this || pages.empty() || index < 0 || index > pageCount()) return false;
  for (int p : pages)
    if (p < 0 || p >= source.pageCount()) return false;
  std::lock_guard lock(engineMutex());
  if (!FPDF_ImportPagesByIndex(asDoc(doc_), asDoc(source.doc_), pages.data(),
                               static_cast<unsigned long>(pages.size()), index))
    return false;
  refreshPageSizes();
  modified_ = true;
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
  // Checked above, so PDFium's "may be left in an indeterminate state" failure can't happen.
  const bool ok = FPDF_MovePages(asDoc(doc_), pages.data(), static_cast<unsigned long>(count), destIndex);
  refreshPageSizes();
  modified_ = true;
  return ok;
}

bool Document::setPageRotation(int index, int degrees) {
  if (index < 0 || index >= pageCount()) return false;
  const int quarter = ((static_cast<int>(std::lround(degrees / 90.0)) % 4) + 4) % 4;
  std::lock_guard lock(engineMutex());
  PageHandle page(asDoc(doc_), index);
  if (!page) return false;
  FPDFPage_SetRotation(page.page, quarter);
  refreshPageSizes();
  modified_ = true;
  return true;
}

bool Document::writeTo(std::vector<std::uint8_t>& out) const {
  out.clear();
  VectorWriter writer(&out);
  return FPDF_SaveAsCopy(asDoc(doc_), &writer, FPDF_NO_INCREMENTAL) != 0;
}

std::vector<std::uint8_t> Document::saveToMemory() const {
  std::vector<std::uint8_t> out;
  std::lock_guard lock(engineMutex());
  if (!writeTo(out)) out.clear();
  return out;
}

bool Document::save(const std::filesystem::path& path) const {
  std::vector<std::uint8_t> bytes;
  {
    std::lock_guard lock(engineMutex());
    if (!writeTo(bytes)) return false;
  }
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
  modified_ = false;
  return true;
}

}  // namespace pdfz
