// SPDX-License-Identifier: GPL-3.0-only
#include "ThumbnailPanel.hpp"

#include "PageRenderer.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QPainter>
#include <QScrollBar>

#include <algorithm>

ThumbnailModel::ThumbnailModel(PageRenderer* renderer, QObject* parent)
    : QAbstractListModel(parent), renderer_(renderer) {
  connect(renderer_, &PageRenderer::previewReady, this, [this](int page) {
    if (page >= 0 && page < rows_) emit dataChanged(index(page), index(page), {Qt::DecorationRole});
  });
}

void ThumbnailModel::reset() {
  beginResetModel();
  const auto& doc = renderer_->document();
  rows_ = doc ? doc->pageCount() : 0;
  endResetModel();
}

int ThumbnailModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : rows_; }

QVariant ThumbnailModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= rows_) return {};
  const int page = index.row();
  switch (role) {
    case Qt::DisplayRole: return QString::number(page + 1);
    case Qt::DecorationRole: {
      QImage img = renderer_->preview(page);
      if (img.isNull()) renderer_->requestPreview(page);  // data() is only asked for rows on screen
      return img;
    }
    case PageSizeRole: {
      const auto& doc = renderer_->document();
      const pdfz::SizeF s = doc ? doc->pageSize(page) : pdfz::SizeF{};
      return QSizeF(s.width, s.height);
    }
    case Qt::ToolTipRole: {
      const auto& doc = renderer_->document();
      const pdfz::SizeF s = doc ? doc->pageSize(page) : pdfz::SizeF{};
      return QStringLiteral("Page %1 — %2 × %3 in")
          .arg(page + 1)
          .arg(s.width / 72, 0, 'f', 2)
          .arg(s.height / 72, 0, 'f', 2);
    }
    default: return {};
  }
}

ThumbnailPanel::ThumbnailPanel(PageRenderer* renderer, QWidget* parent) : QListView(parent) {
  model_ = new ThumbnailModel(renderer, this);
  setModel(model_);
  setItemDelegate(new ThumbnailDelegate(this));
  setUniformItemSizes(true);
  setSelectionMode(QAbstractItemView::ExtendedSelection);
  setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setFrameShape(QFrame::NoFrame);
  verticalScrollBar()->setSingleStep(24);

  connect(selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& cur) {
    if (!syncing_ && cur.isValid()) emit pageActivated(cur.row());
  });
}

void ThumbnailPanel::documentChanged() {
  syncing_ = true;
  model_->reset();
  syncing_ = false;
}

void ThumbnailPanel::setCurrentPage(int page) {
  const QModelIndex idx = model_->index(page);
  if (!idx.isValid() || idx == currentIndex()) return;
  syncing_ = true;
  // Moves the highlight; a multi-page selection the user made is replaced by this page.
  selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect);
  scrollTo(idx, QAbstractItemView::EnsureVisible);
  syncing_ = false;
}

std::vector<int> ThumbnailPanel::selectedPages() const {
  std::vector<int> pages;
  for (const QModelIndex& i : selectionModel()->selectedIndexes()) pages.push_back(i.row());
  std::sort(pages.begin(), pages.end());
  if (pages.empty() && currentIndex().isValid()) pages.push_back(currentIndex().row());
  return pages;
}

QSize ThumbnailPanel::sizeHint() const {
  return {ThumbnailDelegate::kBox + 40 + verticalScrollBar()->sizeHint().width(), 400};
}

void ThumbnailPanel::contextMenuEvent(QContextMenuEvent* e) {
  const QModelIndex idx = indexAt(e->pos());
  if (idx.isValid() && !selectionModel()->isSelected(idx))
    selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect);
  emit contextMenuForPages(e->globalPos());
}

QSize ThumbnailDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const {
  return {kBox + 24, kBox + opt.fontMetrics.height() + 20};
}

void ThumbnailDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const {
  p->save();
  const QRect cell = opt.rect;
  const bool selected = opt.state & QStyle::State_Selected;
  const QPalette& pal = opt.palette;

  // The page, fitted into the box at the top of the cell.
  QSizeF ps = index.data(ThumbnailModel::PageSizeRole).toSizeF();
  if (ps.isEmpty()) ps = {612, 792};
  const double f = std::min(kBox / ps.width(), kBox / ps.height());
  const QSizeF size(ps.width() * f, ps.height() * f);
  const QRectF page(cell.left() + (cell.width() - size.width()) / 2, cell.top() + 8 + (kBox - size.height()) / 2,
                    size.width(), size.height());

  if (selected) {
    QColor hl = pal.color(QPalette::Highlight);
    p->setPen(Qt::NoPen);
    p->setBrush(hl);
    p->drawRoundedRect(page.adjusted(-4, -4, 4, 4), 3, 3);
  }
  p->fillRect(page.translated(1.5, 1.5), QColor(0, 0, 0, 60));
  p->fillRect(page, Qt::white);
  const QImage img = index.data(Qt::DecorationRole).value<QImage>();
  if (!img.isNull()) {
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    p->drawImage(page, img);
  }

  const QRect label(cell.left(), cell.top() + 8 + kBox + 6, cell.width(), opt.fontMetrics.height());
  QFont font = opt.font;
  font.setBold(selected);
  p->setFont(font);
  p->setPen(pal.color(QPalette::Text));
  p->drawText(label, Qt::AlignHCenter | Qt::AlignTop, index.data(Qt::DisplayRole).toString());
  p->restore();
}
