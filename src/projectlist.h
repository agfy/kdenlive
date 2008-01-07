#ifndef PRJECTLIST_H
#define PRJECTLIST_H

#include <QDomNodeList>
#include <QToolBar>
#include <QTreeWidget>

#include <KUndoStack>
#include <KTreeWidgetSearchLine>

#include "docclipbase.h"
#include "kdenlivedoc.h"
#include "renderer.h"
#include "timecode.h"

class ProjectItem;

class ProjectList : public QWidget
{
  Q_OBJECT
  
  public:
    ProjectList(QWidget *parent=0);

    QDomElement producersList();
    void setRenderer(Render *projectRender);

    void addClip(const QStringList &name, const QDomElement &elem, const int clipId, const KUrl &url = KUrl());
    void deleteClip(const int clipId);

  public slots:
    void setDocument(KdenliveDoc *doc);
    void addProducer(QDomElement producer);
    void slotReplyGetImage(int clipId, int pos, const QPixmap &pix, int w, int h);
    void slotReplyGetFileProperties(int clipId, const QMap < QString, QString > &properties, const QMap < QString, QString > &metadata);


  private:
    QTreeWidget *listView;
    KTreeWidgetSearchLine *searchView;
    Render *m_render;
    Timecode m_timecode;
    double m_fps;
    QToolBar *m_toolbar;
    KUndoStack *m_commandStack;
    int m_clipIdCounter;
    void selectItemById(const int clipId);
    ProjectItem *getItemById(int id);

  private slots:
    void slotAddClip();
    void slotRemoveClip();
    void slotEditClip();
    void slotClipSelected();
    void slotAddColorClip();

  signals:
    void clipSelected(const QDomElement &);
    void getFileProperties(const QDomElement&, int);
};

#endif
