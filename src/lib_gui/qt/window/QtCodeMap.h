#ifndef QT_CODE_MAP_H
#define QT_CODE_MAP_H

#include <map>

#include <QJsonObject>
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
// Every node carries a plain language note, and the question box asks Claude about the selection.
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

	void select(const QString& key, const QString& title, const FilePath& path);
	void showNote();
	QString groupOf(size_t fileIndex) const;
	QString relative(const FilePath& path) const;
	QString noteField(const QString& key, const QString& field) const;

	CodeMapData m_data;
	FilePath m_root;
	std::map<Id, size_t> m_fileIndex;

	QString m_notesPath;
	QJsonObject m_notes;
	QSet<QString> m_expanded;

	QString m_currentKey;
	FilePath m_currentPath;
	Id m_currentFileId = 0;

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

	QtThreadedLambdaFunctor m_onQtThread;
};

#endif	  // QT_CODE_MAP_H
