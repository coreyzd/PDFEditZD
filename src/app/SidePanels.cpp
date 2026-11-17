// SPDX-License-Identifier: GPL-3.0-only
#include "SidePanels.hpp"

#include "core/document.hpp"

#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <set>

namespace {

QLabel* emptyNote(const QString& text, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  l->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
  l->setWordWrap(true);
  l->setContentsMargins(8, 12, 8, 8);
  QPalette pal = l->palette();
  pal.setColor(QPalette::WindowText, pal.color(QPalette::Disabled, QPalette::WindowText));
  l->setPalette(pal);
  return l;
}

QToolButton* barButton(const QIcon& icon, const QString& tip, QWidget* parent) {
  auto* b = new QToolButton(parent);
  b->setIcon(icon);
  b->setToolTip(tip);
  b->setAutoRaise(true);
  return b;
}

// An open eye (the layer is shown), or nothing (hidden), as Acrobat marks them.
QIcon eyeIcon(bool open, const QPalette& pal) {
  QPixmap pm(32, 32);
  pm.setDevicePixelRatio(2);
  pm.fill(Qt::transparent);
  if (open) {
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor ink = pal.color(QPalette::WindowText);
    p.setPen(QPen(ink, 1.2));
    QPainterPath eye;
    eye.moveTo(1.5, 8);
    eye.quadTo(8, 1.5, 14.5, 8);
    eye.quadTo(8, 14.5, 1.5, 8);
    p.drawPath(eye);
    p.setBrush(ink);
    p.drawEllipse(QPointF(8, 8), 2.4, 2.4);
  }
  return QIcon(pm);
}

QString sizeText(std::size_t bytes) {
  return QLocale().formattedDataSize(qint64(bytes), 1, QLocale::DataSizeTraditionalFormat);
}

}  // namespace

// ---------------------------------------------------------------------------------------------

BookmarksPanel::BookmarksPanel(QWidget* parent) : QWidget(parent) {
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  tree_ = new QTreeWidget(this);
  tree_->setHeaderHidden(true);
  tree_->setColumnCount(1);
  tree_->setUniformRowHeights(true);
  tree_->setTextElideMode(Qt::ElideRight);
  tree_->setFrameShape(QFrame::NoFrame);
  none_ = emptyNote(tr("This document has no bookmarks."), this);
  lay->addWidget(tree_);
  lay->addWidget(none_);
  // A click goes there, as in Acrobat (the keyboard too: Enter, or moving to one).
  auto go = [this](QTreeWidgetItem* item) {
    if (!item) return;
    const int page = item->data(0, Qt::UserRole).toInt();
    if (page >= 0) emit goTo(page, item->data(0, Qt::UserRole + 1).toDouble());
  };
  connect(tree_, &QTreeWidget::itemClicked, this, go);
  connect(tree_, &QTreeWidget::itemActivated, this, go);
  setDocument(nullptr);
}

void BookmarksPanel::setDocument(std::shared_ptr<pdfz::Document> doc) {
  // Keep what was open, by title path, when the same document's bookmarks are read again.
  QSet<QString> expanded;
  std::function<void(QTreeWidgetItem*, const QString&)> remember = [&](QTreeWidgetItem* item, const QString& path) {
    for (int i = 0; i < item->childCount(); ++i) {
      QTreeWidgetItem* c = item->child(i);
      const QString p = path + QChar(0x1F) + c->text(0);
      if (c->isExpanded()) expanded.insert(p);
      remember(c, p);
    }
  };
  remember(tree_->invisibleRootItem(), {});
  tree_->clear();
  std::vector<pdfz::Document::Bookmark> marks;
  if (doc) marks = doc->bookmarks();
  std::function<void(QTreeWidgetItem*, const std::vector<pdfz::Document::Bookmark>&, const QString&)> add =
      [&](QTreeWidgetItem* parent, const std::vector<pdfz::Document::Bookmark>& list, const QString& path) {
        for (const auto& b : list) {
          auto* item = new QTreeWidgetItem(parent, {QString::fromStdString(b.title)});
          item->setToolTip(0, item->text(0));
          item->setData(0, Qt::UserRole, b.page);
          item->setData(0, Qt::UserRole + 1, b.top);
          const QString p = path + QChar(0x1F) + item->text(0);
          add(item, b.children, p);
          if (expanded.contains(p)) item->setExpanded(true);
        }
      };
  add(tree_->invisibleRootItem(), marks, {});
  tree_->setVisible(!marks.empty());
  none_->setVisible(marks.empty());
}

// ---------------------------------------------------------------------------------------------

AttachmentsPanel::AttachmentsPanel(QWidget* parent) : QWidget(parent) {
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);
  auto* bar = new QHBoxLayout;
  bar->setContentsMargins(4, 2, 4, 2);
  bar->setSpacing(2);
  openButton_ = barButton(style()->standardIcon(QStyle::SP_DialogOpenButton), tr("Open the attachment"), this);
  saveButton_ = barButton(style()->standardIcon(QStyle::SP_DialogSaveButton), tr("Save the attachment as…"), this);
  addButton_ = barButton(QIcon::fromTheme("list-add", style()->standardIcon(QStyle::SP_FileDialogNewFolder)),
                         tr("Attach a file…"), this);
  removeButton_ = barButton(QIcon::fromTheme("edit-delete", style()->standardIcon(QStyle::SP_TrashIcon)),
                            tr("Delete the attachment"), this);
  for (QToolButton* b : {openButton_, saveButton_, addButton_, removeButton_}) bar->addWidget(b);
  bar->addStretch(1);
  lay->addLayout(bar);
  tree_ = new QTreeWidget(this);
  tree_->setColumnCount(3);
  tree_->setHeaderLabels({tr("Name"), tr("Description"), tr("Size")});
  tree_->setRootIsDecorated(false);
  tree_->setUniformRowHeights(true);
  tree_->setFrameShape(QFrame::NoFrame);
  tree_->setContextMenuPolicy(Qt::CustomContextMenu);
  tree_->header()->setStretchLastSection(false);
  // The name whole; the description in what's left (whole in its tooltip).
  tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  lay->addWidget(tree_, 1);

  connect(openButton_, &QToolButton::clicked, this, &AttachmentsPanel::openSelected);
  connect(saveButton_, &QToolButton::clicked, this, &AttachmentsPanel::saveSelected);
  connect(addButton_, &QToolButton::clicked, this, &AttachmentsPanel::addRequested);
  connect(removeButton_, &QToolButton::clicked, this, [this] {
    const int i = selected();
    if (i >= 0) emit removeRequested(i);
  });
  connect(tree_, &QTreeWidget::itemDoubleClicked, this, &AttachmentsPanel::openSelected);
  connect(tree_, &QTreeWidget::itemSelectionChanged, this, &AttachmentsPanel::updateButtons);
  connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    QMenu menu(this);
    const bool has = selected() >= 0;
    menu.addAction(tr("&Open"), this, &AttachmentsPanel::openSelected)->setEnabled(has);
    menu.addAction(tr("&Save As…"), this, &AttachmentsPanel::saveSelected)->setEnabled(has);
    menu.addSeparator();
    menu.addAction(tr("&Attach a File…"), this, &AttachmentsPanel::addRequested)->setEnabled(doc_ != nullptr);
    menu.addAction(tr("&Delete"), this, [this] { emit removeRequested(selected()); })->setEnabled(has);
    menu.exec(tree_->viewport()->mapToGlobal(pos));
  });
  setDocument(nullptr);
}

void AttachmentsPanel::setDocument(std::shared_ptr<pdfz::Document> doc) {
  doc_ = std::move(doc);
  const int was = selected();
  tree_->clear();
  if (doc_) {
    const auto list = doc_->attachments();
    for (std::size_t i = 0; i < list.size(); ++i) {
      const QString name = QString::fromStdString(list[i].name);
      auto* item = new QTreeWidgetItem(tree_, {name, QString::fromStdString(list[i].description), sizeText(list[i].size)});
      item->setData(0, Qt::UserRole, int(i));
      item->setToolTip(0, name);
      item->setToolTip(1, item->text(1));
      item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    }
  }
  if (was >= 0 && was < tree_->topLevelItemCount()) tree_->setCurrentItem(tree_->topLevelItem(was));
  updateButtons();
}

int AttachmentsPanel::selected() const {
  const QTreeWidgetItem* item = tree_->currentItem();
  return item && item->isSelected() ? item->data(0, Qt::UserRole).toInt() : -1;
}

void AttachmentsPanel::updateButtons() {
  const bool has = doc_ && selected() >= 0;
  openButton_->setEnabled(has);
  saveButton_->setEnabled(has);
  removeButton_->setEnabled(has);
  addButton_->setEnabled(doc_ != nullptr);
}

// Written to a folder of its own in the temporary folder and opened with whatever opens that
// kind of file, as Acrobat does.
void AttachmentsPanel::openSelected() {
  const int i = selected();
  if (!doc_ || i < 0) return;
  std::vector<std::uint8_t> bytes;
  const QString name = QFileInfo(tree_->currentItem()->text(0)).fileName();
  if (!doc_->attachmentData(i, bytes) || name.isEmpty()) {
    QMessageBox::warning(this, tr("Attachments"), tr("The attachment couldn't be read."));
    return;
  }
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/PDFEditZD attachments/" +
                      QString::number(QDateTime::currentMSecsSinceEpoch(), 36);
  QDir().mkpath(dir);
  QFile f(dir + "/" + name);
  if (!f.open(QIODevice::WriteOnly) || f.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size())) {
    QMessageBox::warning(this, tr("Attachments"), tr("The attachment couldn't be written to %1.").arg(QDir::toNativeSeparators(dir)));
    return;
  }
  f.close();
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(f.fileName())))
    QMessageBox::information(this, tr("Attachments"),
                             tr("No program opens this kind of file. It was saved as %1.").arg(QDir::toNativeSeparators(f.fileName())));
}

void AttachmentsPanel::saveSelected() {
  const int i = selected();
  if (!doc_ || i < 0) return;
  std::vector<std::uint8_t> bytes;
  if (!doc_->attachmentData(i, bytes)) {
    QMessageBox::warning(this, tr("Attachments"), tr("The attachment couldn't be read."));
    return;
  }
  QSettings s;
  const QString dir = s.value("lastDir", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
  const QString path = QFileDialog::getSaveFileName(this, tr("Save Attachment"), dir + "/" + QFileInfo(tree_->currentItem()->text(0)).fileName());
  if (path.isEmpty()) return;
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size()))
    QMessageBox::warning(this, tr("Attachments"), tr("Couldn't save %1.").arg(QDir::toNativeSeparators(path)));
}

// ---------------------------------------------------------------------------------------------

LayersPanel::LayersPanel(QWidget* parent) : QWidget(parent) {
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  tree_ = new QTreeWidget(this);
  tree_->setHeaderHidden(true);
  tree_->setRootIsDecorated(false);
  tree_->setUniformRowHeights(true);
  tree_->setFrameShape(QFrame::NoFrame);
  tree_->setIconSize(QSize(16, 16));
  none_ = emptyNote(tr("This document has no layers."), this);
  lay->addWidget(tree_);
  lay->addWidget(none_);
  // A click on the eye (or Space) turns the layer off or on.
  auto toggle = [this](QTreeWidgetItem* item) {
    if (!item || !doc_ || filling_) return;
    const std::string name = item->data(0, Qt::UserRole).toString().toStdString();
    const bool on = !item->data(0, Qt::UserRole + 1).toBool();
    doc_->setLayerVisible(name, on);
    item->setData(0, Qt::UserRole + 1, on);
    item->setIcon(0, eyeIcon(on, palette()));
    item->setToolTip(0, on ? tr("Shown: click to hide it") : tr("Hidden: click to show it"));
    emit layersChanged();
  };
  connect(tree_, &QTreeWidget::itemClicked, this, [this, toggle](QTreeWidgetItem* item) {
    // Only the eye, at the left: a click on the name just selects it.
    const QRect row = tree_->visualItemRect(item);
    const QPoint at = tree_->viewport()->mapFromGlobal(QCursor::pos());
    if (at.x() - row.left() <= tree_->iconSize().width() + 8) toggle(item);
  });
  auto* space = new QShortcut(QKeySequence(Qt::Key_Space), tree_, nullptr, nullptr, Qt::WidgetShortcut);
  connect(space, &QShortcut::activated, this, [this, toggle] { toggle(tree_->currentItem()); });
  setDocument(nullptr);
}

void LayersPanel::setDocument(std::shared_ptr<pdfz::Document> doc) {
  doc_ = std::move(doc);
  filling_ = true;
  tree_->clear();
  std::vector<std::string> names;
  std::set<std::string> hidden;
  if (doc_) {
    names = doc_->layers();
    hidden = doc_->hiddenLayers();
  }
  for (const std::string& n : names) {
    const bool on = !hidden.count(n);
    auto* item = new QTreeWidgetItem(tree_, {QString::fromStdString(n)});
    item->setData(0, Qt::UserRole, QString::fromStdString(n));
    item->setData(0, Qt::UserRole + 1, on);
    item->setIcon(0, eyeIcon(on, palette()));
    item->setToolTip(0, on ? tr("Shown: click to hide it") : tr("Hidden: click to show it"));
  }
  tree_->setVisible(!names.empty());
  none_->setVisible(names.empty());
  filling_ = false;
}
