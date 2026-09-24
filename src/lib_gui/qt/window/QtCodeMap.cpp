#include "QtCodeMap.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <QBoxLayout>
#include <QComboBox>
#include <QFile>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QWheelEvent>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>

#include "Application.h"
#include "GraphViewStyle.h"
#include "MessageActivateFile.h"
#include "StorageAccess.h"

namespace
{
const int kHeader = 24;
const int kFileW = 190;
const int kFileH = 22;
const int kGap = 6;
const int kPad = 10;
const int kGroupGap = 40;
const int kLayerGap = 70;
// A layer with many groups is wrapped here, otherwise one row runs off to the right forever.
const qreal kRowWidth = 2400;

QColor color(const std::string& c)
{
	return QColor(QString::fromStdString(c));
}

QString groupOfPath(const QString& rel)
{
	const QStringList parts = rel.split(QLatin1Char('/'));
	if (parts.size() > 2)
	{
		return parts[0] + QLatin1Char('/') + parts[1];
	}
	return parts.size() > 1 ? parts[0] : QStringLiteral(".");
}

// Layers the groups top down. Cycles are broken at their least used member, otherwise a pair of
// groups that use each other would push the whole map into an endless staircase.
std::map<QString, int> layerGroups(
	const QStringList& groups, const std::map<std::pair<QString, QString>, size_t>& deps)
{
	std::map<QString, std::set<QString>> out, in;
	for (const auto& dep: deps)
	{
		out[dep.first.first].insert(dep.first.second);
		in[dep.first.second].insert(dep.first.first);
	}

	std::set<QString> rest(groups.begin(), groups.end());
	QStringList order;
	while (!rest.empty())
	{
		QStringList ready;
		for (const QString& name: rest)
		{
			std::set<QString> open;
			for (const QString& source: in[name])
			{
				if (rest.count(source))
				{
					open.insert(source);
				}
			}
			if (open.empty())
			{
				ready.append(name);
			}
		}

		if (ready.isEmpty())
		{
			QString weakest;
			size_t weakestCount = 0;
			for (const QString& name: rest)
			{
				size_t count = 0;
				for (const QString& source: in[name])
				{
					auto it = deps.find({source, name});
					if (rest.count(source) && it != deps.end())
					{
						count += it->second;
					}
				}
				if (weakest.isEmpty() || count < weakestCount)
				{
					weakest = name;
					weakestCount = count;
				}
			}
			ready.append(weakest);
		}

		for (const QString& name: ready)
		{
			rest.erase(name);
		}
		order.append(ready);
	}

	std::map<QString, int> rank, layer;
	for (int i = 0; i < order.size(); i++)
	{
		rank[order[i]] = i;
		layer[order[i]] = 0;
	}
	for (const QString& source: order)
	{
		for (const QString& target: out[source])
		{
			if (rank.count(target) && rank[target] > rank[source])
			{
				layer[target] = std::max(layer[target], layer[source] + 1);
			}
		}
	}
	return layer;
}

class MapItem: public QGraphicsRectItem
{
public:
	MapItem(QtCodeMap* map, const QString& group, Id fileId)
		: m_map(map), m_group(group), m_fileId(fileId)
	{
	}

protected:
	void mousePressEvent(QGraphicsSceneMouseEvent* event) override
	{
		if (m_fileId)
		{
			m_map->activateFile(m_fileId);
		}
		else if (event->pos().y() < kHeader)
		{
			m_map->toggleGroup(m_group);
		}
		event->accept();
	}

private:
	QtCodeMap* m_map;
	QString m_group;
	Id m_fileId;
};

class MapView: public QGraphicsView
{
public:
	using QGraphicsView::QGraphicsView;

protected:
	void wheelEvent(QWheelEvent* event) override
	{
		if (event->modifiers() & Qt::ControlModifier)
		{
			const double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
			scale(factor, factor);
			event->accept();
			return;
		}
		QGraphicsView::wheelEvent(event);
	}
};
}	 // namespace

QtCodeMap::QtCodeMap(QWidget* parent): QWidget(parent)
{
	QHBoxLayout* layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
	layout->addWidget(splitter);

	// map side
	QWidget* left = new QWidget(splitter);
	QVBoxLayout* leftLayout = new QVBoxLayout(left);
	leftLayout->setContentsMargins(4, 4, 4, 4);

	QHBoxLayout* toolbar = new QHBoxLayout();
	toolbar->addWidget(new QLabel(QStringLiteral("Gruppierung:"), left));
	m_grouping = new QComboBox(left);
	m_grouping->addItem(QStringLiteral("Verzeichnis"));
	m_grouping->addItem(QStringLiteral("Feature"));
	toolbar->addWidget(m_grouping);

	QPushButton* collapse = new QPushButton(QStringLiteral("Alle einklappen"), left);
	toolbar->addWidget(collapse);
	QPushButton* reload = new QPushButton(QStringLiteral("Neu laden"), left);
	toolbar->addWidget(reload);
	m_stats = new QLabel(left);
	toolbar->addWidget(m_stats);
	toolbar->addStretch();
	leftLayout->addLayout(toolbar);

	m_scene = new QGraphicsScene(this);
	m_view = new MapView(m_scene, left);
	m_view->setRenderHint(QPainter::Antialiasing);
	m_view->setDragMode(QGraphicsView::ScrollHandDrag);
	leftLayout->addWidget(m_view);

	// note and question side
	QWidget* right = new QWidget(splitter);
	QVBoxLayout* rightLayout = new QVBoxLayout(right);
	rightLayout->setContentsMargins(6, 6, 6, 6);

	m_title = new QLabel(QStringLiteral("Nichts ausgewählt"), right);
	m_title->setWordWrap(true);
	m_title->setStyleSheet(QStringLiteral("font-weight: bold;"));
	rightLayout->addWidget(m_title);

	rightLayout->addWidget(new QLabel(QStringLiteral("Feature"), right));
	m_feature = new QLineEdit(right);
	rightLayout->addWidget(m_feature);

	rightLayout->addWidget(new QLabel(QStringLiteral("Beschreibung"), right));
	m_note = new QTextEdit(right);
	m_note->setAcceptRichText(false);
	rightLayout->addWidget(m_note, 2);

	rightLayout->addWidget(new QLabel(QStringLiteral("Frage zur Auswahl"), right));
	QHBoxLayout* askLayout = new QHBoxLayout();
	m_question = new QLineEdit(right);
	m_question->setPlaceholderText(QStringLiteral("Was macht das hier?"));
	askLayout->addWidget(m_question);
	m_askButton = new QPushButton(QStringLiteral("Fragen"), right);
	askLayout->addWidget(m_askButton);
	rightLayout->addLayout(askLayout);

	m_answer = new QTextBrowser(right);
	rightLayout->addWidget(m_answer, 3);

	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 1);

	m_noteTimer = new QTimer(this);
	m_noteTimer->setSingleShot(true);
	m_noteTimer->setInterval(1200);

	connect(m_grouping, &QComboBox::currentIndexChanged, this, &QtCodeMap::rebuild);
	connect(collapse, &QPushButton::clicked, this, [this]() {
		m_expanded.clear();
		rebuild();
	});
	connect(reload, &QPushButton::clicked, this, &QtCodeMap::refresh);
	connect(m_noteTimer, &QTimer::timeout, this, &QtCodeMap::saveNote);
	connect(m_note, &QTextEdit::textChanged, this, [this]() { m_noteTimer->start(); });
	connect(m_feature, &QLineEdit::textEdited, this, [this]() { m_noteTimer->start(); });
	connect(m_askButton, &QPushButton::clicked, this, &QtCodeMap::ask);
	connect(m_question, &QLineEdit::returnPressed, this, &QtCodeMap::ask);
}

QtCodeMap::~QtCodeMap()
{
	saveNote();
}

void QtCodeMap::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	if (m_data.files.empty())
	{
		refresh();
	}
}

void QtCodeMap::refresh()
{
	StorageAccess* storage = Application::getInstance() ? Application::getInstance()->getStorageAccess()
													   : nullptr;
	if (!storage)
	{
		return;
	}

	m_data = storage->getCodeMap();
	m_fileIndex.clear();
	for (size_t i = 0; i < m_data.files.size(); i++)
	{
		m_fileIndex.emplace(m_data.files[i].nodeId, i);
	}

	// common directory of all indexed files, so the map shows short relative paths
	std::string root;
	for (const CodeMapData::File& file: m_data.files)
	{
		const std::string path = file.path.str();
		if (root.empty())
		{
			root = path;
			continue;
		}
		size_t i = 0;
		while (i < root.size() && i < path.size() && root[i] == path[i])
		{
			i++;
		}
		root.resize(i);
	}
	const size_t slash = root.rfind('/');
	m_root = FilePath(slash == std::string::npos ? root : root.substr(0, slash));

	const FilePath projectPath = Application::getInstance()->getCurrentProjectPath();
	m_notesPath = QString::fromStdString(
		projectPath.getParentDirectory().concatenate(FilePath("codemap-notes.json")).str());
	m_notes = QJsonObject();
	QFile notes(m_notesPath);
	if (notes.open(QIODevice::ReadOnly))
	{
		m_notes = QJsonDocument::fromJson(notes.readAll()).object();
	}

	m_expanded.clear();
	rebuild();
	m_view->fitInView(m_scene->sceneRect(), Qt::KeepAspectRatio);
}

QString QtCodeMap::relative(const FilePath& path) const
{
	QString full = QString::fromStdString(path.str());
	const QString root = QString::fromStdString(m_root.str()) + QLatin1Char('/');
	if (full.startsWith(root))
	{
		full = full.mid(root.size());
	}
	return full;
}

QString QtCodeMap::noteField(const QString& key, const QString& field) const
{
	return m_notes.value(key).toObject().value(field).toString();
}

QString QtCodeMap::groupOf(size_t fileIndex) const
{
	const QString rel = relative(m_data.files[fileIndex].path);
	if (m_grouping->currentIndex() == 1)
	{
		const QString feature = noteField(QStringLiteral("file:") + rel, QStringLiteral("feature"));
		return feature.isEmpty() ? QStringLiteral("(ohne Feature)") : feature;
	}
	return groupOfPath(rel);
}

void QtCodeMap::rebuild()
{
	m_scene->clear();

	std::map<QString, std::vector<size_t>> groups;
	std::map<Id, QString> fileGroup;
	for (size_t i = 0; i < m_data.files.size(); i++)
	{
		const QString group = groupOf(i);
		groups[group].push_back(i);
		fileGroup[m_data.files[i].nodeId] = group;
	}

	std::map<std::pair<QString, QString>, size_t> deps;
	for (const auto& dep: m_data.dependencies)
	{
		auto source = fileGroup.find(dep.first.first);
		auto target = fileGroup.find(dep.first.second);
		if (source != fileGroup.end() && target != fileGroup.end() && source->second != target->second)
		{
			deps[{source->second, target->second}] += dep.second;
		}
	}

	QStringList names;
	for (const auto& group: groups)
	{
		names.append(group.first);
	}
	if (m_expanded.isEmpty())
	{
		// first build: show everything, that is the point of a map
		for (const QString& name: names)
		{
			m_expanded.insert(name);
		}
	}

	const std::map<QString, int> layers = layerGroups(names, deps);

	const GraphViewStyle::NodeColor groupColor = GraphViewStyle::getNodeColor("namespace", false);
	const GraphViewStyle::NodeColor fileColor = GraphViewStyle::getNodeColor("file", false);
	const GraphViewStyle::NodeColor noteColor = GraphViewStyle::getNodeColor("file", true);

	// size and place every group, one row per layer
	std::map<QString, QRectF> rects;
	std::map<int, QStringList> byLayer;
	for (const QString& name: names)
	{
		byLayer[layers.at(name)].append(name);
	}

	qreal y = 0;
	size_t documented = 0;
	for (const auto& layer: byLayer)
	{
		qreal x = 0;
		qreal rowHeight = 0;
		for (const QString& name: layer.second)
		{
			const std::vector<size_t>& files = groups[name];
			const bool expanded = m_expanded.contains(name);

			int cols = 1;
			int rows = 0;
			qreal w = kFileW + 2 * kPad;
			qreal h = kHeader + kPad;
			if (expanded)
			{
				cols = std::min<int>(8, std::max<int>(1, static_cast<int>(std::ceil(std::sqrt(files.size())))));
				rows = static_cast<int>((files.size() + cols - 1) / cols);
				w = 2 * kPad + cols * kFileW + (cols - 1) * kGap;
				h = kHeader + kPad + rows * (kFileH + kGap);
			}

			if (x > 0 && x + w > kRowWidth)
			{
				x = 0;
				y += rowHeight + kGap;
				rowHeight = 0;
			}

			MapItem* item = new MapItem(this, name, 0);
			item->setRect(0, 0, w, h);
			item->setPos(x, y);
			item->setBrush(color(groupColor.fill));
			item->setPen(QPen(color(groupColor.border), 1));
			item->setZValue(1);
			m_scene->addItem(item);

			QGraphicsSimpleTextItem* title = new QGraphicsSimpleTextItem(
				name + QStringLiteral("  (%1)").arg(files.size()), item);
			title->setBrush(color(groupColor.text));
			QFont titleFont = title->font();
			titleFont.setBold(true);
			title->setFont(titleFont);
			title->setPos(kPad, 5);

			if (expanded)
			{
				for (size_t i = 0; i < files.size(); i++)
				{
					const CodeMapData::File& file = m_data.files[files[i]];
					const QString rel = relative(file.path);
					const QString key = QStringLiteral("file:") + rel;
					const bool hasNote = !noteField(key, QStringLiteral("text")).isEmpty();
					if (hasNote)
					{
						documented++;
					}

					MapItem* node = new MapItem(this, name, file.nodeId);
					node->setRect(0, 0, kFileW, kFileH);
					node->setPos(
						kPad + (i % cols) * (kFileW + kGap),
						kHeader + kPad / 2 + (i / cols) * (kFileH + kGap));
					node->setBrush(color(hasNote ? noteColor.fill : fileColor.fill));
					node->setPen(QPen(color(hasNote ? noteColor.border : fileColor.border), 1));
					node->setParentItem(item);
					node->setToolTip(rel + QStringLiteral("\n%1 Symbole").arg(file.symbolCount));

					QString label = rel.section(QLatin1Char('/'), -1);
					QGraphicsSimpleTextItem* text = new QGraphicsSimpleTextItem(label, node);
					text->setBrush(color(hasNote ? noteColor.text : fileColor.text));
					if (text->boundingRect().width() > kFileW - 10)
					{
						text->setText(
							text->text().left(std::max(4, static_cast<int>((kFileW - 14) / 6))) +
							QStringLiteral("…"));
					}
					text->setPos(6, 3);
				}
			}

			rects[name] = QRectF(x, y, w, h);
			x += w + kGroupGap;
			rowHeight = std::max(rowHeight, h);
		}
		y += rowHeight + kLayerGap;
	}

	// dependency curves behind the groups
	size_t maxCount = 1;
	for (const auto& dep: deps)
	{
		maxCount = std::max(maxCount, dep.second);
	}
	for (const auto& dep: deps)
	{
		const QRectF source = rects[dep.first.first];
		const QRectF target = rects[dep.first.second];
		const QPointF from(source.center().x(), source.bottom());
		const QPointF to(target.center().x(), target.top());

		QPainterPath path(from);
		const qreal bend = std::max<qreal>(40, std::abs(to.y() - from.y()) / 2);
		path.cubicTo(from + QPointF(0, bend), to - QPointF(0, bend), to);

		QGraphicsPathItem* curve = m_scene->addPath(path);
		QColor edgeColor = color(GraphViewStyle::getEdgeColor("call"));
		edgeColor.setAlpha(120);
		curve->setPen(QPen(
			edgeColor, 1.0 + 3.0 * std::log(1.0 + dep.second) / std::log(1.0 + maxCount),
			Qt::SolidLine, Qt::RoundCap));
		curve->setZValue(0);
		curve->setToolTip(QStringLiteral("%1 → %2: %3 Verbindungen")
							  .arg(dep.first.first, dep.first.second)
							  .arg(dep.second));
	}

	m_scene->setSceneRect(m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40));
	m_stats->setText(QStringLiteral("%1 Dateien, %2 Gruppen, %3 beschrieben")
						 .arg(m_data.files.size())
						 .arg(names.size())
						 .arg(documented));
}

void QtCodeMap::toggleGroup(const QString& group)
{
	if (m_expanded.contains(group))
	{
		m_expanded.remove(group);
	}
	else
	{
		m_expanded.insert(group);
	}
	rebuild();
}

void QtCodeMap::activateFile(Id fileNodeId)
{
	auto it = m_fileIndex.find(fileNodeId);
	if (it == m_fileIndex.end())
	{
		return;
	}

	const CodeMapData::File& file = m_data.files[it->second];
	const QString rel = relative(file.path);
	select(QStringLiteral("file:") + rel, rel, file.path);

	MessageActivateFile(file.path).dispatch();
}

void QtCodeMap::select(const QString& key, const QString& title, const FilePath& path)
{
	saveNote();

	m_currentKey = key;
	m_currentPath = path;
	m_title->setText(title);
	showNote();
}

void QtCodeMap::showNote()
{
	const bool blocked = m_note->blockSignals(true);
	m_note->setPlainText(noteField(m_currentKey, QStringLiteral("text")));
	m_note->blockSignals(blocked);
	m_feature->setText(noteField(m_currentKey, QStringLiteral("feature")));
}

void QtCodeMap::saveNote()
{
	if (m_currentKey.isEmpty() || m_notesPath.isEmpty())
	{
		return;
	}

	const QString text = m_note->toPlainText();
	const QString feature = m_feature->text();
	if (text == noteField(m_currentKey, QStringLiteral("text")) &&
		feature == noteField(m_currentKey, QStringLiteral("feature")))
	{
		return;
	}

	if (text.isEmpty() && feature.isEmpty())
	{
		m_notes.remove(m_currentKey);
	}
	else
	{
		QJsonObject note;
		note[QStringLiteral("feature")] = feature;
		note[QStringLiteral("text")] = text;
		m_notes[m_currentKey] = note;
	}

	QFile file(m_notesPath);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		file.write(QJsonDocument(m_notes).toJson(QJsonDocument::Indented));
	}
}

void QtCodeMap::ask()
{
	if (m_ask || m_question->text().isEmpty())
	{
		return;
	}

	const QString what = m_currentKey.isEmpty() ? QStringLiteral("das Projekt") : m_title->text();
	const QString prompt = QStringLiteral(
							   "Frage zu %1 in dieser Codebase: %2\n"
							   "Lies die Datei, bevor du antwortest.")
							   .arg(what, m_question->text());

	m_ask = new QProcess(this);
	m_ask->setWorkingDirectory(QString::fromStdString(m_root.str()));
	m_ask->setProcessChannelMode(QProcess::MergedChannels);
	connect(m_ask, &QProcess::finished, this, &QtCodeMap::askFinished);

	m_askButton->setEnabled(false);
	m_answer->setPlainText(QStringLiteral("Claude denkt nach …"));
	m_ask->start(
		QStringLiteral("claude"),
		{QStringLiteral("-p"),
		 prompt,
		 QStringLiteral("--allowedTools"),
		 QStringLiteral("Read,Grep,Glob"),
		 QStringLiteral("--append-system-prompt"),
		 QStringLiteral("Antworte auf Deutsch, in einfacher Sprache, höchstens 12 Zeilen.")});
}

void QtCodeMap::askFinished()
{
	const QString output = QString::fromUtf8(m_ask->readAll()).trimmed();
	if (m_ask->exitStatus() != QProcess::NormalExit || m_ask->exitCode() != 0)
	{
		m_answer->setPlainText(
			output + QStringLiteral("\n\n(claude endete mit %1 – einmal `claude` im Terminal "
									"starten und einloggen.)")
						 .arg(m_ask->exitCode()));
	}
	else
	{
		m_answer->setPlainText(output);
	}

	m_ask->deleteLater();
	m_ask = nullptr;
	m_askButton->setEnabled(true);
}

void QtCodeMap::handleMessage(MessageActivateTokens* message)
{
	std::vector<Id> tokenIds = message->tokenIds;
	std::vector<SearchMatch> matches = message->getSearchMatches();

	m_onQtThread([this, tokenIds, matches]() {
		if (matches.empty() || tokenIds.empty())
		{
			return;
		}

		const NameHierarchy& name = matches[0].tokenNames.empty() ? NameHierarchy(NameDelimiterType::UNKNOWN)
																  : matches[0].tokenNames[0];
		const QString title = QString::fromStdString(name.getQualifiedName());

		auto it = m_fileIndex.find(tokenIds[0]);
		if (it != m_fileIndex.end())
		{
			const CodeMapData::File& file = m_data.files[it->second];
			const QString rel = relative(file.path);
			select(QStringLiteral("file:") + rel, rel, file.path);
			return;
		}

		// a symbol: note it under its own name, but point the question at its file
		FilePath path;
		StorageAccess* storage = Application::getInstance()->getStorageAccess();
		for (const auto& parent: storage->getNodeIdToParentFileMap({tokenIds[0]}))
		{
			path = FilePath(parent.second.second.getQualifiedName());
		}
		select(QStringLiteral("sym:") + title, title, path);
	});
}

void QtCodeMap::handleMessage(MessageIndexingFinished*  /*message*/)
{
	m_onQtThread([this]() { refresh(); });
}
