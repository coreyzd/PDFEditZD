// SPDX-License-Identifier: GPL-3.0-only
#include "DocumentView.hpp"

#include "PageRenderer.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kMargin = 16;  // around the pages, logical pixels
constexpr double kGap = 12;     // between pages
constexpr double kZoomStep = 1.25;
}  // namespace

DocumentView::DocumentView(PageRenderer* renderer, QWidget* parent)
    : QAbstractScrollArea(parent), renderer_(renderer) {
  // Always shown, so fitting the width never makes it appear and change the width again.
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setFrameShape(QFrame::NoFrame);
  setFocusPolicy(Qt::StrongFocus);
  viewport()->setCursor(Qt::OpenHandCursor);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  verticalScrollBar()->setSingleStep(40);
  horizontalScrollBar()->setSingleStep(40);

  requestTimer_.setSingleShot(true);
  requestTimer_.setInterval(0);
  connect(&requestTimer_, &QTimer::timeout, this, &DocumentView::sendRequests);
  connect(renderer_, &PageRenderer::tileReady, this, &DocumentView::onTileReady);
  connect(renderer_, &PageRenderer::previewReady, this, &DocumentView::onPreviewReady);
}

double DocumentView::pixelsPerPoint() const { return zoom_ * logicalDpiX() / 72.0; }

double DocumentView::fitZoom(FitMode mode) const {
  const auto& doc = renderer_->document();
  if (!doc || doc->pageCount() == 0) return zoom_;
  const double dpiScale = logicalDpiX() / 72.0;
  const double availW = std::max(40.0, viewport()->width() - 2 * kMargin);
  const double availH = std::max(40.0, viewport()->height() - 2 * kMargin);
  if (mode == FitMode::Width) {
    double maxW = 0;
    for (int i = 0; i < doc->pageCount(); ++i) maxW = std::max(maxW, doc->pageSize(i).width);
    return maxW > 0 ? availW / (maxW * dpiScale) : zoom_;
  }
  if (mode == FitMode::Page) {
    const pdfz::SizeF s = doc->pageSize(std::clamp(currentPage_, 0, doc->pageCount() - 1));
    if (s.width <= 0 || s.height <= 0) return zoom_;
    return std::min(availW / (s.width * dpiScale), availH / (s.height * dpiScale));
  }
  return zoom_;
}

void DocumentView::relayout() {
  layoutDpi_ = logicalDpiX();
  if (fitMode_ != FitMode::None) zoom_ = std::clamp(fitZoom(fitMode_), kMinZoom, kMaxZoom);

  pageRects_.clear();
  const auto& doc = renderer_->document();
  if (!doc || doc->pageCount() == 0) {
    contentSize_ = {};
    updateScrollBars();
    return;
  }
  const double ppp = pixelsPerPoint();
  double maxW = 0;
  for (int i = 0; i < doc->pageCount(); ++i) maxW = std::max(maxW, doc->pageSize(i).width * ppp);
  const double contentW = maxW + 2 * kMargin;
  const double layoutW = std::max(contentW, double(viewport()->width()));
  double y = kMargin;
  pageRects_.reserve(std::size_t(doc->pageCount()));
  for (int i = 0; i < doc->pageCount(); ++i) {
    const pdfz::SizeF s = doc->pageSize(i);
    const double w = s.width * ppp;
    const double h = s.height * ppp;
    // Whole logical pixels, so page edges are crisp.
    pageRects_.emplace_back(std::round((layoutW - w) / 2), std::round(y), std::round(w), std::round(h));
    y += std::round(h) + kGap;
  }
  contentSize_ = {contentW, y - kGap + kMargin};
  updateScrollBars();
}

void DocumentView::updateScrollBars() {
  const QSize vp = viewport()->size();
  horizontalScrollBar()->setRange(0, std::max(0, int(std::ceil(contentSize_.width())) - vp.width()));
  horizontalScrollBar()->setPageStep(vp.width());
  verticalScrollBar()->setRange(0, std::max(0, int(std::ceil(contentSize_.height())) - vp.height()));
  verticalScrollBar()->setPageStep(std::max(1, vp.height() - 40));
}

void DocumentView::documentChanged(bool keepPosition) {
  const int keepPage = currentPage_;
  double keepOffset = 0;  // how far into the current page the view's top was
  if (keepPosition && keepPage < int(pageRects_.size()))
    keepOffset = (verticalScrollBar()->value() - pageRects_[keepPage].top()) / pageRects_[keepPage].height();

  relayout();
  const int n = int(pageRects_.size());
  if (keepPosition && n > 0) {
    const int p = std::clamp(keepPage, 0, n - 1);
    verticalScrollBar()->setValue(int(std::lround(pageRects_[p].top() + keepOffset * pageRects_[p].height())));
  } else {
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
  }
  currentPage_ = -1;
  updateCurrentPage();
  emit zoomChanged(zoom_, fitMode_);
  viewport()->update();
}

void DocumentView::goToPage(int page) {
  if (pageRects_.empty()) return;
  page = std::clamp(page, 0, int(pageRects_.size()) - 1);
  verticalScrollBar()->setValue(int(pageRects_[page].top() - kMargin));
  // Asked for by number: that's the current page, even if it's the last one and can't scroll
  // all the way up.
  if (currentPage_ != page) {
    currentPage_ = page;
    emit currentPageChanged(page);
  }
}

void DocumentView::setZoom(double zoom) {
  applyZoom(zoom, FitMode::None, viewport()->rect().center());
}

void DocumentView::setFitMode(FitMode mode) {
  if (mode == FitMode::None) {
    fitMode_ = mode;
    emit zoomChanged(zoom_, fitMode_);
    return;
  }
  const int page = currentPage_;
  applyZoom(fitZoom(mode), mode, viewport()->rect().center());
  if (mode == FitMode::Page) goToPage(page);
}

void DocumentView::zoomIn() { setZoom(zoom_ * kZoomStep); }
void DocumentView::zoomOut() { setZoom(zoom_ / kZoomStep); }

void DocumentView::applyZoom(double zoom, FitMode mode, QPoint anchor) {
  zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
  const QPointF scroll(horizontalScrollBar()->value(), verticalScrollBar()->value());
  const QPointF content = scroll + QPointF(anchor);
  int page = -1;
  QPointF rel;
  if (!pageRects_.empty()) {
    page = pageAt(content.y());
    const QRectF& r = pageRects_[page];
    rel = {(content.x() - r.left()) / r.width(), (content.y() - r.top()) / r.height()};
  }

  zoom_ = zoom;
  fitMode_ = mode;
  relayout();  // in a fit mode this recomputes the same zoom

  if (page >= 0 && page < int(pageRects_.size())) {
    const QRectF& r = pageRects_[page];
    const QPointF now(r.left() + rel.x() * r.width(), r.top() + rel.y() * r.height());
    horizontalScrollBar()->setValue(int(std::lround(now.x() - anchor.x())));
    verticalScrollBar()->setValue(int(std::lround(now.y() - anchor.y())));
  }
  updateCurrentPage();
  emit zoomChanged(zoom_, fitMode_);
  viewport()->update();
}

QRectF DocumentView::visibleRect() const {
  return QRectF(horizontalScrollBar()->value(), verticalScrollBar()->value(), viewport()->width(),
                viewport()->height());
}

int DocumentView::pageAt(double contentY) const {
  if (pageRects_.empty()) return -1;
  // The last page starting at or above y (the gap below a page counts as that page).
  auto it = std::upper_bound(pageRects_.begin(), pageRects_.end(), contentY,
                             [](double y, const QRectF& r) { return y < r.top(); });
  return std::max(0, int(it - pageRects_.begin()) - 1);
}

void DocumentView::updateCurrentPage() {
  if (pageRects_.empty()) return;
  const QRectF vis = visibleRect();
  const int first = pageAt(vis.top());
  const int last = pageAt(vis.bottom());
  int best = first;
  double bestH = -1;
  for (int i = first; i <= last; ++i) {
    const double h = pageRects_[i].intersected(vis).height();
    if (h > bestH + 0.5) {  // ties go to the page higher up
      best = i;
      bestH = h;
    }
  }
  if (best != currentPage_) {
    currentPage_ = best;
    emit currentPageChanged(best);
  }
}

void DocumentView::resizeEvent(QResizeEvent* e) {
  QAbstractScrollArea::resizeEvent(e);
  if (fitMode_ != FitMode::None) {
    // Keep the same spot of the same page at the top while the zoom follows the window.
    applyZoom(fitZoom(fitMode_), fitMode_, QPoint(0, 0));
  } else {
    documentChanged(true);
  }
}

void DocumentView::scrollContentsBy(int, int) {
  updateCurrentPage();
  viewport()->update();
}

void DocumentView::paintEvent(QPaintEvent*) {
  if (layoutDpi_ != logicalDpiX()) documentChanged(true);  // moved to a screen with another DPI

  QPainter p(viewport());
  p.fillRect(viewport()->rect(), palette().color(QPalette::Window).darker(125));
  if (pageRects_.empty()) return;

  const auto& doc = renderer_->document();
  const QRectF vis = visibleRect();
  const QPointF scroll = vis.topLeft();
  const double ppp = pixelsPerPoint();
  const double dpr = devicePixelRatioF();
  const int scaleKey = PageRenderer::scaleKeyFor(ppp * dpr);
  const double scale = scaleKey / 1000.0;
  const double devPerLogical = scale / ppp;  // ~dpr
  const int T = PageRenderer::kTileSize;

  p.setRenderHint(QPainter::SmoothPixmapTransform);
  const int first = pageAt(vis.top());
  const int last = pageAt(vis.bottom());
  for (int i = first; i <= last; ++i) {
    const QRectF r = pageRects_[i].translated(-scroll);
    if (!r.intersects(QRectF(viewport()->rect()))) continue;
    p.fillRect(r.translated(2, 2), QColor(0, 0, 0, 50));  // shadow
    p.fillRect(r, Qt::white);

    const QImage preview = renderer_->preview(i);
    if (!preview.isNull()) p.drawImage(r, preview);

    const pdfz::SizeF s = doc->pageSize(i);
    const int pageDevW = int(std::ceil(s.width * scale));
    const int pageDevH = int(std::ceil(s.height * scale));
    // At this size the preview is as sharp as tiles would be.
    if (!preview.isNull() && pageDevW <= preview.width() && pageDevH <= preview.height()) continue;

    const QRectF part = r.intersected(QRectF(viewport()->rect()));
    const int tx0 = std::max(0, int((part.left() - r.left()) * devPerLogical) / T);
    const int ty0 = std::max(0, int((part.top() - r.top()) * devPerLogical) / T);
    const int tx1 = std::min((pageDevW - 1) / T, int((part.right() - r.left()) * devPerLogical) / T);
    const int ty1 = std::min((pageDevH - 1) / T, int((part.bottom() - r.top()) * devPerLogical) / T);
    for (int ty = ty0; ty <= ty1; ++ty) {
      for (int tx = tx0; tx <= tx1; ++tx) {
        const QImage img = renderer_->tile({i, scaleKey, tx, ty});
        if (img.isNull()) continue;
        const QRectF target(r.left() + tx * T / devPerLogical, r.top() + ty * T / devPerLogical,
                            img.width() / devPerLogical, img.height() / devPerLogical);
        p.drawImage(target, img);
      }
    }
  }
  scheduleRequests();
}

void DocumentView::scheduleRequests() {
  if (!requestTimer_.isActive()) requestTimer_.start();
}

// Asks for what's on screen first, then what's just above and below it.
void DocumentView::sendRequests() {
  const auto& doc = renderer_->document();
  if (!doc || pageRects_.empty()) {
    renderer_->requestView({}, {});
    return;
  }
  const double ppp = pixelsPerPoint();
  const int scaleKey = PageRenderer::scaleKeyFor(ppp * devicePixelRatioF());
  const double scale = scaleKey / 1000.0;
  const double devPerLogical = scale / ppp;
  const int T = PageRenderer::kTileSize;
  const QRectF vis = visibleRect();

  std::vector<int> previews;
  std::vector<TileKey> tiles;
  auto addRegion = [&](const QRectF& region, bool withPreviews) {
    const int first = pageAt(region.top());
    const int last = pageAt(region.bottom());
    for (int i = first; i <= last; ++i) {
      const QRectF& r = pageRects_[i];
      const QRectF part = r.intersected(region);
      if (part.isEmpty()) continue;
      const QImage preview = renderer_->preview(i);
      if (withPreviews && preview.isNull() && std::find(previews.begin(), previews.end(), i) == previews.end())
        previews.push_back(i);
      const pdfz::SizeF s = doc->pageSize(i);
      const int pageDevW = int(std::ceil(s.width * scale));
      const int pageDevH = int(std::ceil(s.height * scale));
      if (pageDevW <= PageRenderer::kPreviewBox && pageDevH <= PageRenderer::kPreviewBox) continue;
      const int tx0 = std::max(0, int((part.left() - r.left()) * devPerLogical) / T);
      const int ty0 = std::max(0, int((part.top() - r.top()) * devPerLogical) / T);
      const int tx1 = std::min((pageDevW - 1) / T, int((part.right() - r.left()) * devPerLogical) / T);
      const int ty1 = std::min((pageDevH - 1) / T, int((part.bottom() - r.top()) * devPerLogical) / T);
      for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) {
          const TileKey key{i, scaleKey, tx, ty};
          if (renderer_->tile(key).isNull() && std::find(tiles.begin(), tiles.end(), key) == tiles.end())
            tiles.push_back(key);
        }
    }
  };
  addRegion(vis, true);
  addRegion(vis.translated(0, vis.height()), true);              // the next screenful
  addRegion(vis.translated(0, -vis.height() / 2).intersected(QRectF(0, 0, contentSize_.width(), vis.top())), false);
  renderer_->requestView(previews, tiles);
}

void DocumentView::onTileReady(const TileKey& key) {
  if (key.scaleKey != PageRenderer::scaleKeyFor(pixelsPerPoint() * devicePixelRatioF())) return;
  if (key.page < 0 || key.page >= int(pageRects_.size())) return;
  if (pageRects_[key.page].intersects(visibleRect())) viewport()->update();
}

void DocumentView::onPreviewReady(int page) {
  if (page < 0 || page >= int(pageRects_.size())) return;
  if (pageRects_[page].intersects(visibleRect())) viewport()->update();
}

void DocumentView::wheelEvent(QWheelEvent* e) {
  if (e->modifiers() & Qt::ControlModifier) {
    const double steps = e->angleDelta().y() / 120.0;
    if (steps != 0) applyZoom(zoom_ * std::pow(1.1, steps), FitMode::None, e->position().toPoint());
    e->accept();
    return;
  }
  QAbstractScrollArea::wheelEvent(e);
}

void DocumentView::keyPressEvent(QKeyEvent* e) {
  const bool noHScroll = horizontalScrollBar()->maximum() == 0;
  switch (e->key()) {
    case Qt::Key_Home: goToPage(0); return;
    case Qt::Key_End: goToPage(int(pageRects_.size()) - 1); return;
    case Qt::Key_Left:
      if (noHScroll) { goToPage(currentPage_ - 1); return; }
      break;
    case Qt::Key_Right:
      if (noHScroll) { goToPage(currentPage_ + 1); return; }
      break;
    case Qt::Key_Space:
      verticalScrollBar()->triggerAction(e->modifiers() & Qt::ShiftModifier ? QScrollBar::SliderPageStepSub
                                                                            : QScrollBar::SliderPageStepAdd);
      return;
    default: break;
  }
  QAbstractScrollArea::keyPressEvent(e);
}

// Dragging with the left or middle button moves the pages (the hand tool).
void DocumentView::mousePressEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
    panning_ = true;
    panLast_ = e->position().toPoint();
    viewport()->setCursor(Qt::ClosedHandCursor);
    e->accept();
    return;
  }
  QAbstractScrollArea::mousePressEvent(e);
}

void DocumentView::mouseMoveEvent(QMouseEvent* e) {
  if (panning_) {
    const QPoint pos = e->position().toPoint();
    const QPoint d = pos - panLast_;
    panLast_ = pos;
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
    verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
    e->accept();
    return;
  }
  QAbstractScrollArea::mouseMoveEvent(e);
}

void DocumentView::mouseReleaseEvent(QMouseEvent* e) {
  if (panning_ && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)) {
    panning_ = false;
    viewport()->setCursor(Qt::OpenHandCursor);
    e->accept();
    return;
  }
  QAbstractScrollArea::mouseReleaseEvent(e);
}

void DocumentView::changeEvent(QEvent* e) {
  QAbstractScrollArea::changeEvent(e);
  if (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange) viewport()->update();
}
