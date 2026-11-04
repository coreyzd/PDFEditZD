// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.hpp"

#include "CommentsPanel.hpp"
#include "DocumentTab.hpp"
#include "FindBar.hpp"
#include "OfficeImport.hpp"
#include "Printing.hpp"
#include "SignatureDialog.hpp"
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
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QInputDialog>
#include <QPainter>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
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
#include <QWidgetAction>

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

// "1-3, 5, 8-" (1-based, "8-" to the end) as 0-based page indices in the order given, each
// once; empty if it isn't a list of pages of a `count`-page document.
std::vector<int> parsePages(const QString& text, int count) {
  std::vector<int> out;
  std::vector<bool> seen(std::size_t(std::max(0, count)), false);
  const QStringList parts = text.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
  for (const QString& part : parts) {
    const QRegularExpressionMatch m =
        QRegularExpression(QStringLiteral("^(\\d*)\\s*(?:[-\u2013]\\s*(\\d*))?$")).match(part);
    if (!m.hasMatch() || (m.captured(1).isEmpty() && m.captured(2).isEmpty())) return {};
    const bool range = part.contains('-') || part.contains(QChar(0x2013));
    const int a = m.captured(1).isEmpty() ? 1 : m.captured(1).toInt();
    const int b = !range ? a : m.captured(2).isEmpty() ? count : m.captured(2).toInt();
    if (a < 1 || b < 1 || a > count || b > count) return {};
    for (int p = a; a <= b ? p <= b : p >= b; p += a <= b ? 1 : -1)
      if (!seen[std::size_t(p - 1)]) {
        seen[std::size_t(p - 1)] = true;
        out.push_back(p - 1);
      }
  }
  return out;
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
  editBar_->hide();  // until Edit PDF is picked

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
    // Fill & Sign's Add Text stays in use, so a form fills in one click after another.
    if (!selectToolAct_->isChecked() && !t->view()->isFillingIn()) selectToolAct_->trigger();
    updateStyleControls();
  });
  connect(v, &DocumentView::textEditFinished, this, [this, t] {
    if (t == tab()) updateStyleControls();
  });
  // Edit PDF.
  for (auto sig : {&DocumentView::contentSelectionChanged, &DocumentView::contentEditStarted})
    connect(v, sig, this, [this, t] {
      if (t == tab()) updateEditControls();
    });
  connect(v, &DocumentView::contentEditFinished, this, [this, t] {
    if (t == tab()) QTimer::singleShot(0, this, &MainWindow::updateEditControls);
  });
  connect(v, &DocumentView::editAddTextEnded, this, [this, t] {
    if (t == tab()) addTextAct_->setChecked(false);
  });
  connect(v, &DocumentView::signatureFieldClicked, this,
          [this, t](int page, const pdfz::RectF& rect) { signField(t, page, rect); });
  v->setHighlightFields(highlightFieldsAct_->isChecked());
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
      tr("Line: drag from end to end"),         tr("Arrow: drag from the tail to the tip"),
      tr("Add text: click where it goes, and type"), tr("Checkmark: click where it goes"),
      tr("Cross: click where it goes"),         tr("Dot: click where it goes"),
      tr("Today's date: click where it goes")};
  for (int i = 0; i < kMarkupToolCount; ++i) {
    const MarkupTool tool = MarkupTool(i);
    auto* a = new QAction(toolIcon(tool, palette()), toolName(tool), this);
    a->setCheckable(true);
    a->setToolTip(tips[i]);
    toolGroup_->addAction(a);
    markupActs_[std::size_t(i)] = a;
  }
  // Sign: checked while a signature waits to be placed (sign() checks it).
  signAct_ = new QAction(signIcon(palette()), tr("&Sign"), this);
  signAct_->setCheckable(true);
  signAct_->setToolTip(tr("Sign: place your signature with a click (the arrow: other signatures, initials)"));
  toolGroup_->addAction(signAct_);
  connect(signAct_, &QAction::triggered, this, [this] {
    const QList<Signature> saved = SignatureStore::load();
    std::optional<Signature> first;
    for (const Signature& s : saved)
      if (!s.initials) {
        first = s;
        break;
      }
    sign(first, false);
  });

  // Stamp: checked while a stamp waits to be placed.
  stampAct_ = new QAction(stampIcon(palette()), tr("S&tamp"), this);
  stampAct_->setCheckable(true);
  stampAct_->setToolTip(tr("Stamps: Approved, Draft, Confidential... and your own pictures"));
  toolGroup_->addAction(stampAct_);

  // Edit PDF: the page's own text and pictures.
  editAct_ = new QAction(editPdfIcon(palette()), tr("&Edit PDF"), this);
  editAct_->setCheckable(true);
  editAct_->setToolTip(tr("Edit PDF: change, move or delete the page's text and pictures, and add new ones"));
  toolGroup_->addAction(editAct_);
  addTextAct_ = new QAction(addTextIcon(palette()), tr("Add &Text"), this);
  addTextAct_->setCheckable(true);
  addTextAct_->setToolTip(tr("Add text: click where it goes on the page, and type"));
  connect(addTextAct_, &QAction::triggered, this, [this](bool on) {
    if (on && !editAct_->isChecked()) editAct_->trigger();
    if (DocumentTab* t = tab()) {
      t->finishTextEditing();
      t->view()->setEditAddText(on);
    }
  });
  addImageAct_ = new QAction(addImageIcon(palette()), tr("Add &Image…"), this);
  addImageAct_->setToolTip(tr("Add a picture to the page"));
  connect(addImageAct_, &QAction::triggered, this, &MainWindow::addImage);
  {
    QSettings st;
    newTextFont_.family = st.value("edit/font", QStringLiteral("Helvetica")).toString();
    newTextFont_.bold = st.value("edit/bold", false).toBool();
    newTextFont_.italic = st.value("edit/italic", false).toBool();
    newTextSize_ = std::clamp(st.value("edit/size", 12.0).toDouble(), 1.0, 500.0);
    newTextColor_ = QColor(st.value("edit/color", QStringLiteral("#000000")).toString());
    if (!newTextColor_.isValid()) newTextColor_ = Qt::black;
  }

  saveFlatAct_ = new QAction(tr("Save as &Flattened…"), this);
  saveFlatAct_->setToolTip(tr("Save a copy in which comments, signatures and form fields can't be changed"));
  connect(saveFlatAct_, &QAction::triggered, this, &MainWindow::saveFlattened);

  highlightFieldsAct_ = new QAction(tr("&Highlight Form Fields"), this);
  highlightFieldsAct_->setCheckable(true);
  highlightFieldsAct_->setChecked(QSettings().value("forms/highlight", true).toBool());
  connect(highlightFieldsAct_, &QAction::toggled, this, [this](bool on) {
    QSettings().setValue("forms/highlight", on);
    for (int i = 0; i < tabs_->count(); ++i)
      if (auto* t = qobject_cast<DocumentTab*>(tabs_->widget(i))) t->view()->setHighlightFields(on);
  });

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
  extractAct_ = new QAction(tr("&Extract Pages…"), this);
  extractAct_->setToolTip(tr("Save some pages as a new PDF"));
  connect(extractAct_, &QAction::triggered, this, &MainWindow::extractPages);
  splitAct_ = new QAction(tr("S&plit Document…"), this);
  splitAct_->setToolTip(tr("Save the document as several PDFs of so many pages each"));
  connect(splitAct_, &QAction::triggered, this, &MainWindow::splitDocument);
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
  file->addAction(saveFlatAct_);
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
  edit->addSeparator();
  edit->addAction(editAct_);
  edit->addAction(addTextAct_);
  edit->addAction(addImageAct_);

  QMenu* comment = menuBar()->addMenu(tr("&Comment"));
  for (int i = 0; i < kCommentToolCount; ++i) comment->addAction(markupActs_[std::size_t(i)]);
  comment->insertSeparator(markupActs_[std::size_t(MarkupTool::Note)]);
  comment->insertSeparator(markupActs_[std::size_t(MarkupTool::Pen)]);
  QMenu* stamps = comment->addMenu(tr("S&tamps"));
  connect(stamps, &QMenu::aboutToShow, this, [this, stamps] { fillStampMenu(stamps); });
  comment->addSeparator();
  comment->addAction(highlightSelAct_);
  comment->addAction(underlineSelAct_);
  comment->addAction(strikeSelAct_);
  comment->addSeparator();
  comment->addAction(authorAct_);

  QMenu* fill = menuBar()->addMenu(tr("F&ill && Sign"));
  for (int i = kCommentToolCount; i < kMarkupToolCount; ++i) fill->addAction(markupActs_[std::size_t(i)]);
  fill->addSeparator();
  signMenu_ = new QMenu(tr("&Sign"), this);
  signMenu_->setIcon(signIcon(palette()));
  connect(signMenu_, &QMenu::aboutToShow, this, [this] { fillSignMenu(signMenu_); });
  fill->addMenu(signMenu_);
  fill->addSeparator();
  fill->addAction(highlightFieldsAct_);
  fill->addAction(saveFlatAct_);

  pagesMenu_ = menuBar()->addMenu(tr("&Pages"));
  pagesMenu_->addAction(insertBlankAct_);
  pagesMenu_->addAction(insertFileAct_);
  pagesMenu_->addSeparator();
  pagesMenu_->addAction(rotateCwAct_);
  pagesMenu_->addAction(rotateCcwAct_);
  pagesMenu_->addSeparator();
  pagesMenu_->addAction(extractAct_);
  pagesMenu_->addAction(splitAct_);
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
  auto* editButton = new QToolButton(this);
  editButton->setDefaultAction(editAct_);
  editButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tb->addWidget(editButton);
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

  // Fill & Sign, beside the main tools.
  fillBar_ = addToolBar(tr("Fill && Sign"));
  fillBar_->setObjectName("fillSignToolBar");
  fillBar_->setMovable(false);
  for (int i = kCommentToolCount; i < kMarkupToolCount; ++i) fillBar_->addAction(markupActs_[std::size_t(i)]);
  fillBar_->addSeparator();
  auto* signButton = new QToolButton(this);
  signButton->setDefaultAction(signAct_);
  signButton->setPopupMode(QToolButton::MenuButtonPopup);
  signButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  auto* signDrop = new QMenu(signButton);
  connect(signDrop, &QMenu::aboutToShow, this, [this, signDrop] { fillSignMenu(signDrop); });
  signButton->setMenu(signDrop);
  fillBar_->addWidget(signButton);

  // The comment tools, on their own row.
  addToolBarBreak();
  markupBar_ = addToolBar(tr("Comment Tools"));
  markupBar_->setObjectName("commentToolBar");
  markupBar_->setMovable(false);
  for (int i = 0; i < kCommentToolCount; ++i) {
    if (i == int(MarkupTool::Note) || i == int(MarkupTool::Pen)) markupBar_->addSeparator();
    markupBar_->addAction(markupActs_[std::size_t(i)]);
  }
  auto* stampButton = new QToolButton(this);
  stampButton->setDefaultAction(stampAct_);
  stampButton->setPopupMode(QToolButton::InstantPopup);
  auto* stampMenu = new QMenu(stampButton);
  connect(stampMenu, &QMenu::aboutToShow, this, [this, stampMenu] { fillStampMenu(stampMenu); });
  stampButton->setMenu(stampMenu);
  markupBar_->addWidget(stampButton);

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

  // Edit PDF's tools, on their own row while it's in use.
  addToolBarBreak();
  editBar_ = addToolBar(tr("Edit PDF"));
  editBar_->setObjectName("editPdfToolBar");
  editBar_->setMovable(false);
  editBar_->toggleViewAction()->setVisible(false);  // shown with the tool, not from the menu
  editBar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  editBar_->addAction(addTextAct_);
  editBar_->addAction(addImageAct_);
  for (QAction* a : {addTextAct_, addImageAct_})
    if (auto* w = editBar_->widgetForAction(a)) w->setFocusPolicy(Qt::NoFocus);
  editBar_->addSeparator();
  editFontCombo_ = styleCombo(tr("Font"));
  for (const QString& f : editFontFamilies()) {
    if (editFontCombo_->count() == 3) editFontCombo_->insertSeparator(3);  // after the standard fonts
    editFontCombo_->addItem(f, f);
  }
  editFontCombo_->setMaxVisibleItems(20);
  editFontCombo_->setMinimumContentsLength(14);
  connect(editFontCombo_, &QComboBox::activated, this,
          [this](int i) { changeEditStyle(EditStyleChange{.family = editFontCombo_->itemData(i).toString()}); });
  editBar_->addWidget(editFontCombo_);
  editSizeCombo_ = styleCombo(tr("Font size"));
  for (int pt : {6, 7, 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48, 60, 72}) editSizeCombo_->addItem(tr("%1 pt").arg(pt), double(pt));
  connect(editSizeCombo_, &QComboBox::activated, this,
          [this](int i) { changeEditStyle(EditStyleChange{.size = editSizeCombo_->itemData(i).toDouble()}); });
  editBar_->addWidget(editSizeCombo_);
  editColorButton_ = styleButton(tr("Text color"));
  editColorButton_->setMenu(colorMenu(editColorButton_, {}, [this](const QColor& c) { changeEditStyle(EditStyleChange{.color = c}); }));
  editBar_->addWidget(editColorButton_);
  editBoldAct_ = editBar_->addAction(tr("B"));
  editBoldAct_->setToolTip(tr("Bold"));
  editBoldAct_->setCheckable(true);
  editItalicAct_ = editBar_->addAction(tr("I"));
  editItalicAct_->setToolTip(tr("Italic"));
  editItalicAct_->setCheckable(true);
  for (auto [a, bold] : {std::pair{editBoldAct_, true}, std::pair{editItalicAct_, false}}) {
    QFont f = font();
    f.setBold(bold);
    f.setItalic(!bold);
    a->setFont(f);
    if (auto* w = editBar_->widgetForAction(a)) {
      w->setFocusPolicy(Qt::NoFocus);
      static_cast<QToolButton*>(w)->setToolButtonStyle(Qt::ToolButtonTextOnly);
    }
    connect(a, &QAction::triggered, this, [this, bold](bool on) {
      changeEditStyle(bold ? EditStyleChange{.bold = on} : EditStyleChange{.italic = on});
    });
  }
  editBar_->hide();

  // In the View menu, under the panels.
  QAction* toggle = markupBar_->toggleViewAction();
  toggle->setText(tr("Comment &Tools"));
  viewMenu_->insertAction(nextTabAct_, toggle);
  QAction* toggleFill = fillBar_->toggleViewAction();
  toggleFill->setText(tr("&Fill && Sign Tools"));
  viewMenu_->insertAction(nextTabAct_, toggleFill);
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
  signAct_->setEnabled(has);
  stampAct_->setEnabled(has);
  for (QAction* a : {editAct_, addTextAct_, addImageAct_}) a->setEnabled(has);
  extractAct_->setEnabled(has);
  splitAct_->setEnabled(has && t->document()->pageCount() > 1);
  signMenu_->setEnabled(has);
  saveFlatAct_->setEnabled(has);
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
  if (t->hasForm())
    statusBar()->showMessage(tr("This document has form fields: click one to fill it in, Tab to go to the next"), 8000);
  else if (!title.isEmpty())
    statusBar()->showMessage(title, 5000);
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
                   [&](pdfz::Document& d) { return d.deletePages(pages); }))
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

// PDFs, Word documents (converted by Word) and pictures (a page each, the size of the page
// they go after), several at once, in the order chosen, after the current page.
void MainWindow::insertPagesFromFile() {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  t->finishTextEditing();
  const QString all = (QStringList{"*.pdf"} + wordPatterns() + imagePatterns()).join(' ');
  const QString filter = tr("PDFs, Word documents and pictures (%1);;PDF documents (*.pdf);;Word documents (%2);;"
                            "Pictures (%3);;All files (*)")
                             .arg(all, wordPatterns().join(' '), imagePatterns().join(' '));
  const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Insert Pages from File"), lastDir(), filter);
  if (paths.isEmpty()) return;
  rememberDir(paths.front());

  const int start = t->view()->currentPage() + 1;
  int at = start;
  int inserted = 0;
  QStringList failed;
  for (const QString& path : paths) {
    const QString name = QFileInfo(path).fileName();
    if (isImageFile(path)) {
      pdfz::Document::PageImage img;
      QString error;
      if (!loadPageImage(path, img, &error)) {
        failed << error;
        continue;
      }
      // The size of the page it goes after (or before, at the start).
      const pdfz::SizeF size = t->document()->pageSize(std::max(0, at - 1));
      if (t->applyEdit(tr("Insert Picture"), at, DocumentTab::Scope::Pages,
                       [&](pdfz::Document& d) { return d.insertImagePage(at, size, img); })) {
        ++at;
        ++inserted;
      } else {
        failed << tr("Couldn't make a page of %1.").arg(name);
      }
      continue;
    }
    QString pdfPath = path;
    QString converted;
    if (isWordFile(path)) {
      QString error;
      converted = convertToPdf(this, path, &error);
      if (converted.isEmpty()) {
        if (!error.isEmpty()) failed << error;
        continue;
      }
      pdfPath = converted;
    }
    auto other = loadWithPrompt(pdfPath);
    if (!converted.isEmpty()) {
      QFile::remove(converted);
      QDir().rmdir(QFileInfo(converted).absolutePath());
    }
    if (!other) continue;
    std::vector<int> pages(std::size_t(other->pageCount()));
    for (int i = 0; i < other->pageCount(); ++i) pages[std::size_t(i)] = i;
    if (!pages.empty() && t->applyEdit(tr("Insert Pages"), at, DocumentTab::Scope::Pages,
                                       [&](pdfz::Document& d) { return d.importPages(*other, pages, at); })) {
      at += int(pages.size());
      inserted += int(pages.size());
    } else {
      failed << tr("Couldn't insert the pages of %1.").arg(name);
    }
  }
  if (inserted > 0) t->view()->goToPage(start);
  if (!failed.isEmpty()) QMessageBox::warning(this, tr("Insert Pages"), failed.join("\n\n"));
  if (inserted > 0)
    statusBar()->showMessage(inserted == 1 ? tr("Inserted 1 page") : tr("Inserted %1 pages").arg(inserted), 4000);
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
  statusBar()->showMessage((text.size() == 1 ? tr("Copied 1 character") : tr("Copied %1 characters").arg(text.size())), 2000);
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
  const bool editing = t && t->hasDocument() && editAct_->isChecked();
  editBar_->setVisible(editing);
  if (!editAct_->isChecked()) addTextAct_->setChecked(false);
  if (!t) return;
  DocumentView* v = t->view();
  if (editAct_->isChecked()) {
    v->setTool(DocumentView::Tool::Edit);
    v->setNewTextStyle(newTextFont_, newTextSize_, newTextColor_);
    v->setEditAddText(addTextAct_->isChecked());
    updateEditControls();
    return;
  }
  if (handToolAct_->isChecked()) {
    v->setTool(DocumentView::Tool::Hand);
    return;
  }
  if (signAct_->isChecked() || stampAct_->isChecked()) {
    if (v->tool() != DocumentView::Tool::Place) selectToolAct_->trigger();  // e.g. another tab's placing
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
  bool fill = false, width = false, text = false, opacity = true;
  if (t && t->hasDocument()) {
    DocumentView* v = t->view();
    int page, index;
    if (const pdfz::Annotation* e = v->editedText()) {
      st = styleOf(*e);
      fill = width = text = true;
    } else if (v->selectedAnnotation(page, index)) {
      if (const pdfz::Annotation* a = t->annotation(page, index); a && (a->editable() || isMark(*a))) {
        st = styleOf(*a);
        fill = usesFill(*a);
        width = usesWidth(*a);
        text = usesText(*a);
        opacity = usesOpacity(*a);
      }
    } else if (v->tool() == DocumentView::Tool::Markup) {
      const MarkupTool tool = v->markupTool();
      st = styles_[std::size_t(tool)];
      fill = usesFill(tool);
      width = usesWidth(tool);
      text = usesText(tool);
      opacity = usesOpacity(tool);
    }
  }
  colorButton_->setEnabled(st.has_value());
  colorButton_->setIcon(strokeColorIcon(st ? st->color : QColor(), palette()));
  fillButton_->setEnabled(st && fill);
  fillButton_->setIcon(fillColorIcon(st && fill ? st->fill : QColor(), palette()));
  opacityCombo_->setEnabled(st && opacity);
  opacityCombo_->setCurrentIndex(st && opacity ? nearestItem(opacityCombo_, st->opacity) : -1);
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

// ---------------------------------------------------------------------------------------------
// Edit PDF

// The style shown: of the text being typed in, else of the selected text, else of new text.
void MainWindow::updateEditControls() {
  DocumentTab* t = tab();
  EditFont font = newTextFont_;
  double size = newTextSize_;
  QColor color = newTextColor_;
  bool textControls = true;
  if (t && t->hasDocument()) {
    DocumentView* v = t->view();
    int page;
    pdfz::ContentBlock b;
    if (const TextEditing* e = v->contentEditing()) {
      font = e->font;
      size = e->size;
      color = e->color;
    } else if (v->selectedContent(page, b)) {
      if (b.kind == pdfz::ContentBlock::Kind::Text && b.editable) {
        font = editFontFor(b);
        size = b.fontSize;
        color = toQColor(b.color);
      } else {
        textControls = false;  // a picture, or text that's turned
      }
    }
  }
  int fi = editFontCombo_->findData(font.family);
  if (fi < 0) {
    editFontCombo_->addItem(font.family, font.family);
    fi = editFontCombo_->count() - 1;
  }
  editFontCombo_->setCurrentIndex(fi);
  const double rounded = std::round(size * 10) / 10;
  int si = -1;
  for (int i = 0; i < editSizeCombo_->count(); ++i)
    if (std::abs(editSizeCombo_->itemData(i).toDouble() - rounded) < 0.05) si = i;
  if (si < 0) {
    int at = 0;
    while (at < editSizeCombo_->count() && editSizeCombo_->itemData(at).toDouble() < rounded) ++at;
    editSizeCombo_->insertItem(at, tr("%1 pt").arg(rounded), rounded);
    si = at;
  }
  editSizeCombo_->setCurrentIndex(si);
  editColorButton_->setIcon(textColorIcon(color, palette()));
  editBoldAct_->setChecked(font.bold);
  editItalicAct_->setChecked(font.italic);
  for (QWidget* w : std::initializer_list<QWidget*>{editFontCombo_, editSizeCombo_, editColorButton_}) w->setEnabled(textControls);
  editBoldAct_->setEnabled(textControls);
  editItalicAct_->setEnabled(textControls);
}

void MainWindow::changeEditStyle(const EditStyleChange& c) {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  DocumentView* v = t->view();
  auto apply = [&c](EditFont& f, double& size, QColor& color) {
    if (c.family) f.family = *c.family;
    if (c.bold) f.bold = *c.bold;
    if (c.italic) f.italic = *c.italic;
    if (c.size) size = *c.size;
    if (c.color && c.color->isValid()) color = *c.color;
  };
  int page;
  pdfz::ContentBlock b;
  if (const TextEditing* e = v->contentEditing()) {
    EditFont f = e->font;
    double size = e->size;
    QColor color = e->color;
    apply(f, size, color);
    if (e->isNew) {  // what Add Text types in next looks like this too
      newTextFont_ = f;
      newTextSize_ = size;
      newTextColor_ = color;
    }
    v->restyleContentEdit(f, size, color);
  } else if (v->selectedContent(page, b) && b.kind == pdfz::ContentBlock::Kind::Text && b.editable) {
    EditFont f = editFontFor(b);
    double size = b.fontSize;
    QColor color = toQColor(b.color);
    apply(f, size, color);
    t->restyleContent(page, b, f, size, color);
  } else {
    apply(newTextFont_, newTextSize_, newTextColor_);
  }
  QSettings st;
  st.setValue("edit/font", newTextFont_.family);
  st.setValue("edit/bold", newTextFont_.bold);
  st.setValue("edit/italic", newTextFont_.italic);
  st.setValue("edit/size", newTextSize_);
  st.setValue("edit/color", newTextColor_.name());
  v->setNewTextStyle(newTextFont_, newTextSize_, newTextColor_);
  updateEditControls();
}

// The picture goes in the middle of what's showing of the page, at most three inches across,
// and is then selected, to move or resize.
void MainWindow::addImage() {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  if (!editAct_->isChecked()) editAct_->trigger();
  t->finishTextEditing();
  QSettings st;
  const QString dir = st.value("edit/imageDir", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
  const QString path = QFileDialog::getOpenFileName(this, tr("Add Image"), dir,
                                                    tr("Pictures (%1)").arg(imagePatterns().join(' ')));
  if (path.isEmpty()) return;
  st.setValue("edit/imageDir", QFileInfo(path).absolutePath());
  pdfz::Document::PageImage img;
  QString error;
  if (!loadPageImage(path, img, &error)) {
    QMessageBox::warning(this, tr("Add Image"), error.isEmpty() ? tr("Couldn't read that picture.") : error);
    return;
  }
  int page;
  pdfz::PointF c;
  if (!t->view()->visibleCenter(page, c)) return;
  const pdfz::SizeF ps = t->document()->pageSize(page);
  const double maxSide = std::min({216.0, ps.width * 0.9, ps.height * 0.9});
  const double k = maxSide / std::max(img.width, img.height);
  const double w = img.width * k, h = img.height * k;
  const double x = std::clamp(c.x - w / 2, 0.0, std::max(0.0, ps.width - w));
  const double y = std::clamp(c.y - h / 2, 0.0, std::max(0.0, ps.height - h));
  t->addContentImage(page, img, pdfz::RectF{x, y, x + w, y + h});
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
    if (a && a->kind != pdfz::AnnotKind::Stamp)
      menu.addAction(textBox ? tr("Edit Text") : tr("Edit Comment Text…"), this, [=] { t->editAnnotation(page, annotIndex); });
    if (a && isMark(*a)) {
      QMenu* colors = colorMenu(&menu, {}, [=](const QColor& c) { t->restyleAnnotation(page, annotIndex, StyleChange{.color = c}); });
      colors->setTitle(tr("Color"));
      menu.addMenu(colors);
    }
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
    menu.addAction(a && a->kind == pdfz::AnnotKind::Stamp ? tr("Delete") : tr("Delete Comment"), this,
                   [=] { t->deleteAnnotation(page, annotIndex); });
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

// ---------------------------------------------------------------------------------------------
// Fill & Sign

void MainWindow::fillSignMenu(QMenu* menu) {
  menu->clear();
  const QList<Signature> saved = SignatureStore::load();
  const qreal dpr = devicePixelRatioF();
  // Saved ones as themselves, big enough to tell apart (menu icons are tiny).
  auto addSaved = [&](bool initials) {
    for (const Signature& s : saved) {
      if (s.initials != initials) continue;
      const QSize box = initials ? QSize(70, 34) : QSize(170, 40);
      auto* button = new QToolButton(menu);
      button->setAutoRaise(true);
      button->setIconSize(box);
      button->setIcon(QIcon(QPixmap::fromImage(s.preview(box, dpr))));
      button->setToolTip(initials ? tr("Place these initials") : tr("Place this signature"));
      button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      auto* action = new QWidgetAction(menu);
      action->setDefaultWidget(button);
      connect(button, &QToolButton::clicked, this, [this, menu, s] {
        menu->close();
        sign(s, s.initials);
      });
      menu->addAction(action);
    }
  };
  addSaved(false);
  menu->addAction(tr("Add Signature…"), this, [this] { sign(std::nullopt, false); });
  menu->addSeparator();
  addSaved(true);
  menu->addAction(tr("Add Initials…"), this, [this] { sign(std::nullopt, true); });
  if (!saved.isEmpty()) {
    menu->addSeparator();
    QMenu* remove = menu->addMenu(tr("Remove Saved"));
    for (const Signature& s : saved) {
      QAction* a = remove->addAction(QIcon(QPixmap::fromImage(s.preview(QSize(100, 30), dpr))),
                                     s.initials ? tr("Initials") : tr("Signature"), this, [this, id = s.id] {
        SignatureStore::remove(id);
        statusBar()->showMessage(tr("Removed the saved signature"), 3000);
      });
      a->setIconVisibleInMenu(true);
    }
  }
}

std::optional<Signature> MainWindow::newSignature(bool initials) {
  SignatureDialog dlg(initials, this);
  if (dlg.exec() != QDialog::Accepted) return std::nullopt;
  Signature s = dlg.signature();
  if (s.isEmpty()) return std::nullopt;
  if (dlg.saveForLater()) SignatureStore::add(s);
  return s;
}

void MainWindow::sign(std::optional<Signature> sig, bool initials) {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  if (!sig) sig = newSignature(initials);
  if (!sig) {
    if (signAct_->isChecked()) selectToolAct_->trigger();
    return;
  }
  // Its usual size on the page: about a third of an inch tall for a signature, less for
  // initials, and never wider than a long signature line.
  const QSizeF size = sig->size();
  if (size.isEmpty()) return;
  double h = initials ? 20 : 30;
  double w = h * size.width() / size.height();
  const double maxW = initials ? 80 : 200;
  if (w > maxW) {
    h *= maxW / w;
    w = maxW;
  }
  pdfz::Annotation a;
  a.kind = pdfz::AnnotKind::Stamp;
  a.subject = initials ? "Initials" : "Signature";
  a.color = fromQColor(sig->color);
  const QImage preview = sig->preview(QSize(int(w * 4), int(h * 4)));
  signAct_->setChecked(true);
  t->view()->startPlacing(a, sig->drawing(), preview, QSizeF(w, h));
  t->view()->setFocus();
  statusBar()->showMessage(tr("Click where your %1 goes (Esc to cancel)").arg(initials ? tr("initials") : tr("signature")), 5000);
  updateStyleControls();
}

void MainWindow::signField(DocumentTab* t, int page, const pdfz::RectF& rect) {
  if (t != tab()) return;
  std::optional<Signature> sig;
  for (const Signature& s : SignatureStore::load())
    if (!s.initials) {
      sig = s;
      break;
    }
  if (!sig) sig = newSignature(false);
  if (!sig) return;
  // As big as fits in the field, keeping its shape, in the middle.
  const QSizeF size = sig->size();
  if (size.isEmpty() || rect.width() <= 0 || rect.height() <= 0) return;
  const double k = std::min(rect.width() / size.width(), rect.height() / size.height()) * 0.92;
  const double w = size.width() * k, h = size.height() * k;
  pdfz::Annotation a;
  a.subject = "Signature";
  a.color = fromQColor(sig->color);
  const double cx = (rect.left + rect.right) / 2, cy = (rect.top + rect.bottom) / 2;
  a.rect = {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2};
  t->addStamp(page, a, sig->drawing());
}

void MainWindow::saveFlattened() {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  t->finishTextEditing();
  QString base = t->path().isEmpty() ? lastDir() + "/Untitled.pdf" : t->path();
  base = QFileInfo(base).absolutePath() + "/" + QFileInfo(base).completeBaseName() + tr(" (flattened)") + ".pdf";
  QString path = QFileDialog::getSaveFileName(this, tr("Save as Flattened"), base, tr(kPdfFilter));
  if (path.isEmpty()) return;
  if (QFileInfo(path).suffix().isEmpty()) path += ".pdf";
  if (tabFor(QFileInfo(path).absoluteFilePath())) {
    QMessageBox::warning(this, tr("Save as Flattened"), tr("%1 is open in a tab. Close it first, or choose another name.")
                                                            .arg(QFileInfo(path).fileName()));
    return;
  }
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const bool ok = t->document()->saveFlattened(toFsPath(path));
  QApplication::restoreOverrideCursor();
  if (!ok) {
    QMessageBox::warning(this, tr("Save as Flattened"), tr("Couldn't save %1.").arg(QDir::toNativeSeparators(path)));
    return;
  }
  rememberDir(path);
  statusBar()->showMessage(tr("Saved a flattened copy as %1").arg(QFileInfo(path).fileName()), 5000);
}

// ---------------------------------------------------------------------------------------------
// Extract and split

namespace {
// `name` (without .pdf) in `dir`, made unique with " 2", " 3"... if something is there already.
QString freeFileName(const QString& dir, const QString& name) {
  QString path = dir + "/" + name + ".pdf";
  for (int i = 2; QFileInfo::exists(path); ++i) path = dir + "/" + name + QStringLiteral(" %1.pdf").arg(i);
  return path;
}
}  // namespace

void MainWindow::extractPages() {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  t->finishTextEditing();
  const int count = t->document()->pageCount();
  std::vector<int> chosen = t->thumbnails()->selectedPages();
  if (chosen.empty()) chosen.assign(1, t->view()->currentPage());

  QDialog dlg(this);
  dlg.setWindowTitle(tr("Extract Pages"));
  auto* lay = new QVBoxLayout(&dlg);
  auto* form = new QFormLayout;
  auto* range = new QLineEdit(describePages(chosen), &dlg);
  range->setToolTip(tr("Page numbers and ranges, e.g. 1-3, 5, 8-"));
  form->addRow(tr("Pages (of %1):").arg(count), range);
  lay->addLayout(form);
  auto* separate = new QCheckBox(tr("Each page as a file of its own"), &dlg);
  auto* remove = new QCheckBox(tr("Then delete them from this document"), &dlg);
  auto* open = new QCheckBox(tr("Open the new file"), &dlg);
  open->setChecked(true);
  connect(separate, &QCheckBox::toggled, open, [open](bool on) { open->setEnabled(!on); });
  lay->addWidget(separate);
  lay->addWidget(remove);
  lay->addWidget(open);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Extract…"));
  connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
    if (parsePages(range->text(), count).empty()) {
      QMessageBox::warning(&dlg, tr("Extract Pages"), tr("Type page numbers between 1 and %1, like 1-3, 5.").arg(count));
      return;
    }
    dlg.accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(buttons);
  if (dlg.exec() != QDialog::Accepted) return;
  const std::vector<int> pages = parsePages(range->text(), count);

  const QString base = t->path().isEmpty() ? tr("Untitled") : QFileInfo(t->path()).completeBaseName();
  const QString startDir = t->path().isEmpty() ? lastDir() : QFileInfo(t->path()).absolutePath();
  QStringList written;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  auto saveOne = [&](const std::vector<int>& which, const QString& path) {
    auto part = t->document()->extract(which);
    const bool ok = part && part->save(toFsPath(path));
    if (ok) written << path;
    return ok;
  };
  bool ok = true;
  if (separate->isChecked()) {
    QApplication::restoreOverrideCursor();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Extract Pages To"), startDir);
    if (dir.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (int p : pages)
      if (!(ok = saveOne(std::vector<int>(1, p), freeFileName(dir, tr("%1 (page %2)").arg(base).arg(p + 1))))) break;
  } else {
    QApplication::restoreOverrideCursor();
    const QString suggested = freeFileName(startDir, pages.size() == 1 ? tr("%1 (page %2)").arg(base).arg(pages[0] + 1)
                                                                        : tr("%1 (pages %2)").arg(base, describePages(pages)));
    QString path = QFileDialog::getSaveFileName(this, tr("Extract Pages"), suggested, tr(kPdfFilter));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += ".pdf";
    if (tabFor(QFileInfo(path).absoluteFilePath())) {
      QMessageBox::warning(this, tr("Extract Pages"), tr("%1 is open in a tab. Close it first, or choose another name.")
                                                          .arg(QFileInfo(path).fileName()));
      return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    ok = saveOne(pages, path);
  }
  QApplication::restoreOverrideCursor();
  if (!ok) {
    QMessageBox::warning(this, tr("Extract Pages"), tr("Couldn't save the extracted pages."));
    return;
  }
  if (!written.isEmpty()) rememberDir(written.front());
  statusBar()->showMessage(written.size() == 1 ? tr("Saved %1").arg(QFileInfo(written.front()).fileName())
                                               : tr("Saved %1 files").arg(written.size()), 5000);
  if (remove->isChecked()) {
    if (int(pages.size()) >= count)
      QMessageBox::information(this, tr("Extract Pages"), tr("Every page was extracted, so none were deleted here (a PDF needs a page)."));
    else
      t->applyEdit(tr("Delete Pages"), pages.front(), DocumentTab::Scope::Pages,
                   [&](pdfz::Document& d) { return d.deletePages(pages); });
  }
  if (open->isEnabled() && open->isChecked() && written.size() == 1) openFile(written.front());
}

void MainWindow::splitDocument() {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument()) return;
  t->finishTextEditing();
  const int count = t->document()->pageCount();
  if (count < 2) return;

  QDialog dlg(this);
  dlg.setWindowTitle(tr("Split Document"));
  auto* lay = new QVBoxLayout(&dlg);
  auto* form = new QFormLayout;
  auto* per = new QSpinBox(&dlg);
  per->setRange(1, count - 1);
  per->setValue(std::clamp(QSettings().value("split/pages", 1).toInt(), 1, count - 1));
  form->addRow(tr("Pages in each file:"), per);
  auto* result = new QLabel(&dlg);
  form->addRow(QString(), result);
  QString dir = t->path().isEmpty() ? lastDir() : QFileInfo(t->path()).absolutePath();
  auto* where = new QPushButton(QDir::toNativeSeparators(dir), &dlg);
  where->setToolTip(tr("The folder the files go in"));
  connect(where, &QPushButton::clicked, &dlg, [&] {
    const QString d = QFileDialog::getExistingDirectory(&dlg, tr("Split Into"), dir);
    if (d.isEmpty()) return;
    dir = d;
    where->setText(QDir::toNativeSeparators(d));
  });
  form->addRow(tr("Save in:"), where);
  lay->addLayout(form);
  const QString base = t->path().isEmpty() ? tr("Untitled") : QFileInfo(t->path()).completeBaseName();
  auto update = [&] {
    const int files = (count + per->value() - 1) / per->value();
    result->setText((files == 1 ? tr("1 file") : tr("%1 files").arg(files)) +
                    tr(": \"%1 (part 1).pdf\" and so on").arg(base));
  };
  connect(per, &QSpinBox::valueChanged, &dlg, update);
  update();
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Split"));
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(buttons);
  if (dlg.exec() != QDialog::Accepted) return;
  QSettings().setValue("split/pages", per->value());

  QApplication::setOverrideCursor(Qt::WaitCursor);
  int written = 0;
  bool ok = true;
  for (int start = 0, part = 1; start < count && ok; start += per->value(), ++part) {
    std::vector<int> pages;
    for (int p = start; p < std::min(count, start + per->value()); ++p) pages.push_back(p);
    auto doc = t->document()->extract(pages);
    ok = doc && doc->save(toFsPath(freeFileName(dir, tr("%1 (part %2)").arg(base).arg(part))));
    if (ok) ++written;
  }
  QApplication::restoreOverrideCursor();
  if (!ok) QMessageBox::warning(this, tr("Split Document"), tr("Couldn't save every part (%1 saved).").arg(written));
  else statusBar()->showMessage(tr("Split into %1 files in %2").arg(written).arg(QDir::toNativeSeparators(dir)), 6000);
}

// ---------------------------------------------------------------------------------------------
// Stamps

void MainWindow::fillStampMenu(QMenu* menu) {
  menu->clear();
  auto addPreview = [&](const QImage& preview, const QString& tip, std::function<void()> act) {
    auto* button = new QToolButton(menu);
    button->setAutoRaise(true);
    const QSize box(160, 34);
    QImage fitted(box * 2, QImage::Format_ARGB32_Premultiplied);
    fitted.setDevicePixelRatio(2);
    fitted.fill(Qt::transparent);
    {
      QPainter p(&fitted);
      p.setRenderHint(QPainter::SmoothPixmapTransform);
      const QSizeF s = QSizeF(preview.size()) / preview.devicePixelRatio();
      const double k = std::min((box.width() - 4) / s.width(), (box.height() - 4) / s.height());
      const QSizeF d = s * k;
      p.drawImage(QRectF(QPointF((box.width() - d.width()) / 2, (box.height() - d.height()) / 2), d), preview);
    }
    button->setIconSize(box);
    button->setIcon(QIcon(QPixmap::fromImage(fitted)));
    button->setToolTip(tip);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(button);
    connect(button, &QToolButton::clicked, this, [menu, act] {
      menu->close();
      act();
    });
    menu->addAction(action);
  };
  const QString author = commentAuthor();
  for (const StampStyle& st : standardStamps()) {
    const MadeStamp made = makeStamp(st, author);
    addPreview(made.preview, st.title, [this, st, author] { placeStamp(makeStamp(st, author), st.title); });
  }
  menu->addSeparator();
  const auto custom = CustomStamps::load();
  for (const auto& c : custom) {
    const MadeStamp made = makeImageStamp(c.image);
    addPreview(made.preview, tr("Your stamp"), [this, img = c.image] { placeStamp(makeImageStamp(img), tr("Stamp")); });
  }
  menu->addAction(tr("Add a Stamp from a Picture…"), this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, tr("Stamp Picture"), lastDir(),
                                                      tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff)"));
    if (path.isEmpty()) return;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull()) {
      QMessageBox::warning(this, tr("Stamp"), tr("That picture couldn't be read."));
      return;
    }
    CustomStamps::add(img);
    placeStamp(makeImageStamp(img), tr("Stamp"));
  });
  if (!custom.isEmpty()) {
    QMenu* remove = menu->addMenu(tr("Remove a Stamp of Yours"));
    for (const auto& c : custom) {
      QAction* a = remove->addAction(QIcon(QPixmap::fromImage(c.image.scaled(64, 32, Qt::KeepAspectRatio))), tr("Stamp"), this,
                                     [path = c.path] { CustomStamps::remove(path); });
      a->setIconVisibleInMenu(true);
    }
  }
}

void MainWindow::placeStamp(const MadeStamp& stamp, const QString& subject) {
  DocumentTab* t = tab();
  if (!t || !t->hasDocument() || stamp.width <= 0 || stamp.height <= 0) return;
  pdfz::Annotation a;
  a.kind = pdfz::AnnotKind::Stamp;
  a.subject = subject.toStdString();
  stampAct_->setChecked(true);
  t->view()->startPlacing(a, stamp.drawing, stamp.preview, QSizeF(stamp.width, stamp.height));
  t->view()->setFocus();
  statusBar()->showMessage(tr("Click where the stamp goes (Esc to cancel)"), 5000);
  updateStyleControls();
}
