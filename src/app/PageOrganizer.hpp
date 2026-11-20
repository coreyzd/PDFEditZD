// SPDX-License-Identifier: GPL-3.0-only
// Pages > Organize Pages, as in Acrobat: the document's pages in a grid of thumbnails, in the
// tab in place of the page view. Click to select (Ctrl+click adds or takes away one, Shift+click
// a run); hovering a page shows buttons to turn it either way or delete it; hovering the gap
// between two pages shows a line that inserts pages there when clicked; and dragging pages
// drops them between others, to put them in another order. Every change is an undoable edit,
// made by the main window (the grid only asks for it).
#pragma once

#include <QAbstractScrollArea>
#include <QIcon>
#include <QTimer>
#include <QWidget>

#include <vector>

class PageRenderer;
class QSlider;
class QToolBar;

class PageGrid : public QAbstractScrollArea {
  Q_OBJECT
public:
  explicit PageGrid(PageRenderer* renderer, QWidget* parent = nullptr);

  // The pages were added, removed, moved or turned: laid out again (the selection is kept as
  // far as the pages are still there).
  void documentChanged();
  // Only how pages look changed (a comment added): drawn again.
  void pagesChanged();

  // The thumbnails' size: the square each page is fitted into, in logical pixels.
  int thumbnailSize() const { return box_; }
  void setThumbnailSize(int px);
  static constexpr int kMinSize = 90;
  static constexpr int kMaxSize = 300;

  // The selected pages, ascending; the current page if none are.
  std::vector<int> selectedPages() const;
  int currentPage() const { return current_; }
  // Selects `page` alone and scrolls it into view.
  void setCurrentPage(int page);
  // Selects pages [first, first + count), the first current, scrolled into view.
  void selectPages(int first, int count);
  void selectPageList(const std::vector<int>& pages);

signals:
  void rotateRequested(const std::vector<int>& pages, int degrees);
  void deleteRequested(const std::vector<int>& pages);
  // The line between two pages was clicked: pages to go before what's now page `before`
  // (the page count for the end). `globalPos`: where to show the choices.
  void insertRequested(int before, const QPoint& globalPos);
  // `pages` (ascending) dragged so they go before what's now page `insertBefore`.
  void pagesMoved(const std::vector<int>& pages, int insertBefore);
  void pageOpened(int page);  // double-click or Enter: shown in the page view
  void contextMenuRequested(const QPoint& globalPos);
  void selectionChanged();
  void thumbnailSizeChanged(int px);

protected:
  void paintEvent(QPaintEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void scrollContentsBy(int dx, int dy) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void contextMenuEvent(QContextMenuEvent* e) override;
  bool viewportEvent(QEvent* e) override;

private:
  int pageCount() const;
  void relayout();
  // Geometry, in viewport coordinates.
  int rowOf(int page) const { return page / cols_; }
  int colOf(int page) const { return page % cols_; }
  QRectF boxRect(int page) const;      // the square the thumbnail is fitted into
  QRectF imageRect(int page) const;    // the page in it
  QRectF cellHighlight(int page) const;  // round the thumbnail and its number
  QRectF controlsRect(int page) const;   // the hover buttons, to the right of the page
  QRectF controlRect(int page, int which) const;  // 0 counterclockwise, 1 clockwise, 2 delete
  int pageAt(QPointF pos) const;          // a page (or its hover buttons) under pos, or -1
  int controlAt(int page, QPointF pos) const;
  // The gap between pages under pos (insert there), or -1; `line` is where its line is drawn.
  int insertionAt(QPointF pos, QRectF* line) const;
  // Where dragged pages would go for a drop at pos: always somewhere (the nearest gap).
  int dropIndexAt(QPointF pos, QRectF* line) const;
  QRectF gapLine(int row, int col) const;  // the gap before column `col` of `row`
  void ensureVisible(int page);
  void selectOnly(int page);
  void setHover(int page, int control, int insert, const QRectF& line);
  void updateHover(QPointF pos);
  void autoScrollStep();
  void endDrag();

  PageRenderer* renderer_;
  int box_ = 170;
  int cols_ = 1;
  double colW_ = 0;
  int rowH_ = 0;
  int labelH_ = 0;
  int contentH_ = 0;
  std::vector<char> selected_;
  int current_ = 0;
  int anchor_ = 0;  // where Shift+click selects from
  int hoverPage_ = -1, hoverControl_ = -1, hoverInsert_ = -1;
  QRectF hoverLine_;
  // Dragging pages.
  int pressPage_ = -1;
  QPoint pressPos_;
  bool pressWasSelected_ = false;
  bool dragging_ = false;
  std::vector<int> dragPages_;
  int dropBefore_ = -1;
  QRectF dropLine_;
  QPoint dragPos_;
  QTimer scrollTimer_;
  QIcon controlIcons_[3];
};

// The grid with its toolbar (filled in by the main window: rotate, delete, insert, extract,
// split), the thumbnails' size and Close.
class PageOrganizer : public QWidget {
  Q_OBJECT
public:
  explicit PageOrganizer(PageRenderer* renderer, QWidget* parent = nullptr);
  PageGrid* grid() const { return grid_; }
  // The toolbar, made again (a tab moved to another window gets that window's buttons): its
  // title, for the main window to add its buttons to, then finishBar().
  QToolBar* newBar();
  // After the main window's buttons: the size slider and Close (which runs `close`).
  void finishBar(QAction* close);

private:
  QToolBar* bar_ = nullptr;
  PageGrid* grid_;
  QSlider* size_ = nullptr;
};
