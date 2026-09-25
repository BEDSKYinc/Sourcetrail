#include "QtCodeMap.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <QBoxLayout>
#include <QCoreApplication>
#include <QComboBox>
#include <QFile>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QHash>
#include <QWheelEvent>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QScrollBar>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>

#include "Application.h"
#include "GraphViewStyle.h"
#include "logging.h"
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

// Only a group's real ties get a curve: everything under this share of its strongest one is
// dropped. Drawing all of them was the reason the feature view read as a hairball — 228 curves
// between 25 groups, 4982 crossings, and no ordering can fix that many. At a fifth the map keeps
// three quarters of all connections in 64 curves and 138 crossings. Fading the weak ones instead
// of dropping them did not work: a whisper is still a line, and there were two hundred of them.
const qreal kCurveShare = 0.20;

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
	// Only a group's strong dependencies decide where it sits. One below a fraction of the
	// group's strongest one may not push anything down a layer: grouping by feature connects
	// almost every pair, and taking all of them turns the map into a fourteen step staircase.
	// The weak edges are still drawn, they just do not move anybody.
	std::map<QString, size_t> strongest;
	for (const auto& dep: deps)
	{
		strongest[dep.first.first] = std::max(strongest[dep.first.first], dep.second);
	}
	std::map<std::pair<QString, QString>, size_t> strong;
	for (const auto& dep: deps)
	{
		if (dep.second * 100 >= strongest[dep.first.first] * 15)
		{
			strong.insert(dep);
		}
	}

	std::map<QString, std::set<QString>> out, in;
	for (const auto& dep: strong)
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
					auto it = strong.find({source, name});
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

// Sorts the groups inside a layer so the connected ones stand next to each other.
//
// The layering above only says how deep a group sits; left to right it was the order the groups
// came out of the map, which is alphabetical. That is why the feature view looked like a hairball:
// two features that only talk to each other could sit at opposite ends of a row with their curve
// crossing everything in between, and nothing about the picture said they belonged together.
//
// These are the usual barycentre sweeps. Every group moves to the average place of what it is
// connected to, weighted by how many connections that is, looking up on one pass and down on the
// next so both ends of a curve get a say. Six passes settle a map this size. Positions are kept
// normalised to 0..1 because layers hold anywhere from one to a dozen groups, and a raw index
// would make the third group of a short row look far left of the third group of a long one.
void orderLayers(
	std::map<int, QStringList>& byLayer,
	const std::map<QString, int>& layers,
	const std::map<std::pair<QString, QString>, size_t>& deps)
{
	std::map<QString, double> place;
	auto renumber = [&place](const QStringList& row) {
		for (qsizetype i = 0; i < row.size(); i++)
		{
			place[row[i]] = (i + 0.5) / row.size();
		}
	};
	for (const auto& layer: byLayer)
	{
		renumber(layer.second);
	}

	// A group's neighbours, split by whether they sit above it or below it. Both directions are
	// kept: a group with only outgoing edges would otherwise never move on the upward pass.
	std::map<QString, std::vector<std::pair<QString, size_t>>> above, below;
	for (const auto& dep: deps)
	{
		const QString& source = dep.first.first;
		const QString& target = dep.first.second;
		const int sourceLayer = layers.at(source);
		const int targetLayer = layers.at(target);
		if (sourceLayer < targetLayer)
		{
			above[target].push_back({source, dep.second});
			below[source].push_back({target, dep.second});
		}
		else if (targetLayer < sourceLayer)
		{
			above[source].push_back({target, dep.second});
			below[target].push_back({source, dep.second});
		}
		// same layer: neither one can pull the other sideways, the curve just stays flat
	}

	for (int pass = 0; pass < 6; pass++)
	{
		const auto& side = (pass % 2 == 0) ? above : below;
		for (auto& layer: byLayer)
		{
			std::map<QString, double> bary;
			for (const QString& name: layer.second)
			{
				auto it = side.find(name);
				double sum = 0;
				double weight = 0;
				if (it != side.end())
				{
					for (const auto& neighbour: it->second)
					{
						sum += place[neighbour.first] * neighbour.second;
						weight += neighbour.second;
					}
				}
				// nothing on this side: stay put rather than drift to the left edge
				bary[name] = weight > 0 ? sum / weight : place[name];
			}
			std::stable_sort(
				layer.second.begin(),
				layer.second.end(),
				[&bary](const QString& a, const QString& b) { return bary[a] < bary[b]; });
			renumber(layer.second);
		}
	}
}

// A node the user can drag. Everything else about it is a plain rect item; the only care needed
// is telling a click apart from a drag, because both start with the same press.
class MapItem: public QGraphicsRectItem
{
public:
	MapItem(QtCodeMap* map, const QString& group, Id fileId, const QString& layoutKey)
		: m_map(map), m_group(group), m_fileId(fileId), m_layoutKey(layoutKey)
	{
		setFlag(QGraphicsItem::ItemIsMovable, true);
		setCursor(Qt::OpenHandCursor);
	}

protected:
	void mousePressEvent(QGraphicsSceneMouseEvent* event) override
	{
		m_pressedAt = event->scenePos();
		m_moved = false;
		QGraphicsRectItem::mousePressEvent(event);
	}

	void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override
	{
		if ((event->scenePos() - m_pressedAt).manhattanLength() > 4)
		{
			m_moved = true;
		}
		QGraphicsRectItem::mouseMoveEvent(event);
	}

	void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override
	{
		QGraphicsRectItem::mouseReleaseEvent(event);

		if (m_moved)
		{
			m_map->nodeMoved(m_layoutKey, pos());
		}
		else if (m_fileId)
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
	QString m_layoutKey;
	QPointF m_pressedAt;
	bool m_moved = false;
};

class MapView: public QGraphicsView
{
public:
	using QGraphicsView::QGraphicsView;

	// Plain wheel zooms, like every other map. The clamp keeps a fast scroll from leaving the
	// map as a single pixel or as one node filling the window.
	void zoom(double factor)
	{
		const double now = transform().m11();
		const double next = std::min(5.0, std::max(0.03, now * factor));
		if (now > 0 && !qFuzzyCompare(next, now))
		{
			const double applied = next / now;
			scale(applied, applied);
		}
	}

protected:
	void wheelEvent(QWheelEvent* event) override
	{
		if (event->angleDelta().y() != 0)
		{
			zoom(event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15);
			event->accept();
			return;
		}
		QGraphicsView::wheelEvent(event);
	}

	// Panning is the middle button, or the left button on empty canvas. It cannot be
	// ScrollHandDrag any more: that mode eats the very drags the nodes now need.
	void mousePressEvent(QMouseEvent* event) override
	{
		const bool onCanvas = itemAt(event->pos()) == nullptr;
		if (event->button() == Qt::MiddleButton ||
			(event->button() == Qt::LeftButton && onCanvas))
		{
			m_panFrom = event->pos();
			m_panning = true;
			setCursor(Qt::ClosedHandCursor);
			event->accept();
			return;
		}
		QGraphicsView::mousePressEvent(event);
	}

	void mouseMoveEvent(QMouseEvent* event) override
	{
		if (m_panning)
		{
			const QPoint delta = event->pos() - m_panFrom;
			m_panFrom = event->pos();
			horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
			verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
			event->accept();
			return;
		}
		QGraphicsView::mouseMoveEvent(event);
	}

	void mouseReleaseEvent(QMouseEvent* event) override
	{
		if (m_panning)
		{
			m_panning = false;
			unsetCursor();
			event->accept();
			return;
		}
		QGraphicsView::mouseReleaseEvent(event);
	}

private:
	QPoint m_panFrom;
	bool m_panning = false;
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

	QPushButton* zoomOut = new QPushButton(QStringLiteral("−"), left);
	zoomOut->setToolTip(QStringLiteral("Herauszoomen (Mausrad)"));
	zoomOut->setFixedWidth(28);
	toolbar->addWidget(zoomOut);
	QPushButton* zoomIn = new QPushButton(QStringLiteral("+"), left);
	zoomIn->setToolTip(QStringLiteral("Hineinzoomen (Mausrad)"));
	zoomIn->setFixedWidth(28);
	toolbar->addWidget(zoomIn);
	QPushButton* fit = new QPushButton(QStringLiteral("Alles zeigen"), left);
	fit->setToolTip(QStringLiteral("Ganze Karte ins Fenster (Ziehen: mittlere Maustaste)"));
	toolbar->addWidget(fit);

	QPushButton* resetLayout = new QPushButton(QStringLiteral("Anordnung zurücksetzen"), left);
	resetLayout->setToolTip(QStringLiteral("Verschobene Knoten wieder ins Raster stellen"));
	toolbar->addWidget(resetLayout);

	m_stats = new QLabel(left);
	toolbar->addWidget(m_stats);
	toolbar->addStretch();
	leftLayout->addLayout(toolbar);

	m_scene = new QGraphicsScene(this);
	m_view = new MapView(m_scene, left);
	m_view->setRenderHint(QPainter::Antialiasing);
	m_view->setDragMode(QGraphicsView::NoDrag);
	m_view->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
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

	QHBoxLayout* chatHead = new QHBoxLayout();
	chatHead->addWidget(new QLabel(QStringLiteral("Chat zur Auswahl"), right));
	chatHead->addStretch();
	QPushButton* newChat = new QPushButton(QStringLiteral("Neuer Chat"), right);
	newChat->setToolTip(QStringLiteral("Gespräch vergessen und von vorne anfangen"));
	chatHead->addWidget(newChat);
	rightLayout->addLayout(chatHead);

	m_answer = new QTextBrowser(right);
	m_answer->setOpenExternalLinks(true);
	rightLayout->addWidget(m_answer, 3);

	QHBoxLayout* askLayout = new QHBoxLayout();
	m_question = new QLineEdit(right);
	m_question->setPlaceholderText(QStringLiteral("Was macht das hier?"));
	askLayout->addWidget(m_question);
	m_askButton = new QPushButton(QStringLiteral("Senden"), right);
	askLayout->addWidget(m_askButton);
	rightLayout->addLayout(askLayout);

	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 1);

	m_noteTimer = new QTimer(this);
	m_noteTimer->setSingleShot(true);
	m_noteTimer->setInterval(1200);

	// Refitting is not cosmetic: the two modes build scenes of wildly different size — directory
	// starts with every group open, feature with every group shut — so keeping the old zoom across
	// a switch leaves the new map as a stamp in the corner.
	connect(m_grouping, &QComboBox::currentIndexChanged, this, [this]() {
		m_layout[QStringLiteral("grouping")] = layoutMode();
		saveLayout();
		rebuild();
		zoomFit();
	});
	connect(collapse, &QPushButton::clicked, this, [this]() {
		m_expanded.clear();
		rebuild();
	});
	connect(reload, &QPushButton::clicked, this, &QtCodeMap::refresh);
	connect(zoomIn, &QPushButton::clicked, this, [this]() { zoomBy(1.25); });
	connect(zoomOut, &QPushButton::clicked, this, [this]() { zoomBy(1.0 / 1.25); });
	connect(fit, &QPushButton::clicked, this, &QtCodeMap::zoomFit);
	connect(resetLayout, &QPushButton::clicked, this, [this]() {
		m_layout.remove(layoutMode());
		saveLayout();
		rebuild();
		zoomFit();
	});
	connect(m_noteTimer, &QTimer::timeout, this, &QtCodeMap::saveNote);
	connect(m_note, &QTextEdit::textChanged, this, [this]() { m_noteTimer->start(); });
	connect(m_feature, &QLineEdit::textEdited, this, [this]() { m_noteTimer->start(); });
	connect(m_askButton, &QPushButton::clicked, this, &QtCodeMap::ask);
	connect(m_question, &QLineEdit::returnPressed, this, &QtCodeMap::ask);
	connect(newChat, &QPushButton::clicked, this, [this]() {
		m_chatSession.clear();
		m_chatContextKey.clear();
		m_chatLog.clear();
		m_answer->clear();
	});
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
	const FilePath projectDir = projectPath.getParentDirectory();
	m_notesPath = QString::fromStdString(
		projectDir.getConcatenated(FilePath("codemap-notes.json")).str());
	m_notes = QJsonObject();
	m_notesStamp = QDateTime();
	m_notesSize = -1;
	reloadNotesIfChanged();

	m_layoutPath = QString::fromStdString(
		projectDir.getConcatenated(FilePath("codemap-layout.json")).str());
	m_layout = QJsonObject();
	QFile layout(m_layoutPath);
	if (layout.open(QIODevice::ReadOnly))
	{
		m_layout = QJsonDocument::fromJson(layout.readAll()).object();
	}

	followRenames();

	// The grouping is a way of looking at one project, not a passing choice — it belongs next to
	// the hand placed nodes and comes back with them. Blocked, because setting it here would
	// rebuild a map that refresh() is about to build anyway.
	{
		const QSignalBlocker blocker(m_grouping);
		m_grouping->setCurrentIndex(
			m_layout.value(QStringLiteral("grouping")).toString() == QStringLiteral("feature") ? 1 : 0);
	}

	// Optional: [{ "stufe": "1 · Hinein", "features": [...] }, ...]. Without it the feature view
	// falls back to the computed layering, which orders by who calls whom — useful, but it cannot
	// know that a file travels from the scanner to the export.
	m_stages = QJsonArray();
	QFile stages(QString::fromStdString(
		projectDir.getConcatenated(FilePath("codemap-stages.json")).str()));
	if (stages.open(QIODevice::ReadOnly))
	{
		m_stages = QJsonDocument::fromJson(stages.readAll()).array();
	}

	m_expanded.clear();
	rebuild();
	zoomFit();
}

// The map keeps the whole notes file in memory, so anything written to it from outside — a
// script, another editor, a second window — is invisible until it is read back. Every write
// therefore starts from what is on disk, and only the note being edited is laid on top.
void QtCodeMap::reloadNotesIfChanged()
{
	if (m_notesPath.isEmpty())
	{
		return;
	}

	const QFileInfo info(m_notesPath);
	if (info.lastModified() == m_notesStamp && info.size() == m_notesSize)
	{
		return;
	}

	QFile file(m_notesPath);
	if (file.open(QIODevice::ReadOnly))
	{
		m_notes = QJsonDocument::fromJson(file.readAll()).object();
		m_notesStamp = info.lastModified();
		m_notesSize = info.size();
	}
}

QString QtCodeMap::layoutMode() const
{
	return m_grouping->currentIndex() == 1 ? QStringLiteral("feature") : QStringLiteral("directory");
}

QJsonObject QtCodeMap::layoutOfMode() const
{
	return m_layout.value(layoutMode()).toObject();
}

void QtCodeMap::saveLayout()
{
	if (m_layoutPath.isEmpty())
	{
		return;
	}
	QFile file(m_layoutPath);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		file.write(QJsonDocument(m_layout).toJson(QJsonDocument::Indented));
	}
}

void QtCodeMap::nodeMoved(const QString& layoutKey, const QPointF& pos)
{
	QJsonObject mode = layoutOfMode();
	mode[layoutKey] = QJsonArray({pos.x(), pos.y()});
	m_layout[layoutMode()] = mode;
	saveLayout();

	// The dependency curves start and end at the old places now. Redrawing them means rebuilding
	// the scene, and that deletes the item Qt is still handing this release event to — hence the
	// detour through the event loop.
	QTimer::singleShot(0, this, &QtCodeMap::rebuild);
}

void QtCodeMap::zoomBy(double factor)
{
	static_cast<MapView*>(m_view)->zoom(factor);
}

void QtCodeMap::zoomFit()
{
	m_view->fitInView(m_scene->sceneRect(), Qt::KeepAspectRatio);
}

void QtCodeMap::writeNotes()
{
	QFile file(m_notesPath);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		file.write(QJsonDocument(m_notes).toJson(QJsonDocument::Indented));
		file.close();
		const QFileInfo info(m_notesPath);
		m_notesStamp = info.lastModified();
		m_notesSize = info.size();
	}
}

// A note is keyed by path, so a renamed file leaves its description behind on a name that no
// longer exists while the file itself turns up undescribed. git knows what became of it, so the
// notes without a file are looked up there and carried over. Only the orphans cost anything, and
// only they trigger the git call.
void QtCodeMap::followRenames()
{
	QSet<QString> indexed;
	for (const CodeMapData::File& file: m_data.files)
	{
		indexed.insert(relative(file.path));
	}

	QStringList orphans;
	for (const QString& key: m_notes.keys())
	{
		if (key.startsWith(QStringLiteral("file:")) && !indexed.contains(key.mid(5)))
		{
			orphans.append(key.mid(5));
		}
	}
	if (orphans.isEmpty())
	{
		return;
	}

	QProcess git;
	git.setWorkingDirectory(QString::fromStdString(m_root.str()));
	git.start(QStringLiteral("git"),
			  {QStringLiteral("log"),
			   QStringLiteral("--diff-filter=R"),
			   QStringLiteral("-M"),
			   QStringLiteral("--name-status"),
			   QStringLiteral("--format="),
			   QStringLiteral("-n"),
			   QStringLiteral("2000")});
	if (!git.waitForFinished(10000) || git.exitCode() != 0)
	{
		return;
	}

	// Newest commit first, so a file renamed twice is already known by its final name when the
	// earlier rename is read: every step points straight at where the file ended up.
	QHash<QString, QString> renamed;
	const QList<QByteArray> lines = git.readAllStandardOutput().split('\n');
	for (const QByteArray& line: lines)
	{
		const QList<QByteArray> parts = line.split('\t');
		if (!line.startsWith('R') || parts.size() < 3)
		{
			continue;
		}
		const QString from = QString::fromUtf8(parts[1].trimmed());
		const QString to = QString::fromUtf8(parts[2].trimmed());
		renamed[from] = renamed.value(to, to);
	}

	bool changed = false;
	for (const QString& orphan: orphans)
	{
		const QString now = renamed.value(orphan);
		const QString oldKey = QStringLiteral("file:") + orphan;
		const QString newKey = QStringLiteral("file:") + now;
		// A note already sitting on the new name wins: it describes the file as it is today.
		if (now.isEmpty() || !indexed.contains(now) || m_notes.contains(newKey))
		{
			continue;
		}
		m_notes[newKey] = m_notes.take(oldKey);
		for (const QString& mode: {QStringLiteral("directory"), QStringLiteral("feature")})
		{
			QJsonObject placed = m_layout.value(mode).toObject();
			if (placed.contains(oldKey))
			{
				placed[newKey] = placed.take(oldKey);
				m_layout[mode] = placed;
			}
		}
		changed = true;
		LOG_INFO("code map: note followed " + orphan.toStdString() + " -> " + now.toStdString());
	}

	if (changed)
	{
		writeNotes();
		saveLayout();
	}
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
	m_nodeItems.clear();
	m_highlighted = nullptr;
	const QJsonObject placed = layoutOfMode();

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

	// The pipeline order wins where it is given: the stages file says how an asset travels, the
	// call graph only says who talks to whom. Anything the file forgot lands in a row of its own
	// at the bottom rather than silently joining a stage it does not belong to.
	std::map<QString, int> layers;
	std::map<int, QString> stageLabels;
	if (m_grouping->currentIndex() == 1 && !m_stages.isEmpty())
	{
		for (int i = 0; i < m_stages.size(); i++)
		{
			const QJsonObject stage = m_stages.at(i).toObject();
			stageLabels[i] = stage.value(QStringLiteral("stufe")).toString();
			for (const QJsonValue& feature: stage.value(QStringLiteral("features")).toArray())
			{
				layers[feature.toString()] = i;
			}
		}
		for (const QString& name: names)
		{
			if (!layers.count(name))
			{
				layers[name] = static_cast<int>(m_stages.size());
				stageLabels[static_cast<int>(m_stages.size())] = QStringLiteral("Ohne Stufe");
			}
		}
	}
	else
	{
		layers = layerGroups(names, deps);
	}

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
	orderLayers(byLayer, layers, deps);

	// A row is centred, so it has to be measured before its first group is placed. Left aligned
	// rows stack into a column and the flow reads as a list; centred they read as one spine with
	// the stages strung along it.
	std::map<QString, qreal> widths;
	for (const QString& name: names)
	{
		const size_t count = groups[name].size();
		if (m_expanded.contains(name))
		{
			const int cols = std::min<int>(
				8, std::max<int>(1, static_cast<int>(std::ceil(std::sqrt(count)))));
			widths[name] = 2 * kPad + cols * kFileW + (cols - 1) * kGap;
		}
		else
		{
			widths[name] = kFileW + 2 * kPad;
		}
	}

	std::map<int, qreal> rowStarts;
	qreal labelX = 0;
	for (const auto& layer: byLayer)
	{
		// The wrap is walked here exactly as it is walked below, because a row that breaks into
		// two lines has to be centred on its widest line — centring it on the sum instead pushes
		// it off the axis, and one stage sitting off to the side breaks the whole column.
		qreal x = 0;
		qreal widest = 0;
		for (const QString& name: layer.second)
		{
			if (x > 0 && x + widths[name] > kRowWidth)
			{
				widest = std::max(widest, x - kGroupGap);
				x = 0;
			}
			x += widths[name] + kGroupGap;
		}
		rowStarts[layer.first] = -std::max(widest, x - kGroupGap) / 2;
		labelX = std::min(labelX, rowStarts[layer.first]);
	}

	qreal y = 0;
	size_t documented = 0;
	for (const auto& layer: byLayer)
	{
		const qreal rowStart = rowStarts.at(layer.first);
		qreal x = rowStart;
		qreal rowHeight = 0;

		auto label = stageLabels.find(layer.first);
		if (label != stageLabels.end() && !label->second.isEmpty())
		{
			QGraphicsSimpleTextItem* text = m_scene->addSimpleText(label->second);
			QFont labelFont = text->font();
			labelFont.setBold(true);
			text->setFont(labelFont);
			// The group text colour is meant to sit on the group's own fill; out here on the dark
			// background it is all but black. The fill colour is the readable half of that pair.
			text->setBrush(color(groupColor.fill));
			text->setPos(labelX - text->boundingRect().width() - 2 * kGroupGap, y + 4);
			text->setZValue(1);
		}
		for (const QString& name: layer.second)
		{
			const std::vector<size_t>& files = groups[name];
			const bool expanded = m_expanded.contains(name);

			// Counted here and not while drawing: collapsed groups draw no file nodes, so a folded
			// map used to report zero described files even when every single one carries a note.
			for (size_t index: files)
			{
				if (!noteField(QStringLiteral("file:") + relative(m_data.files[index].path),
							   QStringLiteral("text"))
						 .isEmpty())
				{
					documented++;
				}
			}

			int cols = 1;
			int rows = 0;
			const qreal w = widths.at(name);
			qreal h = kHeader + kPad;
			if (expanded)
			{
				cols = std::min<int>(8, std::max<int>(1, static_cast<int>(std::ceil(std::sqrt(files.size())))));
				rows = static_cast<int>((files.size() + cols - 1) / cols);
				h = kHeader + kPad + rows * (kFileH + kGap);
			}

			if (x > rowStart && x + w > rowStart + kRowWidth)
			{
				x = rowStart;
				y += rowHeight + kGap;
				rowHeight = 0;
			}

			MapItem* item = new MapItem(this, name, 0, QStringLiteral("group:") + name);
			item->setRect(0, 0, w, h);
			item->setPos(x, y);
			const QJsonArray groupPos = placed.value(QStringLiteral("group:") + name).toArray();
			if (groupPos.size() == 2)
			{
				item->setPos(groupPos[0].toDouble(), groupPos[1].toDouble());
			}
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

					MapItem* node = new MapItem(this, name, file.nodeId, key);
					node->setRect(0, 0, kFileW, kFileH);
					node->setPos(
						kPad + (i % cols) * (kFileW + kGap),
						kHeader + kPad / 2 + (i / cols) * (kFileH + kGap));
					const QJsonArray nodePos = placed.value(key).toArray();
					if (nodePos.size() == 2)
					{
						node->setPos(nodePos[0].toDouble(), nodePos[1].toDouble());
					}
					node->setBrush(color(hasNote ? noteColor.fill : fileColor.fill));
					node->setPen(QPen(color(hasNote ? noteColor.border : fileColor.border), 1));
					node->setParentItem(item);
					node->setToolTip(rel + QStringLiteral("\n%1 Symbole").arg(file.symbolCount));
					m_nodeItems[file.nodeId] = node;

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

			// the curves below follow where the group actually sits, not where the grid put it
			rects[name] = QRectF(item->pos(), QSizeF(w, h));
			x += w + kGroupGap;
			rowHeight = std::max(rowHeight, h);
		}
		y += rowHeight + kLayerGap;
	}

	// dependency curves behind the groups
	size_t maxCount = 1;
	std::map<QString, size_t> strongestOut;
	for (const auto& dep: deps)
	{
		maxCount = std::max(maxCount, dep.second);
		strongestOut[dep.first.first] = std::max(strongestOut[dep.first.first], dep.second);
	}
	size_t hidden = 0;
	for (const auto& dep: deps)
	{
		const qreal share = static_cast<qreal>(dep.second) / strongestOut[dep.first.first];
		if (share < kCurveShare)
		{
			hidden++;
			continue;
		}

		const QRectF source = rects[dep.first.first];
		const QRectF target = rects[dep.first.second];

		// Anchored by where the two groups sit, not by who calls whom. In a pipeline most calls
		// point back up the flow — the exporter calls the database, not the other way round — and
		// leaving those from the bottom edge sent every single one on a loop around the whole map.
		// The map draws no arrow heads anyway; the stage rows carry the direction.
		const bool downward = source.center().y() <= target.center().y();
		const QRectF& upper = downward ? source : target;
		const QRectF& lower = downward ? target : source;
		const QPointF from(upper.center().x(), upper.bottom());
		const QPointF to(lower.center().x(), lower.top());

		QPainterPath path(from);
		const qreal bend = std::max<qreal>(40, std::abs(to.y() - from.y()) / 2);
		path.cubicTo(from + QPointF(0, bend), to - QPointF(0, bend), to);

		QGraphicsPathItem* curve = m_scene->addPath(path);
		// Within what is left, a group's main tie is still drawn stronger than its side ones.
		QColor edgeColor = color(GraphViewStyle::getEdgeColor("call"));
		edgeColor.setAlpha(static_cast<int>(25 + 165 * share));
		curve->setPen(QPen(
			edgeColor, 1.0 + 3.0 * std::log(1.0 + dep.second) / std::log(1.0 + maxCount),
			Qt::SolidLine, Qt::RoundCap));
		curve->setZValue(0);
		curve->setToolTip(QStringLiteral("%1 → %2: %3 Verbindungen")
							  .arg(dep.first.first, dep.first.second)
							  .arg(dep.second));
	}

	m_scene->setSceneRect(m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40));
	m_stats->setText(QStringLiteral("%1 Dateien, %2 Gruppen, %3 beschrieben — %4 Verbindungen, %5 schwache ausgeblendet")
						 .arg(m_data.files.size())
						 .arg(names.size())
						 .arg(documented)
						 .arg(deps.size() - hidden)
						 .arg(hidden));

	// a rebuild throws away the items, so the selection has to be marked again
	if (m_currentFileId)
	{
		highlight(m_currentFileId);
	}
}

// Shows on the map what the rest of Sourcetrail is looking at: the group is opened if it was
// collapsed, the node gets a ring, and the view scrolls to it. Without this the map and the code
// view drift apart the moment a jump comes from the graph or from a search.
void QtCodeMap::highlight(Id fileNodeId)
{
	if (m_highlighted)
	{
		m_highlighted->setPen(m_highlightedPen);
		m_highlighted = nullptr;
	}

	auto item = m_nodeItems.find(fileNodeId);
	if (item == m_nodeItems.end())
	{
		// the file is inside a collapsed group: open it and look again
		auto file = m_fileIndex.find(fileNodeId);
		if (file == m_fileIndex.end())
		{
			return;
		}
		const QString group = groupOf(file->second);
		if (m_expanded.contains(group))
		{
			return;
		}
		m_expanded.insert(group);
		rebuild();	  // rebuild highlights again at its end
		return;
	}

	m_highlightedPen = item->second->pen();
	QPen ring(color(GraphViewStyle::getNodeColor("file", true).border), 3);
	item->second->setPen(ring);
	m_highlighted = item->second;
	m_view->ensureVisible(item->second, 80, 80);
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
	select(QStringLiteral("file:") + rel, rel, file.path, fileNodeId);

	MessageActivateFile(file.path).dispatch();
}

void QtCodeMap::select(const QString& key, const QString& title, const FilePath& path, Id fileNodeId)
{
	saveNote();
	reloadNotesIfChanged();

	m_currentKey = key;
	m_currentPath = path;
	m_currentFileId = fileNodeId;
	m_title->setText(title);
	showNote();

	if (fileNodeId)
	{
		highlight(fileNodeId);
	}
}

void QtCodeMap::showNote()
{
	m_loadedText = noteField(m_currentKey, QStringLiteral("text"));
	m_loadedFeature = noteField(m_currentKey, QStringLiteral("feature"));

	const bool blocked = m_note->blockSignals(true);
	m_note->setPlainText(m_loadedText);
	m_note->blockSignals(blocked);
	m_feature->setText(m_loadedFeature);
}

void QtCodeMap::saveNote()
{
	if (m_currentKey.isEmpty() || m_notesPath.isEmpty())
	{
		return;
	}

	const QString text = m_note->toPlainText();
	const QString feature = m_feature->text();
	// Only what the user actually typed is written. Comparing against the file instead would
	// hand a stale editor the power to undo an edit made outside the app.
	if (text == m_loadedText && feature == m_loadedFeature)
	{
		return;
	}

	reloadNotesIfChanged();

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

	writeNotes();

	m_loadedText = text;
	m_loadedFeature = feature;
}

// Hands the index itself to Claude, not just the source tree: the MCP server answers "who calls
// this" from the same database the map is drawn from, which no amount of grepping does reliably.
// Missing script or missing python is not an error — the chat then simply reads files.
QString QtCodeMap::mcpConfig() const
{
	const FilePath script(
		(QCoreApplication::applicationDirPath() + QStringLiteral("/mcp_server.py")).toStdString());
	if (!script.exists())
	{
		return QString();
	}

	const FilePath db = Application::getInstance()->getCurrentProjectPath().replaceExtension("srctrldb");
	if (!db.exists())
	{
		return QString();
	}

	QJsonObject server;
	server[QStringLiteral("type")] = QStringLiteral("stdio");
	server[QStringLiteral("command")] = QStringLiteral("python3");
	server[QStringLiteral("args")] = QJsonArray({QString::fromStdString(script.str()),
												 QStringLiteral("--db"),
												 QString::fromStdString(db.str()),
												 QStringLiteral("--crate-root"),
												 QString::fromStdString(m_root.str())});

	QJsonObject servers;
	servers[QStringLiteral("index")] = server;
	QJsonObject config;
	config[QStringLiteral("mcpServers")] = servers;
	return QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Compact));
}

void QtCodeMap::appendChat(const QString& who, const QString& text, const QString& color)
{
	m_chatLog += QStringLiteral("<p><b style=\"color:%1\">%2</b><br>%3</p>")
					 .arg(color, who, text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
	m_answer->setHtml(m_chatLog);
	m_answer->verticalScrollBar()->setValue(m_answer->verticalScrollBar()->maximum());
}

void QtCodeMap::ask()
{
	if (m_ask || m_question->text().isEmpty())
	{
		return;
	}

	const QString question = m_question->text();
	m_question->clear();
	appendChat(QStringLiteral("Du"), question, QStringLiteral("#888"));

	// The selection is only spelled out when it changed. Repeating it every turn would push the
	// conversation back to the same file after each follow up question.
	QString prompt;
	if (m_chatContextKey != m_currentKey && !m_currentKey.isEmpty())
	{
		prompt = QStringLiteral("Es geht um %1.\n").arg(m_title->text());
		const QString note = noteField(m_currentKey, QStringLiteral("text"));
		if (!note.isEmpty())
		{
			prompt += QStringLiteral("Meine Notiz dazu: %1\n").arg(note);
		}
		m_chatContextKey = m_currentKey;
	}
	prompt += question;

	QStringList args{QStringLiteral("-p"),
					 prompt,
					 QStringLiteral("--output-format"),
					 QStringLiteral("json"),
					 QStringLiteral("--append-system-prompt"),
					 QStringLiteral("Du beantwortest Fragen zu dieser Codebase in der Landkarte "
									"von Sourcetrail. Antworte auf Deutsch, in einfacher Sprache, "
									"höchstens 12 Zeilen.")};

	QString allowed = QStringLiteral("Read,Grep,Glob");
	const QString mcp = mcpConfig();
	if (!mcp.isEmpty())
	{
		args << QStringLiteral("--mcp-config") << mcp;
		allowed += QStringLiteral(",mcp__index__symbol,mcp__index__search_symbols,mcp__index__file_symbols");
	}
	args << QStringLiteral("--allowedTools") << allowed;

	// Same conversation as the previous question, so "und wer ruft das auf?" makes sense.
	if (!m_chatSession.isEmpty())
	{
		args << QStringLiteral("--resume") << m_chatSession;
	}

	m_ask = new QProcess(this);
	m_ask->setWorkingDirectory(QString::fromStdString(m_root.str()));
	connect(m_ask, &QProcess::finished, this, &QtCodeMap::askFinished);

	m_askButton->setEnabled(false);
	m_askButton->setText(QStringLiteral("…"));
	m_ask->start(QStringLiteral("claude"), args);
}

void QtCodeMap::askFinished()
{
	const QByteArray out = m_ask->readAllStandardOutput();
	const QString err = QString::fromUtf8(m_ask->readAllStandardError()).trimmed();

	const QJsonObject result = QJsonDocument::fromJson(out).object();
	const QString answer = result.value(QStringLiteral("result")).toString();
	const QString session = result.value(QStringLiteral("session_id")).toString();
	if (!session.isEmpty())
	{
		m_chatSession = session;
	}

	// A failed login also comes back as a well formed answer, so the flag decides, not the text.
	const bool failed = result.value(QStringLiteral("is_error")).toBool() ||
		m_ask->exitCode() != 0 || answer.isEmpty();
	if (!failed)
	{
		appendChat(QStringLiteral("Claude"), answer, QStringLiteral("#7ba7d7"));
	}
	else
	{
		appendChat(
			QStringLiteral("Fehler"),
			(answer.isEmpty() ? (err.isEmpty() ? QString::fromUtf8(out).trimmed() : err) : answer) +
				QStringLiteral("\n\n(claude endete mit %1 – einmal `claude` im Terminal starten "
							   "und einloggen.)")
					.arg(m_ask->exitCode()),
			QStringLiteral("#d77b7b"));
	}

	m_ask->deleteLater();
	m_ask = nullptr;
	m_askButton->setEnabled(true);
	m_askButton->setText(QStringLiteral("Senden"));
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
			select(QStringLiteral("file:") + rel, rel, file.path, file.nodeId);
			return;
		}

		// a symbol: note it under its own name, but point the question at its file and light up
		// that file on the map, so a jump from the graph does not leave the map behind
		FilePath path;
		StorageAccess* storage = Application::getInstance()->getStorageAccess();
		for (const auto& parent: storage->getNodeIdToParentFileMap({tokenIds[0]}))
		{
			path = FilePath(parent.second.second.getQualifiedName());
		}

		Id fileNodeId = 0;
		if (!path.empty())
		{
			for (const auto& file: m_data.files)
			{
				if (file.path == path)
				{
					fileNodeId = file.nodeId;
					break;
				}
			}
		}
		select(QStringLiteral("sym:") + title, title, path, fileNodeId);
	});
}

void QtCodeMap::handleMessage(MessageIndexingFinished*  /*message*/)
{
	m_onQtThread([this]() { refresh(); });
}
