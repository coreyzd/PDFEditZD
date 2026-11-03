// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.hpp"

#include "PageRenderer.hpp"
#include "Printing.hpp"
#include "ThumbnailPanel.hpp"

#include "core/document.hpp"
#include "core/engine.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace {

constexpr int kMaxRecent = 10;
const char* const kPdfFilter = QT_TRANSLATE_NOOP("MainWindow", "PDF documents (*.pdf);;All files (*)");

std::filesystem::path toFsPath(const QString& s) { return std::filesystem::path(s.toStdU16String()); }

QIcon themeIcon(const char* name, QStyle::StandardPixmap fallback = QStyle::SP_CustomBase) {
  if (fallback == QStyle::SP_CustomBase) return QIcon::fromTheme(QLatin1String(name));
  return QIcon::fromTheme(QLatin1String(name), QApplication::style()->standardIcon(fallback));
}

QString lastDir() {
  QSettings s;
  const QString d = s.value("lastDir").toString();
  return d.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : d;
}

void rememberDir(const QString& file) { QSettings().setValue("lastDir", QFileInfo(file).absolutePath()); }

// "1-3, 5" for showing which pages an action is about to change.
QString describePages(const std::vector<int>& pages) {
  QStringList parts;
  for (std::size_t i = 0; i < pages.size();) {
    std::size_t j = i;
    while (j + 1 < pages.size() && pages[j + 1] == pages[j] + 1) ++j;
    parts << (i == j ? QString::number(pages[i] + 1) : QStringLiteral("%1-%2").arg(pages[i] + 1).arg(pages[j] + 1));
    i = j + 1;
  }
  return parts.join(QStringLiteral(", "));
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setAcceptDrops(true);
  renderer_ = new PageRenderer(this);
  view_ = new DocumentView(renderer_, this);
  setCentralWidget(view_);

  thumbs_ = new ThumbnailPanel(renderer_, this);
  thumbsDock_ = new QDockWidget(tr("Pages"), this);
  thumbsDock_->setObjectName("pagesDock");
  thumbsDock_->setWidget(thumbs_);
  thumbsDock_->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
  addDockWidget(Qt::LeftDockWidgetArea, thumbsDock_);

  createActions();
  createMenus();
  createToolBar();

  pageSizeLabel_ = new QLabel(this);
  statusBar()->addPermanentWidget(pageSizeLabel_);

  connect(view_, &DocumentView::currentPageChanged, this, &MainWindow::onCurrentPageChanged);
  connect(view_, &DocumentView::zoomChanged, this, &MainWindow::onZoomChanged);
  connect(thumbs_, &ThumbnailPanel::pageActivated, view_, &DocumentView::goToPage);
  connect(thumbs_, &ThumbnailPanel::contextMenuForPages, this,
          [this](const QPoint& pos) { pagesMenu_->exec(pos); });

  QSettings s;
  if (!restoreGeometry(s.value("window/geometry").toByteArray())) resize(1100, 850);
  restoreState(s.value("window/state").toByteArray());

  setDocument(nullptr, {});
  view_->setFocus();
}

MainWindow::~MainWindow() {
  // Views first: they hold on to the renderer's signals.
  delete thumbsDock_;
  delete view_;
}

void MainWindow::createActions() {
  openAct_ = new QAction(themeIcon("document-open", QStyle::SP_DialogOpenButton), tr("&Open…"), this);
  openAct_->setShortcut(QKeySequence::Open);
  connect(openAct_, &QAction::triggered, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, tr("Open PDF"), lastDir(), tr(kPdfFilter));
    if (!path.isEmpty()) openFile(path);
  });

  saveAct_ = new QAction(themeIcon("document-save", QStyle::SP_DialogSaveButton), tr("&Save"), this);
  saveAct_->setShortcut(QKeySequence::Save);
  connect(saveAct_, &QAction::triggered, this, &MainWindow::save);

  saveAsAct_ = new QAction(themeIcon("document-save-as"), tr("Save &As…"), this);
  saveAsAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
  connect(saveAsAct_, &QAction::triggered, this, &MainWindow::saveAs);

  printAct_ = new QAction(themeIcon("document-print"), tr("&Print…"), this);
  printAct_->setShortcut(QKeySequence::Print);
  connect(printAct_, &QAction::triggered, this, &MainWindow::print);

  closeAct_ = new QAction(tr("&Close"), this);
  closeAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
  connect(closeAct_, &QAction::triggered, this, &MainWindow::closeDocument);

  quitAct_ = new QAction(tr("E&xit"), this);
  quitAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
  connect(quitAct_, &QAction::triggered, this, &QWidget::close);

  deletePagesAct_ = new QAction(themeIcon("edit-delete"), tr("&Delete Pages…"), this);
  deletePagesAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
  connect(deletePagesAct_, &QAction::triggered, this, &MainWindow::deletePages);

  insertBlankAct_ = new QAction(tr("Insert &Blank Page"), this);
  connect(insertBlankAct_, &QAction::triggered, this, &MainWindow::insertBlankPage);

  insertFileAct_ = new QAction(tr("Insert Pages from &File…"), this);
  connect(insertFileAct_, &QAction::triggered, this, &MainWindow::insertPagesFromFile);

  rotateCwAct_ = new QAction(themeIcon("object-rotate-right"), tr("Rotate &Clockwise"), this);
  rotateCwAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
  connect(rotateCwAct_, &QAction::triggered, this, [this] { rotatePages(90); });

  rotateCcwAct_ = new QAction(themeIcon("object-rotate-left"), tr("Rotate Counterclock&wise"), this);
  rotateCcwAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
  connect(rotateCcwAct_, &QAction::triggered, this, [this] { rotatePages(-90); });

  zoomInAct_ = new QAction(themeIcon("zoom-in"), tr("Zoom &In"), this);
  zoomInAct_->setShortcuts({QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal)});
  connect(zoomInAct_, &QAction::triggered, view_, &DocumentView::zoomIn);

  zoomOutAct_ = new QAction(themeIcon("zoom-out"), tr("Zoom &Out"), this);
  zoomOutAct_->setShortcut(QKeySequence::ZoomOut);
  connect(zoomOutAct_, &QAction::triggered, view_, &DocumentView::zoomOut);

  // Acrobat's shortcuts: Ctrl+0 fit page, Ctrl+1 actual size, Ctrl+2 fit width.
  actualSizeAct_ = new QAction(themeIcon("zoom-original"), tr("&Actual Size"), this);
  actualSizeAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
  connect(actualSizeAct_, &QAction::triggered, this, [this] { view_->setZoom(1.0); });

  fitPageAct_ = new QAction(themeIcon("zoom-fit-best"), tr("Fit &Page"), this);
  fitPageAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
  fitPageAct_->setCheckable(true);
  connect(fitPageAct_, &QAction::triggered, this, [this] { view_->setFitMode(DocumentView::FitMode::Page); });

  fitWidthAct_ = new QAction(themeIcon("zoom-fit-width"), tr("Fit &Width"), this);
  fitWidthAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_2));
  fitWidthAct_->setCheckable(true);
  connect(fitWidthAct_, &QAction::triggered, this, [this] { view_->setFitMode(DocumentView::FitMode::Width); });

  prevPageAct_ = new QAction(themeIcon("go-up", QStyle::SP_ArrowUp), tr("Previous Page"), this);
  connect(prevPageAct_, &QAction::triggered, this, [this] { view_->goToPage(view_->currentPage() - 1); });
  nextPageAct_ = new QAction(themeIcon("go-down", QStyle::SP_ArrowDown), tr("Next Page"), this);
  connect(nextPageAct_, &QAction::triggered, this, [this] { view_->goToPage(view_->currentPage() + 1); });

  aboutAct_ = new QAction(tr("&About PDFEditZD"), this);
  connect(aboutAct_, &QAction::triggered, this, [this] {
    QMessageBox::about(this, tr("About PDFEditZD"),
                       tr("<h3>PDFEditZD %1</h3>"
                          "<p>A lightweight PDF viewer and editor.</p>"
                          "<p>PDFium %2 · Qt %3</p>"
                          "<p>Licensed under the GNU GPL, version 3.</p>")
                           .arg(QApplication::applicationVersion(),
                                QString::fromStdString(pdfz::Engine::version()), qVersion()));
  });
}

void MainWindow::createMenus() {
  QMenu* file = menuBar()->addMenu(tr("&File"));
  file->addAction(openAct_);
  recentMenu_ = file->addMenu(tr("Open &Recent"));
  file->addSeparator();
  file->addAction(saveAct_);
  file->addAction(saveAsAct_);
  file->addSeparator();
  file->addAction(printAct_);
  file->addSeparator();
  file->addAction(closeAct_);
  file->addAction(quitAct_);
  updateRecentMenu();

  pagesMenu_ = menuBar()->addMenu(tr("&Pages"));
  pagesMenu_->addAction(insertBlankAct_);
  pagesMenu_->addAction(insertFileAct_);
  pagesMenu_->addSeparator();
  pagesMenu_->addAction(rotateCwAct_);
  pagesMenu_->addAction(rotateCcwAct_);
  pagesMenu_->addSeparator();
  pagesMenu_->addAction(deletePagesAct_);

  QMenu* view = menuBar()->addMenu(tr("&View"));
  view->addAction(zoomInAct_);
  view->addAction(zoomOutAct_);
  view->addSeparator();
  view->addAction(actualSizeAct_);
  view->addAction(fitPageAct_);
  view->addAction(fitWidthAct_);
  view->addSeparator();
  QAction* pagesPanel = thumbsDock_->toggleViewAction();
  pagesPanel->setText(tr("Page &Thumbnails"));
  pagesPanel->setShortcut(QKeySequence(Qt::Key_F4));
  view->addAction(pagesPanel);

  QMenu* help = menuBar()->addMenu(tr("&Help"));
  help->addAction(aboutAct_);
}

void MainWindow::createToolBar() {
  QToolBar* tb = addToolBar(tr("Main"));
  tb->setObjectName("mainToolBar");
  tb->setMovable(false);
  tb->addAction(openAct_);
  tb->addAction(saveAct_);
  tb->addAction(printAct_);
  tb->addSeparator();
  tb->addAction(prevPageAct_);
  tb->addAction(nextPageAct_);

  pageSpin_ = new QSpinBox(this);
  pageSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
  pageSpin_->setAlignment(Qt::AlignRight);
  pageSpin_->setKeyboardTracking(false);
  pageSpin_->setMinimumWidth(56);
  connect(pageSpin_, &QSpinBox::valueChanged, this, [this](int v) {
    if (v - 1 != view_->currentPage()) view_->goToPage(v - 1);
  });
  tb->addWidget(pageSpin_);
  pageCountLabel_ = new QLabel(this);
  pageCountLabel_->setContentsMargins(6, 0, 6, 0);
  tb->addWidget(pageCountLabel_);
  tb->addSeparator();

  tb->addAction(zoomOutAct_);
  zoomCombo_ = new QComboBox(this);
  zoomCombo_->setEditable(true);
  zoomCombo_->setInsertPolicy(QComboBox::NoInsert);
  zoomCombo_->setMinimumContentsLength(9);
  zoomCombo_->addItems({tr("Fit Width"), tr("Fit Page"), "50%", "75%", "100%", "125%", "150%", "200%", "300%", "400%"});
  connect(zoomCombo_, &QComboBox::textActivated, this, &MainWindow::applyZoomText);
  tb->addWidget(zoomCombo_);
  tb->addAction(zoomInAct_);
  tb->addSeparator();
  tb->addAction(fitWidthAct_);
  tb->addAction(fitPageAct_);
}

void MainWindow::applyZoomText(const QString& text) {
  if (text == tr("Fit Width")) return view_->setFitMode(DocumentView::FitMode::Width);
  if (text == tr("Fit Page")) return view_->setFitMode(DocumentView::FitMode::Page);
  QString t = text.trimmed();
  t.remove('%');
  bool ok = false;
  const double pct = t.toDouble(&ok);
  if (ok && pct > 0) view_->setZoom(pct / 100.0);
  else onZoomChanged(view_->zoom(), view_->fitMode());  // put the current zoom back
  view_->setFocus();
}

void MainWindow::onZoomChanged(double zoom, DocumentView::FitMode mode) {
  zoomCombo_->setEditText(QStringLiteral("%1%").arg(std::lround(zoom * 100)));
  fitWidthAct_->setChecked(mode == DocumentView::FitMode::Width);
  fitPageAct_->setChecked(mode == DocumentView::FitMode::Page);
  zoomInAct_->setEnabled(doc_ && zoom < DocumentView::kMaxZoom);
  zoomOutAct_->setEnabled(doc_ && zoom > DocumentView::kMinZoom);
}

void MainWindow::onCurrentPageChanged(int page) {
  if (!doc_) return;
  {
    const QSignalBlocker block(pageSpin_);
    pageSpin_->setValue(page + 1);
  }
  thumbs_->setCurrentPage(page);
  prevPageAct_->setEnabled(page > 0);
  nextPageAct_->setEnabled(page + 1 < doc_->pageCount());
  const pdfz::SizeF s = doc_->pageSize(page);
  pageSizeLabel_->setText(tr("%1 × %2 in").arg(s.width / 72, 0, 'f', 2).arg(s.height / 72, 0, 'f', 2));
}

// ---------------------------------------------------------------------------------------------
// Opening, saving, closing

std::unique_ptr<pdfz::Document> MainWindow::loadWithPrompt(const QString& path) {
  std::string password;
  for (bool retry = false;; retry = true) {
    auto [doc, err] = pdfz::Document::open(toFsPath(path), password);
    if (doc) return std::move(doc);
    if (err != pdfz::OpenError::Password) {
      QMessageBox::warning(this, tr("Open PDF"),
                           tr("Can't open %1: %2.").arg(QFileInfo(path).fileName(), pdfz::describe(err)));
      return nullptr;
    }
    bool ok = false;
    const QString prompt = retry ? tr("That password is incorrect. Try again:")
                                 : tr("%1 is protected. Enter its password:").arg(QFileInfo(path).fileName());
    const QString pw = QInputDialog::getText(this, tr("Password"), prompt, QLineEdit::Password, {}, &ok);
    if (!ok) return nullptr;
    password = pw.toStdString();
  }
}

bool MainWindow::openFile(const QString& path) {
  if (!maybeSave()) return false;
  auto doc = loadWithPrompt(path);
  if (!doc) return false;
  const QString abs = QFileInfo(path).absoluteFilePath();
  setDocument(std::shared_ptr<pdfz::Document>(std::move(doc)), abs);
  addRecent(abs);
  rememberDir(abs);
  return true;
}

void MainWindow::setDocument(std::shared_ptr<pdfz::Document> doc, const QString& path) {
  doc_ = std::move(doc);
  path_ = path;
  renderer_->setDocument(doc_);
  thumbs_->documentChanged();
  view_->documentChanged(false);
  updateUi();
  if (doc_) onCurrentPageChanged(view_->currentPage());
}

void MainWindow::updateUi() {
  const bool has = doc_ != nullptr;
  for (QAction* a : {saveAct_, saveAsAct_, printAct_, closeAct_, deletePagesAct_, insertBlankAct_, insertFileAct_,
                     rotateCwAct_, rotateCcwAct_, actualSizeAct_, fitPageAct_, fitWidthAct_, prevPageAct_,
                     nextPageAct_, zoomInAct_, zoomOutAct_})
    a->setEnabled(has);
  pageSpin_->setEnabled(has);
  zoomCombo_->setEnabled(has);
  if (has) {
    const int n = doc_->pageCount();
    pageSpin_->setRange(1, std::max(1, n));
    pageCountLabel_->setText(tr("of %1").arg(n));
    deletePagesAct_->setEnabled(n > 1);
    const QString title = QString::fromStdString(doc_->metadata("Title")).trimmed();
    const QString name = path_.isEmpty() ? tr("Untitled") : QFileInfo(path_).fileName();
    setWindowTitle(QStringLiteral("%1[*] — PDFEditZD").arg(name));
    setWindowFilePath(path_);
    setWindowModified(doc_->modified());
    statusBar()->showMessage(title.isEmpty() ? QString() : title);
    onZoomChanged(view_->zoom(), view_->fitMode());
  } else {
    pageCountLabel_->clear();
    pageSizeLabel_->clear();
    statusBar()->clearMessage();
    setWindowTitle(QStringLiteral("PDFEditZD"));
    setWindowFilePath({});
    setWindowModified(false);
    zoomCombo_->setEditText({});
  }
}

bool MainWindow::maybeSave() {
  if (!doc_ || !doc_->modified()) return true;
  const QString name = path_.isEmpty() ? tr("Untitled") : QFileInfo(path_).fileName();
  const auto answer = QMessageBox::warning(this, tr("Unsaved changes"),
                                           tr("Save the changes to %1 before closing it?").arg(name),
                                           QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                           QMessageBox::Save);
  if (answer == QMessageBox::Save) return save();
  return answer == QMessageBox::Discard;
}

bool MainWindow::save() {
  if (!doc_) return false;
  if (path_.isEmpty()) return saveAs();
  return saveTo(path_);
}

bool MainWindow::saveAs() {
  if (!doc_) return false;
  QString suggested = path_.isEmpty() ? lastDir() + "/Untitled.pdf" : path_;
  QString path = QFileDialog::getSaveFileName(this, tr("Save PDF As"), suggested, tr(kPdfFilter));
  if (path.isEmpty()) return false;
  if (QFileInfo(path).suffix().isEmpty()) path += ".pdf";
  if (!saveTo(path)) return false;
  path_ = QFileInfo(path).absoluteFilePath();
  addRecent(path_);
  rememberDir(path_);
  updateUi();
  return true;
}

bool MainWindow::saveTo(const QString& path) {
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const bool ok = doc_->save(toFsPath(path));
  QApplication::restoreOverrideCursor();
  if (!ok) {
    QMessageBox::warning(this, tr("Save"), tr("Couldn't save %1. Is it open in another program, or read-only?")
                                               .arg(QDir::toNativeSeparators(path)));
    return false;
  }
  setWindowModified(false);
  statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 4000);
  return true;
}

void MainWindow::closeDocument() {
  if (maybeSave()) setDocument(nullptr, {});
}

void MainWindow::print() {
  if (!doc_) return;
  const QString name = path_.isEmpty() ? tr("Untitled") : QFileInfo(path_).fileName();
  printDocument(this, *doc_, name, view_->currentPage());
}

void MainWindow::closeEvent(QCloseEvent* e) {
  if (!maybeSave()) {
    e->ignore();
    return;
  }
  QSettings s;
  s.setValue("window/geometry", saveGeometry());
  s.setValue("window/state", saveState());
  e->accept();
}

void MainWindow::addRecent(const QString& path) {
  QSettings s;
  QStringList files = s.value("recentFiles").toStringList();
  files.removeAll(path);
  files.prepend(path);
  while (files.size() > kMaxRecent) files.removeLast();
  s.setValue("recentFiles", files);
  updateRecentMenu();
}

void MainWindow::updateRecentMenu() {
  recentMenu_->clear();
  const QStringList files = QSettings().value("recentFiles").toStringList();
  for (int i = 0; i < files.size(); ++i) {
    const QString& f = files[i];
    QAction* a = recentMenu_->addAction(QStringLiteral("&%1  %2").arg((i + 1) % 10).arg(QFileInfo(f).fileName()));
    a->setToolTip(QDir::toNativeSeparators(f));
    a->setStatusTip(QDir::toNativeSeparators(f));
    connect(a, &QAction::triggered, this, [this, f] { openFile(f); });
  }
  if (!files.isEmpty()) {
    recentMenu_->addSeparator();
    recentMenu_->addAction(tr("Clear List"), this, [this] {
      QSettings().remove("recentFiles");
      updateRecentMenu();
    });
  }
  recentMenu_->setEnabled(!files.isEmpty());
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) {
    for (const QUrl& u : e->mimeData()->urls())
      if (u.isLocalFile() && u.toLocalFile().endsWith(".pdf", Qt::CaseInsensitive)) {
        e->acceptProposedAction();
        return;
      }
  }
}

void MainWindow::dropEvent(QDropEvent* e) {
  for (const QUrl& u : e->mimeData()->urls())
    if (u.isLocalFile() && u.toLocalFile().endsWith(".pdf", Qt::CaseInsensitive)) {
      e->acceptProposedAction();
      openFile(u.toLocalFile());
      return;
    }
}

// ---------------------------------------------------------------------------------------------
// Page editing

void MainWindow::pagesChanged(int pageToShow) {
  renderer_->invalidate();
  thumbs_->documentChanged();
  view_->documentChanged(true);
  updateUi();
  view_->goToPage(std::clamp(pageToShow, 0, doc_->pageCount() - 1));
  onCurrentPageChanged(view_->currentPage());
}

void MainWindow::deletePages() {
  if (!doc_) return;
  const std::vector<int> pages = thumbs_->selectedPages();
  if (pages.empty()) return;
  if (int(pages.size()) >= doc_->pageCount()) {
    QMessageBox::information(this, tr("Delete Pages"), tr("A PDF needs at least one page, so not every page can be deleted."));
    return;
  }
  const QString which = pages.size() == 1 ? tr("page %1").arg(pages[0] + 1) : tr("pages %1").arg(describePages(pages));
  if (QMessageBox::question(this, tr("Delete Pages"), tr("Delete %1? This can't be undone yet.").arg(which)) !=
      QMessageBox::Yes)
    return;
  for (auto it = pages.rbegin(); it != pages.rend(); ++it) doc_->deletePage(*it);
  pagesChanged(pages.front());
}

void MainWindow::insertBlankPage() {
  if (!doc_) return;
  const int at = view_->currentPage() + 1;
  // The same size as the page it follows, like Acrobat.
  pdfz::SizeF size = doc_->pageSize(view_->currentPage());
  const int rot = doc_->pageRotation(view_->currentPage());
  if (rot == 90 || rot == 270) std::swap(size.width, size.height);  // a new page isn't rotated
  if (size.width <= 0) size = pdfz::kLetter;
  if (doc_->insertBlankPage(at, size)) pagesChanged(at);
}

void MainWindow::insertPagesFromFile() {
  if (!doc_) return;
  const QString path = QFileDialog::getOpenFileName(this, tr("Insert Pages from File"), lastDir(), tr(kPdfFilter));
  if (path.isEmpty()) return;
  rememberDir(path);
  auto other = loadWithPrompt(path);
  if (!other) return;
  std::vector<int> all(std::size_t(other->pageCount()));
  for (int i = 0; i < other->pageCount(); ++i) all[std::size_t(i)] = i;
  const int at = view_->currentPage() + 1;
  if (all.empty() || !doc_->importPages(*other, all, at)) {
    QMessageBox::warning(this, tr("Insert Pages"), tr("Couldn't insert the pages of %1.").arg(QFileInfo(path).fileName()));
    return;
  }
  pagesChanged(at);
  statusBar()->showMessage(tr("Inserted %n page(s) from %1", nullptr, int(all.size())).arg(QFileInfo(path).fileName()), 4000);
}

void MainWindow::rotatePages(int degrees) {
  if (!doc_) return;
  const std::vector<int> pages = thumbs_->selectedPages();
  for (int p : pages) doc_->setPageRotation(p, doc_->pageRotation(p) + degrees);
  if (!pages.empty()) pagesChanged(view_->currentPage());
}
