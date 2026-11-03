// SPDX-License-Identifier: GPL-3.0-only
// The pages, one under another, scrolled continuously. Draws each page from the tiles
// PageRenderer makes at the current zoom, with the page's preview underneath until they arrive.
#pragma once

#include <QAbstractScrollArea>
#include <QPoint>
#include <QRectF>
#include <QTimer>

#include <vector>

class PageRenderer;
struct TileKey;

class DocumentView : public QAbstractScrollArea {
  Q_OBJECT
public:
  enum class FitMode { None, Width, Page };

  explicit DocumentView(PageRenderer* renderer, QWidget* parent = nullptr);

  // The document (or its pages) changed: lay it out again, keeping the current page in view
  // when `keepPosition` is set, otherwise starting at the top.
  void documentChanged(bool keepPosition);

  int currentPage() const { return currentPage_; }
  void goToPage(int page);

  // 1.0 is actual size: a 1-inch line is an inch long at the screen's logical DPI.
  double zoom() const { return zoom_; }
  FitMode fitMode() const { return fitMode_; }
  void setZoom(double zoom);  // leaves fit mode
  void setFitMode(FitMode mode);
  void zoomIn();
  void zoomOut();

  static constexpr double kMinZoom = 0.08;
  static constexpr double kMaxZoom = 64.0;

signals:
  void currentPageChanged(int page);
  void zoomChanged(double zoom, DocumentView::FitMode mode);

protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void scrollContentsBy(int dx, int dy) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void changeEvent(QEvent*) override;

private:
  double pixelsPerPoint() const;  // logical pixels per PDF point at the current zoom
  double fitZoom(FitMode mode) const;
  void relayout();
  void updateScrollBars();
  // Zooms so the content point under viewport position `anchor` stays under it.
  void applyZoom(double zoom, FitMode mode, QPoint anchor);
  QRectF visibleRect() const;  // in content coordinates
  int pageAt(double contentY) const;
  void updateCurrentPage();
  void scheduleRequests();
  void sendRequests();
  void onTileReady(const TileKey& key);
  void onPreviewReady(int page);

  PageRenderer* renderer_;
  double zoom_ = 1.0;
  FitMode fitMode_ = FitMode::Width;
  std::vector<QRectF> pageRects_;  // content coordinates (logical pixels)
  QSizeF contentSize_;
  int currentPage_ = 0;
  double layoutDpi_ = 0;  // the screen DPI the layout was made for
  QTimer requestTimer_;
  bool panning_ = false;
  QPoint panLast_;
};
