// SPDX-License-Identifier: GPL-3.0-only
// What the side panels show besides the pages: bookmarks, attached files and layers.
#include "core/document.hpp"
#include "core/engine.hpp"
#include "core/geometry.hpp"

#include <fpdf_attachment.h>
#include <fpdf_doc.h>
#include <fpdfview.h>

#include <algorithm>
#include <functional>
#include <mutex>

namespace pdfz {

namespace detail {
std::u16string toUtf16(const std::string& s);
std::string toUtf8(const std::u16string& s);
PageFrame frameOfPage(void* page);
}  // namespace detail

namespace {

FPDF_DOCUMENT asDoc(void* d) { return static_cast<FPDF_DOCUMENT>(d); }

// PDFium's "get the length, then the text" calls, for UTF-16 text.
template <typename Get>
std::string wideText(Get get) {
  const unsigned long bytes = get(nullptr, 0);
  if (bytes <= 2 || bytes > (1u << 20)) return {};
  std::u16string buf(bytes / 2, u'\0');
  get(buf.data(), bytes);
  buf.resize(bytes / 2 - 1);
  return detail::toUtf8(buf);
}

void readBookmarks(FPDF_DOCUMENT doc, FPDF_BOOKMARK parent, std::vector<Document::Bookmark>& out, int depth, int& budget,
                   const std::function<double(int, double)>& top) {
  if (depth > 32) return;
  for (FPDF_BOOKMARK b = FPDFBookmark_GetFirstChild(doc, parent); b && budget > 0; b = FPDFBookmark_GetNextSibling(doc, b)) {
    --budget;  // (a looped outline ends)
    Document::Bookmark bm;
    bm.title = wideText([&](void* buf, unsigned long len) { return FPDFBookmark_GetTitle(b, buf, len); });
    FPDF_DEST dest = FPDFBookmark_GetDest(doc, b);
    if (!dest)
      if (FPDF_ACTION action = FPDFBookmark_GetAction(b); action && FPDFAction_GetType(action) == PDFACTION_GOTO)
        dest = FPDFAction_GetDest(doc, action);
    if (dest) {
      bm.page = FPDFDest_GetDestPageIndex(doc, dest);
      FPDF_BOOL hasX = false, hasY = false, hasZoom = false;
      FS_FLOAT x = 0, y = 0, zoom = 0;
      if (bm.page >= 0 && FPDFDest_GetLocationInPage(dest, &hasX, &hasY, &hasZoom, &x, &y, &zoom) && hasY)
        bm.top = top(bm.page, y);
    }
    readBookmarks(doc, b, bm.children, depth + 1, budget, top);
    out.push_back(std::move(bm));
  }
}

}  // namespace

std::vector<Document::Bookmark> Document::bookmarks() const {
  std::lock_guard lock(engineMutex());
  std::vector<Bookmark> out;
  int budget = 100'000;
  auto top = [this](int page, double y) {
    if (page < 0 || page >= pageCount()) return -1.0;
    void* p = cachedPage(page);
    if (!p) return -1.0;
    const PageFrame f = detail::frameOfPage(p);
    return std::max(0.0, f.toDisplay(PointF{f.x0, y}).y);
  };
  readBookmarks(asDoc(doc_), nullptr, out, 0, budget, top);
  return out;
}

std::vector<Document::Attachment> Document::attachments() const {
  std::lock_guard lock(engineMutex());
  std::vector<Attachment> out;
  const int n = FPDFDoc_GetAttachmentCount(asDoc(doc_));
  for (int i = 0; i < n; ++i) {
    FPDF_ATTACHMENT a = FPDFDoc_GetAttachment(asDoc(doc_), i);
    Attachment at;
    if (a) {
      at.name = wideText([&](void* buf, unsigned long len) { return FPDFAttachment_GetName(a, static_cast<FPDF_WCHAR*>(buf), len); });
      if (FPDFAttachment_HasKey(a, "Desc"))
        at.description = wideText(
            [&](void* buf, unsigned long len) { return FPDFAttachment_GetStringValue(a, "Desc", static_cast<FPDF_WCHAR*>(buf), len); });
      unsigned long size = 0;
      if (FPDFAttachment_GetFile(a, nullptr, 0, &size)) at.size = size;
    }
    out.push_back(std::move(at));
  }
  return out;
}

bool Document::attachmentData(int index, std::vector<std::uint8_t>& out) const {
  std::lock_guard lock(engineMutex());
  out.clear();
  FPDF_ATTACHMENT a = FPDFDoc_GetAttachment(asDoc(doc_), index);
  unsigned long size = 0;
  if (!a || !FPDFAttachment_GetFile(a, nullptr, 0, &size)) return false;
  out.resize(size);
  return size == 0 || FPDFAttachment_GetFile(a, out.data(), size, &size);
}

bool Document::addAttachment(const std::string& name, const std::vector<std::uint8_t>& data) {
  if (name.empty()) return false;
  std::lock_guard lock(engineMutex());
  const std::u16string w = detail::toUtf16(name);
  FPDF_ATTACHMENT a = FPDFDoc_AddAttachment(asDoc(doc_), reinterpret_cast<FPDF_WIDESTRING>(w.c_str()));
  if (!a) return false;
  if (!FPDFAttachment_SetFile(a, asDoc(doc_), data.empty() ? nullptr : data.data(), static_cast<unsigned long>(data.size()))) {
    FPDFDoc_DeleteAttachment(asDoc(doc_), FPDFDoc_GetAttachmentCount(asDoc(doc_)) - 1);
    return false;
  }
  markEdited();
  return true;
}

bool Document::removeAttachment(int index) {
  std::lock_guard lock(engineMutex());
  if (index < 0 || index >= FPDFDoc_GetAttachmentCount(asDoc(doc_)) || !FPDFDoc_DeleteAttachment(asDoc(doc_), index))
    return false;
  markEdited();
  return true;
}

std::vector<std::string> Document::layers() const {
  std::lock_guard lock(engineMutex());
  if (layerNames_) return *layerNames_;
  std::set<std::string> names;
  for (int i = 0, n = pageCount(); i < n; ++i)
    if (void* page = cachedPage(i))
      for (const auto& [name, objs] : renderHints(page).layerObjects) names.insert(name);
  layerNames_ = std::vector<std::string>(names.begin(), names.end());
  return *layerNames_;
}

// (Under the engine lock: render() reads them on the drawing thread.)
void Document::setLayerVisible(const std::string& name, bool visible) {
  std::lock_guard lock(engineMutex());
  if (visible) hiddenLayers_.erase(name);
  else hiddenLayers_.insert(name);
}

std::set<std::string> Document::hiddenLayers() const {
  std::lock_guard lock(engineMutex());
  return hiddenLayers_;
}

void Document::setHiddenLayers(std::set<std::string> names) {
  std::lock_guard lock(engineMutex());
  hiddenLayers_ = std::move(names);
}

}  // namespace pdfz
