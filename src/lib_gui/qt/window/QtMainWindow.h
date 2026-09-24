#ifndef QT_MAIN_WINDOW_H
#define QT_MAIN_WINDOW_H

#include "FilePath.h"
#include "QtWindowStack.h"
#include "QtWindowTitleProgress.h"

#include <QMainWindow>

#include <memory>
#include <vector>

class Bookmark;
class MessageBase;
class QDockWidget;
class View;

class QtViewToggle: public QWidget
{
	Q_OBJECT

public:
	QtViewToggle(View* view, QWidget* parent = nullptr);
	void clear();

public Q_SLOTS:
	void toggledByAction();
	void toggledByUI();

private:
	View* m_view;
};


class MouseReleaseFilter: public QObject
{
	Q_OBJECT

public:
	MouseReleaseFilter(QObject* parent);

protected:
	bool eventFilter(QObject* obj, QEvent* event) override;

private:
	size_t m_backButton;
	size_t m_forwardButton;
};


class QtMainWindow: public QMainWindow
{
	Q_OBJECT

public:
	QtMainWindow();
	~QtMainWindow() override;

	void addView(View* view);
	void overrideView(View* view);
	void removeView(View* view);

	void showView(View* view);
	void hideView(View* view);

	View* findFloatingView(const std::string& name) const;

	void loadLayout();
	void saveLayout();

	void loadDockWidgetLayout();
	void loadWindow(bool showStartWindow);

	void updateHistoryMenu(std::shared_ptr<MessageBase> message);
	void clearHistoryMenu();

	void updateBookmarksMenu(const std::vector<std::shared_ptr<Bookmark>>& bookmarks);

	void setContentEnabled(bool enabled);
	void refreshStyle();

	void setWindowTitleProgress(size_t fileCount, size_t totalFileCount);
	void hideWindowTitleProgress();

	void alert();
	
Q_SIGNALS:
	void showScreenSearch();
	void hideScreenSearch();
	void hideIndexingDialog();

protected:
	void showEvent(QShowEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;
	void contextMenuEvent(QContextMenuEvent* event) override;
	void closeEvent(QCloseEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;

	bool focusNextPrevChild(bool next) override;

public Q_SLOTS:
	void about();
	void openSettings();
	static void showChangelog();
	static void showBugtracker();
	static void showDocumentation();
	void showKeyboardShortcuts();
	static void showLegend();

	static void showErrorHelpMessage();
	void showLicenses();

	static void showDataFolder();
	static void showLogFolder();

	static void openTab();
	static void closeTab();
	static void nextTab();
	static void previousTab();

	void showStartScreen();
	void hideStartScreen();

	void newProject();
	void newProjectFromCDB(const FilePath& filePath);
	void openProject();
	void editProject();
	void closeProject();

	static void find();
	static void findFulltext();
	void findOnScreen();
	static void codeReferencePrevious();
	static void codeReferenceNext();
	static void codeLocalReferencePrevious();
	static void codeLocalReferenceNext();
	static void customTrail();
	static void overview();

	static void closeWindow();
	static void refresh();
	static void forceRefresh();

	static void undo();
	static void redo();
	static void zoomIn();
	static void zoomOut();
	static void resetZoom();

	void resetWindowLayout();

	void openRecentProject();
	void updateRecentProjectsMenu();

	void toggleView(View* view, bool fromMenu);
	void saveAsImage();

private Q_SLOTS:
	void toggleShowDockWidgetTitleBars();

	static void showBookmarkCreator();
	static void showBookmarkBrowser();

	void openHistoryAction();
	void activateBookmarkAction();

private:
	struct DockWidget
	{
		QDockWidget* widget;
		View* view;
		QAction* action;
		QtViewToggle* toggle;
	};

	void setupEditMenu();
	void setupProjectMenu();
	void setupViewMenu();
	void setupCodeMap();
	void setupHistoryMenu();
	void setupBookmarksMenu();
	void setupHelpMenu();

	DockWidget* getDockWidgetForView(View* view);

	void setShowDockWidgetTitleBars(bool showTitleBars);

	template <typename T>
	T* createWindow();

	std::vector<DockWidget> m_dockWidgets;

	QMenu* m_viewMenu;
	QAction* m_viewSeparator;

	QMenu* m_historyMenu = nullptr;
	std::vector<std::shared_ptr<MessageBase>> m_history;

	QMenu* m_bookmarksMenu = nullptr;
	std::vector<std::shared_ptr<Bookmark>> m_bookmarks;

	QMenu* m_recentProjectsMenu;

	QAction* m_showTitleBarsAction;

	bool m_showDockWidgetTitleBars = true;

	QtWindowStack m_windowStack;

	QtWindowTitleProgress m_windowTitleProgress;
};

#endif	  // QT_MAIN_WINDOW_H
