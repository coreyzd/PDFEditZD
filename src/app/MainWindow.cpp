// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.hpp"

#include "CommentsPanel.hpp"
#include "DocumentTab.hpp"
#include "FindBar.hpp"
#include "Printing.hpp"
#include "TextSearch.hpp"
#include "ThumbnailPanel.hpp"
#include "ToolIcons.hpp"

#include "core/annotations.hpp"
#include "core/document.hpp"
#include "core/engine.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

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

// "1-3, 5" for showing which pages an action changed.
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

struct PresetColor {
  QColor color;
  const char* name;
};
const std::vector<PresetColor>& presetColors() {
  static const std::vector<PresetColor> colors = {
      {QColor(0xFF, 0xE5, 0x00), QT_TRANSLATE_NOOP("MainWindow", "Yellow")},
      {QColor(0xFF, 0x9F, 0x1C), QT_TRANSLATE_NOOP("MainWindow", "Orange")},
      {QColor(0xE0, 0x1B, 0x24), QT_TRANSLATE_NOOP("MainWindow", "Red")},
      {QColor(0xF0, 0x5C, 0xA8), QT_TRANSLATE_NOOP("MainWindow", "Pink")},
      {QColor(0x8E, 0x44, 0xAD), QT_TRANSLATE_NOOP("MainWindow", "Purple")},
      {QColor(0x1A, 0x5F, 0xD0), QT_TRANSLATE_NOOP("MainWindow", "Blue")},
      {QColor(0x18, 0xB4, 0xD8), QT_TRANSLATE_NOOP("MainWindow", "Cyan")},
      {QColor(0x00, 0x9E, 0x3A), QT_TRANSLATE_NOOP("MainWindow", "Green")},
      {QColor(0x00, 0x00, 0x00), QT_TRANSLATE_NOOP("MainWindow", "Black")},
      {QColor(0x80, 0x80, 0x80), QT_TRANSLATE_NOOP("MainWindow", "Gray")},
      {QColor(0xFF, 0xFF, 0xFF), QT_TRANSLATE_NOOP("MainWindow", "White")}};
  return colors;
}

// The combo box item whose number is closest to `v`.
int nearestItem(const QComboBox* combo, double v) {
  int best = 0;
  for (int i = 1; i < combo->count(); ++i)
    if (std::abs(combo->itemData(i).toDouble() - v) < std::abs(combo->itemData(best).toDouble() - v)) best = i;
  return best;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setAcceptDrops(true);
  for (int i = 0; i < kMarkupToolCount; ++i) styles_[std::size_t(i)] = loadStyle(MarkupTool(i));

  // The center: a welcome page while nothing is open, the document tabs otherwise.
  findBar_ = new FindBar(this);
  findBar_->hide();
  tabs_ = new QTabWidget(this);
  tabs_->setDocumentMode(true);
  tabs_->setTabsClosable(true);
  tabs_->setMovable(true);
  tabs_->tabBar()->setElideMode(Qt::ElideMiddle);
  tabs_->tabBar()->setExpanding(false);
  auto* docs = new QWidget(this);
  auto* docsLayout = new QVBoxLayout(docs);
  docsLayout->setContentsMargins(0, 0, 0, 0);
  docsLayout->setSpacing(0);
  docsLayout->addWidget(findBar_);
  docsLayout->addWidget(tabs_, 1);
  central_ = new QStackedWidget(this);
  welcome_ = createWelcome();
  central_->addWidget(welcome_);
  central_->addWidget(docs);
  setCentralWidget(central_);

  thumbsStack_ = new QStackedWidget(this);
  thumbsDock_ = new QDockWidget(tr("Pages"), this);
  thumbsDock_->setObjectName("pagesDock");
  thumbsDock_->setWidget(thumbsStack_);
  thumbsDock_->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
  addDockWidget(Qt::LeftDockWidgetArea, thumbsDock_);

  commentsStack_ = new QStackedWidget(this);
  commentsDock_ = new QDockWidget(tr("Comments"), this);
  commentsDock_->setObjectName("commentsDock");
  commentsDock_->setWidget(commentsStack_);
  commentsDock_->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
  addDockWidget(Qt::RightDockWidgetArea, commentsDock_);

  createActions();
  createMenus();
  createToolBars();

  pageSizeLabel_ = new QLabel(this);
  statusBar()->addPermanentWidget(pageSizeLabel_);

  connect(tabs_, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);
  connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int i) {
    if (auto* t = qobject_cast<DocumentTab*>(tabs_->widget(i))) closeTab(t);
  });
  connect(findBar_, &FindBar::searchChanged, this, &MainWindow::restartSearch);
  connect(findBar_, &FindBar::findNext, this, [this] { findStep(+1); });
  connect(findBar_, &FindBar::findPrevious, this, [this] { findStep(-1); });
  connect(findBar_, &FindBar::closed, this, [this] {
    if (auto* t = tab()) {
      t->search()->clear();
      t->view()->searchChanged();
      t->view()->setFocus();
    }
  });

  QSettings s;
  if (!restoreGeometry(s.value("window/geometry").toByteArray())) resize(1200, 860);
  restoreState(s.value("window/state").toByteArray());
  if (!s.contains("window/state")) commentsDock_->hide();  // shown when wanted (View > Comments)

  onCurrentTabChanged();
}

MainWindow::~MainWindow() {
  // Tabs first: their panels sit in this window's docks.
  while (tabs_->count() > 0) delete tabs_->widget(0);
}

// ---------------------------------------------------------------------------------------------
// Tabs

DocumentTab* MainWindow::tab() const { return qobject_cast<DocumentTab*>(tabs_->currentWidget()); }

DocumentTab* MainWindow::tabFor(const QString& absolutePath) const {
  for (int i = 0; i < tabs_->count(); ++i) {
    auto* t = qobject_cast<DocumentTab*>(tabs_->widget(i));
    if (t && !t->path().isEmpty() && QFileInfo(t->path()) == QFileInfo(absolutePath)) return t;
  }
  return nullptr;
}

DocumentTab* MainWindow::newTab() {
  auto* t = new DocumentTab(this);
  thumbsStack_->addWidget(t->thumbnails());
  commentsStack_->addWidget(t->comments());
  connectTab(t);
  tabs_->addTab(t, t->displayName());
  return t;
}

void MainWindow::connectTab(DocumentTab* t) {
  DocumentView* v = t->view();
  connect(t, &DocumentTab::stateChanged, this, [this, t] {
    updateTabTitle(t);
    if (t == tab()) updateUi();
  });
  connect(t, &DocumentTab::statusMessage, this, [this](const QString& m, int ms) { statusBar()->showMessage(m, ms); });
  connect(t, &DocumentTab::contentChanged, this, [this, t](bool pages) {
    if (t != tab() || !pages) return;
    if (findBar_->isVisible() && !findBar_->text().isEmpty()) restartSearch();
    else t->search()->clear();
  });
  connect(v, &DocumentView::currentPageChanged, this, [this, t](int page) {
    if (t == tab()) onCurrentPageChanged(page);
  });
  connect(v, &DocumentView::zoomChanged, this, [this, t](double z, DocumentView::FitMode m) {
    if (t == tab()) onZoomChanged(z, m);
  });
  connect(v, &DocumentView::selectionChanged, this, [this, t](bool has) {
    if (t != tab()) return;
    for (QAction* a : {copyAct_, highlightSelAct_, underlineSelAct_, strikeSelAct_}) a->setEnabled(has);
  });
  connect(v, &DocumentView::annotationSelectionChanged, this, [this, t] {
    if (t == tab()) updateStyleControls();
  });
  connect(v, &DocumentView::contextMenuRequested, this,
          [this, t](const QPoint& pos, int page, int index) { showViewMenu(t, pos, page, index); });
  connect(v, &DocumentView::annotationCreated, this, [this](int, const pdfz::Annotation& a) {
    // A note is placed one at a time: back to selecting, to work with it.
    if (a.kind == pdfz::AnnotKind::Note) QTimer::singleShot(0, selectToolAct_, &QAction::trigger);
  });
  // So is a text box, as soon as it's being typed in.
  connect(v, &DocumentView::textEditStarted, this, [this, t] {
    if (t != tab()) return;
    if (!selectToolAct_->isChecked()) selectToolAct_->trigger();
    updateStyleControls();
  });
  connect(v, &DocumentView::textEditFinished, this, [this, t] {
    if (t == tab()) updateStyleControls();
  });
  // Esc, again and again: out of the drag, the selection, and back to the Select tool.
  connect(v, &DocumentView::toolResetRequested, this, [this, t] {
    if (t == tab() && !selectToolAct_->isChecked()) selectToolAct_->trigger();
  });
  // V selects, H is the hand, like Acrobat: only while the pages have the keyboard.
  for (auto [key, act] : {std::pair{Qt::Key_V, selectToolAct_}, std::pair{Qt::Key_H, handToolAct_}}) {
    auto* sc = new QShortcut(QKeySequence(key), v, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
    connect(sc, &QShortcut::activated, act, &QAction::trigger);
  }

  ThumbnailPanel* th = t->thumbnails();
  connect(th, &ThumbnailPanel::contextMenuForPages, this, [this](const QPoint& pos) { pagesMenu_->exec(pos); });
  connect(th, &ThumbnailPanel::pagesDropped, this,
          [this, t](const std::vector<int>& pages, int before) { movePages(t, pages, before); });
  // Delete in the thumbnails deletes the selected pages (in the page view, the selected comment).
  auto* del = new QShortcut(QKeySequence::Delete, th, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
  connect(del, &QShortcut::activated, this, &MainWindow::deletePages);

  TextSearch* s = t->search();
  connect(s, &TextSearch::matchesChanged, this, [this, t] { onSearchMatches(t, false); });
  connect(s, &TextSearch::finished, this, [this, t] { onSearchMatches(t, true); });
}

bool MainWindow::closeTab(DocumentTab* t) {
  if (!t || !maybeSave(t)) return false;
  const int i = tabs_->indexOf(t);
  if (i >= 0) tabs_->removeTab(i);
  delete t;  // takes its panels out of the docks
  if (tabs_->count() == 0) onCurrentTabChanged();
  return true;
}

void MainWindow::onCurrentTabChanged() {
  DocumentTab* t = tab();
  central_->setCurrentIndex(t ? 1 : 0);
  if (t) {
    thumbsStack_->setCurrentWidget(t->thumbnails());
    commentsStack_->setCurrentWidget(t->comments());
    applyTool();
    if (findBar_->isVisible()) {
      if (findBar_->text().isEmpty()) t->search()->clear();
      else if (t->search()->query() != findBar_->text()) restartSearch();
    }
    onCurrentPageChanged(t->view()->currentPage());
    // The keyboard goes to the pages of the tab now showing (otherwise it stays with the
    // hidden tab's view, where the window's shortcuts don't reach).
    if (!findBar_->isAncestorOf(QApplication::focusWidget())) t->view()->setFocus();
  } else {
    findBar_->hide();
    updateRecentMenu();  // the welcome page lists them too
  }
  updateUi();
}

void MainWindow::updateTabTitle(DocumentTab* t) {
  const int i = tabs_->indexOf(t);
  if (i < 0) return;
  tabs_->setTabText(i, t->displayName() + (t->isModified() ? QStringLiteral(" *") : QString()));
  tabs_->setTabToolTip(i, t->path().isEmpty() ? t->displayName() : QDir::toNativeSeparators(t->path()));
}

// ---------------------------------------------------------------------------------------------
// Actions, menus, toolbars

void MainWindow::createActions() {
  openAct_ = new QAction(themeIcon("document-open", QStyle::SP_DialogOpenButton), tr("&Open…"), this);
  openAct_->setShortcut(QKeySequence::Open);
  connect(openAct_, &QAction::triggered, this, [this] {
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Open PDF"), lastDir(), tr(kPdfFilter));
    for (const QString& p : paths) openFile(p);
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
  connect(closeAct_, &QAction::triggered, this, [this] { closeTab(tab()); });

  quitAct_ = new QAction(tr("E&xit"), this);
  quitAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
  connect(quitAct_, &QAction::triggered, this, &QWidget::close);

  undoAct_ = new QAction(themeIcon("edit-undo"), tr("&Undo"), this);
  undoAct_->setShortcut(QKeySequence::Undo);
  connect(undoAct_, &QAction::triggered, this, [this] { if (auto* t = tab()) t->undo(); });
  redoAct_ = new QAction(themeIcon("edit-redo"), tr("&Redo"), this);
  redoAct_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Y), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
  connect(redoAct_, &QAction::triggered, this, [this] { if (auto* t = tab()) t->redo(); });

  copyAct_ = new QAction(themeIcon("edit-copy"), tr("&Copy"), this);
  copyAct_->setShortcut(QKeySequence::Copy);
  connect(copyAct_, &QAction::triggered, this, &MainWindow::copySelection);
  selectAllAct_ = new QAction(tr("Select &All Text"), this);
  selectAllAct_->setShortcut(QKeySequence::SelectAll);
  connect(selectAllAct_, &QAction::triggered, this, [this] { if (auto* t = tab()) t->view()->selectAll(); });

  findAct_ = new QAction(themeIcon("edit-find"), tr("&Find…"), this);
  findAct_->setShortcut(QKeySequence::Find);
  connect(findAct_, &QAction::triggered, this, &MainWindow::showFind);
  findNextAct_ = new QAction(tr("Find &Next"), this);
  findNextAct_->setShortcut(QKeySequence(Qt::Key_F3));
  connect(findNextAct_, &QAction::triggered, this, [this] { findStep(+1); });
  findPrevAct_ = new QAction(tr("Find Pre&vious"), this);
  findPrevAct_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
  connect(findPrevAct_, &QAction::triggered, this, [this] { findStep(-1); });

  authorAct_ = new QAction(tr("Comment &Author Name…"), this);
  connect(authorAct_, &QAction::triggered, this, &MainWindow::setAuthor);

  // Tools: one checked at a time, for every tab.
  toolGroup_ = new QActionGroup(this);
  selectToolAct_ = new QAction(selectToolIcon(palette()), tr("&Select Tool"), this);
  selectToolAct_->setCheckable(true);
  selectToolAct_->setChecked(true);
  selectToolAct_->setToolTip(tr("Select text and comments, or drag elsewhere to move the page (V)"));
  handToolAct_ = new QAction(handToolIcon(palette()), tr("&Hand Tool"), this);
  handToolAct_->setCheckable(true);
  handToolAct_->setToolTip(tr("Drag to move the page (H)"));
  toolGroup_->addAction(selectToolAct_);
  toolGroup_->addAction(handToolAct_);
  const QString tips[kMarkupToolCount] = {
      tr("Highlight: drag over text"),          tr("Underline: drag over text"),
      tr("Strikethrough: drag over text"),      tr("Sticky note: click where it goes"),
      tr("Text box: click, or drag out a box"), tr("Pen: draw freehand"),
      tr("Rectangle: drag out a rectangle"),    tr("Ellipse: drag out an ellipse"),
      tr("Line: drag from end to end"),         tr("Arrow: drag from the tail to the tip")};
  for (int i = 0; i < kMarkupToolCount; ++i) {
    const MarkupTool tool = MarkupTool(i);
    auto* a = new QAction(toolIcon(tool, palette()), toolName(tool), this);
    a->setCheckable(true);
    a->setToolTip(tips[i]);
    toolGroup_->addAction(a);
    markupActs_[std::size_t(i)] = a;
  }
  connect(toolGroup_, &QActionGroup::triggered, this, [this](QAction* picked) {
    // Another tool: the typing in a text box is over (the Select tool is where typing goes on).
    if (picked != selectToolAct_)
      if (DocumentTab* t = tab()) t->finishTextEditing();
    applyTool();
    updateStyleControls();
  });

  highlightSelAct_ = new QAction(tr("&Highlight Selected Text"), this);
  highlightSelAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
  connect(highlightSelAct_, &QAction::triggered, this, [this] { markupSelection(MarkupTool::Highlight); });
  underlineSelAct_ = new QAction(tr("&Underline Selected Text"), this);
  underlineSelAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U));
  connect(underlineSelAct_, &QAction::triggered, this, [this] { markupSelection(MarkupTool::Underline); });
  strikeSelAct_ = new QAction(tr("&Strike Through Selected Text"), this);
  strikeSelAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X));
  connect(strikeSelAct_, &QAction::triggered, this, [this] { markupSelection(MarkupTool::StrikeOut); });

  deletePagesAct_ = new QAction(themeIcon("edit-delete"), tr("&Delete Pages"), this);
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
  connect(zoomInAct_, &QAction::triggered, this, [this] { if (auto* t = tab()) t->view()->zoomIn(); });
  zoomOutAct_ = new QAction(themeIcon("zoom-out"), tr("Zoom &Out"), this);
  zoomOutAct_->setShortcut(QKeySequence::ZoomOut);
  connect(zoomOutAct_, &QAction::triggered, this, [this] { if (auto* t = tab()) t->view()->zoomOut(); });
  // Acrobat's shortcuts: Ctrl+0 fit page, Ctrl+1 actual size, Ctrl+2 fit width.
  actualSizeAct_ = new QAction(themeIcon("zoom-original"), tr("&Actual Size"), this);
  actualSizeAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
  connect(actualSizeAct_, &QAction::triggered, this, [this] { if (auto* t = tab()) t->view()->setZoom(1.0); });
  fitPageAct_ = new QAction(themeIcon("zoom-fit-best"), tr("Fit &Page"), this);
  fitPageAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
  fitPageAct_->setCheckable(true);
  connect(fitPageAct_, &QAction::triggered, this,
          [this] { if (auto* t = tab()) t->view()->setFitMode(DocumentView::FitMode::Page); });
  fitWidthAct_ = new QAction(themeIcon("zoom-fit-width"), tr("Fit &Width"), this);
  fitWidthAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_2));
  fitWidthAct_->setCheckable(true);
  connect(fitWidthAct_, &QAction::triggered, this,
          [this] { if (auto* t = tab()) t->view()->setFitMode(DocumentView::FitMode::Width); });

  prevPageAct_ = new QAction(themeIcon("go-up", QStyle::SP_ArrowUp), tr("Previous Page"), this);
  connect(prevPageAct_, &QAction::triggered, this,
          [this] { if (auto* t = tab()) t->view()->goToPage(t->view()->currentPage() - 1); });
  nextPageAct_ = new QAction(themeIcon("go-down", QStyle::SP_ArrowDown), tr("Next Page"), this);
  connect(nextPageAct_, &QAction::triggered, this,
          [this] { if (auto* t = tab()) t->view()->goToPage(t->view()->currentPage() + 1); });

  nextTabAct_ = new QAction(tr("Next Tab"), this);
  nextTabAct_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageDown)});
  connect(nextTabAct_, &QAction::triggered, this, [this] {
    if (tabs_->count() > 1) tabs_->setCurrentIndex((tabs_->currentIndex() + 1) % tabs_->count());
  });
  prevTabAct_ = new QAction(tr("Previous Tab"), this);
  prevTabAct_->setShortcuts({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageUp)});
  connect(prevTabAct_, &QAction::triggered, this, [this] {
    if (tabs_->count() > 1) tabs_->setCurrentIndex((tabs_->currentIndex() + tabs_->count() - 1) % tabs_->count());
  });
  addAction(nextTabAct_);
  addAction(prevTabAct_);

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

  QMenu* edit = menuBar()->addMenu(tr("&Edit"));
  edit->addAction(undoAct_);
  edit->addAction(redoAct_);
  edit->addSeparator();
  edit->addAction(copyAct_);
  edit->addAction(selectAllAct_);
  edit->addSeparator();
  edit->addAction(findAct_);
  edit->addAction(findNextAct_);
  edit->addAction(findPrevAct_);

  QMenu* comment = menuBar()->addMenu(tr("&Comment"));
  for (QAction* a : markupActs_) comment->addAction(a);
  comment->insertSeparator(markupActs_[std::size_t(MarkupTool::Note)]);
  comment->insertSeparator(markupActs_[std::size_t(MarkupTool::Pen)]);
  comment->addSeparator();
  comment->addAction(highlightSelAct_);
  comment->addAction(underlineSelAct_);
  comment->addAction(strikeSelAct_);
  comment->addSeparator();
  comment->addAction(authorAct_);

  pagesMenu_ = menuBar()->addMenu(tr("&Pages"));
  pagesMenu_->addAction(insertBlankAct_);
  pagesMenu_->addAction(insertFileAct_);
  pagesMenu_->addSeparator();
  pagesMenu_->addAction(rotateCwAct_);
  pagesMenu_->addAction(rotateCcwAct_);
  pagesMenu_->addSeparator();
  pagesMenu_->addAction(deletePagesAct_);

  QMenu* view = viewMenu_ = menuBar()->addMenu(tr("&View"));
  view->addAction(selectToolAct_);
  view->addAction(handToolAct_);
  view->addSeparator();
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
  QAction* commentsPanel = commentsDock_->toggleViewAction();
  commentsPanel->setText(tr("Co&mments"));
  commentsPanel->setShortcut(QKeySequence(Qt::Key_F6));
  view->addAction(commentsPanel);
  view->addSeparator();
  view->addAction(nextTabAct_);
  view->addAction(prevTabAct_);

  QMenu* help = menuBar()->addMenu(tr("&Help"));
  help->addAction(aboutAct_);
  updateRecentMenu();
}

void MainWindow::createToolBars() {
  QToolBar* tb = addToolBar(tr("Main"));
  tb->setObjectName("mainToolBar");
  tb->setMovable(false);
  tb->addAction(openAct_);
  tb->addAction(saveAct_);
  tb->addAction(printAct_);
  tb->addSeparator();
  tb->addAction(undoAct_);
  tb->addAction(redoAct_);
  tb->addSeparator();
  tb->addAction(selectToolAct_);
  tb->addAction(handToolAct_);
  tb->addSeparator();
  tb->addAction(prevPageAct_);
  tb->addAction(nextPageAct_);
  pageSpin_ = new QSpinBox(this);
  pageSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
  pageSpin_->setAlignment(Qt::AlignRight);
  pageSpin_->setKeyboardTracking(false);
  pageSpin_->setMinimumWidth(56);
  connect(pageSpin_, &QSpinBox::valueChanged, this, [this](int v) {
    auto* t = tab();
    if (t && v - 1 != t->view()->currentPage()) t->view()->goToPage(v - 1);
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
  tb->addSeparator();
  tb->addAction(findAct_);

  // The comment tools, on their own row.
  addToolBarBreak();
  markupBar_ = addToolBar(tr("Comment Tools"));
  markupBar_->setObjectName("commentToolBar");
  markupBar_->setMovable(false);
  for (int i = 0; i < kMarkupToolCount; ++i) {
    if (i == int(MarkupTool::Note) || i == int(MarkupTool::Pen)) markupBar_->addSeparator();
    markupBar_->addAction(markupActs_[std::size_t(i)]);
  }
  // The style of the selected comment (or the box being typed in, or what the tool makes
  // next). None of these take the keyboard, so typing in a text box carries on after using them.
  markupBar_->addSeparator();
  auto styleButton = [this](const QString& tip) {
    auto* b = new QToolButton(this);
    b->setToolTip(tip);
    b->setPopupMode(QToolButton::InstantPopup);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
  };
  auto styleCombo = [this](const QString& tip) {
    auto* c = new QComboBox(this);
    c->setToolTip(tip);
    c->setFocusPolicy(Qt::NoFocus);
    return c;
  };
  colorButton_ = styleButton(tr("Color: of the line, markup or note, or a text box's border"));
  colorButton_->setMenu(colorMenu(colorButton_, {}, [this](const QColor& c) { changeStyle(StyleChange{.color = c}); }));
  markupBar_->addWidget(colorButton_);
  fillButton_ = styleButton(tr("Fill color (rectangles, ellipses and text boxes)"));
  fillButton_->setMenu(colorMenu(fillButton_, tr("No Fill"), [this](const QColor& c) { changeStyle(StyleChange{.fill = c}); }));
  markupBar_->addWidget(fillButton_);
  opacityCombo_ = styleCombo(tr("Opacity"));
  for (int pct : {100, 75, 50, 25}) opacityCombo_->addItem(tr("%1%").arg(pct), pct / 100.0);
  connect(opacityCombo_, &QComboBox::activated, this,
          [this](int i) { changeStyle(StyleChange{.opacity = opacityCombo_->itemData(i).toDouble()}); });
  markupBar_->addWidget(opacityCombo_);
  widthCombo_ = styleCombo(tr("Line width (a text box's border)"));
  widthCombo_->addItem(tr("No Border"), 0.0);
  for (double w : {0.5, 1.0, 2.0, 3.0, 5.0, 8.0}) widthCombo_->addItem(tr("%1 pt").arg(w), w);
  connect(widthCombo_, &QComboBox::activated, this,
          [this](int i) { changeStyle(StyleChange{.width = widthCombo_->itemData(i).toDouble()}); });
  markupBar_->addWidget(widthCombo_);

  // Text boxes: the font, its size and color, and the alignment.
  textControls_.push_back(markupBar_->addSeparator());
  fontCombo_ = styleCombo(tr("Font"));
  fontCombo_->addItem(QStringLiteral("Helvetica"), QStringLiteral("Helv"));
  fontCombo_->addItem(QStringLiteral("Times"), QStringLiteral("TiRo"));
  fontCombo_->addItem(QStringLiteral("Courier"), QStringLiteral("Cour"));
  for (int i = 0; i < fontCombo_->count(); ++i)
    fontCombo_->setItemData(i, fontFor(fontCombo_->itemData(i).toString().toStdString(), 13), Qt::FontRole);
  connect(fontCombo_, &QComboBox::activated, this,
          [this](int i) { changeStyle(StyleChange{.fontName = fontCombo_->itemData(i).toString()}); });
  textControls_.push_back(markupBar_->addWidget(fontCombo_));
  sizeCombo_ = styleCombo(tr("Font size"));
  for (int pt : {6, 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 36, 48, 72}) sizeCombo_->addItem(tr("%1 pt").arg(pt), double(pt));
  connect(sizeCombo_, &QComboBox::activated, this,
          [this](int i) { changeStyle(StyleChange{.fontSize = sizeCombo_->itemData(i).toDouble()}); });
  textControls_.push_back(markupBar_->addWidget(sizeCombo_));
  textColorButton_ = styleButton(tr("Text color"));
  textColorButton_->setMenu(colorMenu(textColorButton_, {}, [this](const QColor& c) { changeStyle(StyleChange{.textColor = c}); }));
  textControls_.push_back(markupBar_->addWidget(textColorButton_));
  auto* aligns = new QActionGroup(this);
  const QString alignTips[3] = {tr("Align Left"), tr("Center"), tr("Align Right")};
  for (int i = 0; i < 3; ++i) {
    auto* a = new QAction(alignIcon(i, palette()), alignTips[i], this);
    a->setCheckable(true);
    aligns->addAction(a);
    connect(a, &QAction::triggered, this, [this, i] { changeStyle(StyleChange{.align = i}); });
    markupBar_->addAction(a);
    if (auto* w = markupBar_->widgetForAction(a)) w->setFocusPolicy(Qt::NoFocus);
    alignActs_[std::size_t(i)] = a;
    textControls_.push_back(a);
  }

  // In the View menu, under the panels.
  QAction* toggle = markupBar_->toggleViewAction();
  toggle->setText(tr("Comment &Tools"));
  viewMenu_->insertAction(nextTabAct_, toggle);
  viewMenu_->insertSeparator(nextTabAct_);
}

QWidget* MainWindow::createWelcome() {
  auto* w = new QWidget(this);
  auto* lay = new QVBoxLayout(w);
  lay->addStretch(1);
  auto* title = new QLabel(QStringLiteral("PDFEditZD"), w);
  QFont f = title->font();
  f.setPointSizeF(f.pointSizeF() * 2.2);
  f.setBold(true);
  title->setFont(f);
  title->setAlignment(Qt::AlignHCenter);
  lay->addWidget(title);
  auto* hint = new QLabel(tr("Open a PDF, or drop one here."), w);
  hint->setAlignment(Qt::AlignHCenter);
  hint->setEnabled(false);
  lay->addWidget(hint);
  auto* open = new QPushButton(tr("Open a PDF…"), w);
  open->setDefault(true);
  connect(open, &QPushButton::clicked, this, [this] { openAct_->trigger(); });
  auto* row = new QHBoxLayout;
  row->addStretch(1);
  row->addWidget(open);
  row->addStretch(1);
  lay->addSpacing(8);
  lay->addLayout(row);
  lay->addSpacing(16);
  auto* recentTitle = new QLabel(tr("Recent"), w);
  recentTitle->setAlignment(Qt::AlignHCenter);
  lay->addWidget(recentTitle);
  welcomeRecent_ = new QListWidget(w);
  welcomeRecent_->setMaximumWidth(520);
  welcomeRecent_->setMaximumHeight(260);
  welcomeRecent_->setFrameShape(QFrame::NoFrame);
  connect(welcomeRecent_, &QListWidget::itemActivated, this,
          [this](QListWidgetItem* it) { openFile(it->data(Qt::UserRole).toString()); });
  connect(welcomeRecent_, &QListWidget::itemClicked, this,
          [this](QListWidgetItem* it) { openFile(it->data(Qt::UserRole).toString()); });
  auto* row2 = new QHBoxLayout;
  row2->addStretch(1);
  row2->addWidget(welcomeRecent_, 2);
  row2->addStretch(1);
  lay->addLayout(row2);
  lay->addStretch(2);
  return w;
}

void MainWindow::updateUi() {
  DocumentTab* t = tab();
  const bool has = t && t->hasDocument();
  for (QAction* a : {saveAct_, saveAsAct_, printAct_, closeAct_, deletePagesAct_, insertBlankAct_, insertFileAct_,
                     rotateCwAct_, rotateCcwAct_, actualSizeAct_, fitPageAct_, fitWidthAct_, prevPageAct_,
                     nextPageAct_, zoomInAct_, zoomOutAct_, selectAllAct_, findAct_, findNextAct_, findPrevAct_})
    a->setEnabled(has);
  for (QAction* a : markupActs_) a->setEnabled(has);
  const bool sel = has && t->view()->hasSelection();
  for (QAction* a : {copyAct_, highlightSelAct_, underlineSelAct_, strikeSelAct_}) a->setEnabled(sel);
  static const pdfz::EditHistory none;
  const pdfz::EditHistory& h = has ? t->history() : none;
  undoAct_->setEnabled(has && h.canUndo());
  redoAct_->setEnabled(has && h.canRedo());
  undoAct_->setText(has && h.canUndo() ? tr("&Undo %1").arg(QString::fromStdString(h.undoLabel())) : tr("&Undo"));
  redoAct_->setText(has && h.canRedo() ? tr("&Redo %1").arg(QString::fromStdString(h.redoLabel())) : tr("&Redo"));
  undoAct_->setToolTip(undoAct_->text().remove('&'));
  redoAct_->setToolTip(redoAct_->text().remove('&'));
  undoAct_->setIconText(tr("Undo"));  // the toolbar keeps it short; the menu says what
  redoAct_->setIconText(tr("Redo"));
  pageSpin_->setEnabled(has);
  zoomCombo_->setEnabled(has);
  if (has) {
    const int n = t->document()->pageCount();
    {
      const QSignalBlocker block(pageSpin_);
      pageSpin_->setRange(1, std::max(1, n));
      pageSpin_->setValue(t->view()->currentPage() + 1);
    }
    pageCountLabel_->setText(tr("of %1").arg(n));
    deletePagesAct_->setEnabled(n > 1);
    setWindowTitle(QStringLiteral("%1[*] — PDFEditZD").arg(t->displayName()));
    setWindowFilePath(t->path());
    setWindowModified(t->isModified());
    onZoomChanged(t->view()->zoom(), t->view()->fitMode());
  } else {
    pageCountLabel_->clear();
    pageSizeLabel_->clear();
    setWindowTitle(QStringLiteral("PDFEditZD"));
    setWindowFilePath({});
    setWindowModified(false);
    zoomCombo_->setEditText({});
  }
  updateStyleControls();
}

void MainWindow::applyZoomText(const QString& text) {
  DocumentTab* t = tab();
  if (!t) return;
  if (text == tr("Fit Width")) return t->view()->setFitMode(DocumentView::FitMode::Width);
  if (text == tr("Fit Page")) return t->view()->setFitMode(DocumentView::FitMode::Page);
  QString s = text.trimmed();
  s.remove('%');
  bool ok = false;
  const double pct = s.toDouble(&ok);
  if (ok && pct > 0) t->view()->setZoom(pct / 100.0);
  else onZoomChanged(t->view()->zoom(), t->view()->fitMode());  // put the current zoom back
  t->view()->setFocus();
}

void MainWindow::onZoomChanged(double zoom, DocumentView::FitMode mode) {
  zoomCombo_->setEditText(QStringLiteral("%1%").arg(std::lround(zoom * 100)));
  fitWidthAct_->setChecked(mode == DocumentView::FitMode::Width);
  fitPageAct_->setChecked(mode == DocumentView::FitMode::Page);
  const bool has = tab() && tab()->hasDocument();
  zoomInAct_->setEnabled(has && zoom < DocumentView::kMaxZoom);
  zoomOutAct_->setEnabled(has && zoom > DocumentView::kMinZoom);
}

void MainWindow::onCurrentPageChanged(int page) {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  {
    const QSignalBlocker block(pageSpin_);
    pageSpin_->setValue(page + 1);
  }
  t->thumbnails()->setCurrentPage(page);
  prevPageAct_->setEnabled(page > 0);
  nextPageAct_->setEnabled(page + 1 < t->document()->pageCount());
  const pdfz::SizeF s = t->document()->pageSize(page);
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
  const QString abs = QFileInfo(path).absoluteFilePath();
  if (DocumentTab* open = tabFor(abs)) {
    tabs_->setCurrentWidget(open);
    return true;
  }
  auto doc = loadWithPrompt(abs);
  if (!doc) return false;
  DocumentTab* t = newTab();
  t->setDocument(std::shared_ptr<pdfz::Document>(std::move(doc)), abs);
  tabs_->setCurrentWidget(t);
  onCurrentTabChanged();
  const QString title = QString::fromStdString(t->document()->metadata("Title")).trimmed();
  if (!title.isEmpty()) statusBar()->showMessage(title, 5000);
  addRecent(abs);
  rememberDir(abs);
  t->view()->setFocus();
  return true;
}

bool MainWindow::maybeSave(DocumentTab* t) {
  if (t) t->finishTextEditing();
  if (!t || !t->hasDocument() || !t->isModified()) return true;
  tabs_->setCurrentWidget(t);
  const auto answer = QMessageBox::warning(this, tr("Unsaved changes"),
                                           tr("Save the changes to %1 before closing it?").arg(t->displayName()),
                                           QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                           QMessageBox::Save);
  if (answer == QMessageBox::Save) return save();
  return answer == QMessageBox::Discard;
}

bool MainWindow::save() {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return false;
  if (t->path().isEmpty()) return saveAs();
  return t->saveTo(t->path());
}

bool MainWindow::saveAs() {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return false;
  const QString suggested = t->path().isEmpty() ? lastDir() + "/Untitled.pdf" : t->path();
  QString path = QFileDialog::getSaveFileName(this, tr("Save PDF As"), suggested, tr(kPdfFilter));
  if (path.isEmpty()) return false;
  if (QFileInfo(path).suffix().isEmpty()) path += ".pdf";
  if (DocumentTab* other = tabFor(QFileInfo(path).absoluteFilePath()); other && other != t) {
    QMessageBox::warning(this, tr("Save PDF As"), tr("%1 is open in another tab. Close it first.").arg(QFileInfo(path).fileName()));
    return false;
  }
  if (!t->saveTo(path)) return false;
  addRecent(t->path());
  rememberDir(t->path());
  return true;
}

void MainWindow::print() {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  t->finishTextEditing();
  printDocument(this, *t->document(), t->displayName(), t->view()->currentPage());
}

void MainWindow::closeEvent(QCloseEvent* e) {
  for (int i = 0; i < tabs_->count(); ++i)
    if (!maybeSave(qobject_cast<DocumentTab*>(tabs_->widget(i)))) {
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
  welcomeRecent_->clear();
  const QStringList files = QSettings().value("recentFiles").toStringList();
  for (int i = 0; i < files.size(); ++i) {
    const QString& f = files[i];
    QAction* a = recentMenu_->addAction(QStringLiteral("&%1  %2").arg((i + 1) % 10).arg(QFileInfo(f).fileName()));
    a->setToolTip(QDir::toNativeSeparators(f));
    a->setStatusTip(QDir::toNativeSeparators(f));
    connect(a, &QAction::triggered, this, [this, f] { openFile(f); });
    auto* item = new QListWidgetItem(QFileInfo(f).fileName(), welcomeRecent_);
    item->setToolTip(QDir::toNativeSeparators(f));
    item->setData(Qt::UserRole, f);
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
  bool any = false;
  for (const QUrl& u : e->mimeData()->urls())
    if (u.isLocalFile() && u.toLocalFile().endsWith(".pdf", Qt::CaseInsensitive)) {
      if (!any) e->acceptProposedAction();
      any = true;
      openFile(u.toLocalFile());
    }
}

// ---------------------------------------------------------------------------------------------
// Pages

void MainWindow::movePages(DocumentTab* t, const std::vector<int>& pages, int insertBefore) {
  if (!t || !t->hasDocument() || pages.empty()) return;
  // Where the block starts once the pages are out of the way.
  const int dest = insertBefore - int(std::count_if(pages.begin(), pages.end(), [&](int p) { return p < insertBefore; }));
  const int n = int(pages.size());
  bool unchanged = true;  // already a block starting at dest?
  for (int i = 0; i < n; ++i)
    if (pages[std::size_t(i)] != dest + i) unchanged = false;
  if (unchanged) return;
  if (t->applyEdit(n == 1 ? tr("Move Page") : tr("Move Pages"), dest, DocumentTab::Scope::Pages,
                   [&](pdfz::Document& d) { return d.movePages(pages, dest); })) {
    t->thumbnails()->selectPages(dest, n);
    statusBar()->showMessage(tr("Moved %1").arg(n == 1 ? tr("page %1").arg(pages[0] + 1)
                                                       : tr("pages %1").arg(describePages(pages))), 3000);
  }
}

void MainWindow::deletePages() {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  const std::vector<int> pages = t->thumbnails()->selectedPages();
  if (pages.empty()) return;
  if (int(pages.size()) >= t->document()->pageCount()) {
    QMessageBox::information(this, tr("Delete Pages"), tr("A PDF needs at least one page, so not every page can be deleted."));
    return;
  }
  const QString which = pages.size() == 1 ? tr("page %1").arg(pages[0] + 1) : tr("pages %1").arg(describePages(pages));
  if (t->applyEdit(pages.size() == 1 ? tr("Delete Page") : tr("Delete Pages"), pages.front(), DocumentTab::Scope::Pages,
                   [&](pdfz::Document& d) {
                     bool any = false;
                     for (auto it = pages.rbegin(); it != pages.rend(); ++it) any = d.deletePage(*it) || any;
                     return any;
                   }))
    statusBar()->showMessage(tr("Deleted %1 (Ctrl+Z to undo)").arg(which), 4000);
}

void MainWindow::insertBlankPage() {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  const int cur = t->view()->currentPage();
  const int at = cur + 1;
  // The same size as the page it follows, like Acrobat.
  pdfz::SizeF size = t->document()->pageSize(cur);
  const int rot = t->document()->pageRotation(cur);
  if (rot == 90 || rot == 270) std::swap(size.width, size.height);  // a new page isn't rotated
  if (size.width <= 0) size = pdfz::kLetter;
  t->applyEdit(tr("Insert Blank Page"), at, DocumentTab::Scope::Pages,
               [&](pdfz::Document& d) { return d.insertBlankPage(at, size); });
}

void MainWindow::insertPagesFromFile() {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  const QString path = QFileDialog::getOpenFileName(this, tr("Insert Pages from File"), lastDir(), tr(kPdfFilter));
  if (path.isEmpty()) return;
  rememberDir(path);
  auto other = loadWithPrompt(path);
  if (!other) return;
  std::vector<int> all(std::size_t(other->pageCount()));
  for (int i = 0; i < other->pageCount(); ++i) all[std::size_t(i)] = i;
  const int at = t->view()->currentPage() + 1;
  if (all.empty() || !t->applyEdit(tr("Insert Pages"), at, DocumentTab::Scope::Pages,
                                   [&](pdfz::Document& d) { return d.importPages(*other, all, at); })) {
    QMessageBox::warning(this, tr("Insert Pages"), tr("Couldn't insert the pages of %1.").arg(QFileInfo(path).fileName()));
    return;
  }
  statusBar()->showMessage(tr("Inserted %n page(s) from %1", nullptr, int(all.size())).arg(QFileInfo(path).fileName()), 4000);
}

void MainWindow::rotatePages(int degrees) {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  const std::vector<int> pages = t->thumbnails()->selectedPages();
  if (pages.empty()) return;
  t->applyEdit(pages.size() == 1 ? tr("Rotate Page") : tr("Rotate Pages"), t->view()->currentPage(),
               DocumentTab::Scope::Pages, [&](pdfz::Document& d) {
                 bool any = false;
                 for (int p : pages) any = d.setPageRotation(p, d.pageRotation(p) + degrees) || any;
                 return any;
               });
  ThumbnailPanel* th = t->thumbnails();
  th->selectPages(pages.front(), 1);
  if (pages.size() > 1)  // keep the whole selection, for rotating again
    for (int p : pages) th->selectionModel()->select(th->model()->index(p, 0), QItemSelectionModel::Select);
}

// ---------------------------------------------------------------------------------------------
// Text: copy and find

void MainWindow::copySelection() {
  DocumentTab* t = tab();
  if (!t) return;
  const QString text = t->view()->selectedText();
  if (text.isEmpty()) return;
  QGuiApplication::clipboard()->setText(text);
  statusBar()->showMessage(tr("Copied %n character(s)", nullptr, int(text.size())), 2000);
}

void MainWindow::showFind() {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  // A short selection on one line is probably what to look for.
  QString seed = t->view()->selectedText().trimmed();
  if (seed.size() > 80 || seed.contains('\n')) seed.clear();
  findBar_->activate(seed);
  if (!findBar_->text().isEmpty() && t->search()->query().isEmpty()) restartSearch();
}

void MainWindow::restartSearch() {
  DocumentTab* t = tab();
  if (!t) return;
  if (!t->hasDocument() || findBar_->text().isEmpty()) {
    t->search()->clear();
    t->view()->searchChanged();
    updateFindStatus();
    return;
  }
  t->search()->start(t->document(), findBar_->text(), findBar_->matchCase(), findBar_->wholeWords());
  updateFindStatus();
}

void MainWindow::onSearchMatches(DocumentTab* t, bool finished) {
  if (t != tab()) return;
  TextSearch* s = t->search();
  const auto& m = s->matches();
  // The first matches are in: go to the first one from the current page on (or, once the
  // whole document has been searched, the first one).
  if (s->current() < 0 && !m.empty() && findBar_->isVisible()) {
    int pick = -1;
    for (int i = 0; i < int(m.size()); ++i)
      if (m[std::size_t(i)].page >= t->view()->currentPage()) {
        pick = i;
        break;
      }
    if (pick >= 0 || finished || !s->running()) {
      s->setCurrent(pick >= 0 ? pick : 0);
      const SearchMatch& sm = m[std::size_t(s->current())];
      t->view()->selectRange(sm.page, sm.start, sm.end);
    }
  }
  t->view()->searchChanged();
  updateFindStatus();
}

void MainWindow::findStep(int direction) {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  if (!findBar_->isVisible()) {
    showFind();
    if (findBar_->text().isEmpty()) return;
  }
  TextSearch* s = t->search();
  const auto& m = s->matches();
  if (m.empty()) {
    if (!s->running()) QApplication::beep();
    return;
  }
  const int n = int(m.size());
  int cur = s->current();
  cur = cur < 0 ? (direction > 0 ? 0 : n - 1) : ((cur + direction) % n + n) % n;
  s->setCurrent(cur);
  const SearchMatch& sm = m[std::size_t(cur)];
  t->view()->selectRange(sm.page, sm.start, sm.end);
  t->view()->searchChanged();
  updateFindStatus();
}

void MainWindow::updateFindStatus() {
  DocumentTab* t = tab();
  TextSearch* s = t ? t->search() : nullptr;
  if (!s || s->query().isEmpty()) {
    findBar_->setStatus({});
    return;
  }
  const int n = int(s->matches().size());
  const QString more = s->running() ? QStringLiteral("+") : QString();
  if (n == 0) findBar_->setStatus(s->running() ? tr("Searching…") : tr("No matches"), !s->running());
  else if (s->current() >= 0) findBar_->setStatus(tr("%1 of %2%3").arg(s->current() + 1).arg(n).arg(more));
  else findBar_->setStatus(tr("%1%2 matches").arg(n).arg(more));
}

// ---------------------------------------------------------------------------------------------
// Comments

void MainWindow::applyTool() {
  DocumentTab* t = tab();
  if (!t) return;
  DocumentView* v = t->view();
  if (handToolAct_->isChecked()) {
    v->setTool(DocumentView::Tool::Hand);
    return;
  }
  for (int i = 0; i < kMarkupToolCount; ++i)
    if (markupActs_[std::size_t(i)]->isChecked()) {
      v->setMarkupTool(MarkupTool(i), styles_[std::size_t(i)]);
      return;
    }
  v->setTool(DocumentView::Tool::Select);
}

void MainWindow::changeStyle(const StyleChange& change) {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  DocumentView* v = t->view();
  int page, index;
  if (v->isEditingText()) {
    if (v->isEditingNewText()) {  // what the text box tool makes next looks like this too
      MarkupStyle& s = styles_[std::size_t(MarkupTool::TextBox)];
      applyStyleChange(s, change);
      saveStyle(MarkupTool::TextBox, s);
    }
    v->restyleTextEdit(change);
  } else if (v->selectedAnnotation(page, index)) {
    t->restyleAnnotation(page, index, change);
  } else if (v->tool() == DocumentView::Tool::Markup) {
    const MarkupTool tool = v->markupTool();
    MarkupStyle& s = styles_[std::size_t(tool)];
    applyStyleChange(s, change);
    saveStyle(tool, s);
    v->setMarkupStyle(s);
  }
  updateStyleControls();
}

// The style controls show the box being typed in, else the selected comment, else the tool's
// style; the ones that don't apply are disabled (the text ones hidden).
void MainWindow::updateStyleControls() {
  DocumentTab* t = tab();
  std::optional<MarkupStyle> st;
  bool fill = false, width = false, text = false;
  if (t && t->hasDocument()) {
    DocumentView* v = t->view();
    int page, index;
    if (const pdfz::Annotation* e = v->editedText()) {
      st = styleOf(*e);
      fill = width = text = true;
    } else if (v->selectedAnnotation(page, index)) {
      if (const pdfz::Annotation* a = t->annotation(page, index); a && a->editable()) {
        st = styleOf(*a);
        fill = usesFill(*a);
        width = usesWidth(*a);
        text = usesText(*a);
      }
    } else if (v->tool() == DocumentView::Tool::Markup) {
      const MarkupTool tool = v->markupTool();
      st = styles_[std::size_t(tool)];
      fill = usesFill(tool);
      width = usesWidth(tool);
      text = usesText(tool);
    }
  }
  colorButton_->setEnabled(st.has_value());
  colorButton_->setIcon(strokeColorIcon(st ? st->color : QColor(), palette()));
  fillButton_->setEnabled(st && fill);
  fillButton_->setIcon(fillColorIcon(st && fill ? st->fill : QColor(), palette()));
  opacityCombo_->setEnabled(st.has_value());
  opacityCombo_->setCurrentIndex(st ? nearestItem(opacityCombo_, st->opacity) : -1);
  widthCombo_->setEnabled(st && width);
  if (auto* model = qobject_cast<QStandardItemModel*>(widthCombo_->model()))
    if (QStandardItem* none = model->item(0)) none->setEnabled(text);  // only a text box can do without
  if (st && width) widthCombo_->setCurrentIndex(nearestItem(widthCombo_, text || st->width > 0 ? st->width : 0.5));
  else widthCombo_->setCurrentIndex(-1);  // blank rather than a width that means nothing here
  for (QAction* a : textControls_) a->setVisible(st && text);
  if (st && text) {
    const int f = fontCombo_->findData(st->fontName);
    fontCombo_->setCurrentIndex(f >= 0 ? f : 0);
    sizeCombo_->setCurrentIndex(nearestItem(sizeCombo_, st->fontSize));
    textColorButton_->setIcon(textColorIcon(st->textColor, palette()));
    alignActs_[std::size_t(std::clamp(st->align, 0, 2))]->setChecked(true);
  }
}

QMenu* MainWindow::colorMenu(QWidget* parent, const QString& noneText, std::function<void(const QColor&)> pick) {
  auto* m = new QMenu(parent);
  if (!noneText.isEmpty()) {
    m->addAction(colorIcon(QColor()), noneText, this, [pick] { pick(QColor()); });
    m->addSeparator();
  }
  for (const PresetColor& c : presetColors())
    m->addAction(colorIcon(c.color), tr(c.name), this, [pick, col = c.color] { pick(col); });
  m->addSeparator();
  m->addAction(tr("More Colors…"), this, [this, pick] {
    const QColor c = QColorDialog::getColor(Qt::white, this, tr("Choose a Color"));
    if (c.isValid()) pick(c);
  });
  return m;
}

void MainWindow::markupSelection(MarkupTool tool) {
  DocumentTab* t = tab();
  if (!t || !t->view()->hasSelection()) return;
  t->view()->markupSelection(tool, styles_[std::size_t(tool)]);
  t->view()->clearSelection();
}

void MainWindow::showViewMenu(DocumentTab* t, const QPoint& globalPos, int page, int annotIndex) {
  if (t != tab() || !t->hasDocument()) return;
  QMenu menu(this);
  if (annotIndex >= 0) {
    const pdfz::Annotation* a = t->annotation(page, annotIndex);
    const bool textBox = a && a->kind == pdfz::AnnotKind::FreeText;
    menu.addAction(textBox ? tr("Edit Text") : tr("Edit Comment Text…"), this, [=] { t->editAnnotation(page, annotIndex); });
    if (a && a->editable()) {
      auto restyle = [=](StyleChange c) { t->restyleAnnotation(page, annotIndex, c); };
      QMenu* colors = colorMenu(&menu, {}, [=](const QColor& c) { restyle(StyleChange{.color = c}); });
      colors->setTitle(textBox ? tr("Border Color") : tr("Color"));
      menu.addMenu(colors);
      if (usesFill(*a)) {
        QMenu* fills = colorMenu(&menu, tr("No Fill"), [=](const QColor& c) { restyle(StyleChange{.fill = c}); });
        fills->setTitle(tr("Fill Color"));
        menu.addMenu(fills);
      }
      if (textBox) {
        QMenu* texts = colorMenu(&menu, {}, [=](const QColor& c) { restyle(StyleChange{.textColor = c}); });
        texts->setTitle(tr("Text Color"));
        menu.addMenu(texts);
      }
      QMenu* opacity = menu.addMenu(tr("Opacity"));
      for (int pct : {100, 75, 50, 25}) {
        QAction* o = opacity->addAction(tr("%1%").arg(pct), this, [=] { restyle(StyleChange{.opacity = pct / 100.0}); });
        o->setCheckable(true);
        o->setChecked(std::abs(a->opacity - pct / 100.0) < 0.06);
      }
    }
    menu.addSeparator();
    menu.addAction(tr("Delete Comment"), this, [=] { t->deleteAnnotation(page, annotIndex); });
  } else {
    menu.addAction(copyAct_);
    menu.addSeparator();
    menu.addAction(highlightSelAct_);
    menu.addAction(underlineSelAct_);
    menu.addAction(strikeSelAct_);
    menu.addSeparator();
    menu.addAction(selectAllAct_);
  }
  menu.exec(globalPos);
}

void MainWindow::setAuthor() {
  bool ok = false;
  const QString name = QInputDialog::getText(this, tr("Comment Author"),
                                             tr("Name written on your comments (empty for your login name):"),
                                             QLineEdit::Normal, QSettings().value("comments/author").toString(), &ok);
  if (ok) setCommentAuthor(name);
}
