// SPDX-License-Identifier: GPL-3.0-only
#include "PageRenderer.hpp"

#include "ImageUtil.hpp"

#include <QMetaObject>

#include <algorithm>
#include <cmath>

namespace {

// Cache budgets, in KiB (QCache costs).
constexpr int kTileCacheKiB = 256 * 1024;
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
  std::lock_guard lock(mutex_);
  clearQueues();
  jobDoc_ = doc_;
  jobSerial_ = serial_;
}

QImage PageRenderer::tile(const TileKey& key) const {
  const QImage* img = tiles_.object(key);
  return img ? *img : QImage();
}

QImage PageRenderer::preview(int page) const {
  const QImage* img = previews_.object(page);
  return img ? *img : QImage();
}

void PageRenderer::requestView(const std::vector<int>& previews, const std::vector<TileKey>& tiles) {
  {
    std::lock_guard lock(mutex_);
    viewJobs_.clear();
    auto add = [&](const Job& j) {
      if (current_ != j) viewJobs_.push_back(j);  // the one being drawn right now is coming anyway
    };
    for (int p : previews) add({false, p, {}});
    for (const TileKey& k : tiles) add({true, k.page, k});
  }
  wake_.notify_one();
}

void PageRenderer::requestPreview(int page) {
  if (previews_.contains(page)) return;
  {
    std::lock_guard lock(mutex_);
    const Job j{false, page, {}};
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

    {
      std::lock_guard lock(mutex_);
      current_.reset();
    }
    if (!image.isNull()) {
      QMetaObject::invokeMethod(
          this, [this, serial, job, image] { deliver(serial, job.isTile, job.page, job.key, image); },
          Qt::QueuedConnection);
    }
  }
}

void PageRenderer::deliver(quint64 serial, bool isTile, int page, TileKey key, QImage image) {
  if (serial != serial_) return;  // rendered for a document (or page order) that's gone
  if (isTile) {
    tiles_.insert(key, new QImage(image), costKiB(image));
    emit tileReady(key);
  } else {
    previews_.insert(page, new QImage(image), costKiB(image));
    emit previewReady(page);
  }
}
