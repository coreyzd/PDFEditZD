// SPDX-License-Identifier: GPL-3.0-only
#include "WinPrint.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winspool.h>

#include <algorithm>
#include <cstring>

namespace winprint {

namespace {

std::wstring fixedString(const wchar_t* s, std::size_t max) {
  std::size_t n = 0;
  while (n < max && s[n]) ++n;
  return std::wstring(s, n);
}

// A DEVMODE of the driver's size (the public part plus its private data).
using DevMode = std::vector<std::uint8_t>;
DEVMODEW* dm(DevMode& d) { return reinterpret_cast<DEVMODEW*>(d.data()); }

}  // namespace

std::vector<std::wstring> printers() {
  std::vector<std::wstring> out;
  DWORD needed = 0, count = 0;
  const DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
  EnumPrintersW(flags, nullptr, 4, nullptr, 0, &needed, &count);
  if (!needed) return out;
  std::vector<std::uint8_t> buf(needed);
  if (!EnumPrintersW(flags, nullptr, 4, buf.data(), needed, &needed, &count)) return out;
  const auto* info = reinterpret_cast<const PRINTER_INFO_4W*>(buf.data());
  for (DWORD i = 0; i < count; ++i)
    if (info[i].pPrinterName) out.emplace_back(info[i].pPrinterName);
  return out;
}

std::wstring defaultPrinter() {
  DWORD n = 0;
  GetDefaultPrinterW(nullptr, &n);
  if (!n) return {};
  std::wstring s(n, L'\0');
  if (!GetDefaultPrinterW(s.data(), &n)) return {};
  s.resize(std::wcslen(s.c_str()));
  return s;
}

struct Printer::Impl {
  HANDLE handle = nullptr;
  DevMode settings;  // the job's settings (driver defaults, then the user's)
  DevMode current;   // what the DC was last set up with
  HDC dc = nullptr;
  bool inPage = false;
};

Printer::Printer() : d_(new Impl) {}

Printer::~Printer() {
  if (d_->dc) {
    AbortDoc(d_->dc);
    DeleteDC(d_->dc);
  }
  if (d_->handle) ClosePrinter(d_->handle);
}

bool Printer::open(const std::wstring& name) {
  if (d_->handle) ClosePrinter(d_->handle);
  d_->handle = nullptr;
  name_ = name;
  if (!OpenPrinterW(const_cast<wchar_t*>(name.c_str()), &d_->handle, nullptr)) return false;
  const LONG size = DocumentPropertiesW(nullptr, d_->handle, const_cast<wchar_t*>(name.c_str()), nullptr, nullptr, 0);
  if (size <= 0) return false;
  d_->settings.assign(std::size_t(size), 0);
  return DocumentPropertiesW(nullptr, d_->handle, const_cast<wchar_t*>(name.c_str()), dm(d_->settings), nullptr,
                             DM_OUT_BUFFER) == IDOK;
}

std::vector<Paper> Printer::papers() const {
  std::vector<Paper> out;
  const wchar_t* n = name_.c_str();
  const int count = DeviceCapabilitiesW(n, nullptr, DC_PAPERS, nullptr, nullptr);
  if (count <= 0) return out;
  std::vector<WORD> ids(static_cast<std::size_t>(count));
  std::vector<wchar_t> names(static_cast<std::size_t>(count) * 64);
  std::vector<POINT> sizes(static_cast<std::size_t>(count));  // tenths of a millimeter
  DeviceCapabilitiesW(n, nullptr, DC_PAPERS, reinterpret_cast<LPWSTR>(ids.data()), nullptr);
  DeviceCapabilitiesW(n, nullptr, DC_PAPERNAMES, names.data(), nullptr);
  DeviceCapabilitiesW(n, nullptr, DC_PAPERSIZE, reinterpret_cast<LPWSTR>(sizes.data()), nullptr);
  for (int i = 0; i < count; ++i) {
    Paper p;
    p.id = short(ids[std::size_t(i)]);
    p.name = fixedString(&names[std::size_t(i) * 64], 64);
    const double w = sizes[std::size_t(i)].x / 254.0 * 72, h = sizes[std::size_t(i)].y / 254.0 * 72;
    p.widthPt = std::min(w, h);
    p.heightPt = std::max(w, h);
    if (p.widthPt > 0) out.push_back(p);
  }
  return out;
}

std::vector<Bin> Printer::bins() const {
  std::vector<Bin> out;
  const wchar_t* n = name_.c_str();
  const int count = DeviceCapabilitiesW(n, nullptr, DC_BINS, nullptr, nullptr);
  if (count <= 0) return out;
  std::vector<WORD> ids(static_cast<std::size_t>(count));
  std::vector<wchar_t> names(static_cast<std::size_t>(count) * 24);
  DeviceCapabilitiesW(n, nullptr, DC_BINS, reinterpret_cast<LPWSTR>(ids.data()), nullptr);
  DeviceCapabilitiesW(n, nullptr, DC_BINNAMES, names.data(), nullptr);
  for (int i = 0; i < count; ++i) out.push_back({short(ids[std::size_t(i)]), fixedString(&names[std::size_t(i) * 24], 24)});
  return out;
}

bool Printer::canDuplex() const { return DeviceCapabilitiesW(name_.c_str(), nullptr, DC_DUPLEX, nullptr, nullptr) == 1; }

short Printer::paper() const {
  if (d_->settings.empty()) return 0;
  return dm(const_cast<DevMode&>(d_->settings))->dmPaperSize;
}

short Printer::bin() const {
  if (d_->settings.empty()) return DMBIN_AUTO;
  return dm(const_cast<DevMode&>(d_->settings))->dmDefaultSource;
}

bool Printer::showProperties(void* parent) {
  if (!d_->handle || d_->settings.empty()) return false;
  DevMode out(d_->settings.size());
  const LONG r = DocumentPropertiesW(static_cast<HWND>(parent), d_->handle, const_cast<wchar_t*>(name_.c_str()),
                                     dm(out), dm(d_->settings), DM_IN_BUFFER | DM_IN_PROMPT | DM_OUT_BUFFER);
  if (r != IDOK) return false;
  d_->settings = std::move(out);
  return true;
}

namespace {

DevMode withPaper(const DevMode& base, short paper, short bin) {
  DevMode d = base;
  DEVMODEW* m = dm(d);
  if (paper > 0) {
    m->dmFields |= DM_PAPERSIZE;
    m->dmFields &= ~DWORD(DM_PAPERLENGTH | DM_PAPERWIDTH | DM_FORMNAME);
    m->dmPaperSize = paper;
  }
  m->dmFields |= DM_DEFAULTSOURCE | DM_ORIENTATION;
  m->dmDefaultSource = bin > 0 ? bin : short(DMBIN_AUTO);
  m->dmOrientation = DMORIENT_PORTRAIT;  // pdfz turns landscape pages itself
  return d;
}

Geometry geometryOf(HDC dc) {
  Geometry g;
  g.dpiX = GetDeviceCaps(dc, LOGPIXELSX);
  g.dpiY = GetDeviceCaps(dc, LOGPIXELSY);
  if (g.dpiX <= 0 || g.dpiY <= 0) return g;
  const double kx = 72.0 / g.dpiX, ky = 72.0 / g.dpiY;
  g.paperW = GetDeviceCaps(dc, PHYSICALWIDTH) * kx;
  g.paperH = GetDeviceCaps(dc, PHYSICALHEIGHT) * ky;
  g.areaX = GetDeviceCaps(dc, PHYSICALOFFSETX) * kx;
  g.areaY = GetDeviceCaps(dc, PHYSICALOFFSETY) * ky;
  g.areaW = GetDeviceCaps(dc, HORZRES) * kx;
  g.areaH = GetDeviceCaps(dc, VERTRES) * ky;
  return g;
}

}  // namespace

Geometry Printer::geometry(short paper, short bin) {
  Geometry g;
  if (d_->settings.empty()) return g;
  DevMode d = withPaper(d_->settings, paper, bin);
  HDC ic = CreateICW(L"WINSPOOL", name_.c_str(), nullptr, dm(d));
  if (!ic) return g;
  g = geometryOf(ic);
  DeleteDC(ic);
  return g;
}

bool Printer::begin(const std::wstring& docName, int copies, bool color, bool duplex) {
  if (d_->settings.empty()) return false;
  DEVMODEW* m = dm(d_->settings);
  copiesDone_ = false;
  if (copies > 1 && DeviceCapabilitiesW(name_.c_str(), nullptr, DC_COPIES, nullptr, nullptr) >= copies) {
    m->dmFields |= DM_COPIES | DM_COLLATE;
    m->dmCopies = short(copies);
    m->dmCollate = DMCOLLATE_TRUE;
    copiesDone_ = true;
  } else {
    m->dmFields |= DM_COPIES;
    m->dmCopies = 1;
  }
  m->dmFields |= DM_COLOR;
  m->dmColor = color ? DMCOLOR_COLOR : DMCOLOR_MONOCHROME;
  if (canDuplex()) {
    m->dmFields |= DM_DUPLEX;
    m->dmDuplex = duplex ? DMDUP_VERTICAL : DMDUP_SIMPLEX;
  }
  d_->current = withPaper(d_->settings, m->dmPaperSize, m->dmDefaultSource);
  d_->dc = CreateDCW(L"WINSPOOL", name_.c_str(), nullptr, dm(d_->current));
  if (!d_->dc) return false;
  DOCINFOW info{};
  info.cbSize = sizeof info;
  info.lpszDocName = docName.c_str();
  if (StartDocW(d_->dc, &info) <= 0) {
    DeleteDC(d_->dc);
    d_->dc = nullptr;
    return false;
  }
  return true;
}

bool Printer::startPage(short paper, short bin) {
  if (!d_->dc) return false;
  // A new paper size or tray: the DC is set up again between pages (ResetDC), as the API
  // allows, so one job can mix sizes and trays.
  DevMode want = withPaper(d_->settings, paper, bin);
  if (want != d_->current) {
    if (!ResetDCW(d_->dc, dm(want))) return false;
    d_->current = std::move(want);
  }
  if (StartPage(d_->dc) <= 0) return false;
  d_->inPage = true;
  page_ = geometryOf(d_->dc);
  return true;
}

bool Printer::drawBGR(const std::uint8_t* bgr, int width, int height, int stride, int x, int y) {
  if (!d_->dc || !d_->inPage || width <= 0 || height <= 0) return false;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = width;
  bi.bmiHeader.biHeight = -height;  // top row first
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 24;
  bi.bmiHeader.biCompression = BI_RGB;
  (void)stride;  // rows must be 4-byte aligned, as the caller makes them
  return StretchDIBits(d_->dc, x, y, width, height, 0, 0, width, height, bgr, &bi, DIB_RGB_COLORS, SRCCOPY) != 0 ||
         SetDIBitsToDevice(d_->dc, x, y, DWORD(width), DWORD(height), 0, 0, 0, UINT(height), bgr, &bi, DIB_RGB_COLORS) != 0;
}

bool Printer::endPage() {
  if (!d_->dc || !d_->inPage) return false;
  d_->inPage = false;
  return EndPage(d_->dc) > 0;
}

bool Printer::end() {
  if (!d_->dc) return false;
  const bool ok = EndDoc(d_->dc) > 0;
  DeleteDC(d_->dc);
  d_->dc = nullptr;
  return ok;
}

void Printer::abort() {
  if (!d_->dc) return;
  AbortDoc(d_->dc);
  DeleteDC(d_->dc);
  d_->dc = nullptr;
}

}  // namespace winprint

#endif
