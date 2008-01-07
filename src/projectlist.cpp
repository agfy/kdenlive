
#include <QMouseEvent>
#include <QStylePainter>
#include <QPixmap>
#include <QIcon>
#include <QDialog>

#include <KDebug>
#include <KAction>
#include <KLocale>
#include <KFileDialog>

#include "projectlist.h"
#include "projectitem.h"
#include "kdenlivesettings.h"
#include "ui_colorclip_ui.h"

#include "addclipcommand.h"

#include <QtGui>

  const int NameRole = Qt::UserRole;
  const int DurationRole = NameRole + 1;
  const int FullPathRole = NameRole + 2;
  const int ClipTypeRole = NameRole + 3;

class ItemDelegate: public QItemDelegate
{
  public:
    ItemDelegate(QObject* parent = 0): QItemDelegate(parent)
    {
    }

void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
  if (index.column() == 1)
  {
    const bool hover = option.state & (QStyle::State_Selected|QStyle::State_MouseOver|QStyle::State_HasFocus);
    QRect r1 = option.rect;
    painter->save();
    if (hover) {
        painter->setPen(option.palette.color(QPalette::HighlightedText));
        QColor backgroundColor = option.palette.color(QPalette::Highlight);
        painter->setBrush(QBrush(backgroundColor));
	painter->fillRect(r1, QBrush(backgroundColor));
    }
    QFont font = painter->font();
    font.setPointSize(font.pointSize() - 1 );
    font.setBold(true);
    painter->setFont(font);
    int mid = (int) ((r1.height() / 2 ));
    r1.setBottom(r1.y() + mid);
    QRect r2 = option.rect;
    r2.setTop(r2.y() + mid);
    painter->drawText(r1, Qt::AlignLeft | Qt::AlignBottom , index.data().toString());
    //painter->setPen(Qt::green);
    font.setBold(false);
    painter->setFont(font);
    painter->drawText(r2, Qt::AlignLeft | Qt::AlignVCenter , index.data(DurationRole).toString());
    painter->restore();
  }
  else
  {
    QItemDelegate::paint(painter, option, index);
  }
}
};


ProjectList::ProjectList(QWidget *parent)
    : QWidget(parent), m_render(NULL), m_fps(-1), m_commandStack(NULL)
{

  QWidget *vbox = new QWidget;
  listView = new QTreeWidget(this);;
  QVBoxLayout *layout = new QVBoxLayout;
  m_clipIdCounter = 0;

  // setup toolbar
  searchView = new KTreeWidgetSearchLine (this);
  m_toolbar = new QToolBar("projectToolBar", this);
  m_toolbar->addWidget (searchView);

  QToolButton *addButton = new QToolButton( m_toolbar );
  QMenu *addMenu = new QMenu(this);
  addButton->setMenu( addMenu );
  addButton->setPopupMode(QToolButton::MenuButtonPopup);
  m_toolbar->addWidget (addButton);
  
  QAction *addClipButton = addMenu->addAction (KIcon("document-new"), i18n("Add Clip"));
  connect(addClipButton, SIGNAL(triggered()), this, SLOT(slotAddClip()));

  QAction *addColorClip = addMenu->addAction (KIcon("document-new"), i18n("Add Color Clip"));
  connect(addColorClip, SIGNAL(triggered()), this, SLOT(slotAddColorClip()));

  QAction *deleteClip = m_toolbar->addAction (KIcon("edit-delete"), i18n("Delete Clip"));
  connect(deleteClip, SIGNAL(triggered()), this, SLOT(slotRemoveClip()));

  QAction *editClip = m_toolbar->addAction (KIcon("document-properties"), i18n("Edit Clip"));
  connect(editClip, SIGNAL(triggered()), this, SLOT(slotEditClip()));

  addButton->setDefaultAction( addClipButton );

  layout->addWidget( m_toolbar );
  layout->addWidget( listView );
  setLayout( layout );
  m_toolbar->setEnabled(false);

  searchView->setTreeWidget(listView);
  listView->setColumnCount(3);
  listView->setDragEnabled(true);
  listView->setDragDropMode(QAbstractItemView::DragDrop);
  QStringList headers;
  headers<<i18n("Thumbnail")<<i18n("Filename")<<i18n("Description");
  listView->setHeaderLabels(headers);
  listView->sortByColumn(1, Qt::AscendingOrder);

  connect(listView, SIGNAL(itemSelectionChanged()), this, SLOT(slotClipSelected()));
  //connect(listView, SIGNAL(itemDoubleClicked ( QTreeWidgetItem *, int )), this, SLOT(slotEditClip(QTreeWidgetItem *, int)));


  listView->setItemDelegate(new ItemDelegate(listView));
  listView->setIconSize(QSize(60, 40));
  listView->setSortingEnabled (true);

}

void ProjectList::setRenderer(Render *projectRender)
{
  m_render = projectRender;
}

void ProjectList::slotClipSelected()
{
  ProjectItem *item = (ProjectItem*) listView->currentItem();
  if (item && !item->isGroup()) emit clipSelected(item->toXml());
}

void ProjectList::slotEditClip()
{

}

void ProjectList::slotRemoveClip()
{
  if (!listView->currentItem()) return;
  ProjectItem *item = ((ProjectItem *)listView->currentItem());
  AddClipCommand *command = new AddClipCommand(this, item->names(), item->toXml(), item->clipId(), KUrl(item->data(1, FullPathRole).toString()), false);
  m_commandStack->push(command);

}

void ProjectList::selectItemById(const int clipId)
{
  ProjectItem *item = getItemById(clipId);
  if (item) listView->setCurrentItem(item);
}

void ProjectList::addClip(const QStringList &name, const QDomElement &elem, const int clipId, const KUrl &url)
{
  kDebug()<<"/////////  ADDING VCLIP=: "<<name;
  ProjectItem *item;
  QString group = elem.attribute("group", QString::null);
  if (!group.isEmpty()) {
    // Clip is in a group
    QList<QTreeWidgetItem *> groupList = listView->findItems(group, Qt::MatchExactly, 1);
    ProjectItem *groupItem;
    if (groupList.isEmpty())  {
	QStringList groupName;
	groupName<<QString::null<<group;
	kDebug()<<"-------  CREATING NEW GRP: "<<groupName;
	groupItem = new ProjectItem(listView, groupName);
    }
    else groupItem = (ProjectItem *) groupList.first();
    item = new ProjectItem(groupItem, name, elem, clipId);
  }
  else item = new ProjectItem(listView, name, elem, clipId);
  if (!url.isEmpty()) {
    item->setData(1, FullPathRole, url.path());
  }
  if (elem.isNull() ) {
    QDomDocument doc;
    QDomElement element = doc.createElement("producer");
    element.setAttribute("resource", url.path());
    emit getFileProperties(element, clipId);
  }
  else emit getFileProperties(elem, clipId);
  selectItemById(clipId);
}

void ProjectList::deleteClip(const int clipId)
{
  ProjectItem *item = getItemById(clipId);
  if (item) delete item;
}

void ProjectList::slotAddClip()
{
   
  KUrl::List list = KFileDialog::getOpenUrls( KUrl(), "application/vnd.kde.kdenlive application/vnd.westley.scenelist application/flv application/vnd.rn-realmedia video/x-dv video/x-msvideo video/mpeg video/x-ms-wmv audio/x-mp3 audio/x-wav application/ogg *.m2t *.dv video/mp4 video/quicktime image/gif image/jpeg image/png image/x-bmp image/svg+xml image/tiff image/x-xcf-gimp image/x-vnd.adobe.photoshop image/x-pcx image/x-exr");
  if (list.isEmpty()) return;
  KUrl::List::Iterator it;
  KUrl url;
//  ProjectItem *item;

  for (it = list.begin(); it != list.end(); it++) {
      QStringList itemEntry;
      itemEntry.append(QString::null);
      itemEntry.append((*it).fileName());
      AddClipCommand *command = new AddClipCommand(this, itemEntry, QDomElement(), m_clipIdCounter++, *it, true);
      m_commandStack->push(command);
      //item = new ProjectItem(listView, itemEntry, QDomElement());
      //item->setData(1, FullPathRole, (*it).path());
  }
  //listView->setCurrentItem(item);
}

void ProjectList::slotAddColorClip()
{
  QDialog *dia = new QDialog;
  Ui::ColorClip_UI *dia_ui = new Ui::ColorClip_UI();
  dia_ui->setupUi(dia);
  dia_ui->clip_name->setText(i18n("Color Clip"));
  dia_ui->clip_duration->setText(KdenliveSettings::color_duration());
  if (dia->exec() == QDialog::Accepted)
  {
    QDomDocument doc;
    QDomElement element = doc.createElement("producer");
    element.setAttribute("mlt_service", "colour");
    QString color = dia_ui->clip_color->color().name();
    color = color.replace(0, 1, "0x") + "ff";
    element.setAttribute("colour", color);
    element.setAttribute("type", (int) DocClipBase::COLOR);
    element.setAttribute("in", "0");
    element.setAttribute("out", m_timecode.getFrameCount(dia_ui->clip_duration->text(), m_fps));
    element.setAttribute("name", dia_ui->clip_name->text());
    QStringList itemEntry;
    itemEntry.append(QString::null);
    itemEntry.append(dia_ui->clip_name->text());
    AddClipCommand *command = new AddClipCommand(this, itemEntry, element, m_clipIdCounter++, KUrl(), true);
    m_commandStack->push(command);
    //ProjectItem *item = new ProjectItem(listView, itemEntry, element);
    /*QPixmap pix(60, 40);
    pix.fill(dia_ui->clip_color->color());
    item->setIcon(0, QIcon(pix));*/
    //listView->setCurrentItem(item);
    
  }
  delete dia_ui;
  delete dia;
}

void ProjectList::setDocument(KdenliveDoc *doc)
{
  m_fps = doc->fps();
  m_timecode = doc->timecode();
  m_commandStack = doc->commandStack();
  QDomNodeList prods = doc->producersList();
  int ct = prods.count();
  kDebug()<<"////////////  SETTING DOC, FOUND CLIPS: "<<prods.count();
  listView->clear();
  for (int i = 0; i <  ct ; i++)
  {
    QDomElement e = prods.item(i).toElement();
    kDebug()<<"// IMPORT: "<<i<<", :"<<e.attribute("id", "non")<<", NAME: "<<e.attribute("name", "non");
    if (!e.isNull()) addProducer(e);
  }
  QTreeWidgetItem *first = listView->topLevelItem(0);
  if (first) listView->setCurrentItem(first);
  m_toolbar->setEnabled(true);
}

QDomElement ProjectList::producersList()
{
  QDomDocument doc;
  QDomElement prods = doc.createElement("producerlist");
  doc.appendChild(prods);

    QTreeWidgetItemIterator it(listView);
     while (*it) {
         if (!((ProjectItem *)(*it))->isGroup())
	  prods.appendChild(doc.importNode(((ProjectItem *)(*it))->toXml(), true));
         ++it;
     }
  return prods;
}


void ProjectList::slotReplyGetFileProperties(int clipId, const QMap < QString, QString > &properties, const QMap < QString, QString > &metadata)
{
  ProjectItem *item = getItemById(clipId);
  if (item) item->setProperties(properties, metadata);
}



void ProjectList::slotReplyGetImage(int clipId, int pos, const QPixmap &pix, int w, int h)
{
  ProjectItem *item = getItemById(clipId);
  if (item) item->setIcon(0, pix);
}

ProjectItem *ProjectList::getItemById(int id)
{
    QTreeWidgetItemIterator it(listView);
     while (*it) {
         if (((ProjectItem *)(*it))->clipId() == id)
	  break;
         ++it;
     }
  return ((ProjectItem *)(*it));
}


void ProjectList::addProducer(QDomElement producer)
{
  DocClipBase::CLIPTYPE type = (DocClipBase::CLIPTYPE) producer.attribute("type").toInt();

    /*QDomDocument doc;
    QDomElement prods = doc.createElement("list");
    doc.appendChild(prods);
    prods.appendChild(doc.importNode(producer, true));*/
    

  //kDebug()<<"//////  ADDING PRODUCER:\n "<<doc.toString()<<"\n+++++++++++++++++";
  int id = producer.attribute("id").toInt();
  if (id >= m_clipIdCounter) m_clipIdCounter = id + 1;

  if (type == DocClipBase::AUDIO || type == DocClipBase::VIDEO || type == DocClipBase::AV)
  {
    KUrl resource = KUrl(producer.attribute("resource"));
    if (!resource.isEmpty()) {
      QStringList itemEntry;
      itemEntry.append(QString::null);
      itemEntry.append(resource.fileName());
      addClip(itemEntry, producer, id, resource);
      /*AddClipCommand *command = new AddClipCommand(this, itemEntry, producer, id, resource, true);
      m_commandStack->push(command);*/


      /*ProjectItem *item = new ProjectItem(listView, itemEntry, producer);
      item->setData(1, FullPathRole, resource.path());
      item->setData(1, ClipTypeRole, (int) type);*/
      
    }
  }
  else if (type == DocClipBase::COLOR) {
    QString colour = producer.attribute("colour");
    QPixmap pix(60, 40);
    colour = colour.replace(0, 2, "#");
    pix.fill(QColor(colour.left(7)));
    QStringList itemEntry;
    itemEntry.append(QString::null);
    itemEntry.append(producer.attribute("name"));
    addClip(itemEntry, producer, id, KUrl());
    /*AddClipCommand *command = new AddClipCommand(this, itemEntry, producer, id, KUrl(), true);
    m_commandStack->push(command);*/
    //ProjectItem *item = new ProjectItem(listView, itemEntry, producer);
    /*item->setIcon(0, QIcon(pix));
    item->setData(1, ClipTypeRole, (int) type);*/
  }
      
}

#include "projectlist.moc"
