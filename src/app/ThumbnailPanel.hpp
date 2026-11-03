// SPDX-License-Identifier: GPL-3.0-only
// The page thumbnails down the side. Each row asks PageRenderer for its page's preview only
// when the list first paints it, so long documents don't render every page up front.
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

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;

private:
  PageRenderer* renderer_;
  int rows_ = 0;
};

class ThumbnailPanel : public QListView {
  Q_OBJECT
public:
  explicit ThumbnailPanel(PageRenderer* renderer, QWidget* parent = nullptr);

  void documentChanged();
  // Highlights `page` (the one the main view shows) without signalling pageActivated.
  void setCurrentPage(int page);
  // The selected pages, ascending; the current page if nothing is selected.
  std::vector<int> selectedPages() const;

  QSize sizeHint() const override;

signals:
  void pageActivated(int page);
  void contextMenuForPages(const QPoint& globalPos);

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
