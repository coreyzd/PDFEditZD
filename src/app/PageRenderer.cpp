// SPDX-License-Identifier: GPL-3.0-only
#include "PageRenderer.hpp"

#include "ActivityMonitor.hpp"
#include "ImageUtil.hpp"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace {

// Cache budgets, in KiB (QCache costs).
constexpr int kTileCacheKiB = PageRenderer::kTileBudget * PageRenderer::kTileSize * PageRenderer::kTileSize * 4 / 1024;
constexpr int kPreviewCacheKiB = 96 * 1024;

int costKiB(const QImage& img) { return std::max(1, int(img.sizeInBytes() / 1024)); }

}  // namespace

PageRenderer::PageRenderer(QObject* parent) : QObject(parent), tiles_(kTileCacheKiB), previews_(kPreviewCacheKiB) {
  qRegisterMetaType<TileKey>();
  worker_ = std::thread([this] { run(); });
}

PageRenderer::~PageRenderer() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    clearQueues();
  }
  wake_.notify_all();
  worker_.join();
}

int PageRenderer::scaleKeyFor(double devicePixelsPerPoint) {
  return std::max(1, int(std::lround(devicePixelsPerPoint * 1000.0)));
}

void PageRenderer::clearQueues() {
  viewJobs_.clear();
  lowJobs_.clear();
}

void PageRenderer::setDocument(std::shared_ptr<pdfz::Document> doc) {
  doc_ = std::move(doc);
  invalidate();
}

void PageRenderer::invalidate() {
  ++serial_;
  tiles_.clear();
  previews_.clear();
  pageEpochs_.clear();
  std::lock_guard lock(mutex_);
  clearQueues();
  jobDoc_ = doc_;
  jobSerial_ = serial_;
}

void PageRenderer::invalidatePage(int page) {
  ++pageEpochs_[page];
  std::lock_guard lock(mutex_);
  for (auto* q : {&viewJobs_, &lowJobs_})
    q->erase(std::remove_if(q->begin(), q->end(), [&](const Job& j) { return j.page == page; }), q->end());
}

const PageRenderer::Cached* PageRenderer::TileCache::get(const TileKey& key) const {
  const auto it = entries_.constFind(key);
  if (it == entries_.cend()) return nullptr;
  order_.splice(order_.begin(), order_, it->pos);  // to the front: just used
  return &it->value;
}

const PageRenderer::Cached* PageRenderer::TileCache::peek(const TileKey& key) const {
  const auto it = entries_.constFind(key);
  return it == entries_.cend() ? nullptr : &it->value;
}

void PageRenderer::TileCache::insert(const TileKey& key, Cached value, qsizetype cost) {
  if (const auto it = entries_.find(key); it != entries_.end()) {  // a newer rendering of it
    total_ -= it->cost;
    order_.erase(it->pos);
    entries_.erase(it);
  }
  order_.push_front(key);
  entries_.insert(key, Entry{std::move(value), cost, order_.begin()});
  total_ += cost;
  while (total_ > max_ && order_.size() > 1) {
    // The least recently used tile at another zoom; failing that, at this one (never the new
    // one, at the front).
    auto victim = std::prev(order_.end());
    for (auto it = victim;; --it) {
      if (it->scaleKey != key.scaleKey) {
        victim = it;
        break;
      }
      if (it == std::next(order_.begin())) break;
    }
    const auto e = entries_.find(*victim);
    total_ -= e->cost;
    entries_.erase(e);
    order_.erase(victim);
  }
}

void PageRenderer::TileCache::clear() {
  entries_.clear();
  order_.clear();
  total_ = 0;
}

std::vector<TileKey> PageRenderer::TileCache::keys() const { return {order_.begin(), order_.end()}; }

void PageRenderer::TileCache::remapPages(const std::function<int(int)>& newPage) {
  QHash<TileKey, Entry> entries;
  std::list<TileKey> order;
  total_ = 0;
  for (const TileKey& k : order_) {
    const int to = newPage(k.page);
    if (to < 0) continue;
    Entry e = entries_.take(k);
    TileKey nk = k;
    nk.page = to;
    order.push_back(nk);
    e.pos = std::prev(order.end());
    total_ += e.cost;
    entries.insert(nk, std::move(e));
  }
  entries_ = std::move(entries);
  order_ = std::move(order);
}

void PageRenderer::rearrange(const std::vector<int>& from) {
  // Work asked for, and being done, is for the old page numbers: dropped (asked for again).
  ++serial_;
  {
    std::lock_guard lock(mutex_);
    clearQueues();
    jobSerial_ = serial_;
  }
  std::vector<int> to;  // old index -> new
  for (int i = 0; i < int(from.size()); ++i)
    if (from[std::size_t(i)] >= 0) {
      if (int(to.size()) <= from[std::size_t(i)]) to.resize(std::size_t(from[std::size_t(i)]) + 1, -1);
      to[std::size_t(from[std::size_t(i)])] = i;
    }
  auto newPage = [&](int old) { return old >= 0 && old < int(to.size()) ? to[std::size_t(old)] : -1; };
  tiles_.remapPages(newPage);
  std::vector<std::pair<int, Cached*>> previews;
  for (int old : previews_.keys())
    if (Cached* c = previews_.take(old)) previews.emplace_back(newPage(old), c);
  for (auto& [page, c] : previews) {
    if (page < 0) delete c;
    else previews_.insert(page, c, costKiB(c->image));
  }
  QHash<int, quint64> epochs;
  for (auto it = pageEpochs_.cbegin(); it != pageEpochs_.cend(); ++it)
    if (const int page = newPage(it.key()); page >= 0) epochs.insert(page, it.value());
  pageEpochs_ = std::move(epochs);
}

void PageRenderer::pageTurned(int page, int degrees) {
  tiles_.remapPages([page](int p) { return p == page ? -1 : p; });
  const quint64 was = epochOf(page);
  ++pageEpochs_[page];  // drawn again; the turned preview shows meanwhile
  if (Cached* c = previews_.take(page)) {
    c->image = c->image.transformed(QTransform().rotate(((degrees % 360) + 360) % 360));
    c->epoch = was;
    previews_.insert(page, c, costKiB(c->image));
  }
  std::lock_guard lock(mutex_);
  for (auto* q : {&viewJobs_, &lowJobs_})
    q->erase(std::remove_if(q->begin(), q->end(), [&](const Job& j) { return j.page == page; }), q->end());
}

QImage PageRenderer::tile(const TileKey& key, bool* fresh) const {
  const Cached* c = tiles_.get(key);
  if (fresh) *fresh = c && c->epoch == epochOf(key.page);
  return c ? c->image : QImage();
}

QImage PageRenderer::preview(int page, bool* fresh) const {
  const Cached* c = previews_.object(page);
  if (fresh) *fresh = c && c->epoch == epochOf(page);
  return c ? c->image : QImage();
}

std::vector<std::pair<TileKey, QImage>> PageRenderer::otherTiles(int page, int scaleKey) const {
  std::vector<std::pair<TileKey, QImage>> out;
  for (const TileKey& k : tiles_.keys())
    if (k.page == page && k.scaleKey != scaleKey)
      if (const Cached* c = tiles_.peek(k)) out.emplace_back(k, c->image);  // not marked used
  return out;
}

void PageRenderer::requestView(const std::vector<int>& previews, const std::vector<TileKey>& tiles) {
  {
    std::lock_guard lock(mutex_);
    viewJobs_.clear();
    auto add = [&](const Job& j) {
      if (current_ != j) viewJobs_.push_back(j);  // the one being drawn right now is coming anyway
    };
    for (int p : previews) add({false, p, {}, epochOf(p)});
    for (const TileKey& k : tiles) add({true, k.page, k, epochOf(k.page)});
  }
  wake_.notify_one();
}

void PageRenderer::requestPreview(int page) {
  bool fresh = false;
  if (!preview(page, &fresh).isNull() && fresh) return;
  {
    std::lock_guard lock(mutex_);
    const Job j{false, page, {}, epochOf(page)};
    if (current_ == j) return;
    auto it = std::find(lowJobs_.begin(), lowJobs_.end(), j);
    if (it != lowJobs_.end()) lowJobs_.erase(it);
    lowJobs_.push_front(j);  // newest first: the rows just scrolled into view
  }
  wake_.notify_one();
}

void PageRenderer::run() {
  for (;;) {
    Job job;
    std::shared_ptr<pdfz::Document> doc;
    quint64 serial = 0;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return stop_ || !viewJobs_.empty() || !lowJobs_.empty(); });
      if (stop_) return;
      auto& queue = !viewJobs_.empty() ? viewJobs_ : lowJobs_;
      job = queue.front();
      queue.pop_front();
      current_ = job;
      doc = jobDoc_;
      serial = jobSerial_;
    }

    QImage image;
    QElapsedTimer took;
    took.start();
    if (doc && job.page >= 0 && job.page < doc->pageCount()) {
      if (job.isTile) {
        const double scale = job.key.scale();
        const pdfz::SizeF s = doc->pageSize(job.page);
        const int pageW = int(std::ceil(s.width * scale));
        const int pageH = int(std::ceil(s.height * scale));
        const int x = job.key.tx * kTileSize;
        const int y = job.key.ty * kTileSize;
        const int w = std::min(kTileSize, pageW - x);
        const int h = std::min(kTileSize, pageH - y);
        if (w > 0 && h > 0) image = toQImage(doc->render(job.page, scale, {x, y, w, h}));
      } else {
        image = toQImage(doc->renderFit(job.page, kPreviewBox, kPreviewBox));
      }
    }

    (job.isTile ? ActivityMonitor::tilesRendered : ActivityMonitor::previewsRendered)++;
    ActivityMonitor::renderMicroseconds += long(took.nsecsElapsed() / 1000);
    {
      std::lock_guard lock(mutex_);
      current_.reset();
    }
    if (!image.isNull()) {
      QMetaObject::invokeMethod(
          this, [this, serial, job, image] { deliver(serial, job, image); },
          Qt::QueuedConnection);
    }
  }
}

void PageRenderer::deliver(quint64 serial, const Job& job, QImage image) {
  if (serial != serial_) return;  // rendered for a document (or page order) that's gone
  if (job.epoch != epochOf(job.page)) return;  // the page has changed since
  if (job.isTile) {
    tiles_.insert(job.key, Cached{image, job.epoch}, costKiB(image));
    emit tileReady(job.key);
  } else {
    previews_.insert(job.page, new Cached{image, job.epoch}, costKiB(image));
    emit previewReady(job.page);
  }
}
