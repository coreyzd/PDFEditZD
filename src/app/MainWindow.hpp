// SPDX-License-Identifier: GPL-3.0-only
// The window: menus, toolbars, the find bar, the side panels and a tab per open document.
#pragma once

#include "DocumentView.hpp"
#include "Markup.hpp"
#include "Signatures.hpp"
#include "Stamps.hpp"

#include <QMainWindow>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace pdfz { class Document; }
class DocumentTab;
class FindBar;
class QAction;
class QActionGroup;
class QComboBox;
class QDockWidget;
class QLabel;
class QListWidget;
class QMenu;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QToolBar;
class QToolButton;

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  // Opens a PDF in a new tab (or shows its tab if it's already open).
  bool openFile(const QString& path);

protected:
  void closeEvent(QCloseEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

private:
  // Tabs
  DocumentTab* tab() const;  // the current one, or nullptr
  DocumentTab* tabFor(const QString& absolutePath) const;
  DocumentTab* newTab();
  void connectTab(DocumentTab* t);
  bool closeTab(DocumentTab* t);  // asks to save first; false if the user canceled
  void onCurrentTabChanged();
  void updateTabTitle(DocumentTab* t);

  void createActions();
  void createMenus();
  void createToolBars();
  QWidget* createWelcome();
  void updateUi();
  void updateRecentMenu();
  void addRecent(const QString& path);
  std::unique_ptr<pdfz::Document> loadWithPrompt(const QString& path);

  bool maybeSave(DocumentTab* t);
  bool save();
  bool saveAs();
  void print();

  // Pages
  void movePages(DocumentTab* t, const std::vector<int>& pages, int insertBefore);
  void deletePages();
  void insertBlankPage();
  void insertPagesFromFile();
  void rotatePages(int degrees);
  void extractPages();
  void splitDocument();

  void onCurrentPageChanged(int page);
  void onZoomChanged(double zoom, DocumentView::FitMode mode);
  void applyZoomText(const QString& text);

  // Text: copy and find
  void copySelection();
  void showFind();
  void restartSearch();
  void findStep(int direction);  // +1 next, -1 previous
  void onSearchMatches(DocumentTab* t, bool finished);
  void updateFindStatus();

  // Comments
  void applyTool();  // the checked tool, to the current tab's view
  // A style control was used: restyles the box being typed in, else the selected comment, else
  // the markup tool's style for what it makes next.
  void changeStyle(const StyleChange& change);
  void updateStyleControls();
  // Preset colors, "More Colors…", and first `noneText` (if not empty) picking an invalid color.
  QMenu* colorMenu(QWidget* parent, const QString& noneText, std::function<void(const QColor&)> pick);
  void showViewMenu(DocumentTab* t, const QPoint& globalPos, int page, int annotIndex);
  void markupSelection(MarkupTool tool);
  void setAuthor();

  // Fill & Sign
  void fillSignMenu(QMenu* menu);  // fills the Sign menu: saved signatures, add, remove
  // Places `sig` with the next click; asks for a new one if `sig` is empty.
  void sign(std::optional<Signature> sig, bool initials);
  // A signature field was clicked: the newest saved signature, fitted into it.
  void signField(DocumentTab* t, int page, const pdfz::RectF& rect);
  std::optional<Signature> newSignature(bool initials);  // the dialog; saved if asked
  void saveFlattened();
  void fillStampMenu(QMenu* menu);
  void placeStamp(const MadeStamp& stamp, const QString& subject);

  QStackedWidget* central_;
  QWidget* welcome_;
  QListWidget* welcomeRecent_;
  QTabWidget* tabs_;
  FindBar* findBar_;
  QDockWidget* thumbsDock_;
  QStackedWidget* thumbsStack_;
  QDockWidget* commentsDock_;
  QStackedWidget* commentsStack_;

  QAction* openAct_;
  QAction* saveAct_;
  QAction* saveAsAct_;
  QAction* printAct_;
  QAction* closeAct_;
  QAction* quitAct_;
  QAction* undoAct_;
  QAction* redoAct_;
  QAction* copyAct_;
  QAction* selectAllAct_;
  QAction* findAct_;
  QAction* findNextAct_;
  QAction* findPrevAct_;
  QAction* authorAct_;
  QAction* deletePagesAct_;
  QAction* insertBlankAct_;
  QAction* extractAct_;
  QAction* splitAct_;
  QAction* insertFileAct_;
  QAction* rotateCwAct_;
  QAction* rotateCcwAct_;
  QAction* zoomInAct_;
  QAction* zoomOutAct_;
  QAction* actualSizeAct_;
  QAction* fitPageAct_;
  QAction* fitWidthAct_;
  QAction* prevPageAct_;
  QAction* nextPageAct_;
  QAction* nextTabAct_;
  QAction* prevTabAct_;
  QAction* aboutAct_;
  QAction* saveFlatAct_;
  QAction* signAct_;
  QAction* stampAct_;
  QAction* highlightFieldsAct_;
  QToolBar* fillBar_;
  QMenu* signMenu_;
  QAction* selectToolAct_;
  QAction* handToolAct_;
  std::array<QAction*, kMarkupToolCount> markupActs_{};
  std::array<MarkupStyle, kMarkupToolCount> styles_;
  QAction* highlightSelAct_;
  QAction* underlineSelAct_;
  QAction* strikeSelAct_;
  QActionGroup* toolGroup_;
  QMenu* recentMenu_;
  QMenu* pagesMenu_;
  QMenu* viewMenu_;
  QToolBar* markupBar_;
  QToolButton* colorButton_;
  QToolButton* fillButton_;
  QComboBox* opacityCombo_;
  QComboBox* widthCombo_;
  QComboBox* fontCombo_;
  QComboBox* sizeCombo_;
  QToolButton* textColorButton_;
  std::array<QAction*, 3> alignActs_{};
  std::vector<QAction*> textControls_;  // shown for text boxes only

  QSpinBox* pageSpin_;
  QLabel* pageCountLabel_;
  QComboBox* zoomCombo_;
  QLabel* pageSizeLabel_;
};
