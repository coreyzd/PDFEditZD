// SPDX-License-Identifier: GPL-3.0-only
#include "Printing.hpp"

#include "ImageUtil.hpp"

#include "core/document.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QTransform>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

// Each page is rendered in strips of at most this many bytes, so a page at 1200 DPI never needs
// one huge bitmap.
constexpr double kBandBytes = 16.0 * 1024 * 1024;

unsigned renderFlags(const PrintOptions& o) {
  unsigned f = pdfz::RenderForPrinting;
  if (o.annotations) f |= pdfz::RenderAnnotations;
  if (o.grayscale) f |= pdfz::RenderGrayscale;
  return f;
}

}  // namespace

Placement placePage(QSizeF page, const SheetGeometry& sheet, const PrintOptions& o) {
  Placement out;
  if (page.isEmpty() || !sheet.isValid()) return out;
  const QRectF area = sheet.printable;
  const bool paperWide = sheet.paper.width() > sheet.paper.height();
  const bool pageWide = page.width() > page.height();
  switch (o.orientation) {
    case PrintOptions::Orientation::Auto: out.turn = page.width() != page.height() && pageWide != paperWide; break;
    case PrintOptions::Orientation::Portrait: out.turn = paperWide; break;
    case PrintOptions::Orientation::Landscape: out.turn = !paperWide; break;
  }
  const double w = out.turn ? page.height() : page.width();
  const double h = out.turn ? page.width() : page.height();
  const double fit = std::min(area.width() / w, area.height() / h);
  switch (o.sizing) {
    case PrintOptions::Sizing::Fit: out.scale = fit; break;
    case PrintOptions::Sizing::Actual: out.scale = 1; break;
    case PrintOptions::Sizing::Shrink: out.scale = std::min(1.0, fit); break;
    case PrintOptions::Sizing::Custom: out.scale = std::clamp(o.customScale, 1.0, 1000.0) / 100; break;
  }
  const QSizeF size(w * out.scale, h * out.scale);
  out.target = QRectF(area.center() - QPointF(size.width() / 2, size.height() / 2), size);
  return out;
}

int paperForPage(QSizeF page, const QList<PaperChoice>& papers, const PrintOptions& o, int printersPaper) {
  if (o.paperByPageSize) {
    const int match = matchingPaper(papers, page);
    if (match >= 0) return match;
  }
  return o.paper > 0 ? o.paper : printersPaper;
}

int sourceForPage(const PrintOptions& o) { return o.paperByPageSize ? kAutoSource : o.source; }

// Pages are rendered at the printer's resolution, already turned the way they go on the paper,
// and drawn 1:1 at whole device pixels: printer drivers do badly with scaled or rotated images.
bool printPages(PrintBackend& printer, const pdfz::Document& doc, const QString& title, const PrintOptions& o,
                const std::function<bool(int, int)>& progress) {
  std::vector<int> pages;
  for (int p : o.pages)
    if (p >= 0 && p < doc.pageCount()) pages.push_back(p);
  if (pages.empty()) return false;
  const QList<PaperChoice> papers = printer.papers();
  const int printersPaper = printer.currentPaper();
  if (!printer.begin(title, std::max(1, o.copies), !o.grayscale, o.duplex)) return false;
  const int runs = printer.copiesDone() ? 1 : std::max(1, o.copies);
  const int total = runs * int(pages.size());
  for (int run = 0, done = 0; run < runs; ++run) {
    for (int i : pages) {
      if (progress && !progress(done, total)) {
        printer.abort();
        return false;
      }
      const pdfz::SizeF s = doc.pageSize(i);
      const QSizeF page(s.width, s.height);
      if (!printer.startPage(paperForPage(page, papers, o, printersPaper), sourceForPage(o))) {
        printer.abort();
        return false;
      }
      const SheetGeometry g = printer.pageGeometry();
      const Placement pl = placePage(page, g, o);
      // In device pixels: the page as rendered (before turning), and where it goes, from the
      // printable area's top-left.
      const double dpi = g.dpiX;
      const double scale = pl.scale * dpi / 72;
      const int pw = std::max(1, int(std::floor(s.width * scale)));
      const int ph = std::max(1, int(std::floor(s.height * scale)));
      const int ox = int(std::lround((pl.target.left() - g.printable.left()) * dpi / 72));
      const int oy = int(std::lround((pl.target.top() - g.printable.top()) * g.dpiY / 72));
      const int areaW = int(std::lround(g.printable.width() * dpi / 72));
      const int areaH = int(std::lround(g.printable.height() * g.dpiY / 72));
      const int bandH = std::max(1, int(kBandBytes / (double(pw) * 4)));
      for (int y = 0; y < ph; y += bandH) {
        const int h = std::min(bandH, ph - y);
        // Off the printable area (a large page at actual size): nothing to draw.
        if (pl.turn ? (ox + y + h <= 0 || ox + y >= areaW) : (oy + y + h <= 0 || oy + y >= areaH)) continue;
        if (y > 0 && progress && !progress(done, total)) {
          printer.abort();
          return false;
        }
        QImage band = toQImage(doc.render(i, scale, {0, y, pw, h}, renderFlags(o)));
        if (band.isNull()) break;
        if (pl.turn) {
          // A quarter turn counterclockwise: the page's top along the paper's left edge. Page
          // rows y..y+h become paper columns ox+y..ox+y+h.
          band = band.transformed(QTransform().rotate(-90));
          printer.drawImage(band, QPoint(ox + y, oy));
        } else {
          printer.drawImage(band, QPoint(ox, oy + y));
        }
      }
      if (!printer.endPage()) {
        printer.abort();
        return false;
      }
      ++done;
    }
  }
  return printer.end();
}

// ---------------------------------------------------------------------------------------------
// The dialog

namespace {

// "1-3, 5, 8-" as 0-based pages; empty if it isn't one.
std::vector<int> parseRange(const QString& text, int count) {
  std::vector<int> out;
  const QStringList parts = text.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
  for (const QString& part : parts) {
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("^(\\d*)\\s*(?:[-–]\\s*(\\d*))?$")).match(part);
    if (!m.hasMatch() || (m.captured(1).isEmpty() && m.captured(2).isEmpty())) return {};
    const bool range = part.contains('-') || part.contains(QChar(0x2013));
    const int a = m.captured(1).isEmpty() ? 1 : m.captured(1).toInt();
    const int b = !range ? a : m.captured(2).isEmpty() ? count : m.captured(2).toInt();
    if (a < 1 || b < 1 || a > count || b > count) return {};
    for (int p = a; a <= b ? p <= b : p >= b; p += a <= b ? 1 : -1) out.push_back(p - 1);
  }
  return out;
}

QString inches(double pt) { return QString::number(pt / 72, 'f', pt / 72 == std::floor(pt / 72) ? 0 : 2); }

// A sheet of paper with the page on it, as it will print.
class PreviewWidget : public QWidget {
public:
  explicit PreviewWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(360, 380);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }
  void show(const QImage& page, const SheetGeometry& sheet, const Placement& pl) {
    page_ = page;
    sheet_ = sheet;
    place_ = pl;
    update();
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Window).darker(108));
    if (!sheet_.isValid()) return;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    // Shown the way it reads: a sheet the page is turned on is shown turned too.
    const bool turned = place_.turn;
    const QSizeF paper = turned ? sheet_.paper.transposed() : sheet_.paper;
    const double k = std::min((width() - 24) / paper.width(), (height() - 24) / paper.height());
    const QRectF sheetR(QPointF((width() - paper.width() * k) / 2, (height() - paper.height() * k) / 2), paper * k);
    // Sheet coordinates (points, unturned) to widget coordinates.
    auto map = [&](const QRectF& r) {
      if (!turned) return QRectF(sheetR.topLeft() + r.topLeft() * k, r.size() * k);
      // Turned back a quarter turn clockwise: sheet x runs down, sheet y runs right to left.
      const QSizeF s = sheet_.paper;
      return QRectF(sheetR.left() + (s.height() - r.bottom()) * k, sheetR.top() + r.left() * k, r.height() * k,
                    r.width() * k);
    };
    p.fillRect(sheetR.translated(3, 3), QColor(0, 0, 0, 60));
    p.fillRect(sheetR, Qt::white);
    p.save();
    p.setClipRect(map(sheet_.printable));
    if (!page_.isNull()) p.drawImage(map(place_.target), page_);
    p.restore();
    p.setPen(QPen(QColor(0, 0, 0, 90), 1, Qt::DotLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(map(sheet_.printable));
    p.setPen(QPen(QColor(0x40, 0x40, 0x40), 1));
    p.drawRect(sheetR);
  }

private:
  QImage page_;
  SheetGeometry sheet_;
  Placement place_;
};

class PrintDialog : public QDialog {
public:
  PrintDialog(QWidget* parent, const pdfz::Document& doc, PrintBackend& printer, int currentPage);
  PrintOptions options() const;

private:
  void printerChanged();
  void pageSetup();
  void updatePreview();
  std::vector<int> chosenPages() const;
  void save() const;

  const pdfz::Document& doc_;
  PrintBackend& printer_;
  int currentPage_;
  QList<PaperChoice> papers_;
  int paper_ = 0;  // Page Setup's, 0 for the printer's own
  int source_ = kAutoSource;
  QHash<QString, QImage> previews_;  // "page/flags"

  QComboBox* printers_;
  QPushButton* properties_;
  QSpinBox* copies_;
  QCheckBox* gray_;
  QCheckBox* duplex_;
  QRadioButton* all_;
  QRadioButton* current_;
  QRadioButton* range_;
  QLineEdit* rangeText_;
  QCheckBox* reverse_;
  QRadioButton* fit_;
  QRadioButton* actual_;
  QRadioButton* shrink_;
  QRadioButton* custom_;
  QDoubleSpinBox* customScale_;
  QCheckBox* byPageSize_;
  QRadioButton* auto_;
  QRadioButton* portrait_;
  QRadioButton* landscape_;
  QComboBox* markups_;
  QLabel* scaleLabel_;
  QLabel* sizeLabel_;
  PreviewWidget* preview_;
  QSlider* slider_;
  QLabel* pageLabel_;
  QPushButton* printButton_;
};

PrintDialog::PrintDialog(QWidget* parent, const pdfz::Document& doc, PrintBackend& printer, int currentPage)
    : QDialog(parent), doc_(doc), printer_(printer), currentPage_(currentPage) {
  setWindowTitle(tr("Print"));
  QSettings st;
  auto* root = new QVBoxLayout(this);
  auto* top = new QHBoxLayout;
  root->addLayout(top, 1);
  auto* left = new QVBoxLayout;
  top->addLayout(left, 0);

  // Printer, copies
  auto* printerRow = new QHBoxLayout;
  printerRow->addWidget(new QLabel(tr("Printer:"), this));
  printers_ = new QComboBox(this);
  printers_->addItems(printer_.printers());
  printers_->setMinimumContentsLength(24);
  const QString last = st.value("print/printer").toString();
  const QString wanted = printers_->findText(last) >= 0 ? last : printer_.defaultPrinter();
  if (printers_->findText(wanted) >= 0) printers_->setCurrentText(wanted);
  printerRow->addWidget(printers_, 1);
  properties_ = new QPushButton(tr("Properties…"), this);
  properties_->setToolTip(tr("The printer's own settings"));
  printerRow->addWidget(properties_);
  left->addLayout(printerRow);
  auto* copiesRow = new QHBoxLayout;
  copiesRow->addWidget(new QLabel(tr("Copies:"), this));
  copies_ = new QSpinBox(this);
  copies_->setRange(1, 999);
  copiesRow->addWidget(copies_);
  copiesRow->addSpacing(12);
  gray_ = new QCheckBox(tr("Print in grayscale (black and white)"), this);
  gray_->setChecked(st.value("print/grayscale", false).toBool());
  copiesRow->addWidget(gray_);
  copiesRow->addStretch(1);
  left->addLayout(copiesRow);

  // Pages
  auto* pagesBox = new QGroupBox(tr("Pages to Print"), this);
  auto* pagesLay = new QGridLayout(pagesBox);
  all_ = new QRadioButton(tr("All"), pagesBox);
  current_ = new QRadioButton(tr("Current"), pagesBox);
  range_ = new QRadioButton(tr("Pages"), pagesBox);
  rangeText_ = new QLineEdit(QStringLiteral("1-%1").arg(doc_.pageCount()), pagesBox);
  rangeText_->setToolTip(tr("Page numbers and ranges, e.g. 1-3, 5, 8-"));
  all_->setChecked(true);
  pagesLay->addWidget(all_, 0, 0);
  pagesLay->addWidget(current_, 0, 1);
  pagesLay->addWidget(range_, 0, 2);
  pagesLay->addWidget(rangeText_, 0, 3);
  reverse_ = new QCheckBox(tr("Reverse pages"), pagesBox);
  pagesLay->addWidget(reverse_, 1, 0, 1, 2);
  left->addWidget(pagesBox);

  // Sizing
  auto* sizeBox = new QGroupBox(tr("Page Sizing && Handling"), this);
  auto* sizeLay = new QGridLayout(sizeBox);
  fit_ = new QRadioButton(tr("Fit"), sizeBox);
  actual_ = new QRadioButton(tr("Actual size"), sizeBox);
  shrink_ = new QRadioButton(tr("Shrink oversized pages"), sizeBox);
  custom_ = new QRadioButton(tr("Custom Scale:"), sizeBox);
  customScale_ = new QDoubleSpinBox(sizeBox);
  customScale_->setRange(1, 1000);
  customScale_->setDecimals(0);
  customScale_->setSuffix(QStringLiteral(" %"));
  customScale_->setValue(st.value("print/customScale", 100).toDouble());
  sizeLay->addWidget(fit_, 0, 0);
  sizeLay->addWidget(actual_, 0, 1);
  sizeLay->addWidget(shrink_, 1, 0);
  auto* customRow = new QHBoxLayout;
  customRow->addWidget(custom_);
  customRow->addWidget(customScale_);
  customRow->addStretch(1);
  sizeLay->addLayout(customRow, 1, 1);
  QRadioButton* sizings[] = {fit_, actual_, shrink_, custom_};
  sizings[std::clamp(st.value("print/sizing", 0).toInt(), 0, 3)]->setChecked(true);
  byPageSize_ = new QCheckBox(tr("Choose paper source by PDF page size"), sizeBox);
  byPageSize_->setToolTip(tr("Each page on the printer's paper of its size, from the tray that has it"));
  byPageSize_->setChecked(st.value("print/byPageSize", true).toBool());
  sizeLay->addWidget(byPageSize_, 2, 0, 1, 2);
  left->addWidget(sizeBox);

  // Two-sided, orientation
  auto* handling = new QGroupBox(tr("Orientation"), this);
  auto* hLay = new QGridLayout(handling);
  duplex_ = new QCheckBox(tr("Print on both sides of paper"), handling);
  hLay->addWidget(duplex_, 0, 0, 1, 3);
  auto_ = new QRadioButton(tr("Auto"), handling);
  portrait_ = new QRadioButton(tr("Portrait"), handling);
  landscape_ = new QRadioButton(tr("Landscape"), handling);
  QRadioButton* orients[] = {auto_, portrait_, landscape_};
  orients[std::clamp(st.value("print/orientation", 0).toInt(), 0, 2)]->setChecked(true);
  hLay->addWidget(auto_, 1, 0);
  hLay->addWidget(portrait_, 1, 1);
  hLay->addWidget(landscape_, 1, 2);
  left->addWidget(handling);

  // Comments
  auto* markBox = new QGroupBox(tr("Comments && Forms"), this);
  auto* markLay = new QHBoxLayout(markBox);
  markups_ = new QComboBox(markBox);
  markups_->addItem(tr("Document and Markups"));
  markups_->addItem(tr("Document"));
  markups_->setCurrentIndex(st.value("print/markups", 0).toInt() == 1 ? 1 : 0);
  markLay->addWidget(markups_, 1);
  left->addWidget(markBox);
  left->addStretch(1);

  // Preview
  auto* right = new QVBoxLayout;
  top->addLayout(right, 1);
  auto* labels = new QHBoxLayout;
  scaleLabel_ = new QLabel(this);
  sizeLabel_ = new QLabel(this);
  labels->addWidget(scaleLabel_);
  labels->addStretch(1);
  labels->addWidget(sizeLabel_);
  right->addLayout(labels);
  preview_ = new PreviewWidget(this);
  right->addWidget(preview_, 1);
  auto* nav = new QHBoxLayout;
  auto* prev = new QToolButton(this);
  prev->setArrowType(Qt::LeftArrow);
  auto* next = new QToolButton(this);
  next->setArrowType(Qt::RightArrow);
  slider_ = new QSlider(Qt::Horizontal, this);
  pageLabel_ = new QLabel(this);
  pageLabel_->setAlignment(Qt::AlignCenter);
  nav->addWidget(prev);
  nav->addWidget(slider_, 1);
  nav->addWidget(next);
  right->addLayout(nav);
  right->addWidget(pageLabel_);
  connect(prev, &QToolButton::clicked, slider_, [this] { slider_->setValue(slider_->value() - 1); });
  connect(next, &QToolButton::clicked, slider_, [this] { slider_->setValue(slider_->value() + 1); });

  // Buttons
  auto* bottom = new QHBoxLayout;
  auto* setup = new QPushButton(tr("Page Setup…"), this);
  setup->setToolTip(tr("The paper size and tray for pages that aren't matched by size"));
  bottom->addWidget(setup);
  bottom->addStretch(1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  printButton_ = buttons->button(QDialogButtonBox::Ok);
  printButton_->setText(tr("Print"));
  bottom->addWidget(buttons);
  root->addLayout(bottom);

  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    if (chosenPages().empty()) {
      QMessageBox::warning(this, tr("Print"), tr("Type page numbers between 1 and %1, like 1-3, 5.").arg(doc_.pageCount()));
      return;
    }
    save();
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(setup, &QPushButton::clicked, this, &PrintDialog::pageSetup);
  connect(properties_, &QPushButton::clicked, this, [this] {
    if (printer_.showProperties(this)) updatePreview();
  });
  connect(printers_, &QComboBox::currentTextChanged, this, &PrintDialog::printerChanged);
  auto refresh = [this] { updatePreview(); };
  for (QRadioButton* b : {all_, current_, range_, fit_, actual_, shrink_, custom_, auto_, portrait_, landscape_})
    connect(b, &QRadioButton::toggled, this, refresh);
  for (QCheckBox* b : {gray_, reverse_, byPageSize_}) connect(b, &QCheckBox::toggled, this, refresh);
  connect(customScale_, &QDoubleSpinBox::valueChanged, this, [this] {
    custom_->setChecked(true);
    updatePreview();
  });
  connect(rangeText_, &QLineEdit::textEdited, this, [this] {
    range_->setChecked(true);
    updatePreview();
  });
  connect(markups_, &QComboBox::currentIndexChanged, this, refresh);
  connect(slider_, &QSlider::valueChanged, this, refresh);
  printerChanged();
  resize(940, 640);
}

void PrintDialog::printerChanged() {
  const QString name = printers_->currentText();
  const bool ok = !name.isEmpty() && printer_.select(name);
  papers_ = ok ? printer_.papers() : QList<PaperChoice>{};
  QSettings st;
  paper_ = st.value("print/paper/" + name, 0).toInt();
  source_ = st.value("print/source/" + name, kAutoSource).toInt();
  if (std::none_of(papers_.begin(), papers_.end(), [&](const PaperChoice& p) { return p.id == paper_; })) paper_ = 0;
  properties_->setEnabled(ok && printer_.hasProperties());
  duplex_->setEnabled(ok && printer_.canDuplex());
  if (!printer_.canDuplex()) duplex_->setChecked(false);
  printButton_->setEnabled(ok);
  updatePreview();
}

std::vector<int> PrintDialog::chosenPages() const {
  std::vector<int> pages;
  const int n = doc_.pageCount();
  if (current_->isChecked()) pages = {std::clamp(currentPage_, 0, n - 1)};
  else if (range_->isChecked()) pages = parseRange(rangeText_->text(), n);
  else
    for (int i = 0; i < n; ++i) pages.push_back(i);
  if (reverse_->isChecked()) std::reverse(pages.begin(), pages.end());
  return pages;
}

PrintOptions PrintDialog::options() const {
  PrintOptions o;
  o.pages = chosenPages();
  o.copies = copies_->value();
  o.grayscale = gray_->isChecked();
  o.duplex = duplex_->isChecked();
  o.annotations = markups_->currentIndex() == 0;
  o.sizing = fit_->isChecked() ? PrintOptions::Sizing::Fit
             : actual_->isChecked() ? PrintOptions::Sizing::Actual
             : shrink_->isChecked() ? PrintOptions::Sizing::Shrink
                                    : PrintOptions::Sizing::Custom;
  o.customScale = customScale_->value();
  o.orientation = portrait_->isChecked() ? PrintOptions::Orientation::Portrait
                  : landscape_->isChecked() ? PrintOptions::Orientation::Landscape
                                            : PrintOptions::Orientation::Auto;
  o.paperByPageSize = byPageSize_->isChecked();
  o.paper = paper_;
  o.source = source_;
  return o;
}

void PrintDialog::save() const {
  QSettings st;
  const PrintOptions o = options();
  st.setValue("print/printer", printers_->currentText());
  st.setValue("print/grayscale", o.grayscale);
  st.setValue("print/sizing", int(o.sizing));
  st.setValue("print/customScale", o.customScale);
  st.setValue("print/orientation", int(o.orientation));
  st.setValue("print/byPageSize", o.paperByPageSize);
  st.setValue("print/markups", o.annotations ? 0 : 1);
}

void PrintDialog::updatePreview() {
  const std::vector<int> pages = chosenPages();
  {
    const QSignalBlocker block(slider_);
    slider_->setRange(0, std::max(0, int(pages.size()) - 1));
  }
  slider_->setEnabled(pages.size() > 1);
  if (pages.empty() || papers_.isEmpty()) {
    preview_->show({}, {}, {});
    pageLabel_->setText(pages.empty() ? tr("No pages") : QString());
    scaleLabel_->clear();
    sizeLabel_->clear();
    return;
  }
  const int at = std::clamp(slider_->value(), 0, int(pages.size()) - 1);
  const int page = pages[std::size_t(at)];
  pageLabel_->setText(tr("Page %1 of %2").arg(at + 1).arg(pages.size()));
  const PrintOptions o = options();
  const pdfz::SizeF s = doc_.pageSize(page);
  const QSizeF pageSize(s.width, s.height);
  const int paper = paperForPage(pageSize, papers_, o, printer_.currentPaper());
  const SheetGeometry sheet = printer_.geometry(paper, sourceForPage(o));
  const Placement pl = placePage(pageSize, sheet, o);

  const unsigned flags = renderFlags(o) & ~pdfz::RenderForPrinting;
  const QString key = QStringLiteral("%1/%2").arg(page).arg(flags);
  QImage img = previews_.value(key);
  if (img.isNull()) {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    img = toQImage(doc_.renderFit(page, 900, 900, flags)).copy();
    QApplication::restoreOverrideCursor();
    if (previews_.size() > 40) previews_.clear();
    previews_.insert(key, img);
  }
  preview_->show(img, sheet, pl);  // a turned sheet is shown turned, so the page reads upright
  scaleLabel_->setText(tr("Scale: %1%").arg(std::lround(pl.scale * 100)));
  // The sheet the way the page reads on it.
  QString paperName;
  for (const PaperChoice& p : papers_)
    if (p.id == paper) paperName = p.name;
  const QSizeF shown = pl.turn ? sheet.paper.transposed() : sheet.paper;
  sizeLabel_->setText(tr("%1 x %2 Inches").arg(inches(shown.width()), inches(shown.height())) +
                      (paperName.isEmpty() ? QString() : QStringLiteral("  (%1)").arg(paperName)));
}

void PrintDialog::pageSetup() {
  QDialog dlg(this);
  dlg.setWindowTitle(tr("Page Setup"));
  auto* lay = new QVBoxLayout(&dlg);
  auto* form = new QFormLayout;
  auto* size = new QComboBox(&dlg);
  size->addItem(tr("The printer's setting"), 0);
  for (const PaperChoice& p : papers_)
    size->addItem(QStringLiteral("%1 (%2 x %3 in)").arg(p.name, inches(p.size.width()), inches(p.size.height())), p.id);
  size->setCurrentIndex(std::max(0, size->findData(paper_)));
  auto* source = new QComboBox(&dlg);
  source->addItem(tr("Auto"), kAutoSource);
  for (const SourceChoice& sc : printer_.sources()) source->addItem(sc.name, sc.id);
  source->setCurrentIndex(std::max(0, source->findData(source_)));
  form->addRow(tr("Size:"), size);
  form->addRow(tr("Source:"), source);
  lay->addLayout(form);
  auto* note = new QLabel(tr("With \"Choose paper source by PDF page size\" on, these are for pages whose size "
                             "the printer doesn't have."),
                          &dlg);
  note->setWordWrap(true);
  note->setEnabled(false);
  lay->addWidget(note);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(buttons);
  if (dlg.exec() != QDialog::Accepted) return;
  paper_ = size->currentData().toInt();
  source_ = source->currentData().toInt();
  QSettings st;
  st.setValue("print/paper/" + printers_->currentText(), paper_);
  st.setValue("print/source/" + printers_->currentText(), source_);
  updatePreview();
}

}  // namespace

bool printDocument(QWidget* parent, const pdfz::Document& doc, const QString& title, int currentPage) {
  if (doc.pageCount() == 0) return false;
  auto printer = PrintBackend::create();
  if (printer->printers().isEmpty()) {
    QMessageBox::warning(parent, QObject::tr("Print"), QObject::tr("There are no printers set up on this computer."));
    return false;
  }
  PrintDialog dialog(parent, doc, *printer, currentPage);
  if (dialog.exec() != QDialog::Accepted) return false;
  const PrintOptions options = dialog.options();

  QProgressDialog progress(QObject::tr("Printing…"), QObject::tr("Cancel"), 0, int(options.pages.size()), parent);
  progress.setWindowModality(Qt::WindowModal);
  progress.setMinimumDuration(500);
  const bool ok = printPages(*printer, doc, title, options, [&](int done, int total) {
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
