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

bool isCustomPaper(const PaperChoice& p) {
  // DMPAPER_USER, or a driver's own custom size (PostScript drivers call theirs "PostScript
  // Custom Page Size"; plotters "Custom", "User Defined"...).
  if (p.id == 256) return true;
  const QString n = p.name.toLower();
  return n.contains(QLatin1String("custom")) || n.contains(QLatin1String("user defined")) ||
         n.contains(QLatin1String("user-defined"));
}

PaperPick paperForSize(const QList<PaperChoice>& papers, QSizeF page) {
  const double w = std::min(page.width(), page.height()), h = std::max(page.width(), page.height());
  const double slack = 6;  // points: about 2 mm, for sizes rounded to millimeters or inches
  const PaperChoice* exact = nullptr;
  const PaperChoice* fits = nullptr;
  const PaperChoice* custom = nullptr;
  const PaperChoice* largest = nullptr;
  double bestErr = slack, bestArea = 0, largestArea = 0;
  for (const PaperChoice& p : papers) {
    if (p.size.isEmpty()) continue;
    if (isCustomPaper(p)) {
      if (!custom) custom = &p;
      continue;
    }
    const double err = std::max(std::abs(p.size.width() - w), std::abs(p.size.height() - h));
    if (err < bestErr) {
      bestErr = err;
      exact = &p;
    }
    const double area = p.size.width() * p.size.height();
    if (p.size.width() + slack >= w && p.size.height() + slack >= h && (!fits || area < bestArea)) {
      fits = &p;
      bestArea = area;
    }
    if (!largest || area > largestArea) {
      largest = &p;
      largestArea = area;
    }
  }
  if (exact) return {exact->id, {}};
  if (fits) return {fits->id, {}};
  if (custom) return {custom->id, QSizeF(w, h)};
  if (largest) return {largest->id, {}};
  return {-1, {}};
}

QString paperDescription(const QList<PaperChoice>& papers, const PaperPick& pick) {
  for (const PaperChoice& p : papers)
    if (p.id == pick.id) return pick.customSize.isEmpty() ? p.name : QObject::tr("%1, made the page's size").arg(p.name);
  return {};
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
  SheetGeometry geometry(const PaperPick& paper, int source) override {
    if (!printer_) return {};
    return convert(printer_->geometry(request(paper), short(source == kAutoSource ? 7 : source)));
  }
  bool begin(const QString& docName, int copies, bool color, bool duplex) override {
    return printer_ && printer_->begin(docName.toStdWString(), copies, color, duplex);
  }
  bool copiesDone() const override { return printer_ && printer_->copiesDone(); }
  bool startPage(const PaperPick& paper, int source) override {
    return printer_ && printer_->startPage(request(paper), short(source == kAutoSource ? 7 : source));
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
  winprint::PaperRequest request(const PaperPick& pick) const {
    winprint::PaperRequest r;
    r.id = short(pick.id);
    r.widthPt = pick.customSize.width();
    r.heightPt = pick.customSize.height();
    if (printer_ && pick.customSize.isEmpty())
      for (const auto& p : printer_->papers())
        if (p.id == r.id) r.form = p.name;
    return r;
  }
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
  SheetGeometry geometry(const PaperPick& paper, int) override {
    if (!printer_) return {};
    QPageLayout layout = printer_->pageLayout();
    layout.setOrientation(QPageLayout::Portrait);
    layout.setPageSize(pageSize(paper));
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
  bool startPage(const PaperPick& paper, int source) override {
    QPageLayout layout = printer_->pageLayout();
    layout.setOrientation(QPageLayout::Portrait);
    if (paper.id > 0 || !paper.customSize.isEmpty()) layout.setPageSize(pageSize(paper));
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
  static QPageSize pageSize(const PaperPick& p) {
    if (!p.customSize.isEmpty()) return QPageSize(p.customSize, QPageSize::Point, QString(), QPageSize::ExactMatch);
    return QPageSize(QPageSize::PageSizeId(p.id));
  }
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
