// SPDX-License-Identifier: GPL-3.0-only
#include "Printing.hpp"

#include "ImageUtil.hpp"

#include "core/document.hpp"

#include <QApplication>
#include <QImage>
#include <QMessageBox>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QProgressDialog>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace {

// Each page is rendered in strips of at most this many bytes, so a page at 1200 DPI never needs
// one huge bitmap.
constexpr double kBandBytes = 16.0 * 1024 * 1024;

}  // namespace

// Pages are rendered at exactly the printer's resolution, already turned the way they go on the
// paper, and drawn 1:1 at whole-pixel positions. The painter is never scaled or rotated: Qt's
// Windows print engine handles a scaled or rotated image by making a transformed copy of it the
// size of the whole printout, which is slow and can run out of memory.
bool printPages(QPrinter& printer, const pdfz::Document& doc, int from, int to,
                const std::function<bool(int, int)>& progress) {
  from = std::max(0, from);
  to = std::min(doc.pageCount() - 1, to);
  if (from > to) return false;

  QPainter painter;
  if (!painter.begin(&printer)) return false;

  // Painter coordinates are printer pixels, with (0, 0) at the top-left of the printable area.
  const QRectF area = printer.pageLayout().paintRectPixels(printer.resolution());
  const double printerDpi = printer.resolution();
  const int total = to - from + 1;

  // The printer driver makes the copies when it can; otherwise print the run that many times.
  const int runs = printer.supportsMultipleCopies() ? 1 : std::max(1, printer.copyCount());

  bool first = true;
  for (int run = 0; run < runs; ++run) {
    for (int i = from; i <= to; ++i) {
      const int done = run * total + (i - from);
      if (progress && !progress(done, runs * total)) {
        printer.abort();
        return false;
      }
      if (!first && !printer.newPage()) return false;
      first = false;

      const pdfz::SizeF s = doc.pageSize(i);
      if (s.width <= 0 || s.height <= 0) continue;
      // Turn landscape pages onto portrait paper (and the other way round).
      const bool turn = (s.width > s.height) != (area.width() > area.height());
      const double availW = turn ? area.height() : area.width();
      const double availH = turn ? area.width() : area.height();
      // Printer pixels per point: actual size if it fits, otherwise shrunk to fit.
      const double scale = std::min({availW / s.width, availH / s.height, printerDpi / 72.0});
      const int pw = std::max(1, int(std::floor(s.width * scale)));   // the page as rendered,
      const int ph = std::max(1, int(std::floor(s.height * scale)));  // before any turning
      // Where the page's top-left goes on the paper, centred.
      const int ox = int(std::lround((area.width() - (turn ? ph : pw)) / 2));
      const int oy = int(std::lround((area.height() - (turn ? pw : ph)) / 2));

      const int bandH = std::max(1, int(kBandBytes / (double(pw) * 4)));
      for (int y = 0; y < ph; y += bandH) {
        if (y > 0 && progress && !progress(done, runs * total)) {  // keeps the UI responsive
          printer.abort();
          return false;
        }
        const int h = std::min(bandH, ph - y);
        QImage band = toQImage(
            doc.render(i, scale, {0, y, pw, h}, pdfz::RenderAnnotations | pdfz::RenderForPrinting));
        if (band.isNull()) break;
        if (turn) {
          // A quarter turn counterclockwise: the page's top along the paper's left edge (the
          // usual landscape direction; it also stands a page rotated 90° back upright). Page
          // rows y..y+h become paper columns ox+y..ox+y+h.
          band = band.transformed(QTransform().rotate(-90));
          painter.drawImage(QPoint(ox + y, oy), band);
        } else {
          painter.drawImage(QPoint(ox, oy + y), band);
        }
      }
    }
  }
  return painter.end();
}

bool printDocument(QWidget* parent, const pdfz::Document& doc, const QString& title, int currentPage) {
  if (doc.pageCount() == 0) return false;
  QPrinter printer(QPrinter::HighResolution);
  printer.setDocName(title);
  printer.setFromTo(1, doc.pageCount());

  QPrintDialog dialog(&printer, parent);
  dialog.setWindowTitle(QObject::tr("Print"));
  dialog.setOption(QAbstractPrintDialog::PrintPageRange, true);
  dialog.setOption(QAbstractPrintDialog::PrintCurrentPage, true);
  dialog.setOption(QAbstractPrintDialog::PrintToFile, true);
  dialog.setMinMax(1, doc.pageCount());
  if (dialog.exec() != QDialog::Accepted) return false;

  int from = 0;
  int to = doc.pageCount() - 1;
  switch (printer.printRange()) {
    case QPrinter::PageRange:
      from = printer.fromPage() - 1;
      to = printer.toPage() - 1;
      break;
    case QPrinter::CurrentPage: from = to = currentPage; break;
    default: break;
  }

  QProgressDialog progress(QObject::tr("Printing…"), QObject::tr("Cancel"), 0, to - from + 1, parent);
  progress.setWindowModality(Qt::WindowModal);
  progress.setMinimumDuration(500);
  const bool ok = printPages(printer, doc, from, to, [&](int done, int total) {
    progress.setMaximum(total);
    progress.setValue(done);
    QApplication::processEvents();
    return !progress.wasCanceled();
  });
  progress.setValue(progress.maximum());
  if (!ok && !progress.wasCanceled())
    QMessageBox::warning(parent, QObject::tr("Print"), QObject::tr("Printing failed."));
  return ok;
}
