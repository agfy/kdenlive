/*
Copyright (C) 2012  Till Theato <root@ttill.de>
Copyright (C) 2014  Jean-Baptiste Mardelle <jb@kdenlive.org>
This file is part of Kdenlive. See www.kdenlive.org.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of
the License or (at your option) version 3 or any later version
accepted by the membership of KDE e.V. (or its successor approved
by the membership of KDE e.V.), which shall act as a proxy
defined in Section 14 of version 3 of the license.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "bin.h"
#include "bincommands.h"
#include "clipcreator.hpp"
#include "core.h"
#include "dialogs/clipcreationdialog.h"
#include "doc/documentchecker.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "jobs/audiothumbjob.hpp"
#include "jobs/jobmanager.h"
#include "jobs/loadjob.hpp"
#include "jobs/thumbjob.hpp"
#include "kdenlive_debug.h"
#include "kdenlivesettings.h"
#include "mainwindow.h"
#include "mlt++/Mlt.h"
#include "mltcontroller/clipcontroller.h"
#include "mltcontroller/clippropertiescontroller.h"
#include "monitor/monitor.h"
#include "project/dialogs/slideshowclip.h"
#include "project/invaliddialog.h"
#include "project/projectcommands.h"
#include "project/projectmanager.h"
#include "projectclip.h"
#include "projectfolder.h"
#include "projectitemmodel.h"
#include "projectsortproxymodel.h"
#include "projectsubclip.h"
#include "titler/titlewidget.h"
#include "ui_qtextclip_ui.h"
#include "undohelper.hpp"
#include "xml/xml.hpp"

#include <KColorScheme>
#include <KMessageBox>
#include <KXMLGUIFactory>
#include <QToolBar>

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDrag>
#include <QFile>
#include <QMenu>
#include <QSlider>
#include <QTimeLine>
#include <QUndoCommand>
#include <QUrl>
#include <QVBoxLayout>
#include <utility>
#include <utils/thumbnailcache.hpp>

/**
 * @class BinItemDelegate
 * @brief This class is responsible for drawing items in the QTreeView.
 */

class BinItemDelegate : public QStyledItemDelegate
{
public:
    explicit BinItemDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)

    {
        connect(this, &QStyledItemDelegate::closeEditor, [&]() { m_editorOpen = false; });
    }
    void setDar(double dar) { m_dar = dar; }
    void setEditorData(QWidget *w, const QModelIndex &i) const override
    {
        if (!m_editorOpen) {
            QStyledItemDelegate::setEditorData(w, i);
            m_editorOpen = true;
        }
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        Q_UNUSED(model);
        Q_UNUSED(option);
        Q_UNUSED(index);
        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = (QMouseEvent *)event;
            if (m_audioDragRect.contains(me->pos())) {
                dragType = PlaylistState::AudioOnly;
            } else if (m_videoDragRect.contains(me->pos())) {
                dragType = PlaylistState::VideoOnly;
            } else {
                dragType = PlaylistState::Disabled;
            }
        }
        event->ignore();
        return false;
    }
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.column() != 0) {
            QStyledItemDelegate::updateEditorGeometry(editor, option, index);
            return;
        }
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        QRect r1 = option.rect;
        int type = index.data(AbstractProjectItem::ItemTypeRole).toInt();
        int decoWidth = 0;
        if (opt.decorationSize.height() > 0) {
            decoWidth += r1.height() * m_dar;
        }
        int mid = 0;
        if (type == AbstractProjectItem::ClipItem || type == AbstractProjectItem::SubClipItem) {
            mid = (int)((r1.height() / 2));
        }
        r1.adjust(decoWidth, 0, 0, -mid);
        QFont ft = option.font;
        ft.setBold(true);
        QFontMetricsF fm(ft);
        QRect r2 = fm.boundingRect(r1, Qt::AlignLeft | Qt::AlignTop, index.data(AbstractProjectItem::DataName).toString()).toRect();
        editor->setGeometry(r2);
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        QString text = index.data(AbstractProjectItem::DataName).toString();
        QRectF r = option.rect;
        QFont ft = option.font;
        ft.setBold(true);
        QFontMetricsF fm(ft);
        QStyle *style = option.widget ? option.widget->style() : QApplication::style();
        const int textMargin = style->pixelMetric(QStyle::PM_FocusFrameHMargin) + 1;
        int width = fm.boundingRect(r, Qt::AlignLeft | Qt::AlignTop, text).width() + option.decorationSize.width() + 2 * textMargin;
        hint.setWidth(width);
        int type = index.data(AbstractProjectItem::ItemTypeRole).toInt();
        if (type == AbstractProjectItem::FolderItem) {
            return QSize(hint.width(), qMin(option.fontMetrics.lineSpacing() + 4, hint.height()));
        }
        if (type == AbstractProjectItem::ClipItem) {
            return QSize(hint.width(), qMax(option.fontMetrics.lineSpacing() * 2 + 4, qMax(hint.height(), option.decorationSize.height())));
        }
        if (type == AbstractProjectItem::SubClipItem) {
            return QSize(hint.width(), qMax(option.fontMetrics.lineSpacing() * 2 + 4, qMin(hint.height(), (int)(option.decorationSize.height() / 1.5))));
        }
        QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        QString line1 = index.data(Qt::DisplayRole).toString();
        QString line2 = index.data(Qt::UserRole).toString();

        int textW = qMax(option.fontMetrics.horizontalAdvance(line1), option.fontMetrics.horizontalAdvance(line2));
        QSize iconSize = icon.actualSize(option.decorationSize);
        return {qMax(textW, iconSize.width()) + 4, option.fontMetrics.lineSpacing() * 2 + 4};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.column() == 0 && !index.data().isNull()) {
            QRect r1 = option.rect;
            painter->save();
            painter->setClipRect(r1);
            QStyleOptionViewItem opt(option);
            initStyleOption(&opt, index);
            int type = index.data(AbstractProjectItem::ItemTypeRole).toInt();
            QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
            const int textMargin = style->pixelMetric(QStyle::PM_FocusFrameHMargin) + 1;
            // QRect r = QStyle::alignedRect(opt.direction, Qt::AlignVCenter | Qt::AlignLeft, opt.decorationSize, r1);

            style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);
            if ((option.state & static_cast<int>(QStyle::State_Selected)) != 0) {
                painter->setPen(option.palette.highlightedText().color());
            } else {
                painter->setPen(option.palette.text().color());
            }
            QRect r = r1;
            QFont font = painter->font();
            font.setBold(true);
            painter->setFont(font);
            if (type == AbstractProjectItem::ClipItem || type == AbstractProjectItem::SubClipItem) {
                int decoWidth = 0;
                if (opt.decorationSize.height() > 0) {
                    r.setWidth(r.height() * m_dar);
                    QPixmap pix = opt.icon.pixmap(opt.icon.actualSize(r.size()));
                    // Draw icon
                    decoWidth += r.width() + textMargin;
                    r.setWidth(r.height() * pix.width() / pix.height());
                    painter->drawPixmap(r, pix, QRect(0, 0, pix.width(), pix.height()));
                    m_thumbRect = r;
                }
                int mid = (int)((r1.height() / 2));
                r1.adjust(decoWidth, 0, 0, -mid);
                QRect r2 = option.rect;
                r2.adjust(decoWidth, mid, 0, 0);
                QRectF bounding;
                painter->drawText(r1, Qt::AlignLeft | Qt::AlignTop, index.data(AbstractProjectItem::DataName).toString(), &bounding);
                font.setBold(false);
                painter->setFont(font);
                QString subText = index.data(AbstractProjectItem::DataDuration).toString();
                if (!subText.isEmpty()) {
                    r2.adjust(0, bounding.bottom() - r2.top(), 0, 0);
                    QColor subTextColor = painter->pen().color();
                    subTextColor.setAlphaF(.5);
                    painter->setPen(subTextColor);
                    // Draw usage counter
                    int usage = index.data(AbstractProjectItem::UsageCount).toInt();
                    if (usage > 0) {
                        subText.append(QString::asprintf(" [%d]", usage));
                    }
                    painter->drawText(r2, Qt::AlignLeft | Qt::AlignTop, subText, &bounding);

                    // Add audio/video icons for selective drag
                    int cType = index.data(AbstractProjectItem::ClipType).toInt();
                    bool hasAudioAndVideo = index.data(AbstractProjectItem::ClipHasAudioAndVideo).toBool();
                    if (hasAudioAndVideo && (cType == ClipType::AV || cType == ClipType::Playlist) && (opt.state & QStyle::State_MouseOver)) {
                        bounding.moveLeft(bounding.right() + (2 * textMargin));
                        bounding.adjust(0, textMargin, 0, -textMargin);
                        QIcon aDrag = QIcon::fromTheme(QStringLiteral("audio-volume-medium"));
                        m_audioDragRect = bounding.toRect();
                        m_audioDragRect.setWidth(m_audioDragRect.height());
                        aDrag.paint(painter, m_audioDragRect, Qt::AlignLeft);
                        m_videoDragRect = m_audioDragRect;
                        m_videoDragRect.moveLeft(m_audioDragRect.right());
                        QIcon vDrag = QIcon::fromTheme(QStringLiteral("kdenlive-show-video"));
                        vDrag.paint(painter, m_videoDragRect, Qt::AlignLeft);
                    } else {
                        //m_audioDragRect = QRect();
                        //m_videoDragRect = QRect();
                    }
                }
                if (type == AbstractProjectItem::ClipItem) {
                    // Overlay icon if necessary
                    QVariant v = index.data(AbstractProjectItem::IconOverlay);
                    if (!v.isNull()) {
                        QIcon reload = QIcon::fromTheme(v.toString());
                        r.setTop(r.bottom() - bounding.height());
                        r.setWidth(bounding.height());
                        reload.paint(painter, r);
                    }

                    int jobProgress = index.data(AbstractProjectItem::JobProgress).toInt();
                    auto status = index.data(AbstractProjectItem::JobStatus).value<JobManagerStatus>();
                    if (status == JobManagerStatus::Pending || status == JobManagerStatus::Running) {
                        // Draw job progress bar
                        int progressWidth = option.fontMetrics.averageCharWidth() * 8;
                        int progressHeight = option.fontMetrics.ascent() / 4;
                        QRect progress(r1.x() + 1, opt.rect.bottom() - progressHeight - 2, progressWidth, progressHeight);
                        painter->setPen(Qt::NoPen);
                        painter->setBrush(Qt::darkGray);
                        if (status == JobManagerStatus::Running) {
                            painter->drawRoundedRect(progress, 2, 2);
                            painter->setBrush((option.state & static_cast<int>((QStyle::State_Selected) != 0)) != 0 ? option.palette.text()
                                                                                                                    : option.palette.highlight());
                            progress.setWidth((progressWidth - 2) * jobProgress / 100);
                            painter->drawRoundedRect(progress, 2, 2);
                        } else {
                            // Draw kind of a pause icon
                            progress.setWidth(3);
                            painter->drawRect(progress);
                            progress.moveLeft(progress.right() + 3);
                            painter->drawRect(progress);
                        }
                    }
                    bool jobsucceeded = index.data(AbstractProjectItem::JobSuccess).toBool();
                    if (!jobsucceeded) {
                        QIcon warning = QIcon::fromTheme(QStringLiteral("process-stop"));
                        warning.paint(painter, r2);
                    }
                }
            } else {
                // Folder or Folder Up items
                int decoWidth = 0;
                if (opt.decorationSize.height() > 0) {
                    r.setWidth(r.height() * m_dar);
                    QPixmap pix = opt.icon.pixmap(opt.icon.actualSize(r.size()));
                    // Draw icon
                    decoWidth += r.width() + textMargin;
                    r.setWidth(r.height() * pix.width() / pix.height());
                    painter->drawPixmap(r, pix, QRect(0, 0, pix.width(), pix.height()));
                }
                r1.adjust(decoWidth, 0, 0, 0);
                QRectF bounding;
                painter->drawText(r1, Qt::AlignLeft | Qt::AlignTop, index.data(AbstractProjectItem::DataName).toString(), &bounding);
            }
            painter->restore();
        } else {
            QStyledItemDelegate::paint(painter, option, index);
        }
    }

    int getFrame(QModelIndex index, int mouseX)
    {
        int type = index.data(AbstractProjectItem::ItemTypeRole).toInt();
        if ((type != AbstractProjectItem::ClipItem && type != AbstractProjectItem::SubClipItem) || mouseX < m_thumbRect.x() || mouseX > m_thumbRect.right()) {
            return 0;
        }
        return 100 * (mouseX - m_thumbRect.x()) / m_thumbRect.width();
    }

private:
    mutable bool m_editorOpen{false};
    mutable QRect m_audioDragRect;
    mutable QRect m_videoDragRect;
    mutable QRect m_thumbRect;
    double m_dar{1.778};

public:
    PlaylistState::ClipState dragType{PlaylistState::Disabled};
};

/**
 * @class BinListItemDelegate
 * @brief This class is responsible for drawing items in the QListView.
 */

class BinListItemDelegate : public QStyledItemDelegate
{
public:
    explicit BinListItemDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)

    {
        connect(this, &QStyledItemDelegate::closeEditor, [&]() { m_editorOpen = false; });
    }
    void setDar(double dar) { m_dar = dar; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (!index.data().isNull()) {
            QStyleOptionViewItem opt = option;
            initStyleOption(&opt, index);

            int adjust = (opt.rect.width() - opt.decorationSize.width()) / 2;
            QRect rect(0, 0, opt.rect.width(), opt.rect.height());
            m_thumbRect = adjust > 0 && adjust < rect.width() ? rect.adjusted(adjust, 0, -adjust, 0) : rect;
            QStyledItemDelegate::paint(painter, option, index);
        }
    }

    int getFrame(QModelIndex index, int mouseX)
    {
        int type = index.data(AbstractProjectItem::ItemTypeRole).toInt();
        if ((type != AbstractProjectItem::ClipItem && type != AbstractProjectItem::SubClipItem)|| mouseX < m_thumbRect.x() || mouseX > m_thumbRect.right()) {
            return 0;
        }
        return 100 * (mouseX - m_thumbRect.x()) / m_thumbRect.width();
    }

private:
    mutable bool m_editorOpen{false};
    mutable QRect m_audioDragRect;
    mutable QRect m_videoDragRect;
    mutable QRect m_thumbRect;
    double m_dar{1.778};

public:
    PlaylistState::ClipState dragType{PlaylistState::Disabled};
};


MyListView::MyListView(QWidget *parent)
    : QListView(parent)
{
    setViewMode(QListView::IconMode);
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setWordWrap(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setAcceptDrops(true);
    setDragEnabled(true);
    viewport()->setAcceptDrops(true);
}

void MyListView::focusInEvent(QFocusEvent *event)
{
    QListView::focusInEvent(event);
    if (event->reason() == Qt::MouseFocusReason) {
        emit focusView();
    }
}

void MyListView::mouseMoveEvent(QMouseEvent *event)
{
    if (event->modifiers() == Qt::ShiftModifier) {
        QModelIndex index = indexAt(event->pos());
        if (index.isValid()) {
            QAbstractItemDelegate *del = itemDelegate(index);
            if (del) {
                auto delegate = static_cast<BinListItemDelegate *>(del);
                QRect vRect = visualRect(index);
                int frame = delegate->getFrame(index, event->pos().x() - vRect.x());
                emit displayBinFrame(index, frame);
            } else {
                qDebug()<<"<<< NO DELEGATE!!!";
            }
        }
    }
    QListView::mouseMoveEvent(event);
}

MyTreeView::MyTreeView(QWidget *parent)
    : QTreeView(parent)
{
    setEditing(false);
}

void MyTreeView::mousePressEvent(QMouseEvent *event)
{
    QTreeView::mousePressEvent(event);
    if (event->button() == Qt::LeftButton) {
        m_startPos = event->pos();
        QModelIndex ix = indexAt(m_startPos);
        if (ix.isValid()) {
            QAbstractItemDelegate *del = itemDelegate(ix);
            m_dragType = static_cast<BinItemDelegate *>(del)->dragType;
        } else {
            m_dragType = PlaylistState::Disabled;
        }
    }
}

void MyTreeView::focusInEvent(QFocusEvent *event)
{
    QTreeView::focusInEvent(event);
    if (event->reason() == Qt::MouseFocusReason) {
        emit focusView();
    }
}

void MyTreeView::mouseMoveEvent(QMouseEvent *event)
{
    bool dragged = false;
    if ((event->buttons() & Qt::LeftButton) != 0u) {
        int distance = (event->pos() - m_startPos).manhattanLength();
        if (distance >= QApplication::startDragDistance()) {
            dragged = performDrag();
        }
    } else if (event->modifiers() == Qt::ShiftModifier) {
        QModelIndex index = indexAt(event->pos());
        if (index.isValid()) {
            QAbstractItemDelegate *del = itemDelegate(index);
            int frame = static_cast<BinItemDelegate *>(del)->getFrame(index, event->pos().x());
            emit displayBinFrame(index, frame);
        }
    }
    if (!dragged) {
        QTreeView::mouseMoveEvent(event);
    }
}

void MyTreeView::closeEditor(QWidget *editor, QAbstractItemDelegate::EndEditHint hint)
{
    QAbstractItemView::closeEditor(editor, hint);
    setEditing(false);
}

void MyTreeView::editorDestroyed(QObject *editor)
{
    QAbstractItemView::editorDestroyed(editor);
    setEditing(false);
}

bool MyTreeView::isEditing() const
{
    return state() == QAbstractItemView::EditingState;
}

void MyTreeView::setEditing(bool edit)
{
    setState(edit ? QAbstractItemView::EditingState : QAbstractItemView::NoState);
}

bool MyTreeView::performDrag()
{
    QModelIndexList bases = selectedIndexes();
    QModelIndexList indexes;
    for (int i = 0; i < bases.count(); i++) {
        if (bases.at(i).column() == 0) {
            indexes << bases.at(i);
        }
    }
    if (indexes.isEmpty()) {
        return false;
    }
    // Check if we want audio or video only
    emit updateDragMode(m_dragType);
    auto *drag = new QDrag(this);
    drag->setMimeData(model()->mimeData(indexes));
    QModelIndex ix = indexes.constFirst();
    if (ix.isValid()) {
        QIcon icon = ix.data(AbstractProjectItem::DataThumbnail).value<QIcon>();
        QPixmap pix = icon.pixmap(iconSize());
        QSize size = pix.size();
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setOpacity(0.7);
        p.drawPixmap(0, 0, pix);
        p.setOpacity(1);
        if (indexes.count() > 1) {
            QPalette palette;
            int radius = size.height() / 3;
            p.setBrush(palette.highlight());
            p.setPen(palette.highlightedText().color());
            p.drawEllipse(QPoint(size.width() / 2, size.height() / 2), radius, radius);
            p.drawText(size.width() / 2 - radius, size.height() / 2 - radius, 2 * radius, 2 * radius, Qt::AlignCenter, QString::number(indexes.count()));
        }
        p.end();
        drag->setPixmap(QPixmap::fromImage(image));
    }
    drag->exec();
    return true;
}

SmallJobLabel::SmallJobLabel(QWidget *parent)
    : QPushButton(parent)

{
    setFixedWidth(0);
    setFlat(true);
    m_timeLine = new QTimeLine(500, this);
    QObject::connect(m_timeLine, &QTimeLine::valueChanged, this, &SmallJobLabel::slotTimeLineChanged);
    QObject::connect(m_timeLine, &QTimeLine::finished, this, &SmallJobLabel::slotTimeLineFinished);
    hide();
}

const QString SmallJobLabel::getStyleSheet(const QPalette &p)
{
    KColorScheme scheme(p.currentColorGroup(), KColorScheme::Window);
    QColor bg = scheme.background(KColorScheme::LinkBackground).color();
    QColor fg = scheme.foreground(KColorScheme::LinkText).color();
    QString style =
        QStringLiteral("QPushButton {margin:3px;padding:2px;background-color: rgb(%1, %2, %3);border-radius: 4px;border: none;color: rgb(%4, %5, %6)}")
            .arg(bg.red())
            .arg(bg.green())
            .arg(bg.blue())
            .arg(fg.red())
            .arg(fg.green())
            .arg(fg.blue());

    bg = scheme.background(KColorScheme::ActiveBackground).color();
    fg = scheme.foreground(KColorScheme::ActiveText).color();
    style.append(
        QStringLiteral("\nQPushButton:hover {margin:3px;padding:2px;background-color: rgb(%1, %2, %3);border-radius: 4px;border: none;color: rgb(%4, %5, %6)}")
            .arg(bg.red())
            .arg(bg.green())
            .arg(bg.blue())
            .arg(fg.red())
            .arg(fg.green())
            .arg(fg.blue()));

    return style;
}

void SmallJobLabel::setAction(QAction *action)
{
    m_action = action;
}

void SmallJobLabel::slotTimeLineChanged(qreal value)
{
    setFixedWidth(qMin(value * 2, qreal(1.0)) * sizeHint().width());
    update();
}

void SmallJobLabel::slotTimeLineFinished()
{
    if (m_timeLine->direction() == QTimeLine::Forward) {
        // Show
        m_action->setVisible(true);
    } else {
        // Hide
        m_action->setVisible(false);
        setText(QString());
    }
}

void SmallJobLabel::slotSetJobCount(int jobCount)
{
    QMutexLocker lk(&m_locker);
    if (jobCount > 0) {
        // prepare animation
        setText(i18np("%1 job", "%1 jobs", jobCount));
        setToolTip(i18np("%1 pending job", "%1 pending jobs", jobCount));

        if (style()->styleHint(QStyle::SH_Widget_Animate, nullptr, this) != 0) {
            setFixedWidth(sizeHint().width());
            m_action->setVisible(true);
            return;
        }

        if (m_action->isVisible()) {
            setFixedWidth(sizeHint().width());
            update();
            return;
        }

        setFixedWidth(0);
        m_action->setVisible(true);
        int wantedWidth = sizeHint().width();
        setGeometry(-wantedWidth, 0, wantedWidth, height());
        m_timeLine->setDirection(QTimeLine::Forward);
        if (m_timeLine->state() == QTimeLine::NotRunning) {
            m_timeLine->start();
        }
    } else {
        if (style()->styleHint(QStyle::SH_Widget_Animate, nullptr, this) != 0) {
            setFixedWidth(0);
            m_action->setVisible(false);
            return;
        }
        // hide
        m_timeLine->setDirection(QTimeLine::Backward);
        if (m_timeLine->state() == QTimeLine::NotRunning) {
            m_timeLine->start();
        }
    }
}

LineEventEater::LineEventEater(QObject *parent)
    : QObject(parent)
{
}

bool LineEventEater::eventFilter(QObject *obj, QEvent *event)
{
    switch (event->type()) {
    case QEvent::ShortcutOverride:
        if (((QKeyEvent *)event)->key() == Qt::Key_Escape) {
            emit clearSearchLine();
        }
        break;
    case QEvent::Resize:
        // Workaround Qt BUG 54676
        emit showClearButton(((QResizeEvent *)event)->size().width() > QFontMetrics(QApplication::font()).averageCharWidth() * 8);
        break;
    default:
        break;
    }
    return QObject::eventFilter(obj, event);
}

Bin::Bin(std::shared_ptr<ProjectItemModel> model, QWidget *parent)
    : QWidget(parent)
    , isLoading(false)
    , m_itemModel(std::move(model))
    , m_itemView(nullptr)
    , m_doc(nullptr)
    , m_extractAudioAction(nullptr)
    , m_transcodeAction(nullptr)
    , m_clipsActionsMenu(nullptr)
    , m_inTimelineAction(nullptr)
    , m_listType((BinViewType)KdenliveSettings::binMode())
    , m_iconSize(160, 90)
    , m_propertiesPanel(nullptr)
    , m_blankThumb()
    , m_invalidClipDialog(nullptr)
    , m_gainedFocus(false)
    , m_audioDuration(0)
    , m_processedAudio(0)
{
    m_layout = new QVBoxLayout(this);

    // Create toolbar for buttons
    m_toolbar = new QToolBar(this);
    int size = style()->pixelMetric(QStyle::PM_SmallIconSize);
    QSize iconSize(size, size);
    m_toolbar->setIconSize(iconSize);
    m_toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_layout->addWidget(m_toolbar);
    m_layout->setSpacing(0);
    m_layout->setContentsMargins(0, 0, 0, 0);
    // Search line
    m_searchLine = new QLineEdit(this);
    m_searchLine->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    // m_searchLine->setClearButtonEnabled(true);
    m_searchLine->setPlaceholderText(i18n("Search..."));
    m_searchLine->setFocusPolicy(Qt::ClickFocus);

    auto *leventEater = new LineEventEater(this);
    m_searchLine->installEventFilter(leventEater);
    connect(leventEater, &LineEventEater::clearSearchLine, m_searchLine, &QLineEdit::clear);
    connect(leventEater, &LineEventEater::showClearButton, this, &Bin::showClearButton);

    setFocusPolicy(Qt::ClickFocus);

    connect(m_itemModel.get(), &ProjectItemModel::refreshPanel, this, &Bin::refreshPanel);
    connect(m_itemModel.get(), &ProjectItemModel::refreshClip, this, &Bin::refreshClip);

    connect(m_itemModel.get(), static_cast<void (ProjectItemModel::*)(const QStringList &, const QModelIndex &)>(&ProjectItemModel::itemDropped), this,
            static_cast<void (Bin::*)(const QStringList &, const QModelIndex &)>(&Bin::slotItemDropped));
    connect(m_itemModel.get(), static_cast<void (ProjectItemModel::*)(const QList<QUrl> &, const QModelIndex &)>(&ProjectItemModel::itemDropped), this,
            static_cast<void (Bin::*)(const QList<QUrl> &, const QModelIndex &)>(&Bin::slotItemDropped));
    connect(m_itemModel.get(), &ProjectItemModel::effectDropped, this, &Bin::slotEffectDropped);
    connect(m_itemModel.get(), &QAbstractItemModel::dataChanged, this, &Bin::slotItemEdited);
    connect(this, &Bin::refreshPanel, this, &Bin::doRefreshPanel);

    // Zoom slider
    QWidget *container = new QWidget(this);
    auto *lay = new QHBoxLayout;
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setMaximumWidth(100);
    m_slider->setMinimumWidth(40);
    m_slider->setRange(0, 10);
    m_slider->setValue(KdenliveSettings::bin_zoom());
    connect(m_slider, &QAbstractSlider::valueChanged, this, &Bin::slotSetIconSize);
    auto *tb1 = new QToolButton(this);
    tb1->setIcon(QIcon::fromTheme(QStringLiteral("zoom-in")));
    connect(tb1, &QToolButton::clicked, [&]() { m_slider->setValue(qMin(m_slider->value() + 1, m_slider->maximum())); });
    auto *tb2 = new QToolButton(this);
    tb2->setIcon(QIcon::fromTheme(QStringLiteral("zoom-out")));
    connect(tb2, &QToolButton::clicked, [&]() { m_slider->setValue(qMax(m_slider->value() - 1, m_slider->minimum())); });
    lay->addWidget(tb2);
    lay->addWidget(m_slider);
    lay->addWidget(tb1);
    container->setLayout(lay);
    auto *widgetslider = new QWidgetAction(this);
    widgetslider->setDefaultWidget(container);

    // View type
    KSelectAction *listType = new KSelectAction(QIcon::fromTheme(QStringLiteral("view-list-tree")), i18n("View Mode"), this);
    pCore->window()->actionCollection()->addAction(QStringLiteral("bin_view_mode"), listType);
    QAction *treeViewAction = listType->addAction(QIcon::fromTheme(QStringLiteral("view-list-tree")), i18n("Tree View"));
    listType->addAction(treeViewAction);
    treeViewAction->setData(BinTreeView);
    if (m_listType == treeViewAction->data().toInt()) {
        listType->setCurrentAction(treeViewAction);
    }
    pCore->window()->actionCollection()->addAction(QStringLiteral("bin_view_mode_tree"), treeViewAction);

    QAction *iconViewAction = listType->addAction(QIcon::fromTheme(QStringLiteral("view-list-icons")), i18n("Icon View"));
    iconViewAction->setData(BinIconView);
    if (m_listType == iconViewAction->data().toInt()) {
        listType->setCurrentAction(iconViewAction);
    }
    pCore->window()->actionCollection()->addAction(QStringLiteral("bin_view_mode_icon"), iconViewAction);

    QAction *disableEffects = new QAction(i18n("Disable Bin Effects"), this);
    connect(disableEffects, &QAction::triggered, [this](bool disable) { this->setBinEffectsEnabled(!disable); });
    disableEffects->setIcon(QIcon::fromTheme(QStringLiteral("favorite")));
    disableEffects->setData("disable_bin_effects");
    disableEffects->setCheckable(true);
    disableEffects->setChecked(false);
    pCore->window()->actionCollection()->addAction(QStringLiteral("disable_bin_effects"), disableEffects);

    listType->setToolBarMode(KSelectAction::MenuMode);
    connect(listType, static_cast<void (KSelectAction::*)(QAction *)>(&KSelectAction::triggered), this, &Bin::slotInitView);

    // Settings menu
    QMenu *settingsMenu = new QMenu(i18n("Settings"), this);
    settingsMenu->addAction(listType);
    QMenu *sliderMenu = new QMenu(i18n("Zoom"), this);
    sliderMenu->setIcon(QIcon::fromTheme(QStringLiteral("zoom-in")));
    sliderMenu->addAction(widgetslider);
    settingsMenu->addMenu(sliderMenu);

    // Column show / hide actions
    m_showDate = new QAction(i18n("Show date"), this);
    m_showDate->setCheckable(true);
    connect(m_showDate, &QAction::triggered, this, &Bin::slotShowDateColumn);
    m_showDesc = new QAction(i18n("Show description"), this);
    m_showDesc->setCheckable(true);
    connect(m_showDesc, &QAction::triggered, this, &Bin::slotShowDescColumn);
    settingsMenu->addAction(m_showDate);
    settingsMenu->addAction(m_showDesc);
    settingsMenu->addAction(disableEffects);
    auto *button = new QToolButton;
    button->setIcon(QIcon::fromTheme(QStringLiteral("kdenlive-menu")));
    button->setToolTip(i18n("Options"));
    button->setMenu(settingsMenu);
    button->setPopupMode(QToolButton::InstantPopup);
    m_toolbar->addWidget(button);

    // small info button for pending jobs
    m_infoLabel = new SmallJobLabel(this);
    m_infoLabel->setStyleSheet(SmallJobLabel::getStyleSheet(palette()));
    connect(pCore->jobManager().get(), &JobManager::jobCount, m_infoLabel, &SmallJobLabel::slotSetJobCount);
    QAction *infoAction = m_toolbar->addWidget(m_infoLabel);
    m_jobsMenu = new QMenu(this);
    // connect(m_jobsMenu, &QMenu::aboutToShow, this, &Bin::slotPrepareJobsMenu);
    m_cancelJobs = new QAction(i18n("Cancel All Jobs"), this);
    m_cancelJobs->setCheckable(false);
    m_discardCurrentClipJobs = new QAction(i18n("Cancel Current Clip Jobs"), this);
    m_discardCurrentClipJobs->setCheckable(false);
    m_discardPendingJobs = new QAction(i18n("Cancel Pending Jobs"), this);
    m_discardPendingJobs->setCheckable(false);
    m_jobsMenu->addAction(m_cancelJobs);
    m_jobsMenu->addAction(m_discardCurrentClipJobs);
    m_jobsMenu->addAction(m_discardPendingJobs);
    m_infoLabel->setMenu(m_jobsMenu);
    m_infoLabel->setAction(infoAction);

    connect(m_discardCurrentClipJobs, &QAction::triggered, [&]() {
        const QString currentId = m_monitor->activeClipId();
        if (!currentId.isEmpty()) {
            pCore->jobManager()->discardJobs(currentId);
        }
    });
    connect(m_cancelJobs, &QAction::triggered, [&]() {
        pCore->jobManager()->slotCancelJobs();
    });
    connect(m_discardPendingJobs, &QAction::triggered, [&]() {
        pCore->jobManager()->slotCancelPendingJobs();
    });

    // Hack, create toolbar spacer
    QWidget *spacer = new QWidget();
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_toolbar->addWidget(spacer);

    // Add search line
    m_toolbar->addWidget(m_searchLine);

    m_binTreeViewDelegate = new BinItemDelegate(this);
    m_binListViewDelegate = new BinListItemDelegate(this);
    // connect(pCore->projectManager(), SIGNAL(projectOpened(Project*)), this, SLOT(setProject(Project*)));
    m_headerInfo = QByteArray::fromBase64(KdenliveSettings::treeviewheaders().toLatin1());
    m_propertiesPanel = new QScrollArea(this);
    m_propertiesPanel->setFrameShape(QFrame::NoFrame);
    // Insert listview
    m_itemView = new MyTreeView(this);
    m_layout->addWidget(m_itemView);
    // Info widget for failed jobs, other errors
    m_infoMessage = new KMessageWidget(this);
    m_layout->addWidget(m_infoMessage);
    m_infoMessage->setCloseButtonVisible(false);
    connect(m_infoMessage, &KMessageWidget::hideAnimationFinished, this, &Bin::slotResetInfoMessage);
    // m_infoMessage->setWordWrap(true);
    m_infoMessage->hide();
    connect(this, &Bin::requesteInvalidRemoval, this, &Bin::slotQueryRemoval);
    connect(this, SIGNAL(displayBinMessage(QString, KMessageWidget::MessageType)), this, SLOT(doDisplayMessage(QString, KMessageWidget::MessageType)));
    wheelAccumulatedDelta = 0;
}

Bin::~Bin()
{
    pCore->jobManager()->slotCancelJobs();
    blockSignals(true);
    m_proxyModel->selectionModel()->blockSignals(true);
    setEnabled(false);
    m_propertiesPanel = nullptr;
    abortOperations();
    m_itemModel->clean();
}

QDockWidget *Bin::clipPropertiesDock()
{
    return m_propertiesDock;
}

void Bin::abortOperations()
{
    m_infoMessage->hide();
    blockSignals(true);
    if (m_propertiesPanel) {
        for (QWidget *w : m_propertiesPanel->findChildren<ClipPropertiesController *>()) {
            delete w;
        }
    }
    delete m_itemView;
    m_itemView = nullptr;
    blockSignals(false);
}

bool Bin::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        if (!m_monitor->isActive()) {
            m_monitor->slotActivateMonitor();
        }
        bool success = QWidget::eventFilter(obj, event);
        if (m_gainedFocus) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            auto *view = qobject_cast<QAbstractItemView *>(obj->parent());
            if (view) {
                QModelIndex idx = view->indexAt(mouseEvent->pos());
                m_gainedFocus = false;
                if (idx.isValid() && m_proxyModel) {
                    std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(idx));
                    editMasterEffect(item);
                } else {
                    editMasterEffect(nullptr);
                }
            }
            // make sure we discard the focus indicator
            m_gainedFocus = false;
        }
        return success;
    }
    if (event->type() == QEvent::MouseButtonDblClick) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        auto *view = qobject_cast<QAbstractItemView *>(obj->parent());
        if (view) {
            QModelIndex idx = view->indexAt(mouseEvent->pos());
            if (!idx.isValid()) {
                // User double clicked on empty area
                slotAddClip();
            } else {
                slotItemDoubleClicked(idx, mouseEvent->pos());
            }
        } else {
            qCDebug(KDENLIVE_LOG) << " +++++++ NO VIEW-------!!";
        }
        return true;
    }
    if (event->type() == QEvent::Wheel) {
        auto *e = static_cast<QWheelEvent *>(event);
        if ((e != nullptr) && e->modifiers() == Qt::ControlModifier) {
            wheelAccumulatedDelta += e->delta();
            if (abs(wheelAccumulatedDelta) >= QWheelEvent::DefaultDeltasPerStep) {
                slotZoomView(wheelAccumulatedDelta > 0);
            }
            // emit zoomView(e->delta() > 0);
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

void Bin::refreshIcons()
{
    QList<QMenu *> allMenus = this->findChildren<QMenu *>();
    for (int i = 0; i < allMenus.count(); i++) {
        QMenu *m = allMenus.at(i);
        QIcon ic = m->icon();
        if (ic.isNull() || ic.name().isEmpty()) {
            continue;
        }
        QIcon newIcon = QIcon::fromTheme(ic.name());
        m->setIcon(newIcon);
    }
    QList<QToolButton *> allButtons = this->findChildren<QToolButton *>();
    for (int i = 0; i < allButtons.count(); i++) {
        QToolButton *m = allButtons.at(i);
        QIcon ic = m->icon();
        if (ic.isNull() || ic.name().isEmpty()) {
            continue;
        }
        QIcon newIcon = QIcon::fromTheme(ic.name());
        m->setIcon(newIcon);
    }
}

void Bin::slotSaveHeaders()
{
    if ((m_itemView != nullptr) && m_listType == BinTreeView) {
        // save current treeview state (column width)
        auto *view = static_cast<QTreeView *>(m_itemView);
        m_headerInfo = view->header()->saveState();
        KdenliveSettings::setTreeviewheaders(m_headerInfo.toBase64());
    }
}

void Bin::slotZoomView(bool zoomIn)
{
    wheelAccumulatedDelta = 0;
    if (m_itemModel->rowCount() == 0) {
        // Don't zoom on empty bin
        return;
    }
    int progress = (zoomIn) ? 1 : -1;
    m_slider->setValue(m_slider->value() + progress);
}

Monitor *Bin::monitor()
{
    return m_monitor;
}

void Bin::slotAddClip()
{
    // Check if we are in a folder
    QString parentFolder = getCurrentFolder();
    ClipCreationDialog::createClipsCommand(m_doc, parentFolder, m_itemModel);
}

std::shared_ptr<ProjectClip> Bin::getFirstSelectedClip()
{
    const QModelIndexList indexes = m_proxyModel->selectionModel()->selectedIndexes();
    if (indexes.isEmpty()) {
        return std::shared_ptr<ProjectClip>();
    }
    for (const QModelIndex &ix : indexes) {
        std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(ix));
        if (item->itemType() == AbstractProjectItem::ClipItem) {
            auto clip = std::static_pointer_cast<ProjectClip>(item);
            if (clip) {
                return clip;
            }
        }
    }
    return nullptr;
}

void Bin::slotDeleteClip()
{
    const QModelIndexList indexes = m_proxyModel->selectionModel()->selectedIndexes();
    std::vector<std::shared_ptr<AbstractProjectItem>> items;
    bool included = false;
    bool usedFolder = false;
    auto checkInclusion = [](bool accum, std::shared_ptr<TreeItem> item) {
        return accum || std::static_pointer_cast<AbstractProjectItem>(item)->isIncludedInTimeline();
    };
    for (const QModelIndex &ix : indexes) {
        if (!ix.isValid() || ix.column() != 0) {
            continue;
        }
        std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(ix));
        if (!item) {
            qDebug() << "Suspicious: item not found when trying to delete";
            continue;
        }
        included = included || item->accumulate(false, checkInclusion);
        // Check if we are deleting non-empty folders:
        usedFolder = usedFolder || item->childCount() > 0;
        items.push_back(item);
    }
    if (included && (KMessageBox::warningContinueCancel(this, i18n("This will delete all selected clips from the timeline")) != KMessageBox::Continue)) {
        return;
    }
    if (usedFolder && (KMessageBox::warningContinueCancel(this, i18n("This will delete all folder content")) != KMessageBox::Continue)) {
        return;
    }
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    for (const auto &item : items) {
        m_itemModel->requestBinClipDeletion(item, undo, redo);
    }
    pCore->pushUndo(undo, redo, i18n("Delete bin Clips"));
}

void Bin::slotReloadClip()
{
    const QModelIndexList indexes = m_proxyModel->selectionModel()->selectedIndexes();
    for (const QModelIndex &ix : indexes) {
        if (!ix.isValid() || ix.column() != 0) {
            continue;
        }
        std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(ix));
        std::shared_ptr<ProjectClip> currentItem = nullptr;
        if (item->itemType() == AbstractProjectItem::ClipItem) {
            currentItem = std::static_pointer_cast<ProjectClip>(item);
        } else if (item->itemType() == AbstractProjectItem::SubClipItem) {
            currentItem = std::static_pointer_cast<ProjectSubClip>(item)->getMasterClip();
        }
        if (currentItem) {
            emit openClip(std::shared_ptr<ProjectClip>());
            if (currentItem->clipType() == ClipType::Playlist) {
                // Check if a clip inside playlist is missing
                QString path = currentItem->url();
                QFile f(path);
                QDomDocument doc;
                doc.setContent(&f, false);
                f.close();
                DocumentChecker d(QUrl::fromLocalFile(path), doc);
                if (!d.hasErrorInClips() && doc.documentElement().hasAttribute(QStringLiteral("modified"))) {
                    QString backupFile = path + QStringLiteral(".backup");
                    KIO::FileCopyJob *copyjob = KIO::file_copy(QUrl::fromLocalFile(path), QUrl::fromLocalFile(backupFile));
                    if (copyjob->exec()) {
                        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                            KMessageBox::sorry(this, i18n("Unable to write to file %1", path));
                        } else {
                            QTextStream out(&f);
                            out << doc.toString();
                            f.close();
                            KMessageBox::information(
                                this,
                                i18n("Your project file was modified by Kdenlive.\nTo make sure you do not lose data, a backup copy called %1 was created.",
                                     backupFile));
                        }
                    }
                }
            }
            currentItem->reloadProducer(false, true);
        }
    }
}

void Bin::slotLocateClip()
{
    const QModelIndexList indexes = m_proxyModel->selectionModel()->selectedIndexes();
    for (const QModelIndex &ix : indexes) {
        if (!ix.isValid() || ix.column() != 0) {
            continue;
        }
        std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(ix));
        std::shared_ptr<ProjectClip> currentItem = nullptr;
        if (item->itemType() == AbstractProjectItem::ClipItem) {
            currentItem = std::static_pointer_cast<ProjectClip>(item);
        } else if (item->itemType() == AbstractProjectItem::SubClipItem) {
            currentItem = std::static_pointer_cast<ProjectSubClip>(item)->getMasterClip();
        }
        if (currentItem) {
            QUrl url = QUrl::fromLocalFile(currentItem->url()).adjusted(QUrl::RemoveFilename);
            bool exists = QFile(url.toLocalFile()).exists();
            if (currentItem->hasUrl() && exists) {
                QDesktopServices::openUrl(url);
                qCDebug(KDENLIVE_LOG) << "  / / " + url.toString();
            } else {
                if (!exists) {
                    pCore->displayMessage(i18n("Could not locate %1", url.toString()), ErrorMessage, 300);
                }
                return;
            }
        }
    }
}

void Bin::slotDuplicateClip()
{
    const QModelIndexList indexes = m_proxyModel->selectionModel()->selectedIndexes();
    for (const QModelIndex &ix : indexes) {
        if (!ix.isValid() || ix.column() != 0) {
            continue;
        }
        std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(ix));
        if (item->itemType() == AbstractProjectItem::ClipItem) {
            auto currentItem = std::static_pointer_cast<ProjectClip>(item);
            if (currentItem) {
                QDomDocument doc;
                QDomElement xml = currentItem->toXml(doc);
                if (!xml.isNull()) {
                    QString currentName = Xml::getXmlProperty(xml, QStringLiteral("kdenlive:clipname"));
                    if (currentName.isEmpty()) {
                        QUrl url = QUrl::fromLocalFile(Xml::getXmlProperty(xml, QStringLiteral("resource")));
                        if (url.isValid()) {
                            currentName = url.fileName();
                        }
                    }
                    if (!currentName.isEmpty()) {
                        currentName.append(i18nc("append to clip name to indicate a copied idem", " (copy)"));
                        Xml::setXmlProperty(xml, QStringLiteral("kdenlive:clipname"), currentName);
                    }
                    QString id;
                    m_itemModel->requestAddBinClip(id, xml, item->parent()->clipId(), i18n("Duplicate clip"));
                    selectClipById(id);
                }
            }
        } else if (item->itemType() == AbstractProjectItem::SubClipItem) {
            auto currentItem = std::static_pointer_cast<ProjectSubClip>(item);
            QString id;
            QPoint clipZone = currentItem->zone();
            m_itemModel->requestAddBinSubClip(id, clipZone.x(), clipZone.y(), QString(), currentItem->getMasterClip()->clipId());
            selectClipById(id);
        }
    }
}

void Bin::setMonitor(Monitor *monitor)
{
    m_monitor = monitor;
    connect(m_monitor, &Monitor::addClipToProject, this, &Bin::slotAddClipToProject);
    connect(m_monitor, &Monitor::refreshCurrentClip, this, &Bin::slotOpenCurrent);
    connect(this, &Bin::openClip, [&](std::shared_ptr<ProjectClip> clip, int in, int out) { m_monitor->slotOpenClip(clip, in, out); });
}

void Bin::setDocument(KdenliveDoc *project)
{
    blockSignals(true);
    if (m_proxyModel) {
        m_proxyModel->selectionModel()->blockSignals(true);
    }
    setEnabled(false);

    // Cleanup previous project
    m_itemModel->clean();
    delete m_itemView;
    m_itemView = nullptr;
    m_doc = project;
    m_infoLabel->slotSetJobCount(0);
    int iconHeight = QFontInfo(font()).pixelSize() * 3.5;
    m_iconSize = QSize(iconHeight * pCore->getCurrentDar(), iconHeight);
    setEnabled(true);
    blockSignals(false);
    if (m_proxyModel) {
        m_proxyModel->selectionModel()->blockSignals(false);
    }
    connect(m_proxyAction, SIGNAL(toggled(bool)), m_doc, SLOT(slotProxyCurrentItem(bool)));

    // connect(m_itemModel, SIGNAL(dataChanged(QModelIndex,QModelIndex)), m_itemView
    // connect(m_itemModel, SIGNAL(updateCurrentItem()), this, SLOT(autoSelect()));
    slotInitView(nullptr);
    bool binEffectsDisabled = getDocumentProperty(QStringLiteral("disablebineffects")).toInt() == 1;
    setBinEffectsEnabled(!binEffectsDisabled);
}

void Bin::createClip(const QDomElement &xml)
{
    // Check if clip should be in a folder
    QString groupId = ProjectClip::getXmlProperty(xml, QStringLiteral("kdenlive:folderid"));
    std::shared_ptr<ProjectFolder> parentFolder = m_itemModel->getFolderByBinId(groupId);
    if (!parentFolder) {
        parentFolder = m_itemModel->getRootFolder();
    }
    QString path = Xml::getXmlProperty(xml, QStringLiteral("resource"));
    if (path.endsWith(QStringLiteral(".mlt")) || path.endsWith(QStringLiteral(".kdenlive"))) {
        QFile f(path);
        QDomDocument doc;
        doc.setContent(&f, false);
        f.close();
        DocumentChecker d(QUrl::fromLocalFile(path), doc);
        if (!d.hasErrorInClips() && doc.documentElement().hasAttribute(QStringLiteral("modified"))) {
            QString backupFile = path + QStringLiteral(".backup");
            KIO::FileCopyJob *copyjob = KIO::file_copy(QUrl::fromLocalFile(path), QUrl::fromLocalFile(backupFile));
            if (copyjob->exec()) {
                if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    KMessageBox::sorry(this, i18n("Unable to write to file %1", path));
                } else {
                    QTextStream out(&f);
                    out << doc.toString();
                    f.close();
                    KMessageBox::information(
                        this, i18n("Your project file was modified by Kdenlive.\nTo make sure you do not lose data, a backup copy called %1 was created.",
                                   backupFile));
                }
            }
        }
    }
    QString id = Xml::getTagContentByAttribute(xml, QStringLiteral("property"), QStringLiteral("name"), QStringLiteral("kdenlive:id"));
    if (id.isEmpty()) {
        id = QString::number(m_itemModel->getFreeClipId());
    }
    auto newClip = ProjectClip::construct(id, xml, m_blankThumb, m_itemModel);
    parentFolder->appendChild(newClip);
}

void Bin::slotAddFolder()
{
    auto parentFolder = m_itemModel->getFolderByBinId(getCurrentFolder());
    qDebug() << "pranteforder id" << parentFolder->clipId();
    QString newId;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    m_itemModel->requestAddFolder(newId, i18n("Folder"), parentFolder->clipId(), undo, redo);
    pCore->pushUndo(undo, redo, i18n("Create bin folder"));

    // Edit folder name
    auto folder = m_itemModel->getFolderByBinId(newId);
    auto ix = m_itemModel->getIndexFromItem(folder);
    qDebug() << "selecting" << ix;
    if (ix.isValid()) {
        qDebug() << "ix valid";
        m_proxyModel->selectionModel()->clearSelection();
        int row = ix.row();
        const QModelIndex id = m_itemModel->index(row, 0, ix.parent());
        const QModelIndex id2 = m_itemModel->index(row, m_itemModel->columnCount() - 1, ix.parent());
        if (id.isValid() && id2.isValid()) {
            m_proxyModel->selectionModel()->select(QItemSelection(m_proxyModel->mapFromSource(id), m_proxyModel->mapFromSource(id2)),
                                                   QItemSelectionModel::Select);
        }
        m_itemView->edit(m_proxyModel->mapFromSource(ix));
    }
}

QModelIndex Bin::getIndexForId(const QString &id, bool folderWanted) const
{
    QModelIndexList items = m_itemModel->match(m_itemModel->index(0, 0), AbstractProjectItem::DataId, QVariant::fromValue(id), 1, Qt::MatchRecursive);
    for (int i = 0; i < items.count(); i++) {
        std::shared_ptr<AbstractProjectItem> currentItem = m_itemModel->getBinItemByIndex(items.at(i));
        AbstractProjectItem::PROJECTITEMTYPE type = currentItem->itemType();
        if (folderWanted && type == AbstractProjectItem::FolderItem) {
            // We found our folder
            return items.at(i);
        }
        if (!folderWanted && type == AbstractProjectItem::ClipItem) {
            // We found our clip
            return items.at(i);
        }
    }
    return {};
}

void Bin::selectAll()
{
    m_proxyModel->selectAll();
}

void Bin::selectClipById(const QString &clipId, int frame, const QPoint &zone)
{
    if (m_monitor->activeClipId() == clipId) {
        std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(clipId);
        if (clip) {
            QModelIndex ix = m_itemModel->getIndexFromItem(clip);
            int row = ix.row();
            const QModelIndex id = m_itemModel->index(row, 0, ix.parent());
            const QModelIndex id2 = m_itemModel->index(row, m_itemModel->columnCount() - 1, ix.parent());
            if (id.isValid() && id2.isValid()) {
                m_proxyModel->selectionModel()->select(QItemSelection(m_proxyModel->mapFromSource(id), m_proxyModel->mapFromSource(id2)), QItemSelectionModel::SelectCurrent);
            }
            m_itemView->scrollTo(m_proxyModel->mapFromSource(ix), QAbstractItemView::PositionAtCenter);
        }
    } else {
        m_proxyModel->selectionModel()->clearSelection();
        std::shared_ptr<ProjectClip> clip = getBinClip(clipId);
        if (clip == nullptr) {
            return;
        }
        selectClip(clip);
    }
    if (frame > -1) {
        m_monitor->slotSeek(frame);
    } else {
        m_monitor->slotActivateMonitor();
    }
    if (!zone.isNull()) {
        m_monitor->slotLoadClipZone(zone);
    }
}

void Bin::selectProxyModel(const QModelIndex &id)
{
    if (isLoading) {
        // return;
    }
    if (id.isValid()) {
        if (id.column() != 0) {
            return;
        }
        std::shared_ptr<AbstractProjectItem> currentItem = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(id));
        if (currentItem) {
            // Set item as current so that it displays its content in clip monitor
            setCurrent(currentItem);
            if (currentItem->itemType() == AbstractProjectItem::ClipItem) {
                m_reloadAction->setEnabled(true);
                m_locateAction->setEnabled(true);
                m_duplicateAction->setEnabled(true);
                std::shared_ptr<ProjectClip> clip = std::static_pointer_cast<ProjectClip>(currentItem);
                ClipType::ProducerType type = clip->clipType();
                m_openAction->setEnabled(type == ClipType::Image || type == ClipType::Audio || type == ClipType::Text || type == ClipType::TextTemplate);
                showClipProperties(clip, false);
                m_deleteAction->setText(i18n("Delete Clip"));
                m_proxyAction->setText(i18n("Proxy Clip"));
            } else if (currentItem->itemType() == AbstractProjectItem::FolderItem) {
                // A folder was selected, disable editing clip
                m_openAction->setEnabled(false);
                m_reloadAction->setEnabled(false);
                m_locateAction->setEnabled(false);
                m_duplicateAction->setEnabled(false);
                m_deleteAction->setText(i18n("Delete Folder"));
                m_proxyAction->setText(i18n("Proxy Folder"));
            } else if (currentItem->itemType() == AbstractProjectItem::SubClipItem) {
                showClipProperties(std::static_pointer_cast<ProjectClip>(currentItem->parent()), false);
                m_openAction->setEnabled(false);
                m_reloadAction->setEnabled(false);
                m_locateAction->setEnabled(false);
                m_duplicateAction->setEnabled(false);
                m_deleteAction->setText(i18n("Delete Clip"));
                m_proxyAction->setText(i18n("Proxy Clip"));
            }
            m_deleteAction->setEnabled(true);
        } else {
            m_reloadAction->setEnabled(false);
            m_locateAction->setEnabled(false);
            m_duplicateAction->setEnabled(false);
            m_openAction->setEnabled(false);
            m_deleteAction->setEnabled(false);
        }
    } else {
        // No item selected in bin
        m_openAction->setEnabled(false);
        m_deleteAction->setEnabled(false);
        showClipProperties(nullptr);
        emit requestClipShow(nullptr);
        // clear effect stack
        emit requestShowEffectStack(QString(), nullptr, QSize(), false);
        // Display black bg in clip monitor
        emit openClip(std::shared_ptr<ProjectClip>());
    }
}

std::vector<QString> Bin::selectedClipsIds()
{
    const QModelIndexList indexes = m_proxyModel->selectionModel()->selectedIndexes();
    std::vector<QString> ids;
    // We define the lambda that will be executed on each item of the subset of nodes of the tree that are selected
    auto itemAdder = [&ids](std::vector<QString> &ids_vec, std::shared_ptr<TreeItem> item) {
        auto binItem = std::static_pointer_cast<AbstractProjectItem>(item);
        if (binItem->itemType() == AbstractProjectItem::ClipItem) {
            ids.push_back(binItem->clipId());
        }
        return ids_vec;
    };
    for (const QModelIndex &ix : indexes) {
        if (!ix.isValid() || ix.column() != 0) {
            continue;
        }
        std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(ix));
        item->accumulate(ids, itemAdder);
    }
    return ids;
}

QList<std::shared_ptr<ProjectClip>> Bin::selectedClips()
{
    auto ids = selectedClipsIds();
    QList<std::shared_ptr<ProjectClip>> ret;
    for (const auto &id : ids) {
        ret.push_back(m_itemModel->getClipByBinID(id));
    }
    return ret;
}

void Bin::slotInitView(QAction *action)
{
    if (action) {
        if (m_proxyModel) {
            m_proxyModel->selectionModel()->clearSelection();
        }
        int viewType = action->data().toInt();
        KdenliveSettings::setBinMode(viewType);
        if (viewType == m_listType) {
            return;
        }
        if (m_listType == BinTreeView) {
            // save current treeview state (column width)
            auto *view = static_cast<QTreeView *>(m_itemView);
            m_headerInfo = view->header()->saveState();
            m_showDate->setEnabled(true);
            m_showDesc->setEnabled(true);
            m_upAction->setVisible(true);
            m_upAction->setEnabled(false);
        } else {
            // remove the current folderUp item if any
            m_upAction->setVisible(false);
        }
        m_listType = static_cast<BinViewType>(viewType);
    }
    if (m_itemView) {
        delete m_itemView;
    }

    switch (m_listType) {
    case BinIconView:
        m_itemView = new MyListView(this);
        m_showDate->setEnabled(false);
        m_showDesc->setEnabled(false);
        break;
    default:
        m_itemView = new MyTreeView(this);
        m_showDate->setEnabled(true);
        m_showDesc->setEnabled(true);
        break;
    }
    m_itemView->setMouseTracking(true);
    m_itemView->viewport()->installEventFilter(this);
    QSize zoom = m_iconSize * (m_slider->value() / 4.0);
    m_itemView->setIconSize(zoom);
    QPixmap pix(zoom);
    pix.fill(Qt::lightGray);
    m_blankThumb.addPixmap(pix);
    m_binTreeViewDelegate->setDar(pCore->getCurrentDar());
    m_binListViewDelegate->setDar(pCore->getCurrentDar());
    m_proxyModel.reset(new ProjectSortProxyModel(this));
    // Connect models
    m_proxyModel->setSourceModel(m_itemModel.get());
    connect(m_itemModel.get(), &QAbstractItemModel::dataChanged, m_proxyModel.get(), &ProjectSortProxyModel::slotDataChanged);
    connect(m_proxyModel.get(), &ProjectSortProxyModel::selectModel, this, &Bin::selectProxyModel);
    connect(m_proxyModel.get(), &QAbstractItemModel::layoutAboutToBeChanged, this, &Bin::slotSetSorting);
    connect(m_searchLine, &QLineEdit::textChanged, m_proxyModel.get(), &ProjectSortProxyModel::slotSetSearchString);
    m_itemView->setModel(m_proxyModel.get());
    m_itemView->setSelectionModel(m_proxyModel->selectionModel());
    m_proxyModel->setDynamicSortFilter(true);
    m_layout->insertWidget(1, m_itemView);
    // Reset drag type to normal
    m_itemModel->setDragType(PlaylistState::Disabled);

    // setup some default view specific parameters
    if (m_listType == BinTreeView) {
        m_itemView->setItemDelegate(m_binTreeViewDelegate);
        auto *view = static_cast<MyTreeView *>(m_itemView);
        view->setSortingEnabled(true);
        view->setWordWrap(true);
        connect(view, &MyTreeView::updateDragMode, m_itemModel.get(), &ProjectItemModel::setDragType, Qt::DirectConnection);
        connect(view, &MyTreeView::displayBinFrame, this, &Bin::showBinFrame);
        if (!m_headerInfo.isEmpty()) {
            view->header()->restoreState(m_headerInfo);
        } else {
            view->header()->resizeSections(QHeaderView::ResizeToContents);
            view->resizeColumnToContents(0);
            view->setColumnHidden(1, true);
            view->setColumnHidden(2, true);
        }
        m_showDate->setChecked(!view->isColumnHidden(1));
        m_showDesc->setChecked(!view->isColumnHidden(2));
        connect(view->header(), &QHeaderView::sectionResized, this, &Bin::slotSaveHeaders);
        connect(view->header(), &QHeaderView::sectionClicked, this, &Bin::slotSaveHeaders);
        connect(view, &MyTreeView::focusView, this, &Bin::slotGotFocus);
    } else if (m_listType == BinIconView) {
        m_itemView->setItemDelegate(m_binListViewDelegate);
        auto *view = static_cast<MyListView *>(m_itemView);
        view->setGridSize(QSize(zoom.width() * 1.2, zoom.width()));
        connect(view, &MyListView::focusView, this, &Bin::slotGotFocus);
        connect(view, &MyListView::displayBinFrame, this, &Bin::showBinFrame);
    }
    m_itemView->setEditTriggers(QAbstractItemView::NoEditTriggers); // DoubleClicked);
    m_itemView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_itemView->setDragDropMode(QAbstractItemView::DragDrop);
    m_itemView->setAlternatingRowColors(true);
    m_itemView->setAcceptDrops(true);
    m_itemView->setFocus();
}

void Bin::slotSetIconSize(int size)
{
    if (!m_itemView) {
        return;
    }
    KdenliveSettings::setBin_zoom(size);
    QSize zoom = m_iconSize;
    zoom = zoom * (size / 4.0);
    m_itemView->setIconSize(zoom);
    if (m_listType == BinIconView) {
        auto *view = static_cast<MyListView *>(m_itemView);
        view->setGridSize(QSize(zoom.width() * 1.2, zoom.width()));
    }
    QPixmap pix(zoom);
    pix.fill(Qt::lightGray);
    m_blankThumb.addPixmap(pix);
}

void Bin::rebuildMenu()
{
    m_transcodeAction = static_cast<QMenu *>(pCore->window()->factory()->container(QStringLiteral("transcoders"), pCore->window()));
    m_extractAudioAction = static_cast<QMenu *>(pCore->window()->factory()->container(QStringLiteral("extract_audio"), pCore->window()));
    m_clipsActionsMenu = static_cast<QMenu *>(pCore->window()->factory()->container(QStringLiteral("clip_actions"), pCore->window()));
    m_menu->insertMenu(m_reloadAction, m_extractAudioAction);
    m_menu->insertMenu(m_reloadAction, m_transcodeAction);
    m_menu->insertMenu(m_reloadAction, m_clipsActionsMenu);
}

void Bin::contextMenuEvent(QContextMenuEvent *event)
{
    bool enableClipActions = false;
    ClipType::ProducerType type = ClipType::Unknown;
    bool isFolder = false;
    bool isImported = false;
    AbstractProjectItem::PROJECTITEMTYPE itemType = AbstractProjectItem::FolderItem;
    QString clipService;
    QString audioCodec;
    if (m_itemView) {
        QModelIndex idx = m_itemView->indexAt(m_itemView->viewport()->mapFromGlobal(event->globalPos()));
        if (idx.isValid()) {
            // User right clicked on a clip
            std::shared_ptr<AbstractProjectItem> currentItem = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(idx));
            itemType = currentItem->itemType();
            if (currentItem) {
                enableClipActions = true;
                if (itemType == AbstractProjectItem::ClipItem) {
                    auto clip = std::static_pointer_cast<ProjectClip>(currentItem);
                    if (clip) {
                        m_proxyAction->blockSignals(true);
                        emit findInTimeline(clip->clipId(), clip->timelineInstances());
                        clipService = clip->getProducerProperty(QStringLiteral("mlt_service"));
                        m_proxyAction->setChecked(clip->hasProxy());
                        QList<QAction *> transcodeActions;
                        if (m_transcodeAction) {
                            transcodeActions = m_transcodeAction->actions();
                        }
                        QStringList dataList;
                        QString condition;
                        audioCodec = clip->codec(true);
                        QString videoCodec = clip->codec(false);
                        type = clip->clipType();
                        if (clip->hasUrl()) {
                            isImported = true;
                        }
                        bool noCodecInfo = false;
                        if (audioCodec.isEmpty() && videoCodec.isEmpty()) {
                            noCodecInfo = true;
                        }
                        for (int i = 0; i < transcodeActions.count(); ++i) {
                            dataList = transcodeActions.at(i)->data().toStringList();
                            if (dataList.count() > 4) {
                                condition = dataList.at(4);
                                if (condition.isEmpty()) {
                                    transcodeActions.at(i)->setEnabled(true);
                                    continue;
                                }
                                if (noCodecInfo) {
                                    // No audio / video codec, this is an MLT clip, disable conditionnal transcoding
                                    transcodeActions.at(i)->setEnabled(false);
                                    continue;
                                }
                                if (condition.startsWith(QLatin1String("vcodec"))) {
                                    transcodeActions.at(i)->setEnabled(condition.section(QLatin1Char('='), 1, 1) == videoCodec);
                                } else if (condition.startsWith(QLatin1String("acodec"))) {
                                    transcodeActions.at(i)->setEnabled(condition.section(QLatin1Char('='), 1, 1) == audioCodec);
                                }
                            }
                        }
                    }
                    m_proxyAction->blockSignals(false);
                } else {
                    // Disable find in timeline option
                    emit findInTimeline(QString());
                }
            }
        }
    }
    // Enable / disable clip actions
    m_proxyAction->setEnabled((m_doc->getDocumentProperty(QStringLiteral("enableproxy")).toInt() != 0) && enableClipActions);
    m_openAction->setEnabled(type == ClipType::Image || type == ClipType::Audio || type == ClipType::TextTemplate || type == ClipType::Text);
    m_reloadAction->setEnabled(enableClipActions);
    m_locateAction->setEnabled(enableClipActions);
    m_duplicateAction->setEnabled(enableClipActions);
    m_renameAction->setEnabled(true);

    m_editAction->setVisible(!isFolder);
    m_clipsActionsMenu->setEnabled(enableClipActions);
    m_extractAudioAction->setEnabled(enableClipActions);
    m_openAction->setVisible(itemType != AbstractProjectItem::FolderItem);
    m_reloadAction->setVisible(itemType != AbstractProjectItem::FolderItem);
    m_duplicateAction->setVisible(itemType != AbstractProjectItem::FolderItem);
    m_inTimelineAction->setVisible(itemType != AbstractProjectItem::FolderItem);
    m_renameAction->setVisible(true);

    if (m_transcodeAction) {
        m_transcodeAction->setEnabled(enableClipActions);
        m_transcodeAction->menuAction()->setVisible(itemType != AbstractProjectItem::FolderItem && (type == ClipType::Playlist || type == ClipType::Text || clipService.contains(QStringLiteral("avformat"))));
    }
    m_clipsActionsMenu->menuAction()->setVisible(
        itemType != AbstractProjectItem::FolderItem &&
        (clipService.contains(QStringLiteral("avformat")) || clipService.contains(QStringLiteral("xml")) || clipService.contains(QStringLiteral("consumer"))));
    m_extractAudioAction->menuAction()->setVisible(!isFolder && !audioCodec.isEmpty());
    m_locateAction->setVisible(itemType != AbstractProjectItem::FolderItem && (isImported));

    // Show menu
    event->setAccepted(true);
    if (enableClipActions) {
        m_menu->exec(event->globalPos());
    } else {
        // Clicked in empty area
        m_addButton->menu()->exec(event->globalPos());
    }
}

void Bin::slotItemDoubleClicked(const QModelIndex &ix, const QPoint &pos)
{
    std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(ix));
    if (m_listType == BinIconView) {
        if (item->childCount() > 0 || item->itemType() == AbstractProjectItem::FolderItem) {
            m_itemView->setRootIndex(ix);
            m_upAction->setEnabled(true);
            return;
        }
    } else {
        if (ix.column() == 0 && item->childCount() > 0) {
            QRect IconRect = m_itemView->visualRect(ix);
            IconRect.setWidth((double)IconRect.height() / m_itemView->iconSize().height() * m_itemView->iconSize().width());
            if (!pos.isNull() && (IconRect.contains(pos) || pos.y() > (IconRect.y() + IconRect.height() / 2))) {
                auto *view = static_cast<QTreeView *>(m_itemView);
                view->setExpanded(ix, !view->isExpanded(ix));
                return;
            }
        }
    }
    if (ix.isValid()) {
        QRect IconRect = m_itemView->visualRect(ix);
        IconRect.setWidth((double)IconRect.height() / m_itemView->iconSize().height() * m_itemView->iconSize().width());
        if (!pos.isNull() && ((ix.column() == 2 && item->itemType() == AbstractProjectItem::ClipItem) ||
                              (!IconRect.contains(pos) && pos.y() < (IconRect.y() + IconRect.height() / 2)))) {
            // User clicked outside icon, trigger rename
            m_itemView->edit(ix);
            return;
        }
        if (item->itemType() == AbstractProjectItem::ClipItem) {
            std::shared_ptr<ProjectClip> clip = std::static_pointer_cast<ProjectClip>(item);
            if (clip) {
                if (clip->clipType() == ClipType::Text || clip->clipType() == ClipType::TextTemplate) {
                    // m_propertiesPanel->setEnabled(false);
                    showTitleWidget(clip);
                } else {
                    slotSwitchClipProperties(clip);
                }
            }
        }
    }
}

void Bin::slotEditClip()
{
    QString panelId = m_propertiesPanel->property("clipId").toString();
    QModelIndex current = m_proxyModel->selectionModel()->currentIndex();
    std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(current));
    if (item->clipId() != panelId) {
        // wrong clip
        return;
    }
    auto clip = std::static_pointer_cast<ProjectClip>(item);
    QString parentFolder = getCurrentFolder();
    switch (clip->clipType()) {
    case ClipType::Text:
    case ClipType::TextTemplate:
        showTitleWidget(clip);
        break;
    case ClipType::SlideShow:
        showSlideshowWidget(clip);
        break;
    case ClipType::QText:
        ClipCreationDialog::createQTextClip(m_doc, parentFolder, this, clip.get());
        break;
    default:
        break;
    }
}

void Bin::slotSwitchClipProperties()
{
    QModelIndex current = m_proxyModel->selectionModel()->currentIndex();
    if (current.isValid()) {
        // User clicked in the icon, open clip properties
        std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(current));
        std::shared_ptr<ProjectClip> currentItem = nullptr;
        if (item->itemType() == AbstractProjectItem::ClipItem) {
            currentItem = std::static_pointer_cast<ProjectClip>(item);
        } else if (item->itemType() == AbstractProjectItem::SubClipItem) {
            currentItem = std::static_pointer_cast<ProjectSubClip>(item)->getMasterClip();
        }
        if (currentItem) {
            slotSwitchClipProperties(currentItem);
            return;
        }
    }
    slotSwitchClipProperties(nullptr);
}

void Bin::slotSwitchClipProperties(const std::shared_ptr<ProjectClip> &clip)
{
    if (clip == nullptr) {
        m_propertiesPanel->setEnabled(false);
        return;
    }
    if (clip->clipType() == ClipType::SlideShow) {
        m_propertiesPanel->setEnabled(false);
        showSlideshowWidget(clip);
    } else if (clip->clipType() == ClipType::QText) {
        m_propertiesPanel->setEnabled(false);
        QString parentFolder = getCurrentFolder();
        ClipCreationDialog::createQTextClip(m_doc, parentFolder, this, clip.get());
    } else {
        m_propertiesPanel->setEnabled(true);
        showClipProperties(clip);
        m_propertiesDock->show();
        m_propertiesDock->raise();
    }
    // Check if properties panel is not tabbed under Bin
    // if (!pCore->window()->isTabbedWith(m_propertiesDock, QStringLiteral("project_bin"))) {
}

void Bin::doRefreshPanel(const QString &id)
{
    std::shared_ptr<ProjectClip> currentItem = getFirstSelectedClip();
    if ((currentItem != nullptr) && currentItem->AbstractProjectItem::clipId() == id) {
        showClipProperties(currentItem, true);
    }
}

QAction *Bin::addAction(const QString &name, const QString &text, const QIcon &icon)
{
    auto *action = new QAction(text, this);
    if (!icon.isNull()) {
        action->setIcon(icon);
    }
    pCore->window()->addAction(name, action);
    return action;
}

void Bin::setupAddClipAction(QMenu *addClipMenu, ClipType::ProducerType type, const QString &name, const QString &text, const QIcon &icon)
{
    QAction *action = addAction(name, text, icon);
    action->setData(static_cast<QVariant>(type));
    addClipMenu->addAction(action);
    connect(action, &QAction::triggered, this, &Bin::slotCreateProjectClip);
}

void Bin::showClipProperties(const std::shared_ptr<ProjectClip> &clip, bool forceRefresh)
{
    if ((clip == nullptr) || !clip->isReady()) {
        for (QWidget *w : m_propertiesPanel->findChildren<ClipPropertiesController *>()) {
            delete w;
        }
        m_propertiesPanel->setProperty("clipId", QString());
        m_propertiesPanel->setEnabled(false);
        emit setupTargets(false, {});
        return;
    }
    m_propertiesPanel->setEnabled(true);
    QString panelId = m_propertiesPanel->property("clipId").toString();
    if (!forceRefresh && panelId == clip->AbstractProjectItem::clipId()) {
        // the properties panel is already displaying current clip, do nothing
        return;
    }
    // Cleanup widget for new content
    for (QWidget *w : m_propertiesPanel->findChildren<ClipPropertiesController *>()) {
        delete w;
    }
    m_propertiesPanel->setProperty("clipId", clip->AbstractProjectItem::clipId());
    // Setup timeline targets
    emit setupTargets(clip->hasVideo(), clip->audioStreams());
    auto *lay = static_cast<QVBoxLayout *>(m_propertiesPanel->layout());
    if (lay == nullptr) {
        lay = new QVBoxLayout(m_propertiesPanel);
        m_propertiesPanel->setLayout(lay);
    }
    ClipPropertiesController *panel = clip->buildProperties(m_propertiesPanel);
    connect(this, &Bin::refreshTimeCode, panel, &ClipPropertiesController::slotRefreshTimeCode);
    connect(panel, &ClipPropertiesController::updateClipProperties, this, &Bin::slotEditClipCommand);
    connect(panel, &ClipPropertiesController::seekToFrame, m_monitor, static_cast<void (Monitor::*)(int)>(&Monitor::slotSeek));
    connect(panel, &ClipPropertiesController::editClip, this, &Bin::slotEditClip);
    connect(panel, SIGNAL(editAnalysis(QString, QString, QString)), this, SLOT(slotAddClipExtraData(QString, QString, QString)));

    lay->addWidget(panel);
}

void Bin::slotEditClipCommand(const QString &id, const QMap<QString, QString> &oldProps, const QMap<QString, QString> &newProps)
{
    auto *command = new EditClipCommand(this, id, oldProps, newProps, true);
    m_doc->commandStack()->push(command);
}

void Bin::reloadClip(const QString &id, bool reloadAudio)
{
    std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(id);
    if (!clip) {
        return;
    }
    clip->reloadProducer(false, false, reloadAudio);
}

void Bin::reloadMonitorIfActive(const QString &id)
{
    if (m_monitor->activeClipId() == id) {
        slotOpenCurrent();
    }
}

QStringList Bin::getBinFolderClipIds(const QString &id) const
{
    QStringList ids;
    std::shared_ptr<ProjectFolder> folder = m_itemModel->getFolderByBinId(id);
    if (folder) {
        for (int i = 0; i < folder->childCount(); i++) {
            std::shared_ptr<AbstractProjectItem> child = std::static_pointer_cast<AbstractProjectItem>(folder->child(i));
            if (child->itemType() == AbstractProjectItem::ClipItem) {
                ids << child->clipId();
            }
        }
    }
    return ids;
}

std::shared_ptr<ProjectClip> Bin::getBinClip(const QString &id)
{
    std::shared_ptr<ProjectClip> clip = nullptr;
    if (id.contains(QLatin1Char('_'))) {
        clip = m_itemModel->getClipByBinID(id.section(QLatin1Char('_'), 0, 0));
    } else if (!id.isEmpty()) {
        clip = m_itemModel->getClipByBinID(id);
    }
    return clip;
}

void Bin::setWaitingStatus(const QString &id)
{
    std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(id);
    if (clip) {
        clip->setClipStatus(AbstractProjectItem::StatusWaiting);
    }
}

void Bin::slotRemoveInvalidClip(const QString &id, bool replace, const QString &errorMessage)
{
    Q_UNUSED(replace);

    std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(id);
    if (!clip) {
        return;
    }
    emit requesteInvalidRemoval(id, clip->url(), errorMessage);
}

void Bin::selectClip(const std::shared_ptr<ProjectClip> &clip)
{
    QModelIndex ix = m_itemModel->getIndexFromItem(clip);
    int row = ix.row();
    const QModelIndex id = m_itemModel->index(row, 0, ix.parent());
    const QModelIndex id2 = m_itemModel->index(row, m_itemModel->columnCount() - 1, ix.parent());
    if (id.isValid() && id2.isValid()) {
        m_proxyModel->selectionModel()->select(QItemSelection(m_proxyModel->mapFromSource(id), m_proxyModel->mapFromSource(id2)), QItemSelectionModel::SelectCurrent);
    }
    m_itemView->scrollTo(m_proxyModel->mapFromSource(ix), QAbstractItemView::PositionAtCenter);
}

void Bin::slotOpenCurrent()
{
    std::shared_ptr<ProjectClip> currentItem = getFirstSelectedClip();
    if (currentItem) {
        emit openClip(currentItem);
    }
}

void Bin::openProducer(std::shared_ptr<ProjectClip> controller)
{
    emit openClip(std::move(controller));
}

void Bin::openProducer(std::shared_ptr<ProjectClip> controller, int in, int out)
{
    emit openClip(std::move(controller), in, out);
}

void Bin::emitItemUpdated(std::shared_ptr<AbstractProjectItem> item)
{
    emit itemUpdated(std::move(item));
}

void Bin::emitRefreshPanel(const QString &id)
{
    emit refreshPanel(id);
}

void Bin::setupGeneratorMenu()
{
    if (!m_menu) {
        qCDebug(KDENLIVE_LOG) << "Warning, menu was not created, something is wrong";
        return;
    }

    auto *addMenu = qobject_cast<QMenu *>(pCore->window()->factory()->container(QStringLiteral("generators"), pCore->window()));
    if (addMenu) {
        QMenu *menu = m_addButton->menu();
        menu->addMenu(addMenu);
        addMenu->setEnabled(!addMenu->isEmpty());
        m_addButton->setMenu(menu);
    }

    addMenu = qobject_cast<QMenu *>(pCore->window()->factory()->container(QStringLiteral("extract_audio"), pCore->window()));
    if (addMenu) {
        m_menu->addMenu(addMenu);
        addMenu->setEnabled(!addMenu->isEmpty());
        m_extractAudioAction = addMenu;
    }

    addMenu = qobject_cast<QMenu *>(pCore->window()->factory()->container(QStringLiteral("transcoders"), pCore->window()));
    if (addMenu) {
        m_menu->addMenu(addMenu);
        addMenu->setEnabled(!addMenu->isEmpty());
        m_transcodeAction = addMenu;
    }

    addMenu = qobject_cast<QMenu *>(pCore->window()->factory()->container(QStringLiteral("clip_actions"), pCore->window()));
    if (addMenu) {
        m_menu->addMenu(addMenu);
        addMenu->setEnabled(!addMenu->isEmpty());
        m_clipsActionsMenu = addMenu;
    }

    addMenu = qobject_cast<QMenu *>(pCore->window()->factory()->container(QStringLiteral("clip_in_timeline"), pCore->window()));
    if (addMenu) {
        m_inTimelineAction = m_menu->addMenu(addMenu);
    }

    if (m_locateAction) {
        m_menu->addAction(m_locateAction);
    }
    if (m_reloadAction) {
        m_menu->addAction(m_reloadAction);
    }
    if (m_duplicateAction) {
        m_menu->addAction(m_duplicateAction);
    }
    if (m_proxyAction) {
        m_menu->addAction(m_proxyAction);
    }

    addMenu = qobject_cast<QMenu *>(pCore->window()->factory()->container(QStringLiteral("clip_timeline"), pCore->window()));
    if (addMenu) {
        m_menu->addMenu(addMenu);
        addMenu->setEnabled(false);
    }
    m_menu->addAction(m_editAction);
    m_menu->addAction(m_openAction);
    m_menu->addAction(m_renameAction);
    m_menu->addAction(m_deleteAction);
    m_menu->insertSeparator(m_deleteAction);
}

void Bin::setupMenu()
{
    auto *addClipMenu = new QMenu(this);

    QAction *addClip =
        addAction(QStringLiteral("add_clip"), i18n("Add Clip or Folder"), QIcon::fromTheme(QStringLiteral("kdenlive-add-clip")));
    addClipMenu->addAction(addClip);
    connect(addClip, &QAction::triggered, this, &Bin::slotAddClip);

    setupAddClipAction(addClipMenu, ClipType::Color, QStringLiteral("add_color_clip"), i18n("Add Color Clip"), QIcon::fromTheme(QStringLiteral("kdenlive-add-color-clip")));
    setupAddClipAction(addClipMenu, ClipType::SlideShow, QStringLiteral("add_slide_clip"), i18n("Add Slideshow Clip"), QIcon::fromTheme(QStringLiteral("kdenlive-add-slide-clip")));
    setupAddClipAction(addClipMenu, ClipType::Text, QStringLiteral("add_text_clip"), i18n("Add Title Clip"), QIcon::fromTheme(QStringLiteral("kdenlive-add-text-clip")));
    setupAddClipAction(addClipMenu, ClipType::TextTemplate, QStringLiteral("add_text_template_clip"), i18n("Add Template Title"), QIcon::fromTheme(QStringLiteral("kdenlive-add-text-clip")));

    QAction *downloadResourceAction =
        addAction(QStringLiteral("download_resource"), i18n("Online Resources"), QIcon::fromTheme(QStringLiteral("edit-download")));
    addClipMenu->addAction(downloadResourceAction);
    connect(downloadResourceAction, &QAction::triggered, pCore->window(), &MainWindow::slotDownloadResources);

    m_locateAction =
        addAction(QStringLiteral("locate_clip"), i18n("Locate Clip..."), QIcon::fromTheme(QStringLiteral("edit-file")));
    m_locateAction->setData("locate_clip");
    m_locateAction->setEnabled(false);
    connect(m_locateAction, &QAction::triggered, this, &Bin::slotLocateClip);

    m_reloadAction =
        addAction(QStringLiteral("reload_clip"), i18n("Reload Clip"), QIcon::fromTheme(QStringLiteral("view-refresh")));
    m_reloadAction->setData("reload_clip");
    m_reloadAction->setEnabled(false);
    connect(m_reloadAction, &QAction::triggered, this, &Bin::slotReloadClip);

    m_duplicateAction =
        addAction(QStringLiteral("duplicate_clip"), i18n("Duplicate Clip"), QIcon::fromTheme(QStringLiteral("edit-copy")));
    m_duplicateAction->setData("duplicate_clip");
    m_duplicateAction->setEnabled(false);
    connect(m_duplicateAction, &QAction::triggered, this, &Bin::slotDuplicateClip);

    m_proxyAction = new QAction(i18n("Proxy Clip"), pCore->window());
    pCore->window()->addAction(QStringLiteral("proxy_clip"), m_proxyAction);
    m_proxyAction->setData(QStringList() << QString::number(static_cast<int>(AbstractClipJob::PROXYJOB)));
    m_proxyAction->setCheckable(true);
    m_proxyAction->setChecked(false);

    m_editAction =
        addAction(QStringLiteral("clip_properties"), i18n("Clip Properties"), QIcon::fromTheme(QStringLiteral("document-edit")));
    m_editAction->setData("clip_properties");
    connect(m_editAction, &QAction::triggered, this, static_cast<void (Bin::*)()>(&Bin::slotSwitchClipProperties));

    m_openAction =
        addAction(QStringLiteral("edit_clip"), i18n("Edit Clip"), QIcon::fromTheme(QStringLiteral("document-open")));
    m_openAction->setData("edit_clip");
    m_openAction->setEnabled(false);
    connect(m_openAction, &QAction::triggered, this, &Bin::slotOpenClip);

    m_renameAction =
        addAction(QStringLiteral("rename_clip"), i18n("Rename Clip"), QIcon::fromTheme(QStringLiteral("document-edit")));
    m_renameAction->setData("rename_clip");
    m_renameAction->setEnabled(false);
    connect(m_renameAction, &QAction::triggered, this, &Bin::slotRenameItem);

    m_deleteAction =
        addAction(QStringLiteral("delete_clip"), i18n("Delete Clip"), QIcon::fromTheme(QStringLiteral("edit-delete")));
    m_deleteAction->setData("delete_clip");
    m_deleteAction->setEnabled(false);
    connect(m_deleteAction, &QAction::triggered, this, &Bin::slotDeleteClip);

    QAction *createFolder =
        addAction(QStringLiteral("create_folder"), i18n("Create Folder"), QIcon::fromTheme(QStringLiteral("folder-new")));
    connect(createFolder, &QAction::triggered, this, &Bin::slotAddFolder);
    m_upAction = KStandardAction::up(this, SLOT(slotBack()), pCore->window()->actionCollection());

    // Setup actions
    QAction *first = m_toolbar->actions().at(0);
    m_toolbar->insertAction(first, m_deleteAction);
    m_toolbar->insertAction(m_deleteAction, createFolder);
    m_toolbar->insertAction(createFolder, m_upAction);

    auto *m = new QMenu(this);
    m->addActions(addClipMenu->actions());
    m_addButton = new QToolButton(this);
    m_addButton->setMenu(m);
    m_addButton->setDefaultAction(addClip);
    m_addButton->setPopupMode(QToolButton::MenuButtonPopup);
    m_toolbar->insertWidget(m_upAction, m_addButton);
    m_menu = new QMenu(this);
    m_propertiesDock = pCore->window()->addDock(i18n("Clip Properties"), QStringLiteral("clip_properties"), m_propertiesPanel);
    m_propertiesDock->close();
}

const QString Bin::getDocumentProperty(const QString &key)
{
    return m_doc->getDocumentProperty(key);
}

void Bin::slotUpdateJobStatus(const QString &id, int jobType, int status, const QString &label, const QString &actionName, const QString &details)
{
    Q_UNUSED(id)
    Q_UNUSED(jobType)
    Q_UNUSED(status)
    Q_UNUSED(label)
    Q_UNUSED(actionName)
    Q_UNUSED(details)
    // TODO refac
    /*
    std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(id);
    if (clip) {
        clip->setJobStatus((AbstractClipJob::JOBTYPE)jobType, (ClipJobStatus)status);
    }
    if (status == JobCrashed) {
        QList<QAction *> actions = m_infoMessage->actions();
        if (m_infoMessage->isHidden()) {
            if (!details.isEmpty()) {
                m_infoMessage->setText(label + QStringLiteral(" <a href=\"#\">") + i18n("Show log") + QStringLiteral("</a>"));
            } else {
                m_infoMessage->setText(label);
            }
            m_infoMessage->setWordWrap(m_infoMessage->text().length() > 35);
            m_infoMessage->setMessageType(KMessageWidget::Warning);
        }

        if (!actionName.isEmpty()) {
            QAction *action = nullptr;
            QList<KActionCollection *> collections = KActionCollection::allCollections();
            for (int i = 0; i < collections.count(); ++i) {
                KActionCollection *coll = collections.at(i);
                action = coll->action(actionName);
                if (action) {
                    break;
                }
            }
            if ((action != nullptr) && !actions.contains(action)) {
                m_infoMessage->addAction(action);
            }
        }
        if (!details.isEmpty()) {
            m_errorLog.append(details);
        }
        m_infoMessage->setCloseButtonVisible(true);
        m_infoMessage->animatedShow();
    }
    */
}

void Bin::doDisplayMessage(const QString &text, KMessageWidget::MessageType type, const QList<QAction *> &actions)
{
    // Remove existing actions if any
    QList<QAction *> acts = m_infoMessage->actions();
    while (!acts.isEmpty()) {
        QAction *a = acts.takeFirst();
        m_infoMessage->removeAction(a);
        delete a;
    }
    m_infoMessage->setText(text);
    m_infoMessage->setWordWrap(text.length() > 35);
    for (QAction *action : actions) {
        m_infoMessage->addAction(action);
        connect(action, &QAction::triggered, this, &Bin::slotMessageActionTriggered);
    }
    m_infoMessage->setCloseButtonVisible(actions.isEmpty());
    m_infoMessage->setMessageType(type);
    m_infoMessage->animatedShow();
}

void Bin::doDisplayMessage(const QString &text, KMessageWidget::MessageType type, const QString &logInfo)
{
    // Remove existing actions if any
    QList<QAction *> acts = m_infoMessage->actions();
    while (!acts.isEmpty()) {
        QAction *a = acts.takeFirst();
        m_infoMessage->removeAction(a);
        delete a;
    }
    m_infoMessage->setText(text);
    m_infoMessage->setWordWrap(text.length() > 35);
    QAction *ac = new QAction(i18n("Show log"), this);
    m_infoMessage->addAction(ac);
    connect(ac, &QAction::triggered, [this, logInfo](bool) {
        KMessageBox::sorry(this, logInfo, i18n("Detailed log"));
        slotMessageActionTriggered();
    });
    m_infoMessage->setCloseButtonVisible(false);
    m_infoMessage->setMessageType(type);
    m_infoMessage->animatedShow();
}

void Bin::refreshClip(const QString &id)
{
    if (m_monitor->activeClipId() == id) {
        m_monitor->refreshMonitorIfActive();
    }
}

void Bin::slotCreateProjectClip()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (act == nullptr) {
        // Cannot access triggering action, something is wrong
        qCDebug(KDENLIVE_LOG) << "// Error in clip creation action";
        return;
    }
    ClipType::ProducerType type = (ClipType::ProducerType)act->data().toInt();
    QString parentFolder = getCurrentFolder();
    switch (type) {
    case ClipType::Color:
        ClipCreationDialog::createColorClip(m_doc, parentFolder, m_itemModel);
        break;
    case ClipType::SlideShow:
        ClipCreationDialog::createSlideshowClip(m_doc, parentFolder, m_itemModel);
        break;
    case ClipType::Text:
        ClipCreationDialog::createTitleClip(m_doc, parentFolder, QString(), m_itemModel);
        break;
    case ClipType::TextTemplate:
        ClipCreationDialog::createTitleTemplateClip(m_doc, parentFolder, m_itemModel);
        break;
    case ClipType::QText:
        ClipCreationDialog::createQTextClip(m_doc, parentFolder, this);
        break;
    default:
        break;
    }
}

void Bin::slotItemDropped(const QStringList &ids, const QModelIndex &parent)
{
    std::shared_ptr<AbstractProjectItem> parentItem;
    if (parent.isValid()) {
        parentItem = m_itemModel->getBinItemByIndex(parent);
        parentItem = parentItem->getEnclosingFolder(false);
    } else {
        parentItem = m_itemModel->getRootFolder();
    }
    auto *moveCommand = new QUndoCommand();
    moveCommand->setText(i18np("Move Clip", "Move Clips", ids.count()));
    QStringList folderIds;
    for (const QString &id : ids) {
        if (id.contains(QLatin1Char('/'))) {
            // trying to move clip zone, not allowed. Ignore
            continue;
        }
        if (id.startsWith(QLatin1Char('#'))) {
            // moving a folder, keep it for later
            folderIds << id;
            continue;
        }
        std::shared_ptr<ProjectClip> currentItem = m_itemModel->getClipByBinID(id);
        if (!currentItem) {
            continue;
        }
        std::shared_ptr<AbstractProjectItem> currentParent = currentItem->parent();
        if (currentParent != parentItem) {
            // Item was dropped on a different folder
            new MoveBinClipCommand(this, id, currentParent->clipId(), parentItem->clipId(), moveCommand);
        }
    }
    if (!folderIds.isEmpty()) {
        for (QString id : folderIds) {
            id.remove(0, 1);
            std::shared_ptr<ProjectFolder> currentItem = m_itemModel->getFolderByBinId(id);
            if (!currentItem) {
                continue;
            }
            std::shared_ptr<AbstractProjectItem> currentParent = currentItem->parent();
            if (currentParent != parentItem) {
                // Item was dropped on a different folder
                new MoveBinFolderCommand(this, id, currentParent->clipId(), parentItem->clipId(), moveCommand);
            }
        }
    }
    if (moveCommand->childCount() == 0) {
        pCore->displayMessage(i18n("No valid clip to insert"), InformationMessage, 500);
    } else {
        m_doc->commandStack()->push(moveCommand);
    }
}

void Bin::slotAddEffect(QString id, const QStringList &effectData)
{
    if (id.isEmpty()) {
        id = m_monitor->activeClipId();
    }
    if (!id.isEmpty()) {
        std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(id);
        if (clip) {
            if (effectData.count() == 4) {
                // Paste effect from another stack
                std::shared_ptr<EffectStackModel> sourceStack = pCore->getItemEffectStack(effectData.at(1).toInt(), effectData.at(2).toInt());
                clip->copyEffect(sourceStack, effectData.at(3).toInt());
            } else {
                clip->addEffect(effectData.constFirst());
            }
            return;
        }
    }
    pCore->displayMessage(i18n("Select a clip to apply an effect"), InformationMessage, 500);
}

void Bin::slotEffectDropped(const QStringList &effectData, const QModelIndex &parent)
{
    if (parent.isValid()) {
        std::shared_ptr<AbstractProjectItem> parentItem = m_itemModel->getBinItemByIndex(parent);
        if (parentItem->itemType() != AbstractProjectItem::ClipItem) {
            // effect only supported on clip items
            return;
        }
        m_proxyModel->selectionModel()->clearSelection();
        int row = parent.row();
        const QModelIndex id = m_itemModel->index(row, 0, parent.parent());
        const QModelIndex id2 = m_itemModel->index(row, m_itemModel->columnCount() - 1, parent.parent());
        if (id.isValid() && id2.isValid()) {
            m_proxyModel->selectionModel()->select(QItemSelection(m_proxyModel->mapFromSource(id), m_proxyModel->mapFromSource(id2)),
                                                   QItemSelectionModel::Select);
        }
        setCurrent(parentItem);
        bool res = false;
        if (effectData.count() == 4) {
            // Paste effect from another stack
            std::shared_ptr<EffectStackModel> sourceStack = pCore->getItemEffectStack(effectData.at(1).toInt(), effectData.at(2).toInt());
            res = std::static_pointer_cast<ProjectClip>(parentItem)->copyEffect(sourceStack, effectData.at(3).toInt());
        } else {
            res = std::static_pointer_cast<ProjectClip>(parentItem)->addEffect(effectData.constFirst());
        }
        if (!res) {
            pCore->displayMessage(i18n("Cannot add effect to clip"), InformationMessage);
        }
    }
}

void Bin::editMasterEffect(const std::shared_ptr<AbstractProjectItem> &clip)
{
    if (m_gainedFocus) {
        // Widget just gained focus, updating stack is managed in the eventfilter event, not from item
        return;
    }
    if (clip) {
        if (clip->itemType() == AbstractProjectItem::ClipItem) {
            std::shared_ptr<ProjectClip> clp = std::static_pointer_cast<ProjectClip>(clip);
            emit requestShowEffectStack(clp->clipName(), clp->m_effectStack, clp->getFrameSize(), false);
            return;
        }
        if (clip->itemType() == AbstractProjectItem::SubClipItem) {
            if (auto ptr = clip->parentItem().lock()) {
                std::shared_ptr<ProjectClip> clp = std::static_pointer_cast<ProjectClip>(ptr);
                emit requestShowEffectStack(clp->clipName(), clp->m_effectStack, clp->getFrameSize(), false);
            }
            return;
        }
    }
    emit requestShowEffectStack(QString(), nullptr, QSize(), false);
}

void Bin::slotGotFocus()
{
    m_gainedFocus = true;
}

void Bin::doMoveClip(const QString &id, const QString &newParentId)
{
    std::shared_ptr<ProjectClip> currentItem = m_itemModel->getClipByBinID(id);
    if (!currentItem) {
        return;
    }
    std::shared_ptr<AbstractProjectItem> currentParent = currentItem->parent();
    std::shared_ptr<ProjectFolder> newParent = m_itemModel->getFolderByBinId(newParentId);
    currentItem->changeParent(newParent);
}

void Bin::doMoveFolder(const QString &id, const QString &newParentId)
{
    std::shared_ptr<ProjectFolder> currentItem = m_itemModel->getFolderByBinId(id);
    std::shared_ptr<AbstractProjectItem> currentParent = currentItem->parent();
    std::shared_ptr<ProjectFolder> newParent = m_itemModel->getFolderByBinId(newParentId);
    currentParent->removeChild(currentItem);
    currentItem->changeParent(newParent);
}

void Bin::droppedUrls(const QList<QUrl> &urls, const QString &folderInfo)
{
    QModelIndex current;
    if (folderInfo.isEmpty()) {
        current = m_proxyModel->mapToSource(m_proxyModel->selectionModel()->currentIndex());
    } else {
        // get index for folder
        std::shared_ptr<ProjectFolder> folder = m_itemModel->getFolderByBinId(folderInfo);
        if (!folder) {
            folder = m_itemModel->getRootFolder();
        }
        current = m_itemModel->getIndexFromItem(folder);
    }
    slotItemDropped(urls, current);
}

void Bin::slotAddClipToProject(const QUrl &url)
{
    QList<QUrl> urls;
    urls << url;
    QModelIndex current = m_proxyModel->mapToSource(m_proxyModel->selectionModel()->currentIndex());
    slotItemDropped(urls, current);
}

void Bin::slotItemDropped(const QList<QUrl> &urls, const QModelIndex &parent)
{
    QString parentFolder = m_itemModel->getRootFolder()->clipId();
    if (parent.isValid()) {
        // Check if drop occurred on a folder
        std::shared_ptr<AbstractProjectItem> parentItem = m_itemModel->getBinItemByIndex(parent);
        while (parentItem->itemType() != AbstractProjectItem::FolderItem) {
            parentItem = parentItem->parent();
        }
        parentFolder = parentItem->clipId();
    }
    const QString id = ClipCreator::createClipsFromList(urls, true, parentFolder, m_itemModel);
    if (!id.isEmpty()) {
        std::shared_ptr<AbstractProjectItem> item = m_itemModel->getItemByBinId(id);
        if (item) {
            QModelIndex ix = m_itemModel->getIndexFromItem(item);
            m_itemView->scrollTo(m_proxyModel->mapFromSource(ix), QAbstractItemView::PositionAtCenter);
        }
    }
}

void Bin::slotExpandUrl(const ItemInfo &info, const QString &url, QUndoCommand *command)
{
    Q_UNUSED(info)
    Q_UNUSED(url)
    Q_UNUSED(command)
    // TODO reimplement this
    /*
    // Create folder to hold imported clips
    QString folderName = QFileInfo(url).fileName().section(QLatin1Char('.'), 0, 0);
    QString folderId = QString::number(getFreeFolderId());
    new AddBinFolderCommand(this, folderId, folderName.isEmpty() ? i18n("Folder") : folderName, m_itemModel->getRootFolder()->clipId(), false, command);

    // Parse playlist clips
    QDomDocument doc;
    QFile file(url);
    doc.setContent(&file, false);
    file.close();
    bool invalid = false;
    if (doc.documentElement().isNull()) {
        invalid = true;
    }
    QDomNodeList producers = doc.documentElement().elementsByTagName(QStringLiteral("producer"));
    QDomNodeList tracks = doc.documentElement().elementsByTagName(QStringLiteral("track"));
    if (invalid || producers.isEmpty()) {
        doDisplayMessage(i18n("Playlist clip %1 is invalid.", QFileInfo(url).fileName()), KMessageWidget::Warning);
        delete command;
        return;
    }
    if (tracks.count() > pCore->projectManager()->currentTimeline()->visibleTracksCount() + 1) {
        doDisplayMessage(
            i18n("Playlist clip %1 has too many tracks (%2) to be imported. Add new tracks to your project.", QFileInfo(url).fileName(), tracks.count()),
            KMessageWidget::Warning);
        delete command;
        return;
    }
    // Maps playlist producer IDs to (project) bin producer IDs.
    QMap<QString, QString> idMap;
    // Maps hash IDs to (project) first playlist producer instance ID. This is
    // necessary to detect duplicate producer serializations produced by MLT.
    // This covers, for instance, images and titles.
    QMap<QString, QString> hashToIdMap;
    QDir mltRoot(doc.documentElement().attribute(QStringLiteral("root")));
    for (int i = 0; i < producers.count(); i++) {
        QDomElement prod = producers.at(i).toElement();
        QString originalId = prod.attribute(QStringLiteral("id"));
        // track producer
        if (originalId.contains(QLatin1Char('_'))) {
            originalId = originalId.section(QLatin1Char('_'), 0, 0);
        }
        // slowmotion producer
        if (originalId.contains(QLatin1Char(':'))) {
            originalId = originalId.section(QLatin1Char(':'), 1, 1);
        }

        // We already have seen and mapped this producer.
        if (idMap.contains(originalId)) {
            continue;
        }

        // Check for duplicate producers, based on hash value of producer.
        // Be careful as to the kdenlive:file_hash! It is not unique for
        // title clips, nor color clips. Also not sure about image sequences.
        // So we use mlt service-specific hashes to identify duplicate producers.
        QString hash;
        QString mltService = Xml::getXmlProperty(prod, QStringLiteral("mlt_service"));
        if (mltService == QLatin1String("pixbuf") || mltService == QLatin1String("qimage") || mltService == QLatin1String("kdenlivetitle") ||
            mltService == QLatin1String("color") || mltService == QLatin1String("colour")) {
            hash = mltService + QLatin1Char(':') + Xml::getXmlProperty(prod, QStringLiteral("kdenlive:clipname")) + QLatin1Char(':') +
                   Xml::getXmlProperty(prod, QStringLiteral("kdenlive:folderid")) + QLatin1Char(':');
            if (mltService == QLatin1String("kdenlivetitle")) {
                // Calculate hash based on title contents.
                hash.append(
                    QString(QCryptographicHash::hash(Xml::getXmlProperty(prod, QStringLiteral("xmldata")).toUtf8(), QCryptographicHash::Md5).toHex()));
            } else if (mltService == QLatin1String("pixbuf") || mltService == QLatin1String("qimage") || mltService == QLatin1String("color") ||
                       mltService == QLatin1String("colour")) {
                hash.append(Xml::getXmlProperty(prod, QStringLiteral("resource")));
            }

            QString singletonId = hashToIdMap.value(hash, QString());
            if (singletonId.length() != 0) {
                // map duplicate producer ID to single bin clip producer ID.
                qCDebug(KDENLIVE_LOG) << "found duplicate producer:" << hash << ", reusing newID:" << singletonId;
                idMap.insert(originalId, singletonId);
                continue;
            }
        }

        // First occurrence of a producer, so allocate new bin clip producer ID.
        QString newId = QString::number(getFreeClipId());
        idMap.insert(originalId, newId);
        qCDebug(KDENLIVE_LOG) << "originalId: " << originalId << ", newId: " << newId;

        // Ensure to register new bin clip producer ID in hash hashmap for
        // those clips that MLT likes to serialize multiple times. This is
        // indicated by having a hash "value" unqual "". See also above.
        if (hash.length() != 0) {
            hashToIdMap.insert(hash, newId);
        }

        // Add clip
        QDomElement clone = prod.cloneNode(true).toElement();
        EffectsList::setProperty(clone, QStringLiteral("kdenlive:folderid"), folderId);
        // Do we have a producer that uses a resource property that contains a path?
        if (mltService == QLatin1String("avformat-novalidate") // av clip
            || mltService == QLatin1String("avformat")         // av clip
            || mltService == QLatin1String("pixbuf")           // image (sequence) clip
            || mltService == QLatin1String("qimage")           // image (sequence) clip
            || mltService == QLatin1String("xml")              // MLT playlist clip, someone likes recursion :)
            ) {
            // Make sure to correctly resolve relative resource paths based on
            // the playlist's root, not on this project's root
            QString resource = Xml::getXmlProperty(clone, QStringLiteral("resource"));
            if (QFileInfo(resource).isRelative()) {
                QFileInfo rootedResource(mltRoot, resource);
                qCDebug(KDENLIVE_LOG) << "fixed resource path for producer, newId:" << newId << "resource:" << rootedResource.absoluteFilePath();
                EffectsList::setProperty(clone, QStringLiteral("resource"), rootedResource.absoluteFilePath());
            }
        }

        ClipCreationDialog::createClipsCommand(this, clone, newId, command);
    }
    pCore->projectManager()->currentTimeline()->importPlaylist(info, idMap, doc, command);
    */
}

void Bin::slotItemEdited(const QModelIndex &ix, const QModelIndex &, const QVector<int> &roles)
{
    if (ix.isValid() && roles.contains(AbstractProjectItem::DataName)) {
        // Clip renamed
        std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(ix);
        auto clip = std::static_pointer_cast<ProjectClip>(item);
        if (clip) {
            emit clipNameChanged(clip->AbstractProjectItem::clipId());
        }
    }
}

void Bin::renameSubClipCommand(const QString &id, const QString &newName, const QString &oldName, int in, int out)
{
    auto *command = new RenameBinSubClipCommand(this, id, newName, oldName, in, out);
    m_doc->commandStack()->push(command);
}

void Bin::renameSubClip(const QString &id, const QString &newName, int in, int out)
{
    std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(id);
    if (!clip) {
        return;
    }
    std::shared_ptr<ProjectSubClip> sub = clip->getSubClip(in, out);
    if (!sub) {
        return;
    }
    sub->setName(newName);
    clip->updateZones();
    emit itemUpdated(sub);
}

Timecode Bin::projectTimecode() const
{
    return m_doc->timecode();
}

void Bin::slotStartFilterJob(const ItemInfo &info, const QString &id, QMap<QString, QString> &filterParams, QMap<QString, QString> &consumerParams,
                             QMap<QString, QString> &extraParams)
{
    Q_UNUSED(info)
    Q_UNUSED(id)
    Q_UNUSED(filterParams)
    Q_UNUSED(consumerParams)
    Q_UNUSED(extraParams)
    // TODO refac
    /*
    std::shared_ptr<ProjectClip> clip = getBinClip(id);
    if (!clip) {
        return;
    }

    QMap<QString, QString> producerParams = QMap<QString, QString>();
    producerParams.insert(QStringLiteral("producer"), clip->url());
    if (info.cropDuration != GenTime()) {
        producerParams.insert(QStringLiteral("in"), QString::number((int)info.cropStart.frames(pCore->getCurrentFps())));
        producerParams.insert(QStringLiteral("out"), QString::number((int)(info.cropStart + info.cropDuration).frames(pCore->getCurrentFps())));
        extraParams.insert(QStringLiteral("clipStartPos"), QString::number((int)info.startPos.frames(pCore->getCurrentFps())));
        extraParams.insert(QStringLiteral("clipTrack"), QString::number(info.track));
    } else {
        // We want to process whole clip
        producerParams.insert(QStringLiteral("in"), QString::number(0));
        producerParams.insert(QStringLiteral("out"), QString::number(-1));
    }
    */
}

void Bin::focusBinView() const
{
    m_itemView->setFocus();
}

void Bin::slotOpenClip()
{
    std::shared_ptr<ProjectClip> clip = getFirstSelectedClip();
    if (!clip) {
        return;
    }
    switch (clip->clipType()) {
    case ClipType::Text:
    case ClipType::TextTemplate:
        showTitleWidget(clip);
        break;
    case ClipType::Image:
        if (KdenliveSettings::defaultimageapp().isEmpty()) {
            KMessageBox::sorry(QApplication::activeWindow(), i18n("Please set a default application to open images in the Settings dialog"));
        } else {
            QProcess::startDetached(KdenliveSettings::defaultimageapp(), QStringList() << clip->url());
        }
        break;
    case ClipType::Audio:
        if (KdenliveSettings::defaultaudioapp().isEmpty()) {
            KMessageBox::sorry(QApplication::activeWindow(), i18n("Please set a default application to open audio files in the Settings dialog"));
        } else {
            QProcess::startDetached(KdenliveSettings::defaultaudioapp(), QStringList() << clip->url());
        }
        break;
    default:
        break;
    }
}

void Bin::updateTimecodeFormat()
{
    emit refreshTimeCode();
}

/*
void Bin::slotGotFilterJobResults(const QString &id, int startPos, int track, const stringMap &results, const stringMap &filterInfo)
{
    if (filterInfo.contains(QStringLiteral("finalfilter"))) {
        if (filterInfo.contains(QStringLiteral("storedata"))) {
            // Store returned data as clip extra data
            std::shared_ptr<ProjectClip> clip = getBinClip(id);
            if (clip) {
                QString key = filterInfo.value(QStringLiteral("key"));
                QStringList newValue = clip->updatedAnalysisData(key, results.value(key), filterInfo.value(QStringLiteral("offset")).toInt());
                slotAddClipExtraData(id, newValue.at(0), newValue.at(1));
            }
        }
        if (startPos == -1) {
            // Processing bin clip
            std::shared_ptr<ProjectClip> currentItem = m_itemModel->getClipByBinID(id);
            if (!currentItem) {
                return;
            }
            std::shared_ptr<ClipController> ctl = std::static_pointer_cast<ClipController>(currentItem);
            EffectsList list = ctl->effectList();
            QDomElement effect = list.effectById(filterInfo.value(QStringLiteral("finalfilter")));
            QDomDocument doc;
            QDomElement e = doc.createElement(QStringLiteral("test"));
            doc.appendChild(e);
            e.appendChild(doc.importNode(effect, true));
            if (!effect.isNull()) {
                QDomElement newEffect = effect.cloneNode().toElement();
                QMap<QString, QString>::const_iterator i = results.constBegin();
                while (i != results.constEnd()) {
                    EffectsList::setParameter(newEffect, i.key(), i.value());
                    ++i;
                }
                ctl->updateEffect(newEffect, effect.attribute(QStringLiteral("kdenlive_ix")).toInt());
                emit requestClipShow(currentItem);
                // TODO use undo / redo for bin clip edit effect
                //
                EditEffectCommand *command = new EditEffectCommand(this, clip->track(), clip->startPos(), effect, newEffect, clip->selectedEffectIndex(),
                true, true);
                m_commandStack->push(command);
                emit clipItemSelected(clip);
            }

            // emit gotFilterJobResults(id, startPos, track, results, filterInfo);
            return;
        }
        // This is a timeline filter, forward results
        emit gotFilterJobResults(id, startPos, track, results, filterInfo);
        return;
    }
    // Currently, only the first value of results is used
    std::shared_ptr<ProjectClip> clip = getBinClip(id);
    if (!clip) {
        return;
    }
    // Check for return value
    int markersType = -1;
    if (filterInfo.contains(QStringLiteral("addmarkers"))) {
        markersType = filterInfo.value(QStringLiteral("addmarkers")).toInt();
    }
    if (results.isEmpty()) {
        emit displayBinMessage(i18n("No data returned from clip analysis"), KMessageWidget::Warning);
        return;
    }
    bool dataProcessed = false;
    QString label = filterInfo.value(QStringLiteral("label"));
    QString key = filterInfo.value(QStringLiteral("key"));
    int offset = filterInfo.value(QStringLiteral("offset")).toInt();
    QStringList value = results.value(key).split(QLatin1Char(';'), QString::SkipEmptyParts);
    // qCDebug(KDENLIVE_LOG)<<"// RESULT; "<<key<<" = "<<value;
    if (filterInfo.contains(QStringLiteral("resultmessage"))) {
        QString mess = filterInfo.value(QStringLiteral("resultmessage"));
        mess.replace(QLatin1String("%count"), QString::number(value.count()));
        emit displayBinMessage(mess, KMessageWidget::Information);
    } else {
        emit displayBinMessage(i18n("Processing data analysis"), KMessageWidget::Information);
    }
    if (filterInfo.contains(QStringLiteral("cutscenes"))) {
        // Check if we want to cut scenes from returned data
        dataProcessed = true;
        int cutPos = 0;
        auto *command = new QUndoCommand();
        command->setText(i18n("Auto Split Clip"));
        for (const QString &pos : value) {
            if (!pos.contains(QLatin1Char('='))) {
                continue;
            }
            int newPos = pos.section(QLatin1Char('='), 0, 0).toInt();
            // Don't use scenes shorter than 1 second
            if (newPos - cutPos < 24) {
                continue;
            }
            new AddBinClipCutCommand(this, id, cutPos + offset, newPos + offset, true, command);
            cutPos = newPos;
        }
        if (command->childCount() == 0) {
            delete command;
        } else {
            m_doc->commandStack()->push(command);
        }
    }
    if (markersType >= 0) {
        // Add markers from returned data
        dataProcessed = true;
        int cutPos = 0;
        int index = 1;
        bool simpleList = false;
        double sourceFps = clip->getOriginalFps();
        if (qFuzzyIsNull(sourceFps)) {
            sourceFps = pCore->getCurrentFps();
        }
        if (filterInfo.contains(QStringLiteral("simplelist"))) {
            // simple list
            simpleList = true;
        }
        for (const QString &pos : value) {
            if (simpleList) {
                clip->getMarkerModel()->addMarker(GenTime((int)(pos.toInt() * pCore->getCurrentFps() / sourceFps), pCore->getCurrentFps()), label + pos,
                                                  markersType);
                index++;
                continue;
            }
            if (!pos.contains(QLatin1Char('='))) {
                continue;
            }
            int newPos = pos.section(QLatin1Char('='), 0, 0).toInt();
            // Don't use scenes shorter than 1 second
            if (newPos - cutPos < 24) {
                continue;
            }
            clip->getMarkerModel()->addMarker(GenTime(newPos + offset, pCore->getCurrentFps()), label + QString::number(index), markersType);
            index++;
            cutPos = newPos;
        }
    }
    if (!dataProcessed || filterInfo.contains(QStringLiteral("storedata"))) {
        // Store returned data as clip extra data
        QStringList newValue = clip->updatedAnalysisData(key, results.value(key), offset);
        slotAddClipExtraData(id, newValue.at(0), newValue.at(1));
    }
}
*/

// TODO: move title editing into a better place...
void Bin::showTitleWidget(const std::shared_ptr<ProjectClip> &clip)
{
    QString path = clip->getProducerProperty(QStringLiteral("resource"));
    QDir titleFolder(m_doc->projectDataFolder() + QStringLiteral("/titles"));
    titleFolder.mkpath(QStringLiteral("."));
    TitleWidget dia_ui(QUrl(), m_doc->timecode(), titleFolder.absolutePath(), pCore->monitorManager()->projectMonitor(), pCore->window());
    QDomDocument doc;
    QString xmldata = clip->getProducerProperty(QStringLiteral("xmldata"));
    if (xmldata.isEmpty() && QFile::exists(path)) {
        QFile file(path);
        doc.setContent(&file, false);
        file.close();
    } else {
        doc.setContent(xmldata);
    }
    dia_ui.setXml(doc, clip->AbstractProjectItem::clipId());
    if (dia_ui.exec() == QDialog::Accepted) {
        QMap<QString, QString> newprops;
        newprops.insert(QStringLiteral("xmldata"), dia_ui.xml().toString());
        if (dia_ui.duration() != clip->duration().frames(pCore->getCurrentFps())) {
            // duration changed, we need to update duration
            newprops.insert(QStringLiteral("out"), clip->framesToTime(dia_ui.duration() - 1));
            int currentLength = clip->getProducerDuration();
            if (currentLength != dia_ui.duration()) {
                newprops.insert(QStringLiteral("kdenlive:duration"), clip->framesToTime(dia_ui.duration()));
            }
        }
        // trigger producer reload
        newprops.insert(QStringLiteral("force_reload"), QStringLiteral("2"));
        if (!path.isEmpty()) {
            // we are editing an external file, asked if we want to detach from that file or save the result to that title file.
            if (KMessageBox::questionYesNo(pCore->window(),
                                           i18n("You are editing an external title clip (%1). Do you want to save your changes to the title "
                                                "file or save the changes for this project only?",
                                                path),
                                           i18n("Save Title"), KGuiItem(i18n("Save to title file")),
                                           KGuiItem(i18n("Save in project only"))) == KMessageBox::Yes) {
                // save to external file
                dia_ui.saveTitle(QUrl::fromLocalFile(path));
                return;
            } else {
                newprops.insert(QStringLiteral("resource"), QString());
            }
        }
        slotEditClipCommand(clip->AbstractProjectItem::clipId(), clip->currentProperties(newprops), newprops);
    }
}

void Bin::slotResetInfoMessage()
{
    m_errorLog.clear();
    QList<QAction *> actions = m_infoMessage->actions();
    for (int i = 0; i < actions.count(); ++i) {
        m_infoMessage->removeAction(actions.at(i));
    }
}


void Bin::slotSetSorting()
{
    if (m_listType == BinIconView) {
        m_proxyModel->setFilterKeyColumn(0);
        return;
    }
    auto *view = qobject_cast<QTreeView *>(m_itemView);
    if (view) {
        int ix = view->header()->sortIndicatorSection();
        m_proxyModel->setFilterKeyColumn(ix);
    }
}

void Bin::slotShowDateColumn(bool show)
{
    auto *view = qobject_cast<QTreeView *>(m_itemView);
    if (view) {
        view->setColumnHidden(1, !show);
    }
}

void Bin::slotShowDescColumn(bool show)
{
    auto *view = qobject_cast<QTreeView *>(m_itemView);
    if (view) {
        view->setColumnHidden(2, !show);
    }
}

void Bin::slotQueryRemoval(const QString &id, const QString &url, const QString &errorMessage)
{
    if (m_invalidClipDialog) {
        if (!url.isEmpty()) {
            m_invalidClipDialog->addClip(id, url);
        }
        return;
    }
    QString message = i18n("Clip is invalid, will be removed from project.");
    if (!errorMessage.isEmpty()) {
        message.append("\n" + errorMessage);
    }
    m_invalidClipDialog = new InvalidDialog(i18n("Invalid clip"), message, true, this);
    m_invalidClipDialog->addClip(id, url);
    int result = m_invalidClipDialog->exec();
    if (result == QDialog::Accepted) {
        const QStringList ids = m_invalidClipDialog->getIds();
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        for (const QString &i : ids) {
            auto item = m_itemModel->getClipByBinID(i);
            m_itemModel->requestBinClipDeletion(item, undo, redo);
        }
    }
    delete m_invalidClipDialog;
    m_invalidClipDialog = nullptr;
}

void Bin::slotRefreshClipThumbnail(const QString &id)
{
    std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(id);
    if (!clip) {
        return;
    }
    clip->reloadProducer(true);
}

void Bin::slotAddClipExtraData(const QString &id, const QString &key, const QString &clipData, QUndoCommand *groupCommand)
{
    std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(id);
    if (!clip) {
        return;
    }
    QString oldValue = clip->getProducerProperty(key);
    QMap<QString, QString> oldProps;
    oldProps.insert(key, oldValue);
    QMap<QString, QString> newProps;
    newProps.insert(key, clipData);
    auto *command = new EditClipCommand(this, id, oldProps, newProps, true, groupCommand);
    if (!groupCommand) {
        m_doc->commandStack()->push(command);
    }
}

void Bin::slotUpdateClipProperties(const QString &id, const QMap<QString, QString> &properties, bool refreshPropertiesPanel)
{
    std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(id);
    if (clip) {
        clip->setProperties(properties, refreshPropertiesPanel);
    }
}

void Bin::showSlideshowWidget(const std::shared_ptr<ProjectClip> &clip)
{
    QString folder = QFileInfo(clip->url()).absolutePath();
    qCDebug(KDENLIVE_LOG) << " ** * CLIP ABS PATH: " << clip->url() << " = " << folder;
    SlideshowClip *dia = new SlideshowClip(m_doc->timecode(), folder, clip.get(), this);
    if (dia->exec() == QDialog::Accepted) {
        // edit clip properties
        QMap<QString, QString> properties;
        properties.insert(QStringLiteral("out"), clip->framesToTime(m_doc->getFramePos(dia->clipDuration()) * dia->imageCount() - 1));
        properties.insert(QStringLiteral("kdenlive:duration"), clip->framesToTime(m_doc->getFramePos(dia->clipDuration()) * dia->imageCount()));
        properties.insert(QStringLiteral("kdenlive:clipname"), dia->clipName());
        properties.insert(QStringLiteral("ttl"), QString::number(m_doc->getFramePos(dia->clipDuration())));
        properties.insert(QStringLiteral("loop"), QString::number(static_cast<int>(dia->loop())));
        properties.insert(QStringLiteral("crop"), QString::number(static_cast<int>(dia->crop())));
        properties.insert(QStringLiteral("fade"), QString::number(static_cast<int>(dia->fade())));
        properties.insert(QStringLiteral("luma_duration"), QString::number(m_doc->getFramePos(dia->lumaDuration())));
        properties.insert(QStringLiteral("luma_file"), dia->lumaFile());
        properties.insert(QStringLiteral("softness"), QString::number(dia->softness()));
        properties.insert(QStringLiteral("animation"), dia->animation());

        QMap<QString, QString> oldProperties;
        oldProperties.insert(QStringLiteral("out"), clip->getProducerProperty(QStringLiteral("out")));
        oldProperties.insert(QStringLiteral("kdenlive:duration"), clip->getProducerProperty(QStringLiteral("kdenlive:duration")));
        oldProperties.insert(QStringLiteral("kdenlive:clipname"), clip->name());
        oldProperties.insert(QStringLiteral("ttl"), clip->getProducerProperty(QStringLiteral("ttl")));
        oldProperties.insert(QStringLiteral("loop"), clip->getProducerProperty(QStringLiteral("loop")));
        oldProperties.insert(QStringLiteral("crop"), clip->getProducerProperty(QStringLiteral("crop")));
        oldProperties.insert(QStringLiteral("fade"), clip->getProducerProperty(QStringLiteral("fade")));
        oldProperties.insert(QStringLiteral("luma_duration"), clip->getProducerProperty(QStringLiteral("luma_duration")));
        oldProperties.insert(QStringLiteral("luma_file"), clip->getProducerProperty(QStringLiteral("luma_file")));
        oldProperties.insert(QStringLiteral("softness"), clip->getProducerProperty(QStringLiteral("softness")));
        oldProperties.insert(QStringLiteral("animation"), clip->getProducerProperty(QStringLiteral("animation")));
        slotEditClipCommand(clip->AbstractProjectItem::clipId(), oldProperties, properties);
    }
    delete dia;
}

void Bin::setBinEffectsEnabled(bool enabled)
{
    QAction *disableEffects = pCore->window()->actionCollection()->action(QStringLiteral("disable_bin_effects"));
    if (disableEffects) {
        if (enabled == disableEffects->isChecked()) {
            return;
        }
        disableEffects->blockSignals(true);
        disableEffects->setChecked(!enabled);
        disableEffects->blockSignals(false);
    }
    m_itemModel->setBinEffectsEnabled(enabled);
    pCore->projectManager()->disableBinEffects(!enabled);
}

void Bin::slotRenameItem()
{
    const QModelIndexList indexes = m_proxyModel->selectionModel()->selectedRows(0);
    for (const QModelIndex &ix : indexes) {
        if (!ix.isValid()) {
            continue;
        }
        m_itemView->setCurrentIndex(ix);
        m_itemView->edit(ix);
        return;
    }
}

void Bin::refreshProxySettings()
{
    QList<std::shared_ptr<ProjectClip>> clipList = m_itemModel->getRootFolder()->childClips();
    auto *masterCommand = new QUndoCommand();
    masterCommand->setText(m_doc->useProxy() ? i18n("Enable proxies") : i18n("Disable proxies"));
    // en/disable proxy option in clip properties
    for (QWidget *w : m_propertiesPanel->findChildren<ClipPropertiesController *>()) {
        static_cast<ClipPropertiesController *>(w)->enableProxy(m_doc->useProxy());
    }
    if (!m_doc->useProxy()) {
        // Disable all proxies
        m_doc->slotProxyCurrentItem(false, clipList, false, masterCommand);
    } else {
        QList<std::shared_ptr<ProjectClip>> toProxy;
        for (const std::shared_ptr<ProjectClip> &clp : clipList) {
            ClipType::ProducerType t = clp->clipType();
            if (t == ClipType::Playlist) {
                toProxy << clp;
                continue;
            } else if ((t == ClipType::AV || t == ClipType::Video) &&
                       m_doc->autoGenerateProxy(clp->getProducerIntProperty(QStringLiteral("meta.media.width")))) {
                // Start proxy
                toProxy << clp;
                continue;
            } else if (t == ClipType::Image && m_doc->autoGenerateImageProxy(clp->getProducerIntProperty(QStringLiteral("meta.media.width")))) {
                // Start proxy
                toProxy << clp;
                continue;
            }
        }
        if (!toProxy.isEmpty()) {
            m_doc->slotProxyCurrentItem(true, toProxy, false, masterCommand);
        }
    }
    if (masterCommand->childCount() > 0) {
        m_doc->commandStack()->push(masterCommand);
    } else {
        delete masterCommand;
    }
}

QStringList Bin::getProxyHashList()
{
    QStringList list;
    QList<std::shared_ptr<ProjectClip>> clipList = m_itemModel->getRootFolder()->childClips();
    for (const std::shared_ptr<ProjectClip> &clp : clipList) {
        if (clp->clipType() == ClipType::AV || clp->clipType() == ClipType::Video || clp->clipType() == ClipType::Playlist) {
            list << clp->hash();
        }
    }
    return list;
}

bool Bin::isEmpty() const
{
    if (m_itemModel->getRootFolder() == nullptr) {
        return true;
    }
    return !m_itemModel->getRootFolder()->hasChildClips();
}

void Bin::reloadAllProducers()
{
    if (m_itemModel->getRootFolder() == nullptr || m_itemModel->getRootFolder()->childCount() == 0 || !isEnabled()) {
        return;
    }
    QList<std::shared_ptr<ProjectClip>> clipList = m_itemModel->getRootFolder()->childClips();
    emit openClip(std::shared_ptr<ProjectClip>());
    for (const std::shared_ptr<ProjectClip> &clip : clipList) {
        QDomDocument doc;
        QDomElement xml = clip->toXml(doc, false, false);
        // Make sure we reload clip length
        if (clip->clipType() == ClipType::AV || clip->clipType() == ClipType::Video || clip->clipType() == ClipType::Audio || clip->clipType() == ClipType::Playlist) {
            xml.removeAttribute(QStringLiteral("out"));
            Xml::removeXmlProperty(xml, QStringLiteral("length"));
        }
        if (clip->isValid()) {
            clip->resetProducerProperty(QStringLiteral("kdenlive:duration"));
            clip->resetProducerProperty(QStringLiteral("length"));
        }
        if (!xml.isNull()) {
            clip->setClipStatus(AbstractProjectItem::StatusWaiting);
            pCore->jobManager()->slotDiscardClipJobs(clip->clipId());
            clip->discardAudioThumb();
            // We need to set a temporary id before all outdated producers are replaced;
            int jobId = pCore->jobManager()->startJob<LoadJob>({clip->clipId()}, -1, QString(), xml);
            ThumbnailCache::get()->invalidateThumbsForClip(clip->clipId(), true);
            pCore->jobManager()->startJob<ThumbJob>({clip->clipId()}, jobId, QString(), 150, -1, true, true);
            pCore->jobManager()->startJob<AudioThumbJob>({clip->clipId()}, jobId, QString());
        }
    }
}

void Bin::slotMessageActionTriggered()
{
    m_infoMessage->animatedHide();
}

void Bin::resetUsageCount()
{
    const QList<std::shared_ptr<ProjectClip>> clipList = m_itemModel->getRootFolder()->childClips();
    for (const std::shared_ptr<ProjectClip> &clip : clipList) {
        clip->setRefCount(0);
    }
}

void Bin::getBinStats(uint *used, uint *unused, qint64 *usedSize, qint64 *unusedSize)
{
    QList<std::shared_ptr<ProjectClip>> clipList = m_itemModel->getRootFolder()->childClips();
    for (const std::shared_ptr<ProjectClip> &clip : clipList) {
        if (clip->refCount() == 0) {
            *unused += 1;
            *unusedSize += clip->getProducerInt64Property(QStringLiteral("kdenlive:file_size"));
        } else {
            *used += 1;
            *usedSize += clip->getProducerInt64Property(QStringLiteral("kdenlive:file_size"));
        }
    }
}

QDir Bin::getCacheDir(CacheType type, bool *ok) const
{
    return m_doc->getCacheDir(type, ok);
}

void Bin::rebuildProxies()
{
    QList<std::shared_ptr<ProjectClip>> clipList = m_itemModel->getRootFolder()->childClips();
    QList<std::shared_ptr<ProjectClip>> toProxy;
    for (const std::shared_ptr<ProjectClip> &clp : clipList) {
        if (clp->hasProxy()) {
            toProxy << clp;
            // Abort all pending jobs
            pCore->jobManager()->discardJobs(clp->clipId(), AbstractClipJob::PROXYJOB);
            clp->deleteProxy();
        }
    }
    if (toProxy.isEmpty()) {
        return;
    }
    auto *masterCommand = new QUndoCommand();
    masterCommand->setText(i18n("Rebuild proxies"));
    m_doc->slotProxyCurrentItem(true, toProxy, true, masterCommand);
    if (masterCommand->childCount() > 0) {
        m_doc->commandStack()->push(masterCommand);
    } else {
        delete masterCommand;
    }
}

void Bin::showClearButton(bool show)
{
    m_searchLine->setClearButtonEnabled(show);
}

void Bin::saveZone(const QStringList &info, const QDir &dir)
{
    if (info.size() != 3) {
        return;
    }
    std::shared_ptr<ProjectClip> clip = getBinClip(info.constFirst());
    if (clip) {
        QPoint zone(info.at(1).toInt(), info.at(2).toInt());
        clip->saveZone(zone, dir);
    }
}

void Bin::setCurrent(const std::shared_ptr<AbstractProjectItem> &item)
{
    switch (item->itemType()) {
    case AbstractProjectItem::ClipItem: {
        openProducer(std::static_pointer_cast<ProjectClip>(item));
        std::shared_ptr<ProjectClip> clp = std::static_pointer_cast<ProjectClip>(item);
        emit requestShowEffectStack(clp->clipName(), clp->m_effectStack, clp->getFrameSize(), false);
        break;
    }
    case AbstractProjectItem::SubClipItem: {
        auto subClip = std::static_pointer_cast<ProjectSubClip>(item);
        QPoint zone = subClip->zone();
        openProducer(subClip->getMasterClip(), zone.x(), zone.y());
        break;
    }
    case AbstractProjectItem::FolderItem:
    default:
        break;
    }
}

void Bin::cleanup()
{
    m_itemModel->requestCleanup();
}

std::shared_ptr<EffectStackModel> Bin::getClipEffectStack(int itemId)
{
    std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(QString::number(itemId));
    Q_ASSERT(clip != nullptr);
    std::shared_ptr<EffectStackModel> effectStack = std::static_pointer_cast<ClipController>(clip)->m_effectStack;
    return effectStack;
}

size_t Bin::getClipDuration(int itemId) const
{
    std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(QString::number(itemId));
    Q_ASSERT(clip != nullptr);
    return clip->frameDuration();
}

PlaylistState::ClipState Bin::getClipState(int itemId) const
{
    std::shared_ptr<ProjectClip> clip = m_itemModel->getClipByBinID(QString::number(itemId));
    Q_ASSERT(clip != nullptr);
    bool audio = clip->hasAudio();
    bool video = clip->hasVideo();
    return audio ? (video ? PlaylistState::Disabled : PlaylistState::AudioOnly) : PlaylistState::VideoOnly;
}

QString Bin::getCurrentFolder()
{
    // Check parent item
    QModelIndex ix = m_proxyModel->selectionModel()->currentIndex();
    std::shared_ptr<ProjectFolder> parentFolder = m_itemModel->getRootFolder();
    if (ix.isValid() && m_proxyModel->selectionModel()->isSelected(ix)) {
        std::shared_ptr<AbstractProjectItem> currentItem = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(ix));
        parentFolder = std::static_pointer_cast<ProjectFolder>(currentItem->getEnclosingFolder());
    }
    return parentFolder->clipId();
}

void Bin::adjustProjectProfileToItem()
{
    QModelIndex current = m_proxyModel->selectionModel()->currentIndex();
    if (current.isValid()) {
        // User clicked in the icon, open clip properties
        std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(current));
        auto clip = std::static_pointer_cast<ProjectClip>(item);
        if (clip) {
            QDomDocument doc;
            LoadJob::checkProfile(clip->clipId(), clip->toXml(doc, false), clip->originalProducer());
        }
    }
}

void Bin::showBinFrame(QModelIndex ix, int frame)
{
    std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(ix));
    if (item) {
        ClipType::ProducerType type = item->clipType();
            if (type != ClipType::AV && type != ClipType::Video && type != ClipType::Playlist && type != ClipType::SlideShow) {
            return;
        }
        if (item->itemType() == AbstractProjectItem::ClipItem) {
            auto clip = std::static_pointer_cast<ProjectClip>(item);
            if (clip && (clip->clipType() == ClipType::AV || clip->clipType() == ClipType::Video || clip->clipType() == ClipType::Playlist)) {
                clip->getThumbFromPercent(frame);
            }
        } else if (item->itemType() == AbstractProjectItem::SubClipItem) {
            auto clip = std::static_pointer_cast<ProjectSubClip>(item);
            if (clip && (clip->clipType() == ClipType::AV || clip->clipType() == ClipType::Video || clip->clipType() == ClipType::Playlist)) {
                clip->getThumbFromPercent(frame);
            }
        }
    }
}

void Bin::invalidateClip(const QString &binId)
{
    std::shared_ptr<ProjectClip> clip = getBinClip(binId);
    if (clip && clip->clipType() != ClipType::Audio) {
        QList<int> ids = clip->timelineInstances();
        for (int i : ids) {
            pCore->invalidateItem({ObjectType::TimelineClip,i});
        }
    }
}

QSize Bin::sizeHint() const
{
    return QSize(350, pCore->window()->height() / 2);
}

void Bin::slotBack()
{
    QModelIndex currentRootIx = m_itemView->rootIndex();
    if (!currentRootIx.isValid()) {
        return;
    }
    std::shared_ptr<AbstractProjectItem> item = m_itemModel->getBinItemByIndex(m_proxyModel->mapToSource(currentRootIx));
    if (!item) {
        qDebug()<<"=== ERRO CANNOT FIND ROOT FOR CURRENT VIEW";
        return;
    }
    std::shared_ptr<AbstractProjectItem> parentItem = item->parent();
    if (!parentItem) {
        qDebug()<<"=== ERRO CANNOT FIND PARENT FOR CURRENT VIEW";
        return;
    }
    if (parentItem != m_itemModel->getRootFolder()) {
        // We are entering a parent folder
        QModelIndex parentId = getIndexForId(parentItem->clipId(), parentItem->itemType() == AbstractProjectItem::FolderItem);
        if (parentId.isValid()) {
            m_itemView->setRootIndex(m_proxyModel->mapFromSource(parentId));
        }
    } else {
        m_itemView->setRootIndex(QModelIndex());
        m_upAction->setEnabled(false);
    }
}

