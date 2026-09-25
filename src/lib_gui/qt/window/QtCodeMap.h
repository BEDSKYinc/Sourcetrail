#ifndef QT_CODE_MAP_H
#define QT_CODE_MAP_H

#include <map>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPen>
#include <QSet>
#include <QString>
#include <QWidget>

#include "CodeMapData.h"
#include "FilePath.h"
#include "MessageActivateTokens.h"
#include "MessageIndexingFinished.h"
#include "MessageListener.h"
#include "QtThreadedFunctor.h"

class QComboBox;
class QGraphicsRectItem;
class QGraphicsScene;
class QLabel;
class QLineEdit;
class QProcess;
class QPushButton;
class QTextBrowser;
class QTextEdit;
class QTimer;
class QGraphicsView;

// Map of the whole index: directories or features as groups, files as nodes inside them.
// Every node carries a plain language note, and the chat box asks Claude about the selection.
class QtCodeMap
	: public QWidget
	, public MessageListener<MessageActivateTokens>
	, public MessageListener<MessageIndexingFinished>
{
	Q_OBJECT

public:
	QtCodeMap(QWidget* parent = nullptr);
	~QtCodeMap() override;

	void refresh();

	// called by the scene items
	void toggleGroup(const QString& group);
	void activateFile(Id fileNodeId);
	void nodeMoved(const QString& layoutKey, const QPointF& pos);

private Q_SLOTS:
	void rebuild();
	void saveNote();
	void ask();
	void askFinished();

protected:
	void showEvent(QShowEvent* event) override;

private:
	void handleMessage(MessageActivateTokens* message) override;
	void handleMessage(MessageIndexingFinished* message) override;

	void select(const QString& key, const QString& title, const FilePath& path, Id fileNodeId);
	void showNote();
	QString groupOf(size_t fileIndex) const;
	QString relative(const FilePath& path) const;
	QString noteField(const QString& key, const QString& field) const;

	// notes on disk can change under an open map, so they are re-read before every write
	void reloadNotesIfChanged();
	void writeNotes();

	// renamed files keep their note: git is asked what became of a note without a file
	void followRenames();

	// hand placed nodes, per grouping mode, next to the notes
	QString layoutMode() const;
	QJsonObject layoutOfMode() const;
	void saveLayout();

	// the node Sourcetrail is looking at, expanded and centred
	void highlight(Id fileNodeId);
	void zoomBy(double factor);
	void zoomFit();

	QString mcpConfig() const;
	void appendChat(const QString& who, const QString& text, const QString& color);

	CodeMapData m_data;
	FilePath m_root;
	std::map<Id, size_t> m_fileIndex;

	QString m_notesPath;
	QJsonObject m_notes;
	QDateTime m_notesStamp;
	qint64 m_notesSize = -1;
	QString m_loadedText;
	QString m_loadedFeature;
	QSet<QString> m_expanded;

	QString m_layoutPath;
	QJsonObject m_layout;

	// hand written pipeline order for the feature view: one row per stage, import first
	QJsonArray m_stages;

	QString m_currentKey;
	FilePath m_currentPath;
	Id m_currentFileId = 0;

	std::map<Id, QGraphicsRectItem*> m_nodeItems;
	QGraphicsRectItem* m_highlighted = nullptr;
	QPen m_highlightedPen;

	QComboBox* m_grouping;
	QLabel* m_stats;
	QGraphicsScene* m_scene;
	QGraphicsView* m_view;

	QLabel* m_title;
	QLineEdit* m_feature;
	QTextEdit* m_note;
	QTimer* m_noteTimer;
	QLineEdit* m_question;
	QPushButton* m_askButton;
	QTextBrowser* m_answer;
	QProcess* m_ask = nullptr;
	QString m_chatSession;
	QString m_chatLog;
	QString m_chatContextKey;

	QtThreadedLambdaFunctor m_onQtThread;
};

#endif	  // QT_CODE_MAP_H
