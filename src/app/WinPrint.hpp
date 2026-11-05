// SPDX-License-Identifier: GPL-3.0-only
// Printing through the Windows printing API directly (Windows only, no Qt): what a printer
// offers (paper sizes, trays by the driver's own names, two-sided printing), the driver's own
// settings dialog, and a print job whose paper size and tray can change from page to page,
// which is what choosing the paper by each PDF page's size needs.
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace winprint {

struct Paper {
  short id = 0;           // DMPAPER_*
  std::wstring name;      // as the driver calls it: "Letter", "Ledger (11 x 17")"...
  double widthPt = 0;     // portrait
  double heightPt = 0;
};

// The paper for a page: one of the printer's sizes, or a size of its own (`widthPt` and
// `heightPt` above zero) given to a custom-size paper (a roll on a plotter).
struct PaperRequest {
  short id = 0;
  std::wstring form;          // the paper's name, which some drivers go by
  double widthPt = 0, heightPt = 0;
};

struct Bin {
  short id = 0;           // DMBIN_*, or the driver's own
  std::wstring name;      // "Bypass Tray", "Drawer 1"...
};

// Where printing can go on a sheet, for one paper size, in points.
struct Geometry {
  double paperW = 0, paperH = 0;          // the whole sheet
  double areaX = 0, areaY = 0;            // the printable area's top-left on it
  double areaW = 0, areaH = 0;
  int dpiX = 0, dpiY = 0;
};

std::vector<std::wstring> printers();
std::wstring defaultPrinter();

class Printer {
public:
  Printer();
  ~Printer();
  Printer(const Printer&) = delete;
  Printer& operator=(const Printer&) = delete;

  // Opens `name` and takes its current settings (the driver's defaults).
  bool open(const std::wstring& name);
  const std::wstring& name() const { return name_; }

  std::vector<Paper> papers() const;
  std::vector<Bin> bins() const;
  bool canDuplex() const;
  short paper() const;   // the paper in the settings now
  short bin() const;

  // The driver's own dialog, starting from (and keeping) these settings. False if canceled.
  bool showProperties(void* parentHwnd);

  Geometry geometry(const PaperRequest& paper, short bin);

  // The job. Copies, color and two-sided apply to it all; paper and bin to each page.
  bool begin(const std::wstring& docName, int copies, bool color, bool duplex);
  bool startPage(const PaperRequest& paper, short bin);  // DMBIN_AUTO (7) leaves the tray to the printer
  const Geometry& pageGeometry() const { return page_; }
  // 24-bit pixels, rows top first (stride bytes apart), at device pixel (x, y) from the
  // printable area's top-left.
  bool drawBGR(const std::uint8_t* bgr, int width, int height, int stride, int x, int y);
  bool endPage();
  bool end();
  void abort();
  bool copiesDone() const { return copiesDone_; }  // the driver makes the copies

private:
  struct Impl;
  std::unique_ptr<Impl> d_;
  std::wstring name_;
  Geometry page_;
  bool copiesDone_ = false;
};

}  // namespace winprint

#endif
