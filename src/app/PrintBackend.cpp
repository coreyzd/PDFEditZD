// SPDX-License-Identifier: GPL-3.0-only
#include "PrintBackend.hpp"

#include "WinPrint.hpp"

#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPrinter>
#include <QPrinterInfo>
#include <QWidget>

#include <cmath>

int matchingPaper(const QList<PaperChoice>& papers, QSizeF page) {
  const double w = std::min(page.width(), page.height()), h = std::max(page.width(), page.height());
  int best = -1;
  double bestErr = 6;  // points: about 2 mm, for sizes rounded to millimeters or inches
  for (const PaperChoice& p : papers) {
    const double err = std::max(std::abs(p.size.width() - w), std::abs(p.size.height() - h));
    if (err < bestErr) {
      bestErr = err;
      best = p.id;
    }
  }
  return best;
}

namespace {

#ifdef Q_OS_WIN

QString fromW(const std::wstring& s) { return QString::fromStdWString(s); }

class WinBackend : public PrintBackend {
public:
  QStringList printers() const override {
    QStringList out;
    for (const auto& p : winprint::printers()) out << fromW(p);
    return out;
  }
  QString defaultPrinter() const override { return fromW(winprint::defaultPrinter()); }
  bool select(const QString& printer) override {
    auto p = std::make_unique<winprint::Printer>();
    if (!p->open(printer.toStdWString())) return false;
    printer_ = std::move(p);
    return true;
  }
  QString selected() const override { return printer_ ? fromW(printer_->name()) : QString(); }
  QList<PaperChoice> papers() const override {
    QList<PaperChoice> out;
    if (printer_)
      for (const auto& p : printer_->papers()) out.push_back({p.id, fromW(p.name), QSizeF(p.widthPt, p.heightPt)});
    return out;
  }
  QList<SourceChoice> sources() const override {
    QList<SourceChoice> out;
    if (printer_)
      for (const auto& b : printer_->bins())
        if (b.id != 7) out.push_back({b.id, fromW(b.name)});  // DMBIN_AUTO is offered as Auto
    return out;
  }
  int currentPaper() const override { return printer_ ? printer_->paper() : 0; }
  bool canDuplex() const override { return printer_ && printer_->canDuplex(); }
  bool hasProperties() const override { return printer_ != nullptr; }
  bool showProperties(QWidget* parent) override {
    return printer_ && printer_->showProperties(parent ? reinterpret_cast<void*>(parent->window()->winId()) : nullptr);
  }
  SheetGeometry geometry(int paper, int source) override {
    if (!printer_) return {};
    return convert(printer_->geometry(short(paper), short(source == kAutoSource ? 7 : source)));
  }
  bool begin(const QString& docName, int copies, bool color, bool duplex) override {
    return printer_ && printer_->begin(docName.toStdWString(), copies, color, duplex);
  }
  bool copiesDone() const override { return printer_ && printer_->copiesDone(); }
  bool startPage(int paper, int source) override {
    return printer_ && printer_->startPage(short(paper), short(source == kAutoSource ? 7 : source));
  }
  SheetGeometry pageGeometry() const override { return printer_ ? convert(printer_->pageGeometry()) : SheetGeometry{}; }
  bool drawImage(const QImage& image, QPoint pos) override {
    const QImage rgb = image.convertToFormat(QImage::Format_BGR888);  // rows 4-byte aligned
    return printer_ && printer_->drawBGR(rgb.constBits(), rgb.width(), rgb.height(), int(rgb.bytesPerLine()), pos.x(), pos.y());
  }
  bool endPage() override { return printer_ && printer_->endPage(); }
  bool end() override { return printer_ && printer_->end(); }
  void abort() override {
    if (printer_) printer_->abort();
  }

private:
  static SheetGeometry convert(const winprint::Geometry& g) {
    SheetGeometry s;
    s.paper = QSizeF(g.paperW, g.paperH);
    s.printable = QRectF(g.areaX, g.areaY, g.areaW, g.areaH);
    if (g.dpiX > 0) s.dpiX = g.dpiX;
    if (g.dpiY > 0) s.dpiY = g.dpiY;
    return s;
  }
  std::unique_ptr<winprint::Printer> printer_;
};

#endif

// Qt's printing (CUPS on Linux). Paper sources are Qt's generic ones.
class QtBackend : public PrintBackend {
public:
  QStringList printers() const override { return QPrinterInfo::availablePrinterNames(); }
  QString defaultPrinter() const override { return QPrinterInfo::defaultPrinterName(); }
  bool select(const QString& name) override {
    info_ = QPrinterInfo::printerInfo(name);
    if (info_.isNull()) return false;
    printer_ = std::make_unique<QPrinter>(info_, QPrinter::HighResolution);
    return true;
  }
  QString selected() const override { return info_.printerName(); }
  QList<PaperChoice> papers() const override {
    QList<PaperChoice> out;
    for (const QPageSize& s : info_.supportedPageSizes()) {
      const QSizeF pt = s.size(QPageSize::Point);
      out.push_back({int(s.id()), s.name(), QSizeF(std::min(pt.width(), pt.height()), std::max(pt.width(), pt.height()))});
    }
    return out;
  }
  // Qt doesn't list a CUPS printer's trays: the printer picks.
  QList<SourceChoice> sources() const override { return {}; }
  int currentPaper() const override { return printer_ ? int(printer_->pageLayout().pageSize().id()) : 0; }
  bool canDuplex() const override { return !info_.supportedDuplexModes().isEmpty() && info_.supportedDuplexModes().size() > 1; }
  SheetGeometry geometry(int paper, int) override {
    if (!printer_) return {};
    QPageLayout layout = printer_->pageLayout();
    layout.setOrientation(QPageLayout::Portrait);
    layout.setPageSize(QPageSize(QPageSize::PageSizeId(paper)));
    return sheet(layout);
  }
  bool begin(const QString& docName, int copies, bool color, bool duplex) override {
    if (!printer_) return false;
    printer_->setDocName(docName);
    printer_->setCopyCount(copies);
    printer_->setColorMode(color ? QPrinter::Color : QPrinter::GrayScale);
    printer_->setDuplex(duplex ? QPrinter::DuplexLongSide : QPrinter::DuplexNone);
    started_ = false;
    return true;
  }
  bool copiesDone() const override { return printer_ && printer_->supportsMultipleCopies(); }
  bool startPage(int paper, int source) override {
    QPageLayout layout = printer_->pageLayout();
    layout.setOrientation(QPageLayout::Portrait);
    if (paper > 0) layout.setPageSize(QPageSize(QPageSize::PageSizeId(paper)));
    if (source != kAutoSource) printer_->setPaperSource(QPrinter::PaperSource(source));
    if (!started_) {
      printer_->setPageLayout(layout);
      if (!painter_.begin(printer_.get())) return false;
      started_ = true;
    } else {
      printer_->setPageLayout(layout);  // takes effect on the new page
      if (!printer_->newPage()) return false;
    }
    page_ = sheet(printer_->pageLayout());
    return true;
  }
  SheetGeometry pageGeometry() const override { return page_; }
  bool drawImage(const QImage& image, QPoint pos) override {
    // Painter coordinates are device pixels from the printable area's top-left.
    painter_.drawImage(pos, image);
    return true;
  }
  bool endPage() override { return true; }
  bool end() override { return started_ ? painter_.end() : true; }
  void abort() override {
    if (printer_) printer_->abort();
    if (started_) painter_.end();
  }

private:
  SheetGeometry sheet(const QPageLayout& layout) const {
    SheetGeometry g;
    g.paper = layout.fullRect(QPageLayout::Point).size();
    g.printable = layout.paintRect(QPageLayout::Point);
    g.dpiX = g.dpiY = printer_->resolution();
    return g;
  }
  QPrinterInfo info_;
  std::unique_ptr<QPrinter> printer_;
  QPainter painter_;
  bool started_ = false;
  SheetGeometry page_;
};

}  // namespace

std::unique_ptr<PrintBackend> PrintBackend::create() {
#ifdef Q_OS_WIN
  return std::make_unique<WinBackend>();
#else
  return std::make_unique<QtBackend>();
#endif
}
