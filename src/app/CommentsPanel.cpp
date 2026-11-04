// SPDX-License-Identifier: GPL-3.0-only
#include "CommentsPanel.hpp"

#include "Markup.hpp"

#include "core/annotations.hpp"
#include "core/document.hpp"

#include <QElapsedTimer>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace {

constexpr int kPageRole = Qt::UserRole + 1;
constexpr int kIndexRole = Qt::UserRole + 2;  // -1 on page headings

QIcon swatch(const QColor& c) {
  QPixmap pm(12, 12);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(c.darker(140), 1));
  p.setBrush(c);
  p.drawEllipse(QRectF(1, 1, 10, 10));
  return QIcon(pm);
}

QString describe(const pdfz::Annotation& a) {
  QString head = QObject::tr(pdfz::kindName(a.kind));
  if (a.subject == "Arrow" || a.subject == "Line") head = QObject::tr(a.subject.c_str());
  if (a.kind == pdfz::AnnotKind::Stamp && !a.subject.empty())
    head = a.subject == "Check" ? QObject::tr("Checkmark") : QObject::tr(a.subject.c_str());  // Signature, Initials...
  if (a.kind == pdfz::AnnotKind::FreeText && a.subject == "Date") head = QObject::tr("Date");
  if (a.kind == pdfz::AnnotKind::FreeText && a.subject == "Fill Text") head = QObject::tr("Text");
  QStringList parts{head};
  if (!a.author.empty()) parts << QString::fromStdString(a.author);
  const QString when = formatPdfDate(a.modified);
  if (!when.isEmpty()) parts << when;
  QString text = QString::fromStdString(a.contents).simplified();
  if (text.size() > 140) text = text.left(140) + QChar(0x2026);
  return parts.join(QStringLiteral(" · ")) + (text.isEmpty() ? QString() : "\n" + text);
}

// Keys reach the panel's tree; Delete there deletes the selected comment.
class CommentTree : public QTreeWidget {
public:
  using QTreeWidget::QTreeWidget;
  std::function<void()> onDelete;

protected:
  void keyPressEvent(QKeyEvent* e) override {
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && onDelete) {
      onDelete();
      return;
    }
    QTreeWidget::keyPressEvent(e);
  }
};

}  // namespace

CommentsPanel::CommentsPanel(QWidget* parent) : QWidget(parent) {
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);
  summary_ = new QLabel(this);
  summary_->setContentsMargins(8, 4, 8, 4);
  summary_->setEnabled(false);
  auto* tree = new CommentTree(this);
  tree_ = tree;
  tree_->setHeaderHidden(true);
  tree_->setRootIsDecorated(true);
  tree_->setWordWrap(true);
  tree_->setTextElideMode(Qt::ElideRight);
  tree_->setUniformRowHeights(false);
  tree_->setFrameShape(QFrame::NoFrame);
  tree_->setContextMenuPolicy(Qt::CustomContextMenu);
  lay->addWidget(summary_);
  lay->addWidget(tree_, 1);

  scan_.setInterval(0);
  connect(&scan_, &QTimer::timeout, this, &CommentsPanel::scanSome);

  connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
    int page, index;
    if (itemRef(item, page, index)) emit activated(page, index);
  });
  connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {  // double-click, Enter
    int page, index;
    if (itemRef(item, page, index)) emit editRequested(page, index);
  });
  tree->onDelete = [this] {
    int page, index;
    if (itemRef(tree_->currentItem(), page, index)) emit deleteRequested(page, index);
  };
  connect(tree_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    int page, index;
    if (!itemRef(tree_->itemAt(pos), page, index)) return;
    QMenu menu(this);
    menu.addAction(tr("Go to Comment"), this, [=, this] { emit activated(page, index); });
    menu.addAction(tr("Edit Comment Text…"), this, [=, this] { emit editRequested(page, index); });
    menu.addSeparator();
    menu.addAction(tr("Delete Comment"), this, [=, this] { emit deleteRequested(page, index); });
    menu.exec(tree_->viewport()->mapToGlobal(pos));
  });
  updateSummary();
}

bool CommentsPanel::itemRef(QTreeWidgetItem* item, int& page, int& index) const {
  if (!item) return false;
  index = item->data(0, kIndexRole).toInt();
  page = item->data(0, kPageRole).toInt();
  return index >= 0;
}

void CommentsPanel::setDocument(std::shared_ptr<pdfz::Document> doc) {
  doc_ = std::move(doc);
  tree_->clear();
  nextPage_ = 0;
  if (doc_) scan_.start();
  else scan_.stop();
  updateSummary();
}

void CommentsPanel::scanSome() {
  if (!doc_) {
    scan_.stop();
    return;
  }
  QElapsedTimer clock;
  clock.start();
  while (nextPage_ < doc_->pageCount() && clock.elapsed() < 12) fillPage(nextPage_++);
  if (nextPage_ >= doc_->pageCount()) scan_.stop();
  updateSummary();
}

QTreeWidgetItem* CommentsPanel::pageItem(int page, bool create) {
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    QTreeWidgetItem* it = tree_->topLevelItem(i);
    const int p = it->data(0, kPageRole).toInt();
    if (p == page) return it;
    if (p > page) {
      if (!create) return nullptr;
      auto* item = new QTreeWidgetItem;
      tree_->insertTopLevelItem(i, item);
      return item;
    }
  }
  if (!create) return nullptr;
  auto* item = new QTreeWidgetItem(tree_);
  return item;
}

void CommentsPanel::fillPage(int page) {
  const auto list = doc_->annotations(page);
  QTreeWidgetItem* head = pageItem(page, !list.empty());
  if (!head) return;
  if (list.empty()) {
    delete head;
    return;
  }
  const bool wasExpanded = head->childCount() == 0 || head->isExpanded();
  qDeleteAll(head->takeChildren());
  head->setData(0, kPageRole, page);
  head->setData(0, kIndexRole, -1);
  head->setText(0, tr("Page %1 (%2)").arg(page + 1).arg(list.size()));
  QFont bold = head->font(0);
  bold.setBold(true);
  head->setFont(0, bold);
  head->setFlags(Qt::ItemIsEnabled);
  for (const pdfz::Annotation& a : list) {
    auto* item = new QTreeWidgetItem(head);
    item->setText(0, describe(a));
    // A text box by its text's color (its border may be none).
    item->setIcon(0, swatch(toQColor(a.kind == pdfz::AnnotKind::FreeText ? a.textColor : a.color)));
    item->setToolTip(0, QString::fromStdString(a.contents));
    item->setData(0, kPageRole, page);
    item->setData(0, kIndexRole, a.index);
  }
  head->setExpanded(wasExpanded);
}

void CommentsPanel::refreshPage(int page) {
  if (!doc_) return;
  if (scan_.isActive() && page >= nextPage_) return;  // the scan will get there
  fillPage(page);
  updateSummary();
}

void CommentsPanel::showAnnotation(int page, int index) {
  QTreeWidgetItem* head = pageItem(page, false);
  if (!head) return;
  for (int i = 0; i < head->childCount(); ++i) {
    QTreeWidgetItem* c = head->child(i);
    if (c->data(0, kIndexRole).toInt() == index) {
      syncing_ = true;
      head->setExpanded(true);
      tree_->setCurrentItem(c);
      tree_->scrollToItem(c);
      syncing_ = false;
      return;
    }
  }
}

void CommentsPanel::updateSummary() {
  int n = 0;
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) n += tree_->topLevelItem(i)->childCount();
  QString s = !doc_ ? QString() : n == 0 ? tr("No comments") : (n == 1 ? tr("1 comment") : tr("%1 comments").arg(n));
  if (scan_.isActive()) s += tr(" (still looking…)");
  summary_->setText(s);
}
