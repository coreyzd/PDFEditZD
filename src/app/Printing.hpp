// SPDX-License-Identifier: GPL-3.0-only
// File > Print: a print dialog like Acrobat's (the printer and its own settings, pages, how
// pages are sized onto the paper, paper chosen by each PDF page's size, a preview to page
// through), and printing: each page rendered at the printer's resolution, placed and turned to
// suit the paper.
#pragma once

#include "PrintBackend.hpp"

#include <QRectF>
#include <QSizeF>
#include <QString>

#include <functional>
#include <vector>

class QWidget;
namespace pdfz { class Document; }

struct PrintOptions {
  enum class Sizing { Fit, Actual, Shrink, Custom };
  enum class Orientation { Auto, Portrait, Landscape };
  std::vector<int> pages;  // 0-based, in the order they print
  int copies = 1;
  bool grayscale = false;
  bool duplex = false;
  bool annotations = true;  // comments and filled-in forms
  Sizing sizing = Sizing::Fit;
  double customScale = 100;  // percent
  Orientation orientation = Orientation::Auto;
  // Each page on the printer's paper of the same size (in the tray that has it); pages of a
  // size it doesn't have go on `paper`.
  bool paperByPageSize = true;
  int paper = 0;  // from Page Setup; 0 for the printer's own
  int source = kAutoSource;
};

// Where a page goes on a sheet: turned a quarter turn or not, scaled (paper points per page
// point), and the rect it covers on the sheet (points, from the sheet's top-left).
struct Placement {
  bool turn = false;
  double scale = 1;
  QRectF target;
};
Placement placePage(QSizeF pagePt, const SheetGeometry& sheet, const PrintOptions& options);

// The paper and tray a page prints on.
int paperForPage(QSizeF pagePt, const QList<PaperChoice>& papers, const PrintOptions& options, int printersPaper);
int sourceForPage(const PrintOptions& options);

// Shows the print dialog and prints. Returns false if canceled or printing failed.
bool printDocument(QWidget* parent, const pdfz::Document& doc, const QString& title, int currentPage);

// Prints with no dialog. `progress` gets how many pages are done; returning false cancels.
bool printPages(PrintBackend& printer, const pdfz::Document& doc, const QString& title, const PrintOptions& options,
                const std::function<bool(int done, int total)>& progress = {});
