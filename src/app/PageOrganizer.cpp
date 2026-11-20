// SPDX-License-Identifier: GPL-3.0-only
#include "PageOrganizer.hpp"

#include "PageRenderer.hpp"
#include "ToolIcons.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QToolBar>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kMargin = 24;      // round the grid
constexpr int kColumnRoom = 84;  // a column is this much wider than a thumbnail: the gaps
constexpr int kGapHit = 10;      // how near a gap's line the mouse inserts there
constexpr int kButton = 26;      // the hover buttons
constexpr int kTopPad = 12;      // above a thumbnail, in its row
const QColor kInsertColor(0x2D, 0x7F, 0xF9);

}  // namespace

PageGrid::PageGrid(PageRenderer* renderer, QWidget* parent) : QAbstractScrollArea(parent), renderer_(renderer) {
  setFrameShape(QFrame::NoFrame);
  setFocusPolicy(Qt::StrongFocus);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  viewport()->setMouseTracking(true);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  verticalScrollBar()->setSingleStep(40);
  box_ = std::clamp(QSettings().value("organize/thumbnailSize", 170).toInt(), kMinSize, kMaxSize);
  // White icons on the dark hover buttons.
  QPalette white = palette();
  for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
    white.setColor(group, QPalette::ButtonText, Qt::white);
  controlIcons_[0] = rotateIcon(false, white);
  controlIcons_[1] = rotateIcon(true, white);
  controlIcons_[2] = trashIcon(white);
  scrollTimer_.setInterval(30);
  connect(&scrollTimer_, &QTimer::timeout, this, &PageGrid::autoScrollStep);
  connect(renderer_, &PageRenderer::previewReady, this, [this] {
    if (isVisible()) viewport()->update();
  });
}

int PageGrid::pageCount() const {
  const auto& doc = renderer_->document();
  return doc ? doc->pageCount() : 0;
}

void PageGrid::documentChanged() {
  const int n = pageCount();
  selected_.resize(std::size_t(n), 0);
  current_ = std::clamp(current_, 0, std::max(0, n - 1));
  anchor_ = std::clamp(anchor_, 0, std::max(0, n - 1));
  hoverPage_ = hoverControl_ = hoverInsert_ = -1;
  relayout();
  viewport()->update();
}

void PageGrid::pagesChanged() { viewport()->update(); }

void PageGrid::setThumbnailSize(int px) {
  px = std::clamp(px, kMinSize, kMaxSize);
  if (px == box_) return;
  const int keep = current_;
  box_ = px;
  QSettings().setValue("organize/thumbnailSize", px);
  relayout();
  ensureVisible(keep);
  viewport()->update();
  emit thumbnailSizeChanged(px);
}

void PageGrid::relayout() {
  const int width = viewport()->width() - 2 * kMargin;
  cols_ = std::max(1, width / (box_ + kColumnRoom));
  colW_ = std::max(double(box_ + kColumnRoom), double(width) / cols_);
  labelH_ = fontMetrics().height() + 4;
  rowH_ = kTopPad + box_ + 8 + labelH_ + 18;
  const int rows = (pageCount() + cols_ - 1) / cols_;
  contentH_ = 2 * kMargin + rows * rowH_;
  verticalScrollBar()->setRange(0, std::max(0, contentH_ - viewport()->height()));
  verticalScrollBar()->setPageStep(viewport()->height());
}

QRectF PageGrid::boxRect(int page) const {
  const double x = kMargin + colOf(page) * colW_ + (colW_ - box_) / 2;
  const double y = kMargin + rowOf(page) * rowH_ + kTopPad - verticalScrollBar()->value();
  return {x, y, double(box_), double(box_)};
}

QRectF PageGrid::imageRect(int page) const {
  const QRectF b = boxRect(page);
  const auto& doc = renderer_->document();
  pdfz::SizeF s = doc ? doc->pageSize(page) : pdfz::SizeF{612, 792};
  if (s.width <= 0 || s.height <= 0) s = {612, 792};
  const double f = std::min(b.width() / s.width, b.height() / s.height);
  const double w = std::round(s.width * f), h = std::round(s.height * f);
  return {std::round(b.center().x() - w / 2), std::round(b.center().y() - h / 2), w, h};
}

QRectF PageGrid::cellHighlight(int page) const {
  const QRectF b = boxRect(page);
  return {b.left() - 10, b.top() - 8, b.width() + 20, b.height() + 8 + 6 + labelH_ + 4};
}

QRectF PageGrid::controlsRect(int page) const {
  const QRectF img = imageRect(page);
  return {img.right() + 4, img.top() + 2, double(kButton), 3.0 * kButton + 4};
}

QRectF PageGrid::controlRect(int page, int which) const {
  const QRectF c = controlsRect(page);
  return {c.left(), c.top() + 2 + which * kButton, double(kButton), double(kButton)};
}

int PageGrid::pageAt(QPointF pos) const {
  const int n = pageCount();
  if (n == 0) return -1;
  const double y = pos.y() + verticalScrollBar()->value() - kMargin;
  const double x = pos.x() - kMargin;
  if (y < 0 || x < 0) return -1;
  const int row = int(y / rowH_), col = int(x / colW_);
  if (col >= cols_) return -1;
  const int page = row * cols_ + col;
  if (page >= n) return -1;
  if (cellHighlight(page).contains(pos)) return page;
  if (page == hoverPage_ && controlsRect(page).contains(pos)) return page;  // its buttons
  return -1;
}

int PageGrid::controlAt(int page, QPointF pos) const {
  if (page < 0) return -1;
  for (int i = 0; i < 3; ++i)
    if (controlRect(page, i).contains(pos)) return i;
  return -1;
}

QRectF PageGrid::gapLine(int row, int col) const {
  const double x = kMargin + col * colW_;
  const double top = kMargin + row * rowH_ + kTopPad - verticalScrollBar()->value();
  return {x - 1.5, top, 3, double(box_)};
}

int PageGrid::insertionAt(QPointF pos, QRectF* line) const {
  const int n = pageCount();
  if (n == 0) return -1;
  const double y = pos.y() + verticalScrollBar()->value() - kMargin;
  if (y < 0) return -1;
  const int row = int(y / rowH_);
  const double inRow = y - row * rowH_;
  if (inRow < kTopPad || inRow > kTopPad + box_) return -1;  // beside the thumbnails only
  const int first = row * cols_;
  if (first >= n) return -1;
  const int col = int(std::lround((pos.x() - kMargin) / colW_));
  if (col < 0 || col > cols_ || first + col > n) return -1;
  if (std::abs(pos.x() - (kMargin + col * colW_)) > kGapHit) return -1;
  if (line) *line = gapLine(row, col);
  return first + col;
}

int PageGrid::dropIndexAt(QPointF pos, QRectF* line) const {
  const int n = pageCount();
  const double y = pos.y() + verticalScrollBar()->value() - kMargin;
  const int rows = (n + cols_ - 1) / cols_;
  const int row = std::clamp(int(std::floor(y / rowH_)), 0, std::max(0, rows - 1));
  const int first = row * cols_;
  const int inRow = std::min(cols_, n - first);
  int col = int(std::lround((pos.x() - kMargin) / colW_));
  col = std::clamp(col, 0, inRow);
  if (y >= rows * rowH_) col = inRow;  // below everything: at the end
  if (line) *line = gapLine(row, col);
  return first + col;
}

std::vector<int> PageGrid::selectedPages() const {
  std::vector<int> out;
  for (int i = 0; i < int(selected_.size()); ++i)
    if (selected_[std::size_t(i)]) out.push_back(i);
  if (out.empty() && current_ < pageCount()) out.push_back(current_);
  return out;
}

void PageGrid::selectOnly(int page) {
  std::fill(selected_.begin(), selected_.end(), 0);
  if (page >= 0 && page < int(selected_.size())) selected_[std::size_t(page)] = 1;
  current_ = anchor_ = std::max(0, page);
}

void PageGrid::setCurrentPage(int page) {
  if (page < 0 || page >= pageCount()) return;
  selectOnly(page);
  ensureVisible(page);
  viewport()->update();
  emit selectionChanged();
}

void PageGrid::selectPages(int first, int count) {
  const int n = pageCount();
  std::fill(selected_.begin(), selected_.end(), 0);
  for (int p = first; p < first + count; ++p)
    if (p >= 0 && p < n) selected_[std::size_t(p)] = 1;
  current_ = anchor_ = std::clamp(first, 0, std::max(0, n - 1));
  ensureVisible(current_);
  viewport()->update();
  emit selectionChanged();
}

void PageGrid::selectPageList(const std::vector<int>& pages) {
  std::fill(selected_.begin(), selected_.end(), 0);
  for (int p : pages)
    if (p >= 0 && p < int(selected_.size())) selected_[std::size_t(p)] = 1;
  if (!pages.empty()) current_ = anchor_ = std::clamp(pages.front(), 0, std::max(0, pageCount() - 1));
  viewport()->update();
  emit selectionChanged();
}

void PageGrid::ensureVisible(int page) {
  if (page < 0 || page >= pageCount()) return;
  const int top = kMargin + rowOf(page) * rowH_;
  QScrollBar* sb = verticalScrollBar();
  if (top < sb->value()) sb->setValue(top);
  else if (top + rowH_ > sb->value() + viewport()->height()) sb->setValue(top + rowH_ - viewport()->height());
}

void PageGrid::resizeEvent(QResizeEvent* e) {
  QAbstractScrollArea::resizeEvent(e);
  relayout();
}

void PageGrid::scrollContentsBy(int, int) {
  if (!dragging_) updateHover(viewport()->mapFromGlobal(QCursor::pos()));
  viewport()->update();
}

void PageGrid::paintEvent(QPaintEvent*) {
  QPainter p(viewport());
  p.fillRect(viewport()->rect(), palette().color(QPalette::Window).darker(125));
  const int n = pageCount();
  if (n == 0) return;
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  const QColor accent = palette().color(QPalette::Highlight);
  const QColor text = palette().color(QPalette::WindowText);

  const int scroll = verticalScrollBar()->value();
  const int firstRow = std::max(0, (scroll - kMargin) / rowH_);
  const int lastRow = (scroll + viewport()->height() - kMargin) / rowH_;
  for (int page = firstRow * cols_; page < std::min(n, (lastRow + 1) * cols_); ++page) {
    const bool sel = selected_[std::size_t(page)] != 0;
    const bool dragged = dragging_ && std::find(dragPages_.begin(), dragPages_.end(), page) != dragPages_.end();
    if (sel || page == hoverPage_) {
      QColor bg = accent;
      bg.setAlpha(sel ? 90 : 40);
      p.setPen(sel ? QPen(accent, 1.5) : Qt::NoPen);
      p.setBrush(bg);
      p.drawRoundedRect(cellHighlight(page), 4, 4);
    }
    const QRectF img = imageRect(page);
    p.setOpacity(dragged ? 0.4 : 1.0);
    p.fillRect(img.translated(2, 2), QColor(0, 0, 0, 60));  // shadow
    p.fillRect(img, Qt::white);
    bool fresh = false;
    const QImage preview = renderer_->preview(page, &fresh);
    if (!preview.isNull()) p.drawImage(img, preview);
    if (!fresh) renderer_->requestPreview(page);  // only for the pages in view
    p.setOpacity(1.0);
    QFont f = font();
    f.setBold(sel);
    p.setFont(f);
    p.setPen(text);
    const QRectF b = boxRect(page);
    p.drawText(QRectF(b.left() - 10, b.bottom() + 6, b.width() + 20, labelH_), Qt::AlignHCenter | Qt::AlignTop,
               QString::number(page + 1));
  }

  // The hovered page's buttons.
  if (hoverPage_ >= 0 && hoverPage_ < n && !dragging_) {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(40, 40, 40, 225));
    p.drawRoundedRect(controlsRect(hoverPage_), 5, 5);
    for (int i = 0; i < 3; ++i) {
      const QRectF r = controlRect(hoverPage_, i);
      if (i == hoverControl_) {
        p.setBrush(QColor(255, 255, 255, 50));
        p.drawRoundedRect(r.adjusted(2, 1, -2, -1), 4, 4);
      }
      controlIcons_[i].paint(&p, r.adjusted(4, 4, -4, -4).toRect());
    }
  }

  // The line that inserts pages, or where dragged pages go.
  const QRectF line = dragging_ ? dropLine_ : hoverLine_;
  if ((dragging_ && dropBefore_ >= 0) || (!dragging_ && hoverInsert_ >= 0)) {
    p.setPen(Qt::NoPen);
    p.setBrush(kInsertColor);
    p.drawRoundedRect(line, 1.5, 1.5);
    if (!dragging_) {  // a "+" at its top, for what a click does
      const QPointF c(line.center().x(), line.top() - 9);
      p.drawEllipse(c, 8, 8);
      p.setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(c + QPointF(-4, 0), c + QPointF(4, 0));
      p.drawLine(c + QPointF(0, -4), c + QPointF(0, 4));
    }
  }

  // What's being dragged, under the mouse.
  if (dragging_ && pressPage_ >= 0 && pressPage_ < n) {
    const QRectF img = imageRect(pressPage_);
    const double k = 0.75;
    const QRectF ghost(dragPos_.x() - img.width() * k / 2, dragPos_.y() - img.height() * k / 2, img.width() * k,
                       img.height() * k);
    p.setOpacity(0.75);
    p.fillRect(ghost, Qt::white);
    const QImage preview = renderer_->preview(pressPage_);
    if (!preview.isNull()) p.drawImage(ghost, preview);
    p.setOpacity(1.0);
    p.setPen(QPen(accent, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(ghost);
    if (dragPages_.size() > 1) {  // how many
      const QString count = QString::number(dragPages_.size());
      const QRectF badge(ghost.right() - 12, ghost.top() - 10, std::max(22.0, fontMetrics().horizontalAdvance(count) + 12.0), 22);
      p.setPen(Qt::NoPen);
      p.setBrush(accent);
      p.drawRoundedRect(badge, 11, 11);
      p.setPen(palette().color(QPalette::HighlightedText));
      p.drawText(badge, Qt::AlignCenter, count);
    }
  }
}

void PageGrid::setHover(int page, int control, int insert, const QRectF& line) {
  if (page == hoverPage_ && control == hoverControl_ && insert == hoverInsert_ && line == hoverLine_) return;
  hoverPage_ = page;
  hoverControl_ = control;
  hoverInsert_ = insert;
  hoverLine_ = line;
  viewport()->setCursor(insert >= 0 || control >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
  viewport()->update();
}

void PageGrid::updateHover(QPointF pos) {
  if (!viewport()->rect().contains(pos.toPoint())) {
    setHover(-1, -1, -1, {});
    return;
  }
  QRectF line;
  const int insert = insertionAt(pos, &line);
  const int page = insert >= 0 ? -1 : pageAt(pos);
  setHover(page, controlAt(page, pos), insert, line);
}

void PageGrid::mousePressEvent(QMouseEvent* e) {
  setFocus();
  const QPointF pos = e->position();
  if (e->button() != Qt::LeftButton) return QAbstractScrollArea::mousePressEvent(e);
  updateHover(pos);
  if (hoverInsert_ >= 0) {  // a click on the line between pages
    emit insertRequested(hoverInsert_, viewport()->mapToGlobal(pos.toPoint()));
    return;
  }
  const int page = pageAt(pos);
  if (page >= 0 && hoverControl_ >= 0) {  // a hover button: for that page
    const std::vector<int> one{page};
    const int which = hoverControl_;
    if (which == 2) emit deleteRequested(one);
    else emit rotateRequested(one, which == 0 ? -90 : 90);
    return;
  }
  if (page < 0) {  // the space between: nothing selected but the current page stays
    std::fill(selected_.begin(), selected_.end(), 0);
    viewport()->update();
    emit selectionChanged();
    return;
  }
  pressPage_ = page;
  pressPos_ = pos.toPoint();
  pressWasSelected_ = selected_[std::size_t(page)] != 0;
  const auto mods = e->modifiers();
  if (mods & Qt::ShiftModifier) {
    if (!(mods & Qt::ControlModifier)) std::fill(selected_.begin(), selected_.end(), 0);
    for (int p = std::min(anchor_, page); p <= std::max(anchor_, page); ++p) selected_[std::size_t(p)] = 1;
    current_ = page;
  } else if (mods & Qt::ControlModifier) {
    selected_[std::size_t(page)] = !selected_[std::size_t(page)];
    current_ = anchor_ = page;
  } else if (!pressWasSelected_) {  // (a selected one stays with the others, to be dragged)
    selectOnly(page);
  } else {
    current_ = page;
  }
  viewport()->update();
  emit selectionChanged();
}

void PageGrid::mouseMoveEvent(QMouseEvent* e) {
  const QPointF pos = e->position();
  if (pressPage_ >= 0 && (e->buttons() & Qt::LeftButton)) {
    if (!dragging_ && (pos.toPoint() - pressPos_).manhattanLength() >= QApplication::startDragDistance()) {
      dragging_ = true;
      if (!selected_[std::size_t(pressPage_)]) selectOnly(pressPage_);
      dragPages_ = selectedPages();
      setHover(-1, -1, -1, {});
      viewport()->setCursor(Qt::ClosedHandCursor);
    }
    if (dragging_) {
      dragPos_ = pos.toPoint();
      dropBefore_ = dropIndexAt(pos, &dropLine_);
      const int y = dragPos_.y();
      const bool nearEdge = y < 40 || y > viewport()->height() - 40;
      if (nearEdge && !scrollTimer_.isActive()) scrollTimer_.start();
      if (!nearEdge) scrollTimer_.stop();
      viewport()->update();
      return;
    }
  }
  updateHover(pos);
}

void PageGrid::autoScrollStep() {
  if (!dragging_ || !(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
    scrollTimer_.stop();
    return;
  }
  const int y = dragPos_.y(), h = viewport()->height();
  const int step = y < 40 ? -std::max(4, (40 - y) / 2) : (y > h - 40 ? std::max(4, (y - (h - 40)) / 2) : 0);
  if (step == 0) return;
  verticalScrollBar()->setValue(verticalScrollBar()->value() + step);
  dropBefore_ = dropIndexAt(dragPos_, &dropLine_);
  viewport()->update();
}

void PageGrid::endDrag() {
  dragging_ = false;
  pressPage_ = -1;
  dragPages_.clear();
  dropBefore_ = -1;
  scrollTimer_.stop();
  viewport()->setCursor(Qt::ArrowCursor);
  viewport()->update();
}

void PageGrid::mouseReleaseEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return QAbstractScrollArea::mouseReleaseEvent(e);
  if (dragging_) {
    const std::vector<int> pages = dragPages_;
    const int before = dropIndexAt(e->position(), nullptr);
    endDrag();
    emit pagesMoved(pages, before);
    updateHover(e->position());
    return;
  }
  // A plain click on a page that was selected with others: now it's selected alone.
  if (pressPage_ >= 0 && pressWasSelected_ && !(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
    selectOnly(pressPage_);
    viewport()->update();
    emit selectionChanged();
  }
  pressPage_ = -1;
}

void PageGrid::mouseDoubleClickEvent(QMouseEvent* e) {
  const int page = pageAt(e->position());
  if (e->button() == Qt::LeftButton && page >= 0 && controlAt(page, e->position()) < 0 &&
      insertionAt(e->position(), nullptr) < 0)
    emit pageOpened(page);
}

void PageGrid::wheelEvent(QWheelEvent* e) {
  if (e->modifiers() & Qt::ControlModifier) {  // bigger or smaller thumbnails
    const int steps = e->angleDelta().y() / 120;
    if (steps != 0) setThumbnailSize(box_ + steps * 20);
    e->accept();
    return;
  }
  QAbstractScrollArea::wheelEvent(e);
}

void PageGrid::keyPressEvent(QKeyEvent* e) {
  const int n = pageCount();
  if (n == 0) return QAbstractScrollArea::keyPressEvent(e);
  if (e->key() == Qt::Key_Escape && dragging_) {
    endDrag();
    return;
  }
  if (e->matches(QKeySequence::SelectAll)) {
    std::fill(selected_.begin(), selected_.end(), 1);
    viewport()->update();
    emit selectionChanged();
    return;
  }
  if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
    emit deleteRequested(selectedPages());
    return;
  }
  if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
    emit pageOpened(current_);
    return;
  }
  int to = current_;
  switch (e->key()) {
    case Qt::Key_Left: to = current_ - 1; break;
    case Qt::Key_Right: to = current_ + 1; break;
    case Qt::Key_Up: to = current_ - cols_; break;
    case Qt::Key_Down: to = current_ + cols_; break;
    case Qt::Key_Home: to = 0; break;
    case Qt::Key_End: to = n - 1; break;
    case Qt::Key_PageUp: to = current_ - cols_ * std::max(1, viewport()->height() / rowH_); break;
    case Qt::Key_PageDown: to = current_ + cols_ * std::max(1, viewport()->height() / rowH_); break;
    default: return QAbstractScrollArea::keyPressEvent(e);
  }
  to = std::clamp(to, 0, n - 1);
  if (e->modifiers() & Qt::ShiftModifier) {
    std::fill(selected_.begin(), selected_.end(), 0);
    for (int p = std::min(anchor_, to); p <= std::max(anchor_, to); ++p) selected_[std::size_t(p)] = 1;
    current_ = to;
  } else {
    selectOnly(to);
  }
  ensureVisible(to);
  viewport()->update();
  emit selectionChanged();
}

void PageGrid::contextMenuEvent(QContextMenuEvent* e) {
  const int page = pageAt(e->pos());
  if (page >= 0 && !selected_[std::size_t(page)]) {
    selectOnly(page);
    viewport()->update();
    emit selectionChanged();
  }
  emit contextMenuRequested(e->globalPos());
}

bool PageGrid::viewportEvent(QEvent* e) {
  if (e->type() == QEvent::Leave && !dragging_) setHover(-1, -1, -1, {});
  if (e->type() == QEvent::ToolTip) {
    auto* h = static_cast<QHelpEvent*>(e);
    static const char* const tips[3] = {QT_TR_NOOP("Rotate counterclockwise"), QT_TR_NOOP("Rotate clockwise"),
                                        QT_TR_NOOP("Delete page")};
    if (hoverControl_ >= 0) QToolTip::showText(h->globalPos(), tr(tips[hoverControl_]), viewport());
    else if (hoverInsert_ >= 0) QToolTip::showText(h->globalPos(), tr("Insert pages here"), viewport());
    else QToolTip::hideText();
    return true;
  }
  return QAbstractScrollArea::viewportEvent(e);
}

// ---------------------------------------------------------------------------------------------

PageOrganizer::PageOrganizer(PageRenderer* renderer, QWidget* parent) : QWidget(parent) {
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);
  grid_ = new PageGrid(renderer, this);
  lay->addWidget(grid_, 1);
  newBar();
}

QToolBar* PageOrganizer::newBar() {
  delete bar_;
  size_ = nullptr;
  bar_ = new QToolBar(this);
  bar_->setIconSize(QSize(20, 20));
  auto* title = new QLabel(tr("Organize Pages"), bar_);
  QFont f = title->font();
  f.setBold(true);
  title->setFont(f);
  title->setContentsMargins(8, 0, 16, 0);
  bar_->addWidget(title);
  static_cast<QVBoxLayout*>(layout())->insertWidget(0, bar_);
  return bar_;
}

void PageOrganizer::finishBar(QAction* close) {
  auto* spacer = new QWidget(bar_);
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  bar_->addWidget(spacer);
  // The thumbnails' size (Ctrl+wheel too).
  bar_->addAction(sizeIcon(false, palette()), tr("Smaller Thumbnails"), this,
                  [this] { grid_->setThumbnailSize(grid_->thumbnailSize() - 20); });
  size_ = new QSlider(Qt::Horizontal, bar_);
  size_->setRange(PageGrid::kMinSize, PageGrid::kMaxSize);
  size_->setValue(grid_->thumbnailSize());
  size_->setFixedWidth(140);
  size_->setFocusPolicy(Qt::NoFocus);
  size_->setToolTip(tr("Thumbnail size (Ctrl+wheel)"));
  connect(size_, &QSlider::valueChanged, grid_, &PageGrid::setThumbnailSize);
  connect(grid_, &PageGrid::thumbnailSizeChanged, size_, [this](int px) {
    const QSignalBlocker block(size_);
    size_->setValue(px);
  });
  bar_->addWidget(size_);
  bar_->addAction(sizeIcon(true, palette()), tr("Larger Thumbnails"), this,
                  [this] { grid_->setThumbnailSize(grid_->thumbnailSize() + 20); });
  bar_->addSeparator();
  bar_->addAction(close);
  if (auto* w = bar_->widgetForAction(close)) w->setFocusPolicy(Qt::NoFocus);
}
