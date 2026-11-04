// SPDX-License-Identifier: GPL-3.0-only
// The printers, as the print dialog and the print loop see them. On Windows this is the
// Windows printing API (WinPrint), for the driver's tray names, its settings dialog and paper
// that changes from page to page; elsewhere it's Qt's QPrinter.
#pragma once

#include <QImage>
#include <QList>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <memory>

class QWidget;

struct PaperChoice {
  int id = 0;
  QString name;
  QSizeF size;  // points, portrait
};

struct SourceChoice {
  int id = 0;  // kAutoSource: the printer picks (by the paper size)
  QString name;
};
constexpr int kAutoSource = -1;

// A sheet of one paper size: its size and where printing can go on it, in points.
struct SheetGeometry {
  QSizeF paper;
  QRectF printable;
  double dpiX = 300, dpiY = 300;
  bool isValid() const { return paper.width() > 0 && paper.height() > 0 && printable.width() > 0; }
};

class PrintBackend {
public:
  static std::unique_ptr<PrintBackend> create();
  virtual ~PrintBackend() = default;

  virtual QStringList printers() const = 0;
  virtual QString defaultPrinter() const = 0;
  virtual bool select(const QString& printer) = 0;
  virtual QString selected() const = 0;

  virtual QList<PaperChoice> papers() const = 0;
  virtual QList<SourceChoice> sources() const = 0;  // without Auto
  virtual int currentPaper() const = 0;             // in the printer's settings now
  virtual bool canDuplex() const = 0;
  virtual bool hasProperties() const { return false; }
  virtual bool showProperties(QWidget*) { return false; }

  virtual SheetGeometry geometry(int paper, int source) = 0;

  // The job. drawImage() places a picture 1:1 at device pixel `pos` from the printable area's
  // top-left of the page started last.
  virtual bool begin(const QString& docName, int copies, bool color, bool duplex) = 0;
  virtual bool copiesDone() const = 0;  // the printer makes the copies itself
  virtual bool startPage(int paper, int source) = 0;
  virtual SheetGeometry pageGeometry() const = 0;
  virtual bool drawImage(const QImage& image, QPoint pos) = 0;
  virtual bool endPage() = 0;
  virtual bool end() = 0;
  virtual void abort() = 0;
};

// The printer's paper closest to `pagePt` (either way round), within a few points; -1 if none.
int matchingPaper(const QList<PaperChoice>& papers, QSizeF pagePt);
