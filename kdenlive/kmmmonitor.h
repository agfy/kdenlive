/***************************************************************************
                          kmmmonitor.h  -  description
                             -------------------
    begin                : Sun Mar 24 2002
    copyright            : (C) 2002 by Jason Wood
    email                : jasonwood@blueyonder.co.uk
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef KMMMONITOR_H
#define KMMMONITOR_H

#include <qvbox.h>

#include "kmmscreen.h"
#include "kmmeditpanel.h"
#include "gentime.h"

class KdenliveApp;
class KdenliveDoc;
class DocClipBase;
class AVFile;

/**KMMMonitor provides a multimedia bar and the
ability to play Arts PlayObjects. It is also capable
of editing the object down, and is Drag 'n drop
aware with The Timeline, The Project List, and
external files.
  *@author Jason Wood
  */

class KMMMonitor : public QVBox  {
   Q_OBJECT
public:
	KMMMonitor(KdenliveApp *app, KdenliveDoc *document, QWidget *parent=0, const char *name=0);
	virtual ~KMMMonitor();
	
	/** ***Nasty Hack***
	Swaps the screens of two monitors, reparenting and reconnecting all
	relevant signals/slots. This is required so that if a render instance
	uses xv (of which there is only one port), we can use it in multiple
	monitors. */
	void swapScreens(KMMMonitor *monitor);

	/** Returns the current seek position */
	const GenTime &seekPosition() const;

	/** See m_noSeek property for details. */
	void setNoSeek(bool noSeek);

	
protected:
	void mousePressEvent(QMouseEvent *e);
	/** Set the monitors scenelist to the one specified. */
	void setSceneList(const QDomDocument &scenelist);
private:
	KdenliveDoc *m_document;
	/** a widget that acts as a holding place in the monitor layout - so that we can
	reparent the screen out of the monitor, reparent another screen in, and it ends
	up in the correct place. Without this, the screen ends up at the bottom of the
	widget stack and the editpanel, etc. ends up at the top of the monitor instead of
	at the bottom. */
	QVBox *m_screenHolder;
	KMMScreen *m_screen;
	KMMEditPanel *m_editPanel;
	/** If true, then changing the clip in the monitor will not change the seek position UNLESS
	 * it falls outside of the current inpoint/outpoint range. If false, then changing the clip
	 * will always seek to the inpoint position.*/
	bool m_noSeek;
	/** The clip being displayed by the monitor. */
	DocClipBase *m_clip;
	/** Connects all signals/slots to the screen. */
	void connectScreen();
	/** Disconnects all signals/slots from the screen */
	void disconnectScreen();
	
public slots: // Public slots
	/** Seek the monitor to the given time. */
	void seek(const GenTime &time);
	/** This slot is called when the screen changes position. */
	void screenPositionChanged(const GenTime &time);
	/** If the monitor is currently playing, stops it. If it is stopped, start it at
	normal speed (1.0) */
	void togglePlay();
	/** Sets this monitor to be the active monitor. It's colour changes to show it is active. */
	void slotSetActive();
	/** Sets this monitor to be an inactive monitor. It's colour changes to show it is inactive. */
	void slotSetInactive();
	/** Causes the monitor to act as if a mouse click has happened on it. */
	void slotClickMonitor();
	/** Sets the displayed clip in the timeline. */
	void slotSetClip(DocClipBase *clip);
	/** Sets the displayed clip in the timeline. */
	void slotSetClip(AVFile *file);
	/** Sets the inpoint of this monitor to the current seek position. */
	void slotSetInpoint();
	/** Sets the outpoint of this monitor to the current seek position. */
	void slotSetOutpoint();
	/** Sets the inpoint of this monitor to the specified seek position. */
	void slotSetInpoint(const GenTime &time);
	/** Sets the outpoint of this monitor to the specified seek position. */
	void slotSetOutpoint(const GenTime &time);
	/** Starts a drag operation, using the currently selected clip and the specified in
	 *  and out points for it. */
	void slotStartDrag();
	
signals: // Signals
	/** Emitted when the monitor's current position has changed. */
	void seekPositionChanged(const GenTime &);
	/** Emitted when the monitor's current inpoint has changed. */
	void inpointPositionChanged(const GenTime &);
	/** Emitted when the monitor's current outpoint has changed. */
	void outpointPositionChanged(const GenTime &);
	/** Emitted when the mouse is clicked over the window. */
	void monitorClicked(KMMMonitor *);
};

#endif
