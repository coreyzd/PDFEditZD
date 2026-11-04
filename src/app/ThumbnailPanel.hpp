// SPDX-License-Identifier: GPL-3.0-only
// The page thumbnails down the side. Each row asks PageRenderer for its page's preview only
// when the list first paints it, so long documents don't render every page up front. Dragging
// thumbnails reorders the pages.
#pragma once

#include <QAbstractListModel>
#include <QListView>
#include <QStyledItemDelegate>

#include <vector>

class PageRenderer;

class ThumbnailModel : public QAbstractListModel {
  Q_OBJECT
public:
  enum Roles { PageSizeRole = Qt::UserRole + 1 };  // QSizeF, points

  explicit ThumbnailModel(PageRenderer* renderer, QObject* parent = nullptr);
  void reset();  // the document or its pages changed
  void pageChanged(int page);  // one page looks different now

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;

  // Drag and drop of pages within the list.
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  QStringList mimeTypes() const override;
  QMimeData* mimeData(const QModelIndexList& indexes) const override;
  Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }
  Qt::DropActions supportedDragActions() const override { return Qt::MoveAction; }
  bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                       const QModelIndex& parent) const override;
  // Doesn't change the model: asks for the move (pagesDropped) once the drag is over, and the
  // window does it as an undoable edit.
  bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                    const QModelIndex& parent) override;

signals:
  // `pages` (ascending) were dropped so that they go before what is now page `insertBefore`
  // (rowCount() for the end).
  void pagesDropped(const std::vector<int>& pages, int insertBefore);

private:
  PageRenderer* renderer_;
  int rows_ = 0;
};

class ThumbnailPanel : public QListView {
  Q_OBJECT
public:
  explicit ThumbnailPanel(PageRenderer* renderer, QWidget* parent = nullptr);

  void documentChanged();
  void pageChanged(int page) { model_->pageChanged(page); }
  // Highlights `page` (the one the main view shows) without signalling pageActivated.
  void setCurrentPage(int page);
  // The selected pages, ascending; the current page if nothing is selected.
  std::vector<int> selectedPages() const;
  // Selects pages [first, first + count) and makes `first` current, without signalling.
  void selectPages(int first, int count);

  QSize sizeHint() const override;

signals:
  void pageActivated(int page);
  void contextMenuForPages(const QPoint& globalPos);
  void pagesDropped(const std::vector<int>& pages, int insertBefore);

protected:
  void contextMenuEvent(QContextMenuEvent* e) override;

private:
  ThumbnailModel* model_;
  bool syncing_ = false;
};

class ThumbnailDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  static constexpr int kBox = 130;  // thumbnail box, logical pixels
  void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
  QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
};
