// SPDX-License-Identifier: GPL-3.0-only
#include "DocumentView.hpp"

#include "InlineTextEdit.hpp"
#include "PageRenderer.hpp"
#include "TextSearch.hpp"

#include "core/text.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDate>
#include <QLocale>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLineF>
#include <QMenu>
#include <QToolTip>
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

// Highlights are multiplied onto the page, so the text under them stays as dark as it was.
const QColor kSelectionColor(0x9E, 0xC9, 0xFF);
const QColor kMatchColor(0xFF, 0xE4, 0x5C);
const QColor kCurrentMatchColor(0xFF, 0x9F, 0x43);
const QColor kFieldColor(0xDD, 0xE6, 0xFF);
}  // namespace

DocumentView::DocumentView(PageRenderer* renderer, TextCache* text, TextSearch* search, AnnotationCache* annots,
                           FieldCache* fields, ContentCache* content, QWidget* parent)
    : QAbstractScrollArea(parent), renderer_(renderer), text_(text), search_(search), annots_(annots), fields_(fields),
      content_(content) {
  // Always shown, so fitting the width never makes it appear and change the width again.
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setFrameShape(QFrame::NoFrame);
  setFocusPolicy(Qt::StrongFocus);
  viewport()->setCursor(Qt::OpenHandCursor);
  viewport()->setMouseTracking(true);  // the cursor turns into a text cursor over text
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  verticalScrollBar()->setSingleStep(40);
  horizontalScrollBar()->setSingleStep(40);

  requestTimer_.setSingleShot(true);
  requestTimer_.setInterval(0);
  connect(&requestTimer_, &QTimer::timeout, this, &DocumentView::sendRequests);
  autoScroll_.setInterval(30);
  connect(&autoScroll_, &QTimer::timeout, this, &DocumentView::autoScrollStep);
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
  layoutEditor(true);
  layoutFieldEditor(true);
  layoutContentEditor(true);
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
  layoutEditor(false);
  layoutFieldEditor(false);
  layoutContentEditor(false);
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

    // What was rendered at other zooms, scaled to this one, until this zoom's tiles arrive: a
    // zoom or a resize shows the page a little soft for a moment instead of blank or blurry.
    // Sharpest last, so it ends up on top; the current zoom's tiles cover them all.
    auto others = renderer_->otherTiles(i, scaleKey);
    if (!others.empty()) {
      const double previewScale = preview.isNull() ? 0 : preview.width() / s.width;
      std::sort(others.begin(), others.end(),
                [](const auto& a, const auto& b) { return a.first.scaleKey < b.first.scaleKey; });
      const double pagePpp = r.width() / s.width;  // logical pixels per point on this page
      for (const auto& [key, img] : others) {
        const double os = key.scale();
        if (os <= previewScale) continue;  // no better than the preview
        const QRectF target(r.left() + key.tx * T / os * pagePpp, r.top() + key.ty * T / os * pagePpp,
                            img.width() / os * pagePpp, img.height() / os * pagePpp);
        if (target.intersects(part)) p.drawImage(target, img);
      }
    }

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

  // Form fields that can be filled in, tinted (multiplied, so what's in them stays readable).
  p.setCompositionMode(QPainter::CompositionMode_Multiply);
  p.setPen(Qt::NoPen);
  if (highlightFields_ && fields_->hasForm())
    for (int i = first; i <= last; ++i)
      for (const pdfz::FormField& f : *fields_->get(i))
        if (isFillable(f) && !f.readOnly) p.fillRect(toViewport(i, f.rect), kFieldColor);

  // Search matches, then the selection, over the pages.
  for (int i = first; i <= last; ++i) {
    const auto [m0, m1] = search_->matchesOnPage(i);
    const auto [s0, s1] = selectionOn(i);
    if (m0 == m1 && s0 == s1) continue;
    const auto text = text_->get(i);
    if (!text || text->empty()) continue;
    for (int m = m0; m < m1; ++m) {
      const SearchMatch& match = search_->matches()[std::size_t(m)];
      const QColor c = m == search_->current() ? kCurrentMatchColor : kMatchColor;
      for (const pdfz::RectF& r : text->selectionRects(match.start, match.end)) p.fillRect(toViewport(i, r), c);
    }
    // Going to a match selects it (so Ctrl+C copies it); it's shown in the match color alone.
    bool selectionIsCurrentMatch = false;
    if (search_->current() >= m0 && search_->current() < m1) {
      const SearchMatch& cur = search_->matches()[std::size_t(search_->current())];
      selectionIsCurrentMatch = cur.start == s0 && cur.end == s1;
    }
    if (s0 != s1 && !selectionIsCurrentMatch)
      for (const pdfz::RectF& r : text->selectionRects(s0, s1)) p.fillRect(toViewport(i, r), kSelectionColor);
  }
  paintOverlays(p);
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
      bool previewFresh = false;
      renderer_->preview(i, &previewFresh);
      if (withPreviews && !previewFresh && std::find(previews.begin(), previews.end(), i) == previews.end())
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
          bool fresh = false;
          renderer_->tile(key, &fresh);
          if (!fresh && std::find(tiles.begin(), tiles.end(), key) == tiles.end()) tiles.push_back(key);
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
    case Qt::Key_Escape:
      // Each press undoes one thing: the drag in progress, then the selection, then the tool.
      if (drag_ != Drag::None) {
        drag_ = Drag::None;
        inkPoints_.clear();
        viewport()->update();
        return;
      }
      if (selPage_ >= 0 || hasSelection() || focusPage_ >= 0 || contentPage_ >= 0) {
        clearAnnotationSelection();
        clearSelection();
        setFieldFocus(-1, -1);
        clearContentSelection();
        return;
      }
      if (tool_ != Tool::Select) emit toolResetRequested();
      return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
      if (selPage_ >= 0) {
        emit annotationDeleteRequested(selPage_, selIndex_);
        return;
      }
      if (contentPage_ >= 0) {
        const int page = contentPage_;
        const std::vector<int> objs = contentSel_.objects;
        clearContentSelection();
        emit contentDeleteRequested(page, objs);
        return;
      }
      break;
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
      if (const pdfz::FormField* f = focusedField()) {
        const pdfz::FormField copy = *f;
        activateField(focusPage_, copy);
        return;
      }
      if (e->key() != Qt::Key_Space) break;
      verticalScrollBar()->triggerAction(e->modifiers() & Qt::ShiftModifier ? QScrollBar::SliderPageStepSub
                                                                            : QScrollBar::SliderPageStepAdd);
      return;
    case Qt::Key_Home: goToPage(0); return;
    case Qt::Key_End: goToPage(int(pageRects_.size()) - 1); return;
    case Qt::Key_Left:
      if (noHScroll) { goToPage(currentPage_ - 1); return; }
      break;
    case Qt::Key_Right:
      if (noHScroll) { goToPage(currentPage_ + 1); return; }
      break;
    default: break;
  }
  QAbstractScrollArea::keyPressEvent(e);
}

namespace {
bool isTextMarkupTool(MarkupTool t) {
  return t == MarkupTool::Highlight || t == MarkupTool::Underline || t == MarkupTool::StrikeOut;
}
bool isDragTool(MarkupTool t) {
  return t == MarkupTool::TextBox || t == MarkupTool::FillText || t == MarkupTool::Pen || t == MarkupTool::Rectangle ||
         t == MarkupTool::Ellipse || t == MarkupTool::Line || t == MarkupTool::Arrow;
}
}  // namespace

// Left button, depending on the tool: pick a comment, select text, draw a markup, or move the
// pages. The middle button always moves the pages.
void DocumentView::mousePressEvent(QMouseEvent* e) {
  const QPointF pos = e->position();
  const bool left = e->button() == Qt::LeftButton;
  commitTextEdit();  // a click outside the box being typed in ends the typing
  commitFieldEdit();
  commitContentEdit();
  setFocus();
  if (tool_ == Tool::Edit && e->button() == Qt::LeftButton) {
    editPress(e);
    return;
  }

  // The selected comment's corner handles, then the comment itself.
  if (left && tool_ != Tool::Hand) {
    const int corner = resizeHandleUnder(pos);
    if (corner >= 0) {
      drag_ = Drag::ResizeAnnot;
      dragPage_ = selPage_;
      resizeCorner_ = corner;
      dragStart_ = dragNow_ = pointOnPage(selPage_, pos);
      dragMoved_ = false;
      e->accept();
      return;
    }
  }
  if (left && tool_ == Tool::Select) {
    int page, pos2;
    if (annotationUnder(pos, page, pos2)) {
      const auto list = annots_->get(page);
      const pdfz::Annotation& a = (*list)[std::size_t(pos2)];
      selectAnnotation(page, a.index, false);
      clearSelection();
      if (canMove(a)) {
        drag_ = Drag::MoveAnnot;
        dragPage_ = page;
        dragStart_ = dragNow_ = pointOnPage(page, pos);
        dragMoved_ = false;
      }
      e->accept();
      return;
    }
    clearAnnotationSelection();
    int fieldPage, fieldPos;
    if (fieldUnder(pos, fieldPage, fieldPos)) {
      clearSelection();
      const pdfz::FormField f = (*fields_->get(fieldPage))[std::size_t(fieldPos)];
      activateField(fieldPage, f);
      e->accept();
      return;
    }
    setFieldFocus(-1, -1);
  }

  const bool selectsText = tool_ == Tool::Select || (tool_ == Tool::Markup && isTextMarkupTool(markupTool_));
  if (left && selectsText) {
    // The third click of a triple click selects the whole line.
    if (tool_ == Tool::Select && sinceDoubleClick_.isValid() &&
        sinceDoubleClick_.elapsed() < QApplication::doubleClickInterval() &&
        (e->position().toPoint() - doubleClickPos_).manhattanLength() < 6) {
      sinceDoubleClick_.invalidate();
      int page;
      QPointF pt;
      if (pagePointAt(pos, page, pt, false))
        if (const auto text = text_->get(page)) {
          const int c = text->charAt(pt.x(), pt.y(), 2);
          if (c >= 0) {
            const auto [ls, le] = text->lineAt(c);
            setSelection({page, ls}, {page, le});
          }
        }
      e->accept();
      return;
    }
    if (overText(pos) || ((e->modifiers() & Qt::ShiftModifier) && hasSelection())) {
      int page;
      QPointF pt;
      pagePointAt(pos, page, pt, true);
      const auto text = text_->get(page);
      const int caret = text ? std::max(0, text->caretAt(pt.x(), pt.y())) : 0;
      if (e->modifiers() & Qt::ShiftModifier) setSelection(anchor_, {page, caret});  // extend
      else setSelection({page, caret}, {page, caret});
      selecting_ = true;
      lastMousePos_ = pos.toPoint();
      e->accept();
      return;
    }
    clearSelection();
  }

  if (left && tool_ == Tool::Place) {
    int page;
    QPointF pt;
    if (pagePointAt(pos, page, pt, false)) {
      const pdfz::SizeF s = renderer_->document()->pageSize(page);
      const double w = std::min(placeSize_.width(), s.width), h = std::min(placeSize_.height(), s.height);
      const double x = std::clamp(pt.x() - w / 2, 0.0, s.width - w), y = std::clamp(pt.y() - h / 2, 0.0, s.height - h);
      pdfz::Annotation a = placeStamp_;
      a.rect = {x, y, x + w, y + h};
      hoverPage_ = -1;
      emit stampCreated(page, a, placeDrawing_);
      emit toolResetRequested();  // one at a time
    }
    e->accept();
    return;
  }

  if (left && tool_ == Tool::Markup && !isTextMarkupTool(markupTool_)) {
    int page;
    QPointF pt;
    if (pagePointAt(pos, page, pt, false)) {
      clearSelection();
      clearAnnotationSelection();
      if (markupTool_ == MarkupTool::Note) {
        pdfz::Annotation a;
        a.kind = pdfz::AnnotKind::Note;
        const pdfz::SizeF s = renderer_->document()->pageSize(page);
        const double x = std::clamp(pt.x(), 0.0, std::max(0.0, s.width - 20));
        const double y = std::clamp(pt.y(), 0.0, std::max(0.0, s.height - 20));
        a.rect = {x, y, x + 20, y + 20};
        applyStyle(a, markupStyle_);
        emit annotationCreated(page, a);
      } else if (markupTool_ == MarkupTool::FillText && [&] {
                   int ap, al;
                   if (!annotationUnder(pos, ap, al)) return false;
                   const pdfz::Annotation& hit = (*annots_->get(ap))[std::size_t(al)];
                   if (hit.kind != pdfz::AnnotKind::FreeText) return false;
                   emit annotationEditRequested(ap, hit.index);  // type in it again
                   return true;
                 }()) {
        // done
      } else if (markSubject(markupTool_)) {
        placeMark(page, pt);
      } else if (markupTool_ == MarkupTool::Date) {
        placeDate(page, pt);
      } else if (isDragTool(markupTool_)) {
        drag_ = Drag::Create;
        dragPage_ = page;
        dragStart_ = dragNow_ = pt;
        dragMoved_ = false;
        inkPoints_.assign(1, pdfz::PointF{pt.x(), pt.y()});
      }
      e->accept();
      return;
    }
  }

  if (left || e->button() == Qt::MiddleButton) {
    panning_ = true;
    panLast_ = pos.toPoint();
    viewport()->setCursor(Qt::ClosedHandCursor);
    e->accept();
    return;
  }
  QAbstractScrollArea::mousePressEvent(e);
}

void DocumentView::mouseDoubleClickEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton && tool_ == Tool::Edit) {
    int page, pos;
    if (contentUnder(e->position(), page, pos)) {
      const pdfz::ContentBlock b = (*content_->get(page))[std::size_t(pos)];
      drag_ = Drag::None;
      if (b.kind == pdfz::ContentBlock::Kind::Text && b.editable) emit contentEditRequested(page, b);
    }
    e->accept();
    return;
  }
  if (e->button() != Qt::LeftButton || tool_ != Tool::Select) return QAbstractScrollArea::mouseDoubleClickEvent(e);
  int page, listPos;
  if (annotationUnder(e->position(), page, listPos)) {
    drag_ = Drag::None;
    emit annotationEditRequested(page, (*annots_->get(page))[std::size_t(listPos)].index);
    e->accept();
    return;
  }
  QPointF pt;
  if (pagePointAt(e->position(), page, pt, false)) {
    if (const auto text = text_->get(page)) {
      const int c = text->charAt(pt.x(), pt.y(), 2);
      if (c >= 0) {
        const auto [ws, we] = text->wordAt(c);
        setSelection({page, ws}, {page, we});
        selecting_ = false;
        sinceDoubleClick_.start();
        doubleClickPos_ = e->position().toPoint();
      }
    }
  }
  e->accept();
}

void DocumentView::mouseMoveEvent(QMouseEvent* e) {
  if (drag_ != Drag::None) {
    QPointF pt = pointOnPage(dragPage_, e->position());
    if (renderer_->document()) {  // keep drawing on the page
      const pdfz::SizeF s = renderer_->document()->pageSize(dragPage_);
      pt = {std::clamp(pt.x(), 0.0, s.width), std::clamp(pt.y(), 0.0, s.height)};
    }
    const double movedPx = QLineF(dragStart_, pt).length() * pagePixelsPerPoint(dragPage_);
    if (movedPx > 3) dragMoved_ = true;
    if (drag_ == Drag::Create && markupTool_ == MarkupTool::Pen) {
      const pdfz::PointF& last = inkPoints_.back();
      if (std::hypot(pt.x() - last.x, pt.y() - last.y) * pagePixelsPerPoint(dragPage_) >= 1.5)
        inkPoints_.push_back({pt.x(), pt.y()});
    }
    dragNow_ = pt;
    viewport()->update();
    e->accept();
    return;
  }
  if (selecting_) {
    lastMousePos_ = e->position().toPoint();
    extendSelectionTo(e->position());
    // Past the top or bottom edge: keep scrolling (and selecting) while the mouse is there.
    const bool outside = lastMousePos_.y() < 0 || lastMousePos_.y() > viewport()->height();
    if (outside && !autoScroll_.isActive()) autoScroll_.start();
    if (!outside) autoScroll_.stop();
    e->accept();
    return;
  }
  if (panning_) {
    const QPoint pos = e->position().toPoint();
    const QPoint d = pos - panLast_;
    panLast_ = pos;
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
    verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
    e->accept();
    return;
  }
  if (tool_ == Tool::Place) {
    int page;
    QPointF pt;
    if (!pagePointAt(e->position(), page, pt, false)) page = -1;
    hoverPage_ = page;
    hoverPoint_ = pt;
    viewport()->update();
  }
  updateHoverCursor(e->position());
  QAbstractScrollArea::mouseMoveEvent(e);
}

void DocumentView::mouseReleaseEvent(QMouseEvent* e) {
  if (drag_ != Drag::None && e->button() == Qt::LeftButton) {
    const Drag d = drag_;
    const pdfz::RectF contentTo = contentDragRect();  // while drag_ still says how
    drag_ = Drag::None;
    if (d == Drag::MoveAnnot && dragMoved_) {
      emit annotationMoved(dragPage_, selIndex_, dragNow_.x() - dragStart_.x(), dragNow_.y() - dragStart_.y());
    } else if (d == Drag::ResizeAnnot && dragMoved_) {
      const pdfz::RectF r = resizedBounds();
      if (r.width() >= 4 && r.height() >= 4) emit annotationResized(dragPage_, selIndex_, r);
    } else if (d == Drag::Create) {
      finishCreate();
    } else if (d == Drag::MoveContent && dragMoved_) {
      emit contentMoved(dragPage_, contentSel_.objects, dragNow_.x() - dragStart_.x(), dragNow_.y() - dragStart_.y());
    } else if (d == Drag::ResizeContent && dragMoved_) {
      if (contentTo.width() >= 2 && contentTo.height() >= 2)
        emit contentResized(dragPage_, contentSel_.objects, contentSel_.rect, contentTo);
    } else if (d == Drag::MoveContent && contentSel_.kind == pdfz::ContentBlock::Kind::Text && contentSel_.editable &&
               editPressWasSelected_) {
      emit contentEditRequested(dragPage_, contentSel_);  // a click on selected text: type in it
    }
    inkPoints_.clear();
    viewport()->update();
    e->accept();
    return;
  }
  if (selecting_ && e->button() == Qt::LeftButton) {
    selecting_ = false;
    autoScroll_.stop();
    if (tool_ == Tool::Markup && isTextMarkupTool(markupTool_) && hasSelection()) {
      markupSelection(markupTool_, markupStyle_);
      clearSelection();
    }
    e->accept();
    return;
  }
  if (panning_ && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)) {
    panning_ = false;
    updateHoverCursor(e->position());
    e->accept();
    return;
  }
  QAbstractScrollArea::mouseReleaseEvent(e);
}

void DocumentView::finishCreate() {
  const auto& doc = renderer_->document();
  if (!doc || dragPage_ < 0) return;
  pdfz::Annotation a;
  const pdfz::RectF box = pdfz::RectF{dragStart_.x(), dragStart_.y(), dragNow_.x(), dragNow_.y()}.normalized();
  const pdfz::PointF p0{dragStart_.x(), dragStart_.y()}, p1{dragNow_.x(), dragNow_.y()};
  switch (markupTool_) {
    case MarkupTool::TextBox:
    case MarkupTool::FillText: {
      a.kind = pdfz::AnnotKind::FreeText;
      applyStyle(a, markupStyle_);
      if (markupTool_ == MarkupTool::FillText) {
        a.borderWidth = 0;  // typed on the form: no box
        a.hasFill = false;
        a.subject = "Fill Text";
      }
      if (box.width() < 12 || box.height() < 8) {  // a click: a box for a line of text there
        const pdfz::SizeF s = doc->pageSize(dragPage_);
        const double line = QFontMetricsF(fontFor(a.fontName, 1000)).height() / 1000 * a.fontSize;
        // Text on a form starts narrow and grows as it's typed; a text box has a usual width.
        const double wanted = markupTool_ == MarkupTool::FillText ? fillTextMinWidth(a) : 200.0;
        const double w = std::min(wanted, s.width), h = std::min(line + 2 * textBoxPadding(a), s.height);
        // Added text starts where the click was, a little up so the line sits on it.
        const double top = markupTool_ == MarkupTool::FillText ? p0.y - h / 2 : p0.y;
        const double x = std::clamp(p0.x, 0.0, s.width - w), y = std::clamp(top, 0.0, s.height - h);
        a.rect = {x, y, x + w, y + h};
      } else {
        a.rect = box;
      }
      startTextEdit(dragPage_, -1, a);  // added to the document once it has some text
      return;
    }
    case MarkupTool::Rectangle:
    case MarkupTool::Ellipse:
      if (box.width() < 3 || box.height() < 3) return;
      a.kind = markupTool_ == MarkupTool::Rectangle ? pdfz::AnnotKind::Square : pdfz::AnnotKind::Circle;
      applyStyle(a, markupStyle_);
      a.rect = box;
      break;
    case MarkupTool::Line:
    case MarkupTool::Arrow: {
      const double len = std::hypot(p1.x - p0.x, p1.y - p0.y);
      if (len < 3) return;
      a.kind = pdfz::AnnotKind::Ink;  // see AnnotKind::Line
      applyStyle(a, markupStyle_);
      a.subject = markupTool_ == MarkupTool::Line ? "Line" : "Arrow";
      if (markupTool_ == MarkupTool::Arrow) a.ink = arrowStrokes(p0, p1, a.borderWidth);
      else a.ink = {{p0, p1}};
      break;
    }
    case MarkupTool::Pen:
      a.kind = pdfz::AnnotKind::Ink;
      applyStyle(a, markupStyle_);
      a.ink = {inkPoints_};
      if (a.ink[0].size() == 1) a.ink[0].push_back({a.ink[0][0].x + 0.5, a.ink[0][0].y + 0.5});  // a dot
      break;
    default:
      return;
  }
  emit annotationCreated(dragPage_, a);
}

void DocumentView::markupSelection(MarkupTool tool, const MarkupStyle& style) {
  if (!hasSelection()) return;
  const TextPos a = std::min(anchor_, focus_);
  const TextPos b = std::max(anchor_, focus_);
  for (int p = a.page; p <= b.page; ++p) {
    const auto text = text_->get(p);
    if (!text) continue;
    const auto [s0, s1] = selectionOn(p);
    pdfz::Annotation m;
    m.kind = tool == MarkupTool::Highlight ? pdfz::AnnotKind::Highlight
             : tool == MarkupTool::Underline ? pdfz::AnnotKind::Underline
                                             : pdfz::AnnotKind::StrikeOut;
    applyStyle(m, style);
    m.quads = text->selectionRects(s0, s1);
    // The marked text becomes the comment's text, as Acrobat does by default, so the comments
    // list says what was marked (and it can be edited into a real comment).
    m.contents = text->textUtf8(s0, s1);
    if (m.contents.size() > 2000) m.contents.resize(2000);
    if (!m.quads.empty()) emit annotationCreated(p, m);
  }
}

void DocumentView::autoScrollStep() {
  if (!selecting_) {
    autoScroll_.stop();
    return;
  }
  const int y = lastMousePos_.y();
  const int over = y < 0 ? y : (y > viewport()->height() ? y - viewport()->height() : 0);
  if (over == 0) return;
  verticalScrollBar()->setValue(verticalScrollBar()->value() + std::clamp(over, -60, 60));
  extendSelectionTo(lastMousePos_);
}

void DocumentView::updateHoverCursor(QPointF pos) {
  if (tool_ == Tool::Edit) {
    const int corner = contentHandleUnder(pos);
    int page, listPos;
    if (corner >= 0) viewport()->setCursor(corner == 0 || corner == 3 ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
    else if (editAddText_) viewport()->setCursor(Qt::IBeamCursor);
    else if (contentUnder(pos, page, listPos)) {
      const pdfz::ContentBlock& b = (*content_->get(page))[std::size_t(listPos)];
      const bool selectedText = page == contentPage_ && b.objects == contentSel_.objects &&
                                b.kind == pdfz::ContentBlock::Kind::Text && b.editable;
      viewport()->setCursor(selectedText ? Qt::IBeamCursor : Qt::SizeAllCursor);
    } else {
      viewport()->setCursor(Qt::ArrowCursor);
    }
    return;
  }
  const int corner = tool_ != Tool::Hand ? resizeHandleUnder(pos) : -1;
  if (corner >= 0) {
    viewport()->setCursor(corner == 0 || corner == 3 ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
    return;
  }
  int page, listPos;
  const bool onAnnot = tool_ != Tool::Hand && annotationUnder(pos, page, listPos);
  if (onAnnot) {
    const pdfz::Annotation& a = (*annots_->get(page))[std::size_t(listPos)];
    // Hovering a comment shows its text, as a note's pop-up would.
    const QString tip = QString::fromStdString(a.contents).trimmed();
    if (!tip.isEmpty() && tool_ == Tool::Select) {
      QString head = QString::fromStdString(a.author);
      if (head.isEmpty()) head = QString::fromLatin1(pdfz::kindName(a.kind));
      QToolTip::showText(viewport()->mapToGlobal(pos.toPoint()),
                         QStringLiteral("<b>%1</b><br>%2").arg(head.toHtmlEscaped(), tip.toHtmlEscaped().replace('\n', "<br>")),
                         viewport());
    }
    if (tool_ == Tool::Select) {
      viewport()->setCursor(canMove(a) ? Qt::SizeAllCursor : Qt::PointingHandCursor);
      return;
    }
  } else {
    QToolTip::hideText();
  }
  if (tool_ == Tool::Place) {
    viewport()->setCursor(Qt::CrossCursor);
    return;
  }
  if (tool_ == Tool::Markup) {
    if (isTextMarkupTool(markupTool_)) viewport()->setCursor(overText(pos) ? Qt::IBeamCursor : Qt::OpenHandCursor);
    else viewport()->setCursor(Qt::CrossCursor);
    return;
  }
  if (tool_ == Tool::Select) {
    int fieldPage, fieldPos;
    if (fieldUnder(pos, fieldPage, fieldPos)) {
      const pdfz::FormField& f = (*fields_->get(fieldPage))[std::size_t(fieldPos)];
      viewport()->setCursor(f.readOnly ? Qt::ArrowCursor : f.type == pdfz::FieldType::Text ? Qt::IBeamCursor
                                                                                          : Qt::PointingHandCursor);
      const QString tip = QString::fromStdString(f.tooltip).trimmed();
      if (!tip.isEmpty()) QToolTip::showText(viewport()->mapToGlobal(pos.toPoint()), tip.toHtmlEscaped(), viewport());
      return;
    }
  }
  if (tool_ == Tool::Select && overText(pos)) viewport()->setCursor(Qt::IBeamCursor);
  else viewport()->setCursor(Qt::OpenHandCursor);
}

void DocumentView::setTool(Tool tool) {
  if (tool != Tool::Edit) {
    commitContentEdit();
    clearContentSelection();
  }
  tool_ = tool;
  drag_ = Drag::None;
  if (tool != Tool::Select) clearAnnotationSelection();
  if (tool != Tool::Place) hoverPage_ = -1;
  viewport()->setCursor(tool == Tool::Markup || tool == Tool::Place ? Qt::CrossCursor : Qt::OpenHandCursor);
  viewport()->update();
}

void DocumentView::setMarkupTool(MarkupTool tool, const MarkupStyle& style) {
  markupTool_ = tool;
  markupStyle_ = style;
  setTool(Tool::Markup);
}

void DocumentView::contextMenuEvent(QContextMenuEvent* e) {
  int page = currentPage_, listPos = -1, index = -1;
  if (annotationUnder(e->pos(), page, listPos)) {
    index = (*annots_->get(page))[std::size_t(listPos)].index;
    selectAnnotation(page, index, false);
  } else {
    QPointF pt;
    if (!pagePointAt(e->pos(), page, pt, true)) page = currentPage_;
  }
  emit contextMenuRequested(e->globalPos(), page, index);
}

double DocumentView::pagePixelsPerPoint(int page) const {
  if (page < 0 || page >= int(pageRects_.size()) || !renderer_->document()) return pixelsPerPoint();
  return pageRects_[std::size_t(page)].width() / std::max(1e-9, renderer_->document()->pageSize(page).width);
}

QPointF DocumentView::toViewport(int page, double x, double y) const {
  const QRectF& pr = pageRects_[std::size_t(page)];
  const double ppp = pagePixelsPerPoint(page);
  return QPointF(pr.left() + x * ppp, pr.top() + y * ppp) - visibleRect().topLeft();
}

QPointF DocumentView::pointOnPage(int page, QPointF vp) const {
  if (page < 0 || page >= int(pageRects_.size())) return {};
  const QPointF c = vp + visibleRect().topLeft();
  return (c - pageRects_[std::size_t(page)].topLeft()) / pagePixelsPerPoint(page);
}

bool DocumentView::annotationUnder(QPointF vp, int& page, int& listPos) const {
  QPointF pt;
  if (!pagePointAt(vp, page, pt, false)) return false;
  const auto list = annots_->get(page);
  if (!list || list->empty()) return false;
  listPos = annotationAt(*list, pt.x(), pt.y(), 4 / pagePixelsPerPoint(page));
  return listPos >= 0;
}

const pdfz::Annotation* DocumentView::selected() const {
  if (selPage_ < 0) return nullptr;
  const auto list = annots_->get(selPage_);
  if (!list) return nullptr;
  for (const pdfz::Annotation& a : *list)
    if (a.index == selIndex_) return &a;
  return nullptr;
}

pdfz::RectF DocumentView::selectedBounds() const {
  const pdfz::Annotation* a = selected();
  if (!a) return {};
  if (pdfz::isTextMarkup(a->kind) && !a->quads.empty()) {
    pdfz::RectF r;
    for (const pdfz::RectF& q : a->quads) r = r.united(q);
    return r;
  }
  return a->rect;
}

pdfz::RectF DocumentView::resizedBounds() const {
  const pdfz::Annotation* a = selected();
  pdfz::RectF r = selectedBounds();
  if (!a) return r;
  const pdfz::RectF start = r;
  const double dx = dragNow_.x() - dragStart_.x(), dy = dragNow_.y() - dragStart_.y();
  const bool left = resizeCorner_ == 0 || resizeCorner_ == 2, top = resizeCorner_ == 0 || resizeCorner_ == 1;
  if (left) r.left += dx; else r.right += dx;
  if (top) r.top += dy; else r.bottom += dy;
  // A stamp (a signature) keeps its shape: the corner follows the larger of the two moves.
  if (a->kind == pdfz::AnnotKind::Stamp && start.width() > 0 && start.height() > 0) {
    const double k = std::max(std::abs(r.width()) / start.width(), std::abs(r.height()) / start.height());
    const double w = start.width() * k, h = start.height() * k;
    if (left) r.left = start.right - w; else r.right = start.left + w;
    if (top) r.top = start.bottom - h; else r.bottom = start.top + h;
  }
  return r.normalized();
}

void DocumentView::placeMark(int page, QPointF pt) {
  const char* subject = markSubject(markupTool_);
  const pdfz::SizeF s = renderer_->document()->pageSize(page);
  const double size = markSize(markupTool_);
  const double x = std::clamp(pt.x() - size / 2, 0.0, std::max(0.0, s.width - size));
  const double y = std::clamp(pt.y() - size / 2, 0.0, std::max(0.0, s.height - size));
  pdfz::Annotation a;
  a.kind = pdfz::AnnotKind::Stamp;
  a.subject = subject;
  a.color = fromQColor(markupStyle_.color);
  a.rect = {x, y, x + size, y + size};
  emit stampCreated(page, a, markDrawing(subject, markupStyle_.color));
}

void DocumentView::placeDate(int page, QPointF pt) {
  pdfz::Annotation a;
  a.kind = pdfz::AnnotKind::FreeText;
  applyStyle(a, markupStyle_);
  a.borderWidth = 0;
  a.hasFill = false;
  a.subject = "Date";
  a.contents = QLocale::system().toString(QDate::currentDate(), QLocale::ShortFormat).toStdString();
  const QFont font = fontFor(a.fontName, 1000);
  const QFontMetricsF fm(font);
  const double pad = textBoxPadding(a);
  const double w = fm.horizontalAdvance(QString::fromStdString(a.contents)) / 1000 * a.fontSize + 2 * pad + 2;
  const double h = fm.height() / 1000 * a.fontSize + 2 * pad;
  const pdfz::SizeF s = renderer_->document()->pageSize(page);
  const double x = std::clamp(pt.x(), 0.0, std::max(0.0, s.width - w));
  const double y = std::clamp(pt.y() - h / 2, 0.0, std::max(0.0, s.height - h));
  a.rect = {x, y, x + w, y + h};
  emit annotationCreated(page, a);
}

void DocumentView::startPlacing(const pdfz::Annotation& stamp, const pdfz::StampDrawing& drawing,
                                const QImage& preview, QSizeF size) {
  commitTextEdit();
  commitFieldEdit();
  placeStamp_ = stamp;
  placeDrawing_ = drawing;
  placePreview_ = preview;
  placeSize_ = size;
  hoverPage_ = -1;
  setTool(Tool::Place);
}

int DocumentView::resizeHandleUnder(QPointF vp) const {
  const pdfz::Annotation* a = selected();
  if (!a || !canResize(*a)) return -1;
  const QRectF r = toViewport(selPage_, a->rect);
  const QPointF corners[] = {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()};
  for (int i = 0; i < 4; ++i)
    if (std::abs(vp.x() - corners[i].x()) <= 6 && std::abs(vp.y() - corners[i].y()) <= 6) return i;
  return -1;
}

bool DocumentView::selectedAnnotation(int& page, int& index) const {
  if (selPage_ < 0 || !selected()) return false;
  page = selPage_;
  index = selIndex_;
  return true;
}

void DocumentView::selectAnnotation(int page, int index, bool scrollIntoView) {
  if (selPage_ == page && selIndex_ == index && !scrollIntoView) return;
  selPage_ = page;
  selIndex_ = index;
  if (scrollIntoView && selected()) ensureVisible(page, selectedBounds());
  viewport()->update();
  emit annotationSelectionChanged(page, index);
}

void DocumentView::clearAnnotationSelection() {
  if (selPage_ < 0) return;
  selPage_ = selIndex_ = -1;
  drag_ = Drag::None;
  viewport()->update();
  emit annotationSelectionChanged(-1, -1);
}

void DocumentView::annotationsChanged(int page) {
  if (page == selPage_ && !selected()) clearAnnotationSelection();
  viewport()->update();
}

void DocumentView::paintOverlays(QPainter& p) {
  p.setCompositionMode(QPainter::CompositionMode_SourceOver);
  p.setRenderHint(QPainter::Antialiasing);
  const QColor accent = palette().color(QPalette::Highlight);
  if (tool_ == Tool::Edit) paintContent(p);

  // The selected comment: a dashed frame, with corner handles if it can be resized.
  if (const pdfz::Annotation* a = selected()) {
    QRectF r = toViewport(selPage_, selectedBounds());
    if (drag_ == Drag::MoveAnnot && dragMoved_) {
      const double ppp = pagePixelsPerPoint(selPage_);
      r.translate((dragNow_ - dragStart_) * ppp);
    } else if (drag_ == Drag::ResizeAnnot && dragMoved_) {
      r = toViewport(selPage_, resizedBounds());
    }
    r.adjust(-3, -3, 3, 3);
    QPen pen(accent, 1.5, Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    if (canResize(*a)) {
      p.setPen(QPen(accent, 1));
      p.setBrush(Qt::white);
      const QRectF ar = toViewport(selPage_, a->rect);
      for (QPointF c : {ar.topLeft(), ar.topRight(), ar.bottomLeft(), ar.bottomRight()})
        p.drawRect(QRectF(c.x() - 4, c.y() - 4, 8, 8));
    }
  }

  // The stamp about to be placed, under the mouse.
  if (tool_ == Tool::Place && hoverPage_ >= 0 && hoverPage_ < int(pageRects_.size()) && !placePreview_.isNull()) {
    const double w = placeSize_.width(), h = placeSize_.height();
    const QRectF r = toViewport(hoverPage_, pdfz::RectF{hoverPoint_.x() - w / 2, hoverPoint_.y() - h / 2,
                                                        hoverPoint_.x() + w / 2, hoverPoint_.y() + h / 2});
    p.setOpacity(0.75);
    p.drawImage(r, placePreview_);
    p.setOpacity(1);
    p.setPen(QPen(accent, 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
  }

  // The form field with the keyboard.
  if (const pdfz::FormField* f = focusedField(); f && !fieldEditor_) {
    p.setPen(QPen(palette().color(QPalette::Text), 1, Qt::DotLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(toViewport(focusPage_, f->rect).adjusted(-2, -2, 2, 2));
  }

  // The text box being typed in: its fill and border (the editor over it shows the text).
  if (editor_ && editPage_ >= 0 && editPage_ < int(pageRects_.size())) {
    const QRectF r = toViewport(editPage_, editAnnot_.rect.normalized());
    const double ppp = pagePixelsPerPoint(editPage_);
    p.setOpacity(editAnnot_.opacity);
    if (editAnnot_.hasFill) p.fillRect(r, toQColor(editAnnot_.fill));
    if (editAnnot_.borderWidth > 0) {
      const double w = editAnnot_.borderWidth * ppp;
      p.setPen(QPen(toQColor(editAnnot_.color), w, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
      p.setBrush(Qt::NoBrush);
      p.drawRect(r.adjusted(w / 2, w / 2, -w / 2, -w / 2));
    }
    p.setOpacity(1);
    p.setPen(QPen(accent, 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(-3, -3, 3, 3));
  }

  // The markup being drawn, as it will look.
  if (drag_ == Drag::Create && dragPage_ >= 0) {
    const double ppp = pagePixelsPerPoint(dragPage_);
    QColor c = markupStyle_.color;
    QPen pen(c, std::max(1.0, markupStyle_.width * ppp), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.setOpacity(markupStyle_.opacity);
    if (markupStyle_.fill.isValid() && usesFill(markupTool_) && markupTool_ != MarkupTool::TextBox)
      p.setBrush(markupStyle_.fill);
    const QPointF a = toViewport(dragPage_, dragStart_.x(), dragStart_.y());
    const QPointF b = toViewport(dragPage_, dragNow_.x(), dragNow_.y());
    switch (markupTool_) {
      case MarkupTool::Rectangle: {
        pen.setJoinStyle(Qt::MiterJoin);
        p.setPen(pen);
        const double w = pen.widthF() / 2;
        p.drawRect(QRectF(a, b).normalized().adjusted(w, w, -w, -w));
        break;
      }
      case MarkupTool::Ellipse: {
        const double w = pen.widthF() / 2;
        p.drawEllipse(QRectF(a, b).normalized().adjusted(w, w, -w, -w));
        break;
      }
      case MarkupTool::Line: p.drawLine(a, b); break;
      case MarkupTool::Arrow:
        for (const auto& stroke : arrowStrokes({dragStart_.x(), dragStart_.y()}, {dragNow_.x(), dragNow_.y()},
                                               markupStyle_.width)) {
          QPolygonF poly;
          for (const pdfz::PointF& pt : stroke) poly << toViewport(dragPage_, pt.x, pt.y);
          p.drawPolyline(poly);
        }
        break;
      case MarkupTool::TextBox:
        p.setPen(QPen(c, 1, Qt::DashLine));
        p.drawRect(QRectF(a, b).normalized());
        break;
      case MarkupTool::Pen: {
        QPolygonF poly;
        for (const pdfz::PointF& pt : inkPoints_) poly << toViewport(dragPage_, pt.x, pt.y);
        p.drawPolyline(poly);
        break;
      }
      default: break;
    }
    p.setOpacity(1);
  }
}

// ---------------------------------------------------------------------------------------------
// Typing in a text box

void DocumentView::startTextEdit(int page, int index, const pdfz::Annotation& a) {
  commitTextEdit();
  if (page < 0 || page >= int(pageRects_.size())) return;
  editPage_ = page;
  editIndex_ = index;
  editAnnot_ = a;
  editAnnot_.rect = a.rect.normalized();
  editAnnot_.hidden = false;
  editOriginal_ = editAnnot_;
  editAutoWidth_ = a.kind == pdfz::AnnotKind::FreeText && (a.subject == "Fill Text" || a.subject == "Date");
  editMinWidth_ = editAutoWidth_ ? std::min(fillTextMinWidth(editAnnot_), editAnnot_.rect.width()) : 0;
  const quint64 gen = ++editGeneration_;
  editor_ = new InlineTextEdit(viewport());
  editor_->setAutoWidth(editAutoWidth_);
  editor_->setPlainText(QString::fromStdString(a.contents));
  connect(editor_, &InlineTextEdit::finished, this, [this, gen] {
    if (gen == editGeneration_) commitTextEdit();
  }, Qt::QueuedConnection);
  connect(editor_, &InlineTextEdit::heightNeeded, this, [this, gen] {
    if (gen == editGeneration_) fitEditorHeight();
  });
  layoutEditor(true);
  editor_->show();
  editor_->setFocus(Qt::OtherFocusReason);
  editor_->moveCursor(QTextCursor::End);
  ensureVisible(page, editAnnot_.rect);
  fitEditorHeight();
  viewport()->update();
  emit textEditStarted();
}

void DocumentView::commitTextEdit() {
  if (!editor_) return;
  InlineTextEdit* ed = editor_;
  editor_ = nullptr;
  ++editGeneration_;
  editAnnot_.contents = ed->toPlainText().toStdString();
  const pdfz::Annotation& o = editOriginal_;
  const pdfz::Annotation& n = editAnnot_;
  const bool changed = n.contents != o.contents || std::abs(n.rect.bottom - o.rect.bottom) > 0.01 ||
                       std::abs(n.rect.right - o.rect.right) > 0.01 ||
                       n.color != o.color || n.hasFill != o.hasFill || (n.hasFill && n.fill != o.fill) ||
                       n.opacity != o.opacity || n.borderWidth != o.borderWidth || n.fontSize != o.fontSize ||
                       n.fontName != o.fontName || n.textColor != o.textColor || n.align != o.align;
  if (ed->hasFocus()) setFocus();  // the keyboard comes back to the pages, unless it went elsewhere
  ed->hide();
  ed->deleteLater();
  const int page = editPage_, index = editIndex_;
  editPage_ = editIndex_ = -1;
  viewport()->update();
  emit textEditFinished(page, index, editAnnot_, changed);
}

void DocumentView::restyleTextEdit(const StyleChange& change) {
  if (!editor_) return;
  applyStyleChange(editAnnot_, change);
  layoutEditor(true);
  fitEditorHeight();
  viewport()->update();
}

void DocumentView::layoutEditor(bool restyle) {
  if (!editor_) return;
  if (editPage_ < 0 || editPage_ >= int(pageRects_.size())) {
    editor_->hide();
    return;
  }
  const QRectF r = toViewport(editPage_, editAnnot_.rect);
  editor_->setGeometry(QRect(QPoint(int(std::floor(r.left())), int(std::floor(r.top()))),
                             QPoint(int(std::ceil(r.right())) - 1, int(std::ceil(r.bottom())) - 1)));
  if (!restyle) return;
  const double ppp = pagePixelsPerPoint(editPage_);
  QFont font = fontFor(editAnnot_.fontName, 10);
  font.setPointSizeF(std::max(0.5, editAnnot_.fontSize * ppp * 72.0 / logicalDpiY()));  // fractional sizes too
  QColor text = toQColor(editAnnot_.textColor);
  text.setAlphaF(float(editAnnot_.opacity));
  editor_->setLook(font, text, editAnnot_.align, textBoxPadding(editAnnot_) * ppp);
}

// The box grows downwards to fit the text (it doesn't shrink below where it started).
void DocumentView::fitEditorHeight() {
  if (!editor_ || editPage_ < 0 || !renderer_->document()) return;
  const double ppp = pagePixelsPerPoint(editPage_);
  const pdfz::SizeF page = renderer_->document()->pageSize(editPage_);
  bool changed = false;
  if (editAutoWidth_) {
    // As wide as the longest line, with room for the caret, up to the page's edge, where the
    // text starts to wrap.
    const double pad = textBoxPadding(editAnnot_);
    const double slack = editAnnot_.fontSize * 0.6;
    const double wanted = std::max(editMinWidth_, editor_->textWidth() / ppp + 2 * pad + slack);
    const double room = std::max(editMinWidth_, page.width - editAnnot_.rect.left);
    const double width = std::min(wanted, room);
    editor_->setAutoWidth(wanted <= room);
    if (std::abs(editAnnot_.rect.left + width - editAnnot_.rect.right) >= 0.01) {
      editAnnot_.rect.right = editAnnot_.rect.left + width;
      changed = true;
    }
  }
  const double needed = editor_->contentHeight() / ppp;
  const double height = std::max(editOriginal_.rect.height(), needed);
  const double bottom = std::min(page.height, editAnnot_.rect.top + height);
  if (std::abs(bottom - editAnnot_.rect.bottom) >= 0.01) {
    editAnnot_.rect.bottom = bottom;
    changed = true;
  }
  if (!changed) return;
  layoutEditor(false);
  viewport()->update();
}

// ---------------------------------------------------------------------------------------------
// Edit PDF

void DocumentView::setEditAddText(bool on) {
  editAddText_ = on;
  if (on) clearContentSelection();
  updateHoverCursor(viewport()->mapFromGlobal(QCursor::pos()));
}

void DocumentView::contentChanged(int page) {
  // The selected block may have changed: find it again by its first object.
  if (page == contentPage_) {
    bool found = false;
    for (const pdfz::ContentBlock& b : *content_->get(page))
      if (!contentSel_.objects.empty() && std::find(b.objects.begin(), b.objects.end(), contentSel_.objects.front()) != b.objects.end() &&
          b.kind == contentSel_.kind) {
        contentSel_ = b;
        found = true;
        break;
      }
    if (!found) clearContentSelection();
  }
  viewport()->update();
}

bool DocumentView::selectedContent(int& page, pdfz::ContentBlock& block) const {
  if (contentPage_ < 0) return false;
  page = contentPage_;
  block = contentSel_;
  return true;
}

void DocumentView::selectContentAt(int page, pdfz::PointF pt) {
  const auto list = content_->get(page);
  const int i = blockAt(*list, pt.x, pt.y, 1);
  if (i < 0) {
    clearContentSelection();
    return;
  }
  contentPage_ = page;
  contentSel_ = (*list)[std::size_t(i)];
  viewport()->update();
  emit contentSelectionChanged();
}

void DocumentView::clearContentSelection() {
  if (contentPage_ < 0) return;
  contentPage_ = -1;
  contentSel_ = {};
  viewport()->update();
  emit contentSelectionChanged();
}

bool DocumentView::contentUnder(QPointF vp, int& page, int& listPos) const {
  QPointF pt;
  if (!pagePointAt(vp, page, pt, false)) return false;
  listPos = blockAt(*content_->get(page), pt.x(), pt.y(), 2 / pagePixelsPerPoint(page));
  return listPos >= 0;
}

int DocumentView::contentHandleUnder(QPointF vp) const {
  if (contentPage_ < 0 || contentSel_.kind != pdfz::ContentBlock::Kind::Image) return -1;
  const QRectF r = toViewport(contentPage_, contentSel_.rect);
  const QPointF corners[] = {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()};
  for (int i = 0; i < 4; ++i)
    if (std::abs(vp.x() - corners[i].x()) <= 6 && std::abs(vp.y() - corners[i].y()) <= 6) return i;
  return -1;
}

pdfz::RectF DocumentView::contentDragRect() const {
  pdfz::RectF r = contentSel_.rect;
  const double dx = dragNow_.x() - dragStart_.x(), dy = dragNow_.y() - dragStart_.y();
  if (drag_ == Drag::MoveContent) return {r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
  if (drag_ != Drag::ResizeContent) return r;
  const pdfz::RectF start = r;
  const bool left = resizeCorner_ == 0 || resizeCorner_ == 2, top = resizeCorner_ == 0 || resizeCorner_ == 1;
  if (left) r.left += dx; else r.right += dx;
  if (top) r.top += dy; else r.bottom += dy;
  // A picture keeps its shape.
  if (start.width() > 0 && start.height() > 0) {
    const double k = std::max(std::abs(r.width()) / start.width(), std::abs(r.height()) / start.height());
    if (left) r.left = start.right - start.width() * k; else r.right = start.left + start.width() * k;
    if (top) r.top = start.bottom - start.height() * k; else r.bottom = start.top + start.height() * k;
  }
  return r.normalized();
}

void DocumentView::editPress(QMouseEvent* e) {
  const QPointF pos = e->position();
  e->accept();
  const int corner = contentHandleUnder(pos);
  if (corner >= 0) {
    drag_ = Drag::ResizeContent;
    dragPage_ = contentPage_;
    resizeCorner_ = corner;
    dragStart_ = dragNow_ = pointOnPage(contentPage_, pos);
    dragMoved_ = false;
    return;
  }
  int page;
  QPointF pt;
  if (editAddText_) {
    if (pagePointAt(pos, page, pt, false)) {
      editAddText_ = false;
      emit editAddTextEnded();
      startNewText(page, pt);
    }
    return;
  }
  int listPos;
  if (contentUnder(pos, page, listPos)) {
    const pdfz::ContentBlock& b = (*content_->get(page))[std::size_t(listPos)];
    editPressWasSelected_ = page == contentPage_ && b.objects == contentSel_.objects;
    contentPage_ = page;
    contentSel_ = b;
    drag_ = Drag::MoveContent;
    dragPage_ = page;
    dragStart_ = dragNow_ = pointOnPage(page, pos);
    dragMoved_ = false;
    viewport()->update();
    emit contentSelectionChanged();
    return;
  }
  clearContentSelection();
  panning_ = true;  // empty space: move the page, as elsewhere
  panLast_ = pos.toPoint();
  viewport()->setCursor(Qt::ClosedHandCursor);
}

void DocumentView::startNewText(int page, QPointF pt) {
  TextEditing t;
  t.page = page;
  t.isNew = true;
  t.font = editNewFont_;
  t.size = editNewSize_;
  t.color = editNewColor_;
  t.lineSpacing = t.size * 1.2;
  // The click is where the text's top-left goes.
  const double ascent = QFontMetricsF(qtFont(t.font, 1000)).ascent() / 1000 * t.size;
  t.origin = {pt.x(), pt.y() + ascent};
  t.block.rect = {pt.x(), pt.y(), pt.x() + 1, pt.y() + t.lineSpacing};
  startContentEdit(t);
}

void DocumentView::startContentEdit(const TextEditing& e) {
  commitContentEdit();
  if (e.page < 0 || e.page >= int(pageRects_.size())) return;
  editing_ = e;
  contentPage_ = -1;
  contentSel_ = {};
  const quint64 gen = ++contentGeneration_;
  contentEditor_ = new InlineTextEdit(viewport());
  contentEditor_->setAutoWidth(e.width <= 0);
  contentEditor_->setPlainText(e.original);
  connect(contentEditor_, &InlineTextEdit::finished, this, [this, gen] {
    if (gen == contentGeneration_) commitContentEdit();
  }, Qt::QueuedConnection);
  connect(contentEditor_, &InlineTextEdit::heightNeeded, this, [this, gen] {
    if (gen == contentGeneration_) fitContentEditor();
  });
  layoutContentEditor(true);
  contentEditor_->show();
  contentEditor_->setFocus(Qt::OtherFocusReason);
  contentEditor_->moveCursor(QTextCursor::End);
  fitContentEditor();
  viewport()->update();
  emit contentEditStarted();
}

void DocumentView::commitContentEdit() {
  if (!contentEditor_) return;
  InlineTextEdit* ed = contentEditor_;
  contentEditor_ = nullptr;
  ++contentGeneration_;
  const QString text = ed->toPlainText();
  if (ed->hasFocus()) setFocus();
  ed->hide();
  ed->deleteLater();
  viewport()->update();
  const TextEditing e = editing_;
  emit contentEditFinished(e, text);
}

void DocumentView::restyleContentEdit(const EditFont& font, double size, const QColor& color) {
  if (!contentEditor_) return;
  const double ratio = editing_.size > 0 ? size / editing_.size : 1;
  editing_.font = font;
  editing_.color = color;
  editing_.lineSpacing *= ratio;
  editing_.size = size;
  layoutContentEditor(true);
  fitContentEditor();
}

// The editor's text sits where the page's did: its first baseline on the block's.
void DocumentView::layoutContentEditor(bool restyle) {
  if (!contentEditor_) return;
  const int page = editing_.page;
  if (page < 0 || page >= int(pageRects_.size())) {
    contentEditor_->hide();
    return;
  }
  const double ppp = pagePixelsPerPoint(page);
  const double pad = 2;  // pixels
  const QFontMetricsF fm(qtFont(editing_.font, 1000));
  const double ascent = fm.ascent() / 1000 * editing_.size;
  const double lines = std::max(1, int(contentEditor_->document()->blockCount()));
  const double top = editing_.origin.y - ascent - pad / ppp;
  const double width = editing_.width > 0 ? editing_.width + 2 * pad / ppp + editing_.size * 0.3
                                          : std::max(editing_.size * 2, contentEditor_->textWidth() / ppp + editing_.size * 0.8 + 2 * pad / ppp);
  const double height = std::max(editing_.lineSpacing * lines, contentEditor_->contentHeight() / ppp);
  const QRectF r = toViewport(page, pdfz::RectF{editing_.origin.x - pad / ppp, top, editing_.origin.x - pad / ppp + width,
                                                top + height + 2 * pad / ppp});
  contentEditor_->setGeometry(QRect(QPoint(int(std::floor(r.left())), int(std::floor(r.top()))),
                                    QPoint(int(std::ceil(r.right())), int(std::ceil(r.bottom())))));
  if (!restyle) return;
  QFont font = qtFont(editing_.font, 10);
  font.setPointSizeF(std::max(0.5, editing_.size * ppp * 72.0 / logicalDpiY()));
  contentEditor_->setLook(font, editing_.color, 0, pad);
  contentEditor_->setLineHeight(editing_.lineSpacing * ppp);
}

void DocumentView::fitContentEditor() {
  layoutContentEditor(false);
  viewport()->update();  // its frame
}

bool DocumentView::visibleCenter(int& page, pdfz::PointF& point) const {
  if (pageRects_.empty()) return false;
  page = currentPage_;
  const QPointF c = pointOnPage(page, QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
  const pdfz::SizeF s = renderer_->document()->pageSize(page);
  point = {std::clamp(c.x(), 0.0, s.width), std::clamp(c.y(), 0.0, s.height)};
  return true;
}

void DocumentView::paintContent(QPainter& p) {
  const QColor accent = palette().color(QPalette::Highlight);
  const QRectF vis = visibleRect();
  const int first = pageAt(vis.top()), last = pageAt(vis.bottom());
  p.setBrush(Qt::NoBrush);
  for (int i = first; i <= last && i >= 0; ++i) {
    p.setPen(QPen(QColor(0x60, 0x60, 0x60, 150), 1, Qt::DotLine));
    for (const pdfz::ContentBlock& b : *content_->get(i)) {
      if (i == contentPage_ && b.objects == contentSel_.objects) continue;
      p.drawRect(toViewport(i, b.rect).adjusted(-1.5, -1.5, 1.5, 1.5));
    }
  }
  if (contentPage_ >= 0 && contentPage_ < int(pageRects_.size())) {
    const bool dragging = (drag_ == Drag::MoveContent || drag_ == Drag::ResizeContent) && dragMoved_;
    const QRectF r = toViewport(contentPage_, dragging ? contentDragRect() : contentSel_.rect).adjusted(-1.5, -1.5, 1.5, 1.5);
    p.setPen(QPen(accent, 1.5));
    p.drawRect(r);
    if (contentSel_.kind == pdfz::ContentBlock::Kind::Image) {
      p.setPen(QPen(accent, 1));
      p.setBrush(Qt::white);
      for (QPointF c : {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()}) p.drawRect(QRectF(c.x() - 4, c.y() - 4, 8, 8));
    }
  }
  if (contentEditor_) {
    p.setPen(QPen(accent, 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(contentEditor_->geometry()).adjusted(-2, -2, 2, 2));
  }
}

// ---------------------------------------------------------------------------------------------
// Form fields

void DocumentView::setHighlightFields(bool on) {
  highlightFields_ = on;
  viewport()->update();
}

void DocumentView::fieldsChanged() {
  if (focusPage_ >= 0 && !focusedField()) focusPage_ = focusIndex_ = -1;
  viewport()->update();
}

bool DocumentView::fieldUnder(QPointF vp, int& page, int& listPos) const {
  if (!fields_->hasForm()) return false;
  QPointF pt;
  if (!pagePointAt(vp, page, pt, false)) return false;
  listPos = fieldAt(*fields_->get(page), pt.x(), pt.y());
  return listPos >= 0;
}

const pdfz::FormField* DocumentView::focusedField() const {
  if (focusPage_ < 0) return nullptr;
  const auto list = fields_->get(focusPage_);
  for (const pdfz::FormField& f : *list)
    if (f.index == focusIndex_) return &f;  // the cache keeps the list alive
  return nullptr;
}

void DocumentView::setFieldFocus(int page, int index) {
  if (focusPage_ == page && focusIndex_ == index) return;
  focusPage_ = page;
  focusIndex_ = index;
  viewport()->update();
}

void DocumentView::activateField(int page, const pdfz::FormField& f) {
  setFieldFocus(page, f.index);
  if (f.readOnly) return;
  using T = pdfz::FieldType;
  switch (f.type) {
    case T::Text: startFieldEdit(page, f); break;
    case T::CheckBox: emit fieldCheckRequested(page, f.index, !f.checked); break;
    case T::RadioButton:
      if (!f.checked) emit fieldCheckRequested(page, f.index, true);
      break;
    case T::ComboBox:
    case T::ListBox: showChoices(page, f); break;
    case T::Signature: emit signatureFieldClicked(page, f.rect); break;
    default: break;
  }
}

void DocumentView::showChoices(int page, const pdfz::FormField& f) {
  QMenu menu(this);
  for (std::size_t i = 0; i < f.options.size(); ++i) {
    QAction* a = menu.addAction(QString::fromStdString(f.options[i]));
    a->setCheckable(true);
    a->setChecked(i < f.selected.size() && f.selected[i]);
    a->setData(int(i));
  }
  QAction* other = nullptr;
  if (f.editable) {  // a combo box that takes any text
    if (!f.options.empty()) menu.addSeparator();
    other = menu.addAction(tr("Type Something Else…"));
  }
  const QRectF r = toViewport(page, f.rect);
  menu.setMinimumWidth(int(r.width()));
  QAction* picked = menu.exec(viewport()->mapToGlobal(r.bottomLeft().toPoint()));
  if (!picked) return;
  if (picked == other) {
    startFieldEdit(page, f);
    return;
  }
  const int option = picked->data().toInt();
  if (option >= 0 && !(std::size_t(option) < f.selected.size() && f.selected[std::size_t(option)] && !f.multiSelect))
    emit fieldChoiceRequested(page, f.index, option);
}

void DocumentView::startFieldEdit(int page, const pdfz::FormField& f) {
  commitTextEdit();
  commitFieldEdit();
  if (page < 0 || page >= int(pageRects_.size())) return;
  fieldPage_ = page;
  fieldEdited_ = f;
  setFieldFocus(page, f.index);
  const quint64 gen = ++fieldGeneration_;
  auto onFinished = [this, gen] {
    if (gen == fieldGeneration_) commitFieldEdit();
  };
  auto onTab = [this, gen](bool back) {
    if (gen != fieldGeneration_) return;
    commitFieldEdit();
    moveFieldFocus(back);
  };
  const QString value = QString::fromStdString(f.value);
  if (f.multiline) {
    auto* ed = new InlineTextEdit(viewport());
    ed->setFormField(Qt::white);
    ed->setPlainText(value);
    connect(ed, &InlineTextEdit::finished, this, onFinished, Qt::QueuedConnection);
    connect(ed, &InlineTextEdit::tabbed, this, onTab);
    fieldEditor_ = ed;
  } else {
    auto* ed = new FieldLineEdit(viewport());
    ed->setText(value);
    if (f.maxLength > 0) ed->setMaxLength(f.maxLength);
    if (f.password) ed->setEchoMode(QLineEdit::Password);
    connect(ed, &FieldLineEdit::finished, this, onFinished, Qt::QueuedConnection);
    connect(ed, &FieldLineEdit::tabbed, this, onTab);
    fieldEditor_ = ed;
  }
  layoutFieldEditor(true);
  ensureVisible(page, f.rect);
  fieldEditor_->show();
  fieldEditor_->setFocus(Qt::OtherFocusReason);
  if (auto* line = qobject_cast<FieldLineEdit*>(fieldEditor_)) line->selectAll();  // typing replaces
  else static_cast<InlineTextEdit*>(fieldEditor_)->moveCursor(QTextCursor::End);
  viewport()->update();
}

void DocumentView::commitFieldEdit() {
  if (!fieldEditor_) return;
  QWidget* ed = fieldEditor_;
  fieldEditor_ = nullptr;
  ++fieldGeneration_;
  QString text;
  if (auto* line = qobject_cast<FieldLineEdit*>(ed)) text = line->text();
  else text = static_cast<InlineTextEdit*>(ed)->toPlainText();
  if (ed->hasFocus()) setFocus();
  ed->hide();
  ed->deleteLater();
  const int page = fieldPage_;
  fieldPage_ = -1;
  viewport()->update();
  if (text != QString::fromStdString(fieldEdited_.value)) emit fieldTextChanged(page, fieldEdited_.index, text);
}

void DocumentView::layoutFieldEditor(bool restyle) {
  if (!fieldEditor_) return;
  if (fieldPage_ < 0 || fieldPage_ >= int(pageRects_.size())) {
    fieldEditor_->hide();
    return;
  }
  const QRectF r = toViewport(fieldPage_, fieldEdited_.rect);
  fieldEditor_->setGeometry(QRect(QPoint(int(std::floor(r.left())), int(std::floor(r.top()))),
                                  QPoint(int(std::ceil(r.right())) - 1, int(std::ceil(r.bottom())) - 1)));
  if (!restyle) return;
  const pdfz::FormField& f = fieldEdited_;
  const double ppp = pagePixelsPerPoint(fieldPage_);
  // "Auto" size: what fits the field's height (a multi-line field: a usual size).
  const double size = f.fontSize > 0 ? f.fontSize : f.multiline ? 10 : std::clamp(f.rect.height() * 0.62, 6.0, 14.0);
  QFont font = fontFor("Helv", 10);
  font.setPointSizeF(std::max(0.5, size * ppp * 72.0 / logicalDpiY()));
  const QColor color = toQColor(f.textColor);
  if (auto* line = qobject_cast<FieldLineEdit*>(fieldEditor_)) {
    line->setFont(font);
    QPalette pal = line->palette();
    pal.setColor(QPalette::Base, Qt::white);
    pal.setColor(QPalette::Text, color);
    line->setPalette(pal);
    line->setAlignment((f.alignment == 1 ? Qt::AlignHCenter : f.alignment == 2 ? Qt::AlignRight : Qt::AlignLeft) |
                       Qt::AlignVCenter);
    const int pad = int(std::lround(2 * ppp));
    line->setTextMargins(pad, 0, pad, 0);
  } else {
    static_cast<InlineTextEdit*>(fieldEditor_)->setLook(font, color, f.alignment, 2 * ppp);
  }
}

// Tab order: the fields in the order of the page's widgets, page after page, around the end.
void DocumentView::moveFieldFocus(bool backwards) {
  const int pages = int(pageRects_.size());
  if (!fields_->hasForm() || pages == 0) return;
  int page = focusPage_ >= 0 ? focusPage_ : currentPage_;
  int from = focusPage_ >= 0 ? focusIndex_ : (backwards ? INT_MAX : -1);
  for (int step = 0; step <= pages; ++step) {
    const auto list = fields_->get(page);
    const pdfz::FormField* best = nullptr;
    for (const pdfz::FormField& f : *list) {
      if (!isFillable(f) || f.readOnly) continue;
      if (backwards ? (f.index < from && (!best || f.index > best->index))
                    : (f.index > from && (!best || f.index < best->index)))
        best = &f;
    }
    if (best) {
      const pdfz::FormField f = *best;
      setFieldFocus(page, f.index);
      ensureVisible(page, f.rect);
      if (f.type == pdfz::FieldType::Text) startFieldEdit(page, f);
      return;
    }
    page = backwards ? (page - 1 + pages) % pages : (page + 1) % pages;
    from = backwards ? INT_MAX : -1;
  }
}

bool DocumentView::focusNextPrevChild(bool next) {
  // Tab goes from field to field while the pages have the keyboard.
  if (fields_->hasForm() && tool_ == Tool::Select && hasFocus()) {
    moveFieldFocus(!next);
    return true;
  }
  return QAbstractScrollArea::focusNextPrevChild(next);
}

bool DocumentView::pagePointAt(QPointF vp, int& page, QPointF& point, bool clamp) const {
  if (pageRects_.empty()) return false;
  const QPointF c = vp + visibleRect().topLeft();
  page = pageAt(c.y());
  const QRectF& r = pageRects_[std::size_t(page)];
  if (!clamp && !r.contains(c)) return false;
  const double ppp = pageRects_[std::size_t(page)].width() / std::max(1e-9, renderer_->document()->pageSize(page).width);
  point = (c - r.topLeft()) / ppp;
  return true;
}

QRectF DocumentView::toViewport(int page, const pdfz::RectF& r) const {
  const QRectF& pr = pageRects_[std::size_t(page)];
  const double ppp = pr.width() / std::max(1e-9, renderer_->document()->pageSize(page).width);
  return QRectF(pr.left() + r.left * ppp, pr.top() + r.top * ppp, r.width() * ppp, r.height() * ppp)
      .translated(-visibleRect().topLeft());
}

bool DocumentView::overText(QPointF vp) const {
  int page;
  QPointF pt;
  if (!pagePointAt(vp, page, pt, false)) return false;
  const auto text = text_->get(page);
  return text && text->charAt(pt.x(), pt.y(), 1.5) >= 0;
}

void DocumentView::extendSelectionTo(QPointF vp) {
  int page;
  QPointF pt;
  if (!pagePointAt(vp, page, pt, true)) return;
  const auto text = text_->get(page);
  int caret = 0;
  if (text && !text->empty()) {
    const QRectF& r = pageRects_[std::size_t(page)];
    const double contentY = vp.y() + visibleRect().top();
    if (contentY > r.bottom()) caret = text->size();  // in the gap below the page
    else if (contentY < r.top()) caret = 0;
    else caret = std::max(0, text->caretAt(pt.x(), pt.y()));
  }
  setSelection(anchor_, {page, caret});
}

void DocumentView::setSelection(TextPos anchor, TextPos focus) {
  const bool had = hasSelection();
  if (anchor == anchor_ && focus == focus_) return;
  anchor_ = anchor;
  focus_ = focus;
  viewport()->update();
  if (had != hasSelection() || hasSelection()) emit selectionChanged(hasSelection());
}

bool DocumentView::hasSelection() const { return !(anchor_ == focus_); }

void DocumentView::clearSelection() {
  selecting_ = false;
  setSelection({}, {});
}

std::pair<int, int> DocumentView::selectionOn(int page) const {
  if (!hasSelection()) return {0, 0};
  const TextPos a = std::min(anchor_, focus_);
  const TextPos b = std::max(anchor_, focus_);
  if (page < a.page || page > b.page) return {0, 0};
  return {page == a.page ? a.caret : 0, page == b.page ? b.caret : INT_MAX};
}

QString DocumentView::selectedText() const {
  if (!hasSelection()) return {};
  const TextPos a = std::min(anchor_, focus_);
  const TextPos b = std::max(anchor_, focus_);
  QString out;
  for (int p = a.page; p <= b.page; ++p) {
    const auto text = text_->get(p);
    if (!text) continue;
    const auto [s0, s1] = selectionOn(p);
    if (p > a.page && !out.isEmpty() && !out.endsWith('\n')) out += '\n';
    out += QString::fromStdString(text->textUtf8(s0, s1));
  }
  return out;
}

void DocumentView::selectAll() {
  if (pageRects_.empty()) return;
  setSelection({0, 0}, {int(pageRects_.size()) - 1, INT_MAX});
}

void DocumentView::selectRange(int page, int start, int end) {
  if (page < 0 || page >= int(pageRects_.size())) return;
  setSelection({page, start}, {page, end});
  const auto text = text_->get(page);
  if (!text) return;
  pdfz::RectF all;
  for (const pdfz::RectF& r : text->selectionRects(start, end)) all = all.united(r);
  if (!all.empty()) ensureVisible(page, all);
}

void DocumentView::ensureVisible(int page, const pdfz::RectF& rect) {
  if (page < 0 || page >= int(pageRects_.size())) return;
  const QRectF r = toViewport(page, rect);  // viewport coordinates
  const QRectF vp = QRectF(viewport()->rect()).adjusted(20, 20, -20, -20);
  if (r.top() < vp.top() || r.bottom() > vp.bottom())
    verticalScrollBar()->setValue(verticalScrollBar()->value() + int(r.center().y() - viewport()->height() / 2.0));
  if (r.left() < vp.left() || r.right() > vp.right())
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + int(r.center().x() - viewport()->width() / 2.0));
  viewport()->update();
}

void DocumentView::searchChanged() { viewport()->update(); }

void DocumentView::changeEvent(QEvent* e) {
  QAbstractScrollArea::changeEvent(e);
  if (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange) viewport()->update();
}
