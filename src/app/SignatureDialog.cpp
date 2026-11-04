// SPDX-License-Identifier: GPL-3.0-only
#include "SignatureDialog.hpp"

#include "Markup.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QRadioButton>
#include <QSettings>
#include <QSlider>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cmath>

// The signing pad: a white strip with a line to sign on.
class SignaturePad : public QWidget {
  Q_OBJECT
public:
  explicit SignaturePad(QWidget* parent) : QWidget(parent) {
    setMinimumSize(520, 170);
    setCursor(Qt::CrossCursor);
    setAttribute(Qt::WA_OpaquePaintEvent);
  }
  const QList<QPolygonF>& strokes() const { return strokes_; }
  void clear() {
    strokes_.clear();
    update();
    emit changed();
  }
  void setColor(const QColor& c) {
    color_ = c;
    update();
  }
  static constexpr double kPenWidth = 3.2;

signals:
  void changed();

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.fillRect(rect(), Qt::white);
    p.setRenderHint(QPainter::Antialiasing);
    const double baseline = height() * 0.72;
    p.setPen(QPen(QColor(0xB0, 0xB0, 0xB0), 1));
    p.drawLine(QPointF(24, baseline), QPointF(width() - 24, baseline));
    if (strokes_.isEmpty()) {
      p.setPen(QColor(0x99, 0x99, 0x99));
      p.drawText(QRectF(24, baseline + 4, width() - 48, 24), Qt::AlignLeft | Qt::AlignTop, tr("Sign above the line"));
    }
    Signature s;
    s.strokes = strokes_;
    s.penWidth = kPenWidth;
    p.fillPath(s.outline(), color_);
    p.setPen(QPen(palette().color(QPalette::Mid), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
  }
  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton) return;
    strokes_ << QPolygonF{e->position()};
    update();
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (!(e->buttons() & Qt::LeftButton) || strokes_.isEmpty()) return;
    QPolygonF& s = strokes_.back();
    if (QLineF(s.back(), e->position()).length() >= 1.5) s << e->position();
    update();
  }
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton) emit changed();
  }

private:
  QList<QPolygonF> strokes_;
  QColor color_ = Qt::black;
};

namespace {
const QColor kInkColors[] = {QColor(0, 0, 0), QColor(0x14, 0x3C, 0x9C), QColor(0xB0, 0x10, 0x10)};

QString initialsOf(const QString& name) {
  QString out;
  for (const QString& part : name.split(QRegularExpression(QStringLiteral("[\\s._-]+")), Qt::SkipEmptyParts))
    out += part.front().toUpper();
  return out.left(4);
}
}  // namespace

SignatureDialog::SignatureDialog(bool initials, QWidget* parent) : QDialog(parent), initials_(initials) {
  setWindowTitle(initials ? tr("Add Initials") : tr("Add Signature"));
  auto* lay = new QVBoxLayout(this);
  tabs_ = new QTabWidget(this);
  lay->addWidget(tabs_);

  // Draw
  auto* drawPage = new QWidget(this);
  auto* drawLay = new QVBoxLayout(drawPage);
  pad_ = new SignaturePad(drawPage);
  drawLay->addWidget(pad_, 1);
  auto* clear = new QPushButton(tr("Clear"), drawPage);
  auto* clearRow = new QHBoxLayout;
  clearRow->addStretch(1);
  clearRow->addWidget(clear);
  drawLay->addLayout(clearRow);
  connect(clear, &QPushButton::clicked, pad_, &SignaturePad::clear);
  connect(pad_, &SignaturePad::changed, this, &SignatureDialog::updateOk);
  tabs_->addTab(drawPage, tr("Draw"));

  // Type
  auto* typePage = new QWidget(this);
  auto* typeLay = new QVBoxLayout(typePage);
  typed_ = new QLineEdit(typePage);
  const QString name = commentAuthor();
  typed_->setText(initials ? initialsOf(name) : name);
  typed_->setPlaceholderText(initials ? tr("Your initials") : tr("Your name"));
  typeLay->addWidget(typed_);
  fonts_ = new QListWidget(typePage);
  fonts_->setMinimumHeight(170);
  const QStringList families = handwritingFonts();
  for (const QString& f : families) {
    auto* item = new QListWidgetItem(fonts_);
    item->setData(Qt::UserRole, f);
    item->setToolTip(f);
  }
  if (families.isEmpty()) {
    auto* item = new QListWidgetItem(fonts_);
    item->setData(Qt::UserRole, QString());
    item->setToolTip(tr("No handwriting fonts were found: an italic font is used instead"));
  }
  const QString lastFont = QSettings().value("signatures/font").toString();
  fonts_->setCurrentRow(std::max(0, int(families.indexOf(lastFont))));
  typeLay->addWidget(fonts_, 1);
  connect(typed_, &QLineEdit::textChanged, this, &SignatureDialog::updateTyped);
  connect(fonts_, &QListWidget::currentRowChanged, this, &SignatureDialog::updateOk);
  tabs_->addTab(typePage, tr("Type"));

  // Image
  auto* imagePage = new QWidget(this);
  auto* imageLay = new QVBoxLayout(imagePage);
  auto* choose = new QPushButton(tr("Choose a Picture…"), imagePage);
  imageLay->addWidget(choose, 0, Qt::AlignLeft);
  imagePreview_ = new QLabel(tr("A photo or scan of your signature, on white paper."), imagePage);
  imagePreview_->setAlignment(Qt::AlignCenter);
  imagePreview_->setMinimumHeight(150);
  imagePreview_->setAutoFillBackground(true);
  QPalette pal = imagePreview_->palette();
  pal.setColor(QPalette::Window, QColor(0xF4, 0xF4, 0xF4));
  pal.setColor(QPalette::WindowText, QColor(0x70, 0x70, 0x70));
  imagePreview_->setPalette(pal);
  imageLay->addWidget(imagePreview_, 1);
  auto* thRow = new QHBoxLayout;
  thRow->addWidget(new QLabel(tr("Remove background:"), imagePage));
  threshold_ = new QSlider(Qt::Horizontal, imagePage);
  threshold_->setRange(120, 250);
  threshold_->setValue(200);
  threshold_->setToolTip(tr("Further right clears more of the paper (and lighter ink)"));
  thRow->addWidget(threshold_, 1);
  imageLay->addLayout(thRow);
  connect(choose, &QPushButton::clicked, this, &SignatureDialog::chooseImage);
  connect(threshold_, &QSlider::valueChanged, this, &SignatureDialog::updateImage);
  tabs_->addTab(imagePage, tr("Image"));

  // Ink color, saving, OK
  auto* bottom = new QHBoxLayout;
  bottom->addWidget(new QLabel(tr("Color:"), this));
  colors_ = new QButtonGroup(this);
  const QString colorNames[] = {tr("Black"), tr("Blue"), tr("Red")};
  for (int i = 0; i < 3; ++i) {
    auto* b = new QRadioButton(colorNames[i], this);
    colors_->addButton(b, i);
    bottom->addWidget(b);
  }
  colors_->button(std::clamp(QSettings().value("signatures/color", 0).toInt(), 0, 2))->setChecked(true);
  connect(colors_, &QButtonGroup::idClicked, this, [this](int id) {
    pad_->setColor(kInkColors[id]);
    updateTyped();
  });
  pad_->setColor(kInkColors[colors_->checkedId()]);
  bottom->addStretch(1);
  save_ = new QCheckBox(initials ? tr("Save these initials") : tr("Save this signature"), this);
  save_->setChecked(true);
  save_->setToolTip(tr("Keep it to use again from the Sign button"));
  bottom->addWidget(save_);
  lay->addLayout(bottom);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  ok_ = buttons->button(QDialogButtonBox::Ok);
  ok_->setText(tr("Place It"));
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    QSettings s;
    s.setValue("signatures/color", colors_->checkedId());
    if (fonts_->currentItem()) s.setValue("signatures/font", fonts_->currentItem()->data(Qt::UserRole));
    s.setValue("signatures/tab", tabs_->currentIndex());
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  lay->addWidget(buttons);
  connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
    if (i == 1) typed_->setFocus();
    for (QAbstractButton* b : colors_->buttons()) b->setEnabled(i != 2);  // a picture keeps its own colors
    updateOk();
  });
  tabs_->setCurrentIndex(std::clamp(QSettings().value("signatures/tab", 0).toInt(), 0, 2));
  updateTyped();
  updateOk();
}

void SignatureDialog::updateTyped() {
  // Each font's row shows the name as it would look.
  const QColor ink = kInkColors[std::max(0, colors_ ? colors_->checkedId() : 0)];
  const QString text = typed_->text().trimmed().isEmpty() ? typed_->placeholderText() : typed_->text().trimmed();
  const qreal dpr = devicePixelRatioF();
  fonts_->setIconSize(QSize(440, 56));
  for (int i = 0; i < fonts_->count(); ++i) {
    QListWidgetItem* item = fonts_->item(i);
    Signature s;
    s.kind = Signature::Kind::Typed;
    s.text = text;
    s.family = item->data(Qt::UserRole).toString();
    s.color = ink;
    QImage img = s.preview(QSize(440, 56), dpr);
    item->setIcon(QIcon(QPixmap::fromImage(img)));
    item->setSizeHint(QSize(450, 62));
  }
  updateOk();
}

void SignatureDialog::chooseImage() {
  const QString path = QFileDialog::getOpenFileName(this, tr("Signature Picture"), {},
                                                    tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff)"));
  if (path.isEmpty()) return;
  QImageReader reader(path);
  reader.setAutoTransform(true);  // a phone photo's orientation
  sourceImage_ = reader.read();
  if (sourceImage_.isNull()) {
    imagePreview_->setText(tr("That picture couldn't be read."));
    return;
  }
  updateImage();
}

void SignatureDialog::updateImage() {
  if (sourceImage_.isNull()) return;
  cleanImage_ = cleanSignatureImage(sourceImage_, threshold_->value());
  if (cleanImage_.isNull()) {
    imagePreview_->setPixmap({});
    imagePreview_->setText(tr("No signature found: try moving the slider left."));
  } else {
    Signature s;
    s.kind = Signature::Kind::Image;
    s.image = cleanImage_;
    const QSize box(std::max(200, imagePreview_->width() - 16), std::max(100, imagePreview_->height() - 16));
    imagePreview_->setPixmap(QPixmap::fromImage(s.preview(box, devicePixelRatioF())));
  }
  updateOk();
}

Signature SignatureDialog::signature() const {
  Signature s;
  s.initials = initials_;
  s.color = kInkColors[std::max(0, colors_->checkedId())];
  switch (tabs_->currentIndex()) {
    case 0:
      s.kind = Signature::Kind::Drawn;
      s.strokes = pad_->strokes();
      s.penWidth = SignaturePad::kPenWidth;
      break;
    case 1:
      s.kind = Signature::Kind::Typed;
      s.text = typed_->text().trimmed();
      if (fonts_->currentItem()) s.family = fonts_->currentItem()->data(Qt::UserRole).toString();
      break;
    default:
      s.kind = Signature::Kind::Image;
      s.image = cleanImage_;
      break;
  }
  return s;
}

bool SignatureDialog::saveForLater() const { return save_->isChecked(); }

void SignatureDialog::updateOk() { ok_->setEnabled(!signature().isEmpty()); }

#include "SignatureDialog.moc"
