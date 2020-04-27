/*
Copyright (C) 2017  Jean-Baptiste Mardelle <jb@kdenlive.org>
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

#include "timelinefunctions.hpp"
#include "bin/bin.h"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "clipmodel.hpp"
#include "compositionmodel.hpp"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "groupsmodel.hpp"
#include "logger.hpp"
#include "timelineitemmodel.hpp"
#include "trackmodel.hpp"
#include "transitions/transitionsrepository.hpp"

#include <QApplication>
#include <QDebug>
#include <QInputDialog>
#include <klocalizedstring.h>
#include <unordered_map>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wfloat-equal"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#include <rttr/registration>
#pragma GCC diagnostic pop

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<TimelineFunctions>("TimelineFunctions")
        .method("requestClipCut", select_overload<bool(std::shared_ptr<TimelineItemModel>, int, int)>(&TimelineFunctions::requestClipCut))(
            parameter_names("timeline", "clipId", "position"));
}

bool TimelineFunctions::cloneClip(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, int &newId, PlaylistState::ClipState state, Fun &undo,
                                  Fun &redo)
{
    // Special case: slowmotion clips
    double clipSpeed = timeline->m_allClips[clipId]->getSpeed();
    bool res = timeline->requestClipCreation(timeline->getClipBinId(clipId), newId, state, clipSpeed, undo, redo);
    timeline->m_allClips[newId]->m_endlessResize = timeline->m_allClips[clipId]->m_endlessResize;

    // copy useful timeline properties
    timeline->m_allClips[clipId]->passTimelineProperties(timeline->m_allClips[newId]);

    int duration = timeline->getClipPlaytime(clipId);
    int init_duration = timeline->getClipPlaytime(newId);
    if (duration != init_duration) {
        int in = timeline->m_allClips[clipId]->getIn();
        res = res && timeline->requestItemResize(newId, init_duration - in, false, true, undo, redo);
        res = res && timeline->requestItemResize(newId, duration, true, true, undo, redo);
    }
    if (!res) {
        return false;
    }
    std::shared_ptr<EffectStackModel> sourceStack = timeline->getClipEffectStackModel(clipId);
    std::shared_ptr<EffectStackModel> destStack = timeline->getClipEffectStackModel(newId);
    destStack->importEffects(sourceStack, state);
    return res;
}

bool TimelineFunctions::requestMultipleClipsInsertion(const std::shared_ptr<TimelineItemModel> &timeline, const QStringList &binIds, int trackId, int position,
                                                      QList<int> &clipIds, bool logUndo, bool refreshView)
{
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    for (const QString &binId : binIds) {
        int clipId;
        if (timeline->requestClipInsertion(binId, trackId, position, clipId, logUndo, refreshView, false, undo, redo)) {
            clipIds.append(clipId);
            position += timeline->getItemPlaytime(clipId);
        } else {
            undo();
            clipIds.clear();
            return false;
        }
    }

    if (logUndo) {
        pCore->pushUndo(undo, redo, i18n("Insert Clips"));
    }

    return true;
}

bool TimelineFunctions::processClipCut(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, int position, int &newId, Fun &undo, Fun &redo)
{
    int trackId = timeline->getClipTrackId(clipId);
    int trackDuration = timeline->getTrackById_const(trackId)->trackDuration();
    int start = timeline->getClipPosition(clipId);
    int duration = timeline->getClipPlaytime(clipId);
    if (start > position || (start + duration) < position) {
        return false;
    }
    PlaylistState::ClipState state = timeline->m_allClips[clipId]->clipState();
    bool res = cloneClip(timeline, clipId, newId, state, undo, redo);
    timeline->m_blockRefresh = true;
    res = res && timeline->requestItemResize(clipId, position - start, true, true, undo, redo);
    int newDuration = timeline->getClipPlaytime(clipId);
    // parse effects
    std::shared_ptr<EffectStackModel> sourceStack = timeline->getClipEffectStackModel(clipId);
    sourceStack->cleanFadeEffects(true, undo, redo);
    std::shared_ptr<EffectStackModel> destStack = timeline->getClipEffectStackModel(newId);
    destStack->cleanFadeEffects(false, undo, redo);
    res = res && timeline->requestItemResize(newId, duration - newDuration, false, true, undo, redo);
    // The next requestclipmove does not check for duration change since we don't invalidate timeline, so check duration change now
    bool durationChanged = trackDuration != timeline->getTrackById_const(trackId)->trackDuration();
    res = res && timeline->requestClipMove(newId, trackId, position, true, true, false, true, undo, redo);
    if (durationChanged) {
        // Track length changed, check project duration
        Fun updateDuration = [timeline]() {
            timeline->updateDuration();
            return true;
        };
        updateDuration();
        PUSH_LAMBDA(updateDuration, redo);
    }
    timeline->m_blockRefresh = false;
    return res;
}

bool TimelineFunctions::requestClipCut(std::shared_ptr<TimelineItemModel> timeline, int clipId, int position)
{
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    TRACE_STATIC(timeline, clipId, position);
    bool result = TimelineFunctions::requestClipCut(timeline, clipId, position, undo, redo);
    if (result) {
        pCore->pushUndo(undo, redo, i18n("Cut clip"));
    }
    TRACE_RES(result);
    return result;
}

bool TimelineFunctions::requestClipCut(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, int position, Fun &undo, Fun &redo)
{
    const std::unordered_set<int> clipselect = timeline->getGroupElements(clipId);
    // Remove locked items
    std::unordered_set<int> clips;
    for (int cid : clipselect) {
        if (!timeline->isClip(cid)) {
            continue;
        }
        int tk = timeline->getClipTrackId(cid);
        if (tk != -1 && !timeline->getTrackById_const(tk)->isLocked()) {
            clips.insert(cid);
        }
    }
    // We need to call clearSelection before attempting the split or the group split will be corrupted by the selection group (no undo support)
    timeline->requestClearSelection();

    std::unordered_set<int> topElements;
    std::transform(clips.begin(), clips.end(), std::inserter(topElements, topElements.begin()), [&](int id) { return timeline->m_groups->getRootId(id); });

    int count = 0;
    QList<int> newIds;
    QList<int> clipsToCut;
    for (int cid : clips) {
        if (!timeline->isClip(cid)) {
            continue;
        }
        int start = timeline->getClipPosition(cid);
        int duration = timeline->getClipPlaytime(cid);
        if (start < position && (start + duration) > position) {
            clipsToCut << cid;
        }
    }
    if (clipsToCut.isEmpty()) {
        return true;
    }
    for (int cid : clipsToCut) {
        count++;
        int newId;
        bool res = processClipCut(timeline, cid, position, newId, undo, redo);
        if (!res) {
            bool undone = undo();
            Q_ASSERT(undone);
            return false;
        }
        // splitted elements go temporarily in the same group as original ones.
        timeline->m_groups->setInGroupOf(newId, cid, undo, redo);
        newIds << newId;
    }
    if (count > 0 && timeline->m_groups->isInGroup(clipId)) {
        // we now split the group hierarchy.
        // As a splitting criterion, we compare start point with split position
        auto criterion = [timeline, position](int cid) { return timeline->getClipPosition(cid) < position; };
        bool res = true;
        for (const int topId : topElements) {
            qDebug()<<"// CHECKING REGROUP ELEMENT: "<<topId<<", ISCLIP: "<<timeline->isClip(topId)<<timeline->isGroup(topId);
            res = res && timeline->m_groups->split(topId, criterion, undo, redo);
        }
        if (!res) {
            bool undone = undo();
            Q_ASSERT(undone);
            return false;
        }
    }
    return count > 0;
}

int TimelineFunctions::requestSpacerStartOperation(const std::shared_ptr<TimelineItemModel> &timeline, int trackId, int position)
{
    std::unordered_set<int> clips = timeline->getItemsInRange(trackId, position, -1);
    if (!clips.empty()) {
        timeline->requestSetSelection(clips);
        return (*clips.cbegin());
    }
    return -1;
}

bool TimelineFunctions::requestSpacerEndOperation(const std::shared_ptr<TimelineItemModel> &timeline, int itemId, int startPosition, int endPosition)
{
    // Move group back to original position
    int track = timeline->getItemTrackId(itemId);
    bool isClip = timeline->isClip(itemId);
    if (isClip) {
        timeline->requestClipMove(itemId, track, startPosition, true, false, false);
    } else {
        timeline->requestCompositionMove(itemId, track, startPosition, false, false);
    }
    std::unordered_set<int> clips = timeline->getGroupElements(itemId);
    // Start undoable command
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    //int res = timeline->requestClipsGroup(clips, undo, redo, GroupType::Selection);
    int res = timeline->m_groups->getRootId(itemId);
    bool final = false;
    if (res > -1 || clips.size() == 1) {
        if (clips.size() > 1) {
            final = timeline->requestGroupMove(itemId, res, 0, endPosition - startPosition, true, true, undo, redo);
        } else {
            // only 1 clip to be moved
            if (isClip) {
                final = timeline->requestClipMove(itemId, track, endPosition, true, true, true, true, undo, redo);
            } else {
                final = timeline->requestCompositionMove(itemId, track, -1, endPosition, true, true, undo, redo);
            }
        }
    }
    timeline->requestClearSelection();
    if (final) {
        if (startPosition < endPosition) {
            pCore->pushUndo(undo, redo, i18n("Insert space"));
        } else {
            pCore->pushUndo(undo, redo, i18n("Remove space"));
        }
        return true;
    }
    return false;
}


bool TimelineFunctions::breakAffectedGroups(const std::shared_ptr<TimelineItemModel> &timeline, QVector<int> tracks, QPoint zone, Fun &undo, Fun &redo)
{
    // Check if we have grouped clips that are on unaffected tracks, and ungroup them
    bool result = true;
    std::unordered_set<int> affectedItems;
    // First find all affected items
    for (int &trackId : tracks) {
        std::unordered_set<int> items = timeline->getItemsInRange(trackId, zone.x(), zone.y());
        affectedItems.insert(items.begin(), items.end());
    }
    for (int item : affectedItems) {
        if (timeline->m_groups->isInGroup(item)) {
            int groupId = timeline->m_groups->getRootId(item);
            std::unordered_set<int> all_children = timeline->m_groups->getLeaves(groupId);
            for (int child: all_children) {
                int childTrackId = timeline->getItemTrackId(child);
                if (!tracks.contains(childTrackId)) {
                    // This item should not be affected by the operation, ungroup it
                    result = result && timeline->requestClipUngroup(child, undo, redo);
                }
            }
        }
    }
    return result;
}

bool TimelineFunctions::extractZone(const std::shared_ptr<TimelineItemModel> &timeline, QVector<int> tracks, QPoint zone, bool liftOnly)
{
    // Start undoable command
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    bool result = true;
    result = breakAffectedGroups(timeline, tracks, zone, undo, redo);

    for (int &trackId : tracks) {
        if (timeline->getTrackById_const(trackId)->isLocked()) {
            continue;
        }
        result = result && TimelineFunctions::liftZone(timeline, trackId, zone, undo, redo);
    }
    if (result && !liftOnly) {
        result = TimelineFunctions::removeSpace(timeline, -1, zone, undo, redo, tracks);
    }
    pCore->pushUndo(undo, redo, liftOnly ? i18n("Lift zone") : i18n("Extract zone"));
    return result;
}

bool TimelineFunctions::insertZone(const std::shared_ptr<TimelineItemModel> &timeline, QList<int> trackIds, const QString &binId, int insertFrame, QPoint zone,
                                   bool overwrite, bool useTargets)
{
    // Start undoable command
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    bool result = true;
    QVector<int> affectedTracks;
    auto it = timeline->m_allTracks.cbegin();
    if (!useTargets && overwrite) {
        // Timeline drop in overwrite mode
        for (int target_track : trackIds) {
            if (!timeline->getTrackById_const(target_track)->isLocked()) {
                affectedTracks << target_track;
            }
        }
    } else {
        while (it != timeline->m_allTracks.cend()) {
            int target_track = (*it)->getId();
            if (!useTargets || timeline->getTrackById_const(target_track)->shouldReceiveTimelineOp()) {
                affectedTracks << target_track;
            } else if (trackIds.contains(target_track)) {
                // Track is marked as target but not active, remove it
                trackIds.removeAll(target_track);
            }
            ++it;
        }
    }
    if (affectedTracks.isEmpty()) {
        pCore->displayMessage(i18n("Please activate a track by clicking on a track's label"), InformationMessage);
        return false;
    }
    result = breakAffectedGroups(timeline, affectedTracks, QPoint(insertFrame, insertFrame + (zone.y() - zone.x())), undo, redo);
    if (overwrite) {
        // Cut all tracks
        for (int target_track : affectedTracks) {
            result = result && TimelineFunctions::liftZone(timeline, target_track, QPoint(insertFrame, insertFrame + (zone.y() - zone.x())), undo, redo);
            if (!result) {
                qDebug() << "// LIFTING ZONE FAILED\n";
                break;
            }
        }
    } else {
        // Cut all tracks
        for (int target_track : affectedTracks) {
            int startClipId = timeline->getClipByPosition(target_track, insertFrame);
            if (startClipId > -1) {
                // There is a clip, cut it
                result = result && TimelineFunctions::requestClipCut(timeline, startClipId, insertFrame, undo, redo);
            }
        }
        result = result && TimelineFunctions::requestInsertSpace(timeline, QPoint(insertFrame, insertFrame + (zone.y() - zone.x())), undo, redo, affectedTracks);
    }
    bool clipInserted = false;
    if (result) {
        if (!trackIds.isEmpty()) {
            int newId = -1;
            QString binClipId;
            if (binId.contains(QLatin1Char('/'))) {
                binClipId = QString("%1/%2/%3").arg(binId.section(QLatin1Char('/'), 0, 0)).arg(zone.x()).arg(zone.y() - 1);
            } else {
                binClipId = QString("%1/%2/%3").arg(binId).arg(zone.x()).arg(zone.y() - 1);
            }
            result = timeline->requestClipInsertion(binClipId, trackIds.first(), insertFrame, newId, true, true, useTargets, undo, redo, affectedTracks);
            if (result) {
                clipInserted = true;
            }
        }
        if (result) {
            pCore->pushUndo(undo, redo, overwrite ? i18n("Overwrite zone") : i18n("Insert zone"));
        }
    }
    if (!result) {
        qDebug() << "// REQUESTING SPACE FAILED";
        undo();
    }
    return clipInserted;
}

bool TimelineFunctions::liftZone(const std::shared_ptr<TimelineItemModel> &timeline, int trackId, QPoint zone, Fun &undo, Fun &redo)
{
    // Check if there is a clip at start point
    int startClipId = timeline->getClipByPosition(trackId, zone.x());
    if (startClipId > -1) {
        // There is a clip, cut it
        if (timeline->getClipPosition(startClipId) < zone.x()) {
            qDebug() << "/// CUTTING AT START: " << zone.x() << ", ID: " << startClipId;
            TimelineFunctions::requestClipCut(timeline, startClipId, zone.x(), undo, redo);
            qDebug() << "/// CUTTING AT START DONE";
        }
    }
    int endClipId = timeline->getClipByPosition(trackId, zone.y());
    if (endClipId > -1) {
        // There is a clip, cut it
        if (timeline->getClipPosition(endClipId) + timeline->getClipPlaytime(endClipId) > zone.y()) {
            qDebug() << "/// CUTTING AT END: " << zone.y() << ", ID: " << endClipId;
            TimelineFunctions::requestClipCut(timeline, endClipId, zone.y(), undo, redo);
            qDebug() << "/// CUTTING AT END DONE";
        }
    }
    std::unordered_set<int> clips = timeline->getItemsInRange(trackId, zone.x(), zone.y());
    for (const auto &clipId : clips) {
        timeline->requestItemDeletion(clipId, undo, redo);
    }
    return true;
}

bool TimelineFunctions::removeSpace(const std::shared_ptr<TimelineItemModel> &timeline, int trackId, QPoint zone, Fun &undo, Fun &redo, QVector<int> allowedTracks)
{
    Q_UNUSED(trackId)
    std::unordered_set<int> clips;
    auto it = timeline->m_allTracks.cbegin();
    while (it != timeline->m_allTracks.cend()) {
        int target_track = (*it)->getId();
        if (timeline->getTrackById_const(target_track)->shouldReceiveTimelineOp()) {
            std::unordered_set<int> subs = timeline->getItemsInRange(target_track, zone.y() - 1, -1, true);
            clips.insert(subs.begin(), subs.end());
        }
        ++it;
    }
    if (clips.size() == 0) {
        // TODO: inform user no change will be performed
        return true;
    }
    bool result = false;
    timeline->requestSetSelection(clips);
    int itemId = *clips.begin();
    int targetTrackId = timeline->getItemTrackId(itemId);
    int targetPos = timeline->getItemPosition(itemId) + zone.x() - zone.y();

    if (timeline->m_groups->isInGroup(itemId)) {
        result = timeline->requestGroupMove(itemId, timeline->m_groups->getRootId(itemId), 0, zone.x() - zone.y(), true, true, undo, redo, true, true, allowedTracks);
    } else if (timeline->isClip(itemId)) {
        result = timeline->requestClipMove(itemId, targetTrackId, targetPos, true, true, true, true, undo, redo);
    } else {
        result = timeline->requestCompositionMove(itemId, targetTrackId, timeline->m_allCompositions[itemId]->getForcedTrack(), targetPos, true, true, undo, redo);
    }
    timeline->requestClearSelection();
    if (!result) {
        undo();
    }
    return result;
}

bool TimelineFunctions::requestInsertSpace(const std::shared_ptr<TimelineItemModel> &timeline, QPoint zone, Fun &undo, Fun &redo, QVector<int> allowedTracks)
{
    timeline->requestClearSelection();
    Fun local_undo = []() { return true; };
    Fun local_redo = []() { return true; };
    std::unordered_set<int> items;
    if (allowedTracks.isEmpty()) {
        // Select clips in all tracks
        items = timeline->getItemsInRange(-1, zone.x(), -1, true);
    } else {
        // Select clips in target and active tracks only
        for (int target_track : allowedTracks) {
            std::unordered_set<int> subs = timeline->getItemsInRange(target_track, zone.x(), -1, true);
            items.insert(subs.begin(), subs.end());
        }
    }
    if (items.empty()) {
        return true;
    }
    timeline->requestSetSelection(items);
    bool result = true;
    int itemId = *(items.begin());
    int targetTrackId = timeline->getItemTrackId(itemId);
    int targetPos = timeline->getItemPosition(itemId) + zone.y() - zone.x();

    // TODO the three move functions should be unified in a "requestItemMove" function
    if (timeline->m_groups->isInGroup(itemId)) {
        result =
            result && timeline->requestGroupMove(itemId, timeline->m_groups->getRootId(itemId), 0, zone.y() - zone.x(), true, true, local_undo, local_redo, true, true, allowedTracks);
    } else if (timeline->isClip(itemId)) {
        result = result && timeline->requestClipMove(itemId, targetTrackId, targetPos, true, true, true, true, local_undo, local_redo);
    } else {
        result = result && timeline->requestCompositionMove(itemId, targetTrackId, timeline->m_allCompositions[itemId]->getForcedTrack(), targetPos, true, true,
                                                            local_undo, local_redo);
    }
    timeline->requestClearSelection();
    if (!result) {
        bool undone = local_undo();
        Q_ASSERT(undone);
        pCore->displayMessage(i18n("Cannot move selected group"), ErrorMessage);
    }
    UPDATE_UNDO_REDO_NOLOCK(local_redo, local_undo, undo, redo);
    return result;
}

bool TimelineFunctions::requestItemCopy(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, int trackId, int position)
{
    Q_ASSERT(timeline->isClip(clipId) || timeline->isComposition(clipId));
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    int deltaTrack = timeline->getTrackPosition(trackId) - timeline->getTrackPosition(timeline->getItemTrackId(clipId));
    int deltaPos = position - timeline->getItemPosition(clipId);
    std::unordered_set<int> allIds = timeline->getGroupElements(clipId);
    std::unordered_map<int, int> mapping; // keys are ids of the source clips, values are ids of the copied clips
    bool res = true;
    for (int id : allIds) {
        int newId = -1;
        if (timeline->isClip(id)) {
            PlaylistState::ClipState state = timeline->m_allClips[id]->clipState();
            res = cloneClip(timeline, id, newId, state, undo, redo);
            res = res && (newId != -1);
        }
        int target_position = timeline->getItemPosition(id) + deltaPos;
        int target_track_position = timeline->getTrackPosition(timeline->getItemTrackId(id)) + deltaTrack;
        if (target_track_position >= 0 && target_track_position < timeline->getTracksCount()) {
            auto it = timeline->m_allTracks.cbegin();
            std::advance(it, target_track_position);
            int target_track = (*it)->getId();
            if (timeline->isClip(id)) {
                res = res && timeline->requestClipMove(newId, target_track, target_position, true, true, true, true, undo, redo);
            } else {
                const QString &transitionId = timeline->m_allCompositions[id]->getAssetId();
                std::unique_ptr<Mlt::Properties> transProps(timeline->m_allCompositions[id]->properties());
                res = res && timeline->requestCompositionInsertion(transitionId, target_track, -1, target_position,
                                                                   timeline->m_allCompositions[id]->getPlaytime(), std::move(transProps), newId, undo, redo);
            }
        } else {
            res = false;
        }
        if (!res) {
            bool undone = undo();
            Q_ASSERT(undone);
            return false;
        }
        mapping[id] = newId;
    }
    qDebug() << "Successful copy, coping groups...";
    res = timeline->m_groups->copyGroups(mapping, undo, redo);
    if (!res) {
        bool undone = undo();
        Q_ASSERT(undone);
        return false;
    }
    return true;
}

void TimelineFunctions::showClipKeyframes(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, bool value)
{
    timeline->m_allClips[clipId]->setShowKeyframes(value);
    QModelIndex modelIndex = timeline->makeClipIndexFromID(clipId);
    timeline->dataChanged(modelIndex, modelIndex, {TimelineModel::ShowKeyframesRole});
}

void TimelineFunctions::showCompositionKeyframes(const std::shared_ptr<TimelineItemModel> &timeline, int compoId, bool value)
{
    timeline->m_allCompositions[compoId]->setShowKeyframes(value);
    QModelIndex modelIndex = timeline->makeCompositionIndexFromID(compoId);
    timeline->dataChanged(modelIndex, modelIndex, {TimelineModel::ShowKeyframesRole});
}

bool TimelineFunctions::switchEnableState(const std::shared_ptr<TimelineItemModel> &timeline, std::unordered_set<int> selection)
{
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    bool result = false;
    bool disable = true;
    for (int clipId : selection) {
        if (!timeline->isClip(clipId)) {
            continue;
        }
        PlaylistState::ClipState oldState = timeline->getClipPtr(clipId)->clipState();
        PlaylistState::ClipState state = PlaylistState::Disabled;
        disable = true;
        if (oldState == PlaylistState::Disabled) {
            state = timeline->getTrackById_const(timeline->getClipTrackId(clipId))->trackType();
            disable = false;
        }
        result = changeClipState(timeline, clipId, state, undo, redo);
        if (!result) {
            break;
        }
    }
    if (result) {
            pCore->pushUndo(undo, redo, disable ? i18n("Disable clip") : i18n("Enable clip"));
    }
    return result;
}

bool TimelineFunctions::changeClipState(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, PlaylistState::ClipState status, Fun &undo, Fun &redo)
{
    int track = timeline->getClipTrackId(clipId);
    int start = -1;
    bool invalidate = false;
    if (track > -1) {
        if (!timeline->getTrackById_const(track)->isAudioTrack()) {
            invalidate = true;
        }
        start = timeline->getItemPosition(clipId);
    }
    Fun local_undo = []() { return true; };
    Fun local_redo = []() { return true; };
    // For the state change to work, we need to unplant/replant the clip
    bool result = true;
    if (track > -1) {
        result = timeline->getTrackById(track)->requestClipDeletion(clipId, true, invalidate, local_undo, local_redo, false, false);
    }
    result = timeline->m_allClips[clipId]->setClipState(status, local_undo, local_redo);
    if (result && track > -1) {
        result = timeline->getTrackById(track)->requestClipInsertion(clipId, start, true, true, local_undo, local_redo);
    }
    UPDATE_UNDO_REDO_NOLOCK(local_redo, local_undo, undo, redo);
    return result;
}

bool TimelineFunctions::requestSplitAudio(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, int audioTarget)
{
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    const std::unordered_set<int> clips = timeline->getGroupElements(clipId);
    bool done = false;
    // Now clear selection so we don't mess with groups
    timeline->requestClearSelection(false, undo, redo);
    for (int cid : clips) {
        if (!timeline->getClipPtr(cid)->canBeAudio() || timeline->getClipPtr(cid)->clipState() == PlaylistState::AudioOnly) {
            // clip without audio or audio only, skip
            pCore->displayMessage(i18n("One or more clips do not have audio, or are already audio"), ErrorMessage);
            return false;
        }
        int position = timeline->getClipPosition(cid);
        int track = timeline->getClipTrackId(cid);
        QList<int> possibleTracks = audioTarget >= 0 ? QList<int>() << audioTarget : timeline->getLowerTracksId(track, TrackType::AudioTrack);
        if (possibleTracks.isEmpty()) {
            // No available audio track for splitting, abort
            undo();
            pCore->displayMessage(i18n("No available audio track for split operation"), ErrorMessage);
            return false;
        }
        int newId;
        bool res = cloneClip(timeline, cid, newId, PlaylistState::AudioOnly, undo, redo);
        if (!res) {
            bool undone = undo();
            Q_ASSERT(undone);
            pCore->displayMessage(i18n("Audio split failed"), ErrorMessage);
            return false;
        }
        bool success = false;
        while (!success && !possibleTracks.isEmpty()) {
            int newTrack = possibleTracks.takeFirst();
            success = timeline->requestClipMove(newId, newTrack, position, true, true, false, true, undo, redo);
        }
        TimelineFunctions::changeClipState(timeline, cid, PlaylistState::VideoOnly, undo, redo);
        success = success && timeline->m_groups->createGroupAtSameLevel(cid, std::unordered_set<int>{newId}, GroupType::AVSplit, undo, redo);
        if (!success) {
            bool undone = undo();
            Q_ASSERT(undone);
            pCore->displayMessage(i18n("Audio split failed"), ErrorMessage);
            return false;
        }
        done = true;
    }
    if (done) {
        timeline->requestSetSelection(clips, undo, redo);
        pCore->pushUndo(undo, redo, i18n("Split Audio"));
    }
    return done;
}

bool TimelineFunctions::requestSplitVideo(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, int videoTarget)
{
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    const std::unordered_set<int> clips = timeline->getGroupElements(clipId);
    bool done = false;
    // Now clear selection so we don't mess with groups
    timeline->requestClearSelection();
    for (int cid : clips) {
        if (!timeline->getClipPtr(cid)->canBeVideo() || timeline->getClipPtr(cid)->clipState() == PlaylistState::VideoOnly) {
            // clip without audio or audio only, skip
            continue;
        }
        int position = timeline->getClipPosition(cid);
        QList<int> possibleTracks = QList<int>() << videoTarget;
        if (possibleTracks.isEmpty()) {
            // No available audio track for splitting, abort
            undo();
            pCore->displayMessage(i18n("No available video track for split operation"), ErrorMessage);
            return false;
        }
        int newId;
        bool res = cloneClip(timeline, cid, newId, PlaylistState::VideoOnly, undo, redo);
        if (!res) {
            bool undone = undo();
            Q_ASSERT(undone);
            pCore->displayMessage(i18n("Video split failed"), ErrorMessage);
            return false;
        }
        bool success = false;
        while (!success && !possibleTracks.isEmpty()) {
            int newTrack = possibleTracks.takeFirst();
            success = timeline->requestClipMove(newId, newTrack, position, true, true, true, true, undo, redo);
        }
        TimelineFunctions::changeClipState(timeline, cid, PlaylistState::AudioOnly, undo, redo);
        success = success && timeline->m_groups->createGroupAtSameLevel(cid, std::unordered_set<int>{newId}, GroupType::AVSplit, undo, redo);
        if (!success) {
            bool undone = undo();
            Q_ASSERT(undone);
            pCore->displayMessage(i18n("Video split failed"), ErrorMessage);
            return false;
        }
        done = true;
    }
    if (done) {
        pCore->pushUndo(undo, redo, i18n("Split Video"));
    }
    return done;
}

void TimelineFunctions::setCompositionATrack(const std::shared_ptr<TimelineItemModel> &timeline, int cid, int aTrack)
{
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    std::shared_ptr<CompositionModel> compo = timeline->getCompositionPtr(cid);
    int previousATrack = compo->getATrack();
    int previousAutoTrack = static_cast<int>(compo->getForcedTrack() == -1);
    bool autoTrack = aTrack < 0;
    if (autoTrack) {
        // Automatic track compositing, find lower video track
        aTrack = timeline->getPreviousVideoTrackPos(compo->getCurrentTrackId());
    }
    int start = timeline->getItemPosition(cid);
    int end = start + timeline->getItemPlaytime(cid);
    Fun local_redo = [timeline, cid, aTrack, autoTrack, start, end]() {
        timeline->unplantComposition(cid);
        QScopedPointer<Mlt::Field> field(timeline->m_tractor->field());
        field->lock();
        timeline->getCompositionPtr(cid)->setForceTrack(!autoTrack);
        timeline->getCompositionPtr(cid)->setATrack(aTrack, aTrack <= 0 ? -1 : timeline->getTrackIndexFromPosition(aTrack - 1));
        field->unlock();
        timeline->replantCompositions(cid, true);
        timeline->invalidateZone(start, end);
        timeline->checkRefresh(start, end);
        return true;
    };
    Fun local_undo = [timeline, cid, previousATrack, previousAutoTrack, start, end]() {
        timeline->unplantComposition(cid);
        QScopedPointer<Mlt::Field> field(timeline->m_tractor->field());
        field->lock();
        timeline->getCompositionPtr(cid)->setForceTrack(previousAutoTrack == 0);
        timeline->getCompositionPtr(cid)->setATrack(previousATrack, previousATrack <= 0 ? -1 : timeline->getTrackIndexFromPosition(previousATrack - 1));
        field->unlock();
        timeline->replantCompositions(cid, true);
        timeline->invalidateZone(start, end);
        timeline->checkRefresh(start, end);
        return true;
    };
    if (local_redo()) {
        PUSH_LAMBDA(local_undo, undo);
        PUSH_LAMBDA(local_redo, redo);
    }
    pCore->pushUndo(undo, redo, i18n("Change Composition Track"));
}

void TimelineFunctions::enableMultitrackView(const std::shared_ptr<TimelineItemModel> &timeline, bool enable, bool refresh)
{
    QList<int> videoTracks;
    for (const auto &track : timeline->m_iteratorTable) {
        if (timeline->getTrackById_const(track.first)->isAudioTrack() || timeline->getTrackById_const(track.first)->isHidden()) {
            continue;
        }
        videoTracks << track.first;
    }
    if (videoTracks.size() < 2) {
        pCore->displayMessage(i18n("Cannot enable multitrack view on a single track"), InformationMessage);
    }
    // First, dis/enable track compositing
    QScopedPointer<Mlt::Service> service(timeline->m_tractor->field());
    Mlt::Field *field = timeline->m_tractor->field();
    field->lock();
    while ((service != nullptr) && service->is_valid()) {
        if (service->type() == transition_type) {
            Mlt::Transition t((mlt_transition)service->get_service());
            QString serviceName = t.get("mlt_service");
            int added = t.get_int("internal_added");
            if (added == 237 && serviceName != QLatin1String("mix")) {
                // remove all compositing transitions
                t.set("disable", enable ? "1" : nullptr);
            } else if (!enable && added == 200) {
                field->disconnect_service(t);
            }
        }
        service.reset(service->producer());
    }
    if (enable) {
        for (int i = 0; i < videoTracks.size(); ++i) {
            Mlt::Transition transition(*timeline->m_tractor->profile(), "composite");
            transition.set("mlt_service", "composite");
            transition.set("a_track", 0);
            transition.set("b_track", timeline->getTrackMltIndex(videoTracks.at(i)));
            transition.set("distort", 0);
            transition.set("aligned", 0);
            // 200 is an arbitrary number so we can easily remove these transition later
            transition.set("internal_added", 200);
            QString geometry;
            switch (i) {
            case 0:
                switch (videoTracks.size()) {
                case 2:
                    geometry = QStringLiteral("0 0 50% 100%");
                    break;
                case 3:
                    geometry = QStringLiteral("0 0 33% 100%");
                    break;
                case 4:
                    geometry = QStringLiteral("0 0 50% 50%");
                    break;
                case 5:
                case 6:
                    geometry = QStringLiteral("0 0 33% 50%");
                    break;
                default:
                    geometry = QStringLiteral("0 0 33% 33%");
                    break;
                }
                break;
            case 1:
                switch (videoTracks.size()) {
                case 2:
                    geometry = QStringLiteral("50% 0 50% 100%");
                    break;
                case 3:
                    geometry = QStringLiteral("33% 0 33% 100%");
                    break;
                case 4:
                    geometry = QStringLiteral("50% 0 50% 50%");
                    break;
                case 5:
                case 6:
                    geometry = QStringLiteral("33% 0 33% 50%");
                    break;
                default:
                    geometry = QStringLiteral("33% 0 33% 33%");
                    break;
                }
                break;
            case 2:
                switch (videoTracks.size()) {
                case 3:
                    geometry = QStringLiteral("66% 0 33% 100%");
                    break;
                case 4:
                    geometry = QStringLiteral("0 50% 50% 50%");
                    break;
                case 5:
                case 6:
                    geometry = QStringLiteral("66% 0 33% 50%");
                    break;
                default:
                    geometry = QStringLiteral("66% 0 33% 33%");
                    break;
                }
                break;
            case 3:
                switch (videoTracks.size()) {
                case 4:
                    geometry = QStringLiteral("50% 50% 50% 50%");
                    break;
                case 5:
                case 6:
                    geometry = QStringLiteral("0 50% 33% 50%");
                    break;
                default:
                    geometry = QStringLiteral("0 33% 33% 33%");
                    break;
                }
                break;
            case 4:
                switch (videoTracks.size()) {
                case 5:
                case 6:
                    geometry = QStringLiteral("33% 50% 33% 50%");
                    break;
                default:
                    geometry = QStringLiteral("33% 33% 33% 33%");
                    break;
                }
                break;
            case 5:
                switch (videoTracks.size()) {
                case 6:
                    geometry = QStringLiteral("66% 50% 33% 50%");
                    break;
                default:
                    geometry = QStringLiteral("66% 33% 33% 33%");
                    break;
                }
                break;
            case 6:
                geometry = QStringLiteral("0 66% 33% 33%");
                break;
            case 7:
                geometry = QStringLiteral("33% 66% 33% 33%");
                break;
            default:
                geometry = QStringLiteral("66% 66% 33% 33%");
                break;
            }
            // Add transition to track:
            transition.set("geometry", geometry.toUtf8().constData());
            transition.set("always_active", 1);
            field->plant_transition(transition, 0, timeline->getTrackMltIndex(videoTracks.at(i)));
        }
    }
    field->unlock();
    if (refresh) {
        timeline->requestMonitorRefresh();
    }
}

void TimelineFunctions::saveTimelineSelection(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &selection,
                                              const QDir &targetDir)
{
    bool ok;
    QString name = QInputDialog::getText(qApp->activeWindow(), i18n("Add Clip to Library"), i18n("Enter a name for the clip in Library"), QLineEdit::Normal,
                                         QString(), &ok);
    if (name.isEmpty() || !ok) {
        return;
    }
    if (targetDir.exists(name + QStringLiteral(".mlt"))) {
        // TODO: warn and ask for overwrite / rename
    }
    int offset = -1;
    int lowerAudioTrack = -1;
    int lowerVideoTrack = -1;
    QString fullPath = targetDir.absoluteFilePath(name + QStringLiteral(".mlt"));
    // Build a copy of selected tracks.
    QMap<int, int> sourceTracks;
    for (int i : selection) {
        int sourceTrack = timeline->getItemTrackId(i);
        int clipPos = timeline->getItemPosition(i);
        if (offset < 0 || clipPos < offset) {
            offset = clipPos;
        }
        int trackPos = timeline->getTrackMltIndex(sourceTrack);
        if (!sourceTracks.contains(trackPos)) {
            sourceTracks.insert(trackPos, sourceTrack);
        }
    }
    // Build target timeline
    Mlt::Tractor newTractor(*timeline->m_tractor->profile());
    QScopedPointer<Mlt::Field> field(newTractor.field());
    int ix = 0;
    QString composite = TransitionsRepository::get()->getCompositingTransition();
    QMapIterator<int, int> i(sourceTracks);
    QList<Mlt::Transition *> compositions;
    while (i.hasNext()) {
        i.next();
        QScopedPointer<Mlt::Playlist> newTrackPlaylist(new Mlt::Playlist(*newTractor.profile()));
        newTractor.set_track(*newTrackPlaylist, ix);
        // QScopedPointer<Mlt::Producer> trackProducer(newTractor.track(ix));
        int trackId = i.value();
        sourceTracks.insert(timeline->getTrackMltIndex(trackId), ix);
        std::shared_ptr<TrackModel> track = timeline->getTrackById_const(trackId);
        bool isAudio = track->isAudioTrack();
        if (isAudio) {
            newTrackPlaylist->set("hide", 1);
            if (lowerAudioTrack < 0) {
                lowerAudioTrack = ix;
            }
        } else {
            newTrackPlaylist->set("hide", 2);
            if (lowerVideoTrack < 0) {
                lowerVideoTrack = ix;
            }
        }
        for (int itemId : selection) {
            if (timeline->getItemTrackId(itemId) == trackId) {
                // Copy clip on the destination track
                if (timeline->isClip(itemId)) {
                    int clip_position = timeline->m_allClips[itemId]->getPosition();
                    auto clip_loc = track->getClipIndexAt(clip_position);
                    int target_clip = clip_loc.second;
                    QSharedPointer<Mlt::Producer> clip = track->getClipProducer(target_clip);
                    newTrackPlaylist->insert_at(clip_position - offset, clip.data(), 1);
                } else if (timeline->isComposition(itemId)) {
                    // Composition
                    auto *t = new Mlt::Transition(*timeline->m_allCompositions[itemId].get());
                    QString id(t->get("kdenlive_id"));
                    QString internal(t->get("internal_added"));
                    if (internal.isEmpty()) {
                        compositions << t;
                        if (id.isEmpty()) {
                            qDebug() << "// Warning, this should not happen, transition without id: " << t->get("id") << " = " << t->get("mlt_service");
                            t->set("kdenlive_id", t->get("mlt_service"));
                        }
                    }
                }
            }
        }
        ix++;
    }
    // Sort compositions and insert
    if (!compositions.isEmpty()) {
        std::sort(compositions.begin(), compositions.end(), [](Mlt::Transition *a, Mlt::Transition *b) { return a->get_b_track() < b->get_b_track(); });
        while (!compositions.isEmpty()) {
            QScopedPointer<Mlt::Transition> t(compositions.takeFirst());
            if (sourceTracks.contains(t->get_a_track()) && sourceTracks.contains(t->get_b_track())) {
                Mlt::Transition newComposition(*newTractor.profile(), t->get("mlt_service"));
                Mlt::Properties sourceProps(t->get_properties());
                newComposition.inherit(sourceProps);
                QString id(t->get("kdenlive_id"));
                int in = qMax(0, t->get_in() - offset);
                int out = t->get_out() - offset;
                newComposition.set_in_and_out(in, out);
                int a_track = sourceTracks.value(t->get_a_track());
                int b_track = sourceTracks.value(t->get_b_track());
                field->plant_transition(newComposition, a_track, b_track);
            }
        }
    }
    // Track compositing
    i.toFront();
    ix = 0;
    while (i.hasNext()) {
        i.next();
        int trackId = i.value();
        std::shared_ptr<TrackModel> track = timeline->getTrackById_const(trackId);
        bool isAudio = track->isAudioTrack();
        if ((isAudio && ix > lowerAudioTrack) || (!isAudio && ix > lowerVideoTrack)) {
            // add track compositing / mix
            Mlt::Transition t(*newTractor.profile(), isAudio ? "mix" : composite.toUtf8().constData());
            if (isAudio) {
                t.set("sum", 1);
            }
            t.set("always_active", 1);
            t.set("internal_added", 237);
            field->plant_transition(t, isAudio ? lowerAudioTrack : lowerVideoTrack, ix);
        }
        ix++;
    }
    Mlt::Consumer xmlConsumer(*newTractor.profile(), ("xml:" + fullPath).toUtf8().constData());
    xmlConsumer.set("terminate_on_pause", 1);
    xmlConsumer.connect(newTractor);
    xmlConsumer.run();
}

int TimelineFunctions::getTrackOffset(const std::shared_ptr<TimelineItemModel> &timeline, int startTrack, int destTrack)
{
    qDebug() << "+++++++\nGET TRACK OFFSET: " << startTrack << " - " << destTrack;
    int masterTrackMltIndex = timeline->getTrackMltIndex(startTrack);
    int destTrackMltIndex = timeline->getTrackMltIndex(destTrack);
    int offset = 0;
    qDebug() << "+++++++\nGET TRACK MLT: " << masterTrackMltIndex << " - " << destTrackMltIndex;
    if (masterTrackMltIndex == destTrackMltIndex) {
        return offset;
    }
    int step = masterTrackMltIndex > destTrackMltIndex ? -1 : 1;
    bool isAudio = timeline->isAudioTrack(startTrack);
    int track = masterTrackMltIndex;
    while (track != destTrackMltIndex) {
        track += step;
        qDebug() << "+ + +TESTING TRACK: " << track;
        int trackId = timeline->getTrackIndexFromPosition(track - 1);
        if (isAudio == timeline->isAudioTrack(trackId)) {
            offset += step;
        }
    }
    return offset;
}

int TimelineFunctions::getOffsetTrackId(const std::shared_ptr<TimelineItemModel> &timeline, int startTrack, int offset, bool audioOffset)
{
    int masterTrackMltIndex = timeline->getTrackMltIndex(startTrack);
    bool isAudio = timeline->isAudioTrack(startTrack);
    if (isAudio != audioOffset) {
        offset = -offset;
    }
    qDebug() << "* ** * MASTER INDEX: " << masterTrackMltIndex << ", OFFSET: " << offset;
    while (offset != 0) {
        masterTrackMltIndex += offset > 0 ? 1 : -1;
        qDebug() << "#### TESTING TRACK: " << masterTrackMltIndex;
        if (masterTrackMltIndex < 0) {
            masterTrackMltIndex = 0;
            break;
        }
        if (masterTrackMltIndex > (int)timeline->m_allTracks.size()) {
            masterTrackMltIndex = (int)timeline->m_allTracks.size();
            break;
        }
        int trackId = timeline->getTrackIndexFromPosition(masterTrackMltIndex - 1);
        if (timeline->isAudioTrack(trackId) == isAudio) {
            offset += offset > 0 ? -1 : 1;
        }
    }
    return timeline->getTrackIndexFromPosition(masterTrackMltIndex - 1);
}

QPair<QList<int>, QList<int>> TimelineFunctions::getAVTracksIds(const std::shared_ptr<TimelineItemModel> &timeline)
{
    QList<int> audioTracks;
    QList<int> videoTracks;
    for (const auto &track : timeline->m_allTracks) {
        if (track->isAudioTrack()) {
            audioTracks << track->getId();
        } else {
            videoTracks << track->getId();
        }
    }
    return {audioTracks, videoTracks};
}

QString TimelineFunctions::copyClips(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &itemIds)
{
    int clipId = *(itemIds.begin());
    // We need to retrieve ALL the involved clips, ie those who are also grouped with the given clips
    std::unordered_set<int> allIds;
    for (const auto &itemId : itemIds) {
        std::unordered_set<int> siblings = timeline->getGroupElements(itemId);
        allIds.insert(siblings.begin(), siblings.end());
    }

    timeline->requestClearSelection();
    // TODO better guess for master track
    int masterTid = timeline->getItemTrackId(clipId);
    bool audioCopy = timeline->isAudioTrack(masterTid);
    int masterTrack = timeline->getTrackPosition(masterTid);
    QDomDocument copiedItems;
    int offset = -1;
    QDomElement container = copiedItems.createElement(QStringLiteral("kdenlive-scene"));
    copiedItems.appendChild(container);
    QStringList binIds;
    for (int id : allIds) {
        if (offset == -1 || timeline->getItemPosition(id) < offset) {
            offset = timeline->getItemPosition(id);
        }
        if (timeline->isClip(id)) {
            container.appendChild(timeline->m_allClips[id]->toXml(copiedItems));
            const QString bid = timeline->m_allClips[id]->binId();
            if (!binIds.contains(bid)) {
                binIds << bid;
            }
        } else if (timeline->isComposition(id)) {
            container.appendChild(timeline->m_allCompositions[id]->toXml(copiedItems));
        } else {
            Q_ASSERT(false);
        }
    }
    QDomElement container2 = copiedItems.createElement(QStringLiteral("bin"));
    container.appendChild(container2);
    for (const QString &id : binIds) {
        std::shared_ptr<ProjectClip> clip = pCore->projectItemModel()->getClipByBinID(id);
        QDomDocument tmp;
        container2.appendChild(clip->toXml(tmp));
    }
    container.setAttribute(QStringLiteral("offset"), offset);
    if (audioCopy) {
        container.setAttribute(QStringLiteral("masterAudioTrack"), masterTrack);
        int masterMirror = timeline->getMirrorVideoTrackId(masterTid);
        if (masterMirror == -1) {
            QPair<QList<int>, QList<int>> projectTracks = TimelineFunctions::getAVTracksIds(timeline);
            if (!projectTracks.second.isEmpty()) {
                masterTrack = timeline->getTrackPosition(projectTracks.second.first());
            }
        } else {
            masterTrack = timeline->getTrackPosition(masterMirror);
        }
    }
    /* masterTrack contains the reference track over which we want to paste.
       this is a video track, unless audioCopy is defined */
    container.setAttribute(QStringLiteral("masterTrack"), masterTrack);
    container.setAttribute(QStringLiteral("documentid"), pCore->currentDoc()->getDocumentProperty(QStringLiteral("documentid")));
    QDomElement grp = copiedItems.createElement(QStringLiteral("groups"));
    container.appendChild(grp);

    std::unordered_set<int> groupRoots;
    std::transform(allIds.begin(), allIds.end(), std::inserter(groupRoots, groupRoots.begin()), [&](int id) { return timeline->m_groups->getRootId(id); });

    qDebug() << "==============\n GROUP ROOTS: ";
    for (int gp : groupRoots) {
        qDebug() << "GROUP: " << gp;
    }
    qDebug() << "\n=======";
    grp.appendChild(copiedItems.createTextNode(timeline->m_groups->toJson(groupRoots)));

    qDebug() << " / // / PASTED DOC: \n\n" << copiedItems.toString() << "\n\n------------";
    return copiedItems.toString();
}

bool TimelineFunctions::pasteClips(const std::shared_ptr<TimelineItemModel> &timeline, const QString &pasteString, int trackId, int position)
{
    timeline->requestClearSelection();
    QDomDocument copiedItems;
    copiedItems.setContent(pasteString);
    if (copiedItems.documentElement().tagName() == QLatin1String("kdenlive-scene")) {
        qDebug() << " / / READING CLIPS FROM CLIPBOARD";
    } else {
        return false;
    }
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    const QString docId = copiedItems.documentElement().attribute(QStringLiteral("documentid"));
    QMap<QString, QString> mappedIds;
    // Check available tracks
    QPair<QList<int>, QList<int>> projectTracks = TimelineFunctions::getAVTracksIds(timeline);
    int masterSourceTrack = copiedItems.documentElement().attribute(QStringLiteral("masterTrack")).toInt();
    QDomNodeList clips = copiedItems.documentElement().elementsByTagName(QStringLiteral("clip"));
    QDomNodeList compositions = copiedItems.documentElement().elementsByTagName(QStringLiteral("composition"));
    // find paste tracks
    // List of all source audio tracks
    QList<int> audioTracks;
    // List of all source video tracks
    QList<int> videoTracks;
    // List of all audio tracks with their corresponding video mirror
    std::unordered_map<int, int> audioMirrors;
    // List of all source audio tracks that don't have video mirror
    QList<int> singleAudioTracks;
    for (int i = 0; i < clips.count(); i++) {
        QDomElement prod = clips.at(i).toElement();
        int trackPos = prod.attribute(QStringLiteral("track")).toInt();
        bool audioTrack = prod.hasAttribute(QStringLiteral("audioTrack"));
        if (audioTrack) {
            if (!audioTracks.contains(trackPos)) {
                audioTracks << trackPos;
            }
            int videoMirror = prod.attribute(QStringLiteral("mirrorTrack")).toInt();
            if (videoMirror == -1) {
                if (singleAudioTracks.contains(trackPos)) {
                    continue;
                }
                singleAudioTracks << trackPos;
                continue;
            }
            audioMirrors[trackPos] = videoMirror;
            if (videoTracks.contains(videoMirror)) {
                continue;
            }
            videoTracks << videoMirror;
        } else {
            if (videoTracks.contains(trackPos)) {
                continue;
            }
            videoTracks << trackPos;
        }
    }
    for (int i = 0; i < compositions.count(); i++) {
        QDomElement prod = compositions.at(i).toElement();
        int trackPos = prod.attribute(QStringLiteral("track")).toInt();
        if (!videoTracks.contains(trackPos)) {
            videoTracks << trackPos;
        }
        int atrackPos = prod.attribute(QStringLiteral("a_track")).toInt();
        if (atrackPos == 0 || videoTracks.contains(atrackPos)) {
            continue;
        }
        videoTracks << atrackPos;
    }
    // Now we have a list of all source tracks, check that we have enough target tracks
    std::sort(videoTracks.begin(), videoTracks.end());
    std::sort(audioTracks.begin(), audioTracks.end());
    std::sort(singleAudioTracks.begin(), singleAudioTracks.end());
    //qDebug()<<"== GOT WANTED TKS\n VIDEO: "<<videoTracks<<"\n AUDIO TKS: "<<audioTracks<<"\n SINGLE AUDIO: "<<singleAudioTracks;
    int requestedVideoTracks = videoTracks.isEmpty() ? 0 : videoTracks.last() - videoTracks.first() + 1;
    int requestedAudioTracks = audioTracks.isEmpty() ? 0 : audioTracks.last() - audioTracks.first() + 1;
    if (requestedVideoTracks > projectTracks.second.size() || requestedAudioTracks > projectTracks.first.size()) {
        pCore->displayMessage(i18n("Not enough tracks to paste clipboard"), InformationMessage, 500);
        return false;
    }

    // Find destination master track
    // Check we have enough tracks above/below
    if (requestedVideoTracks > 0) {
        qDebug() << "MASTERSTK: " << masterSourceTrack << ", VTKS: " << videoTracks;
        int tracksBelow = masterSourceTrack - videoTracks.first();
        int tracksAbove = videoTracks.last() - masterSourceTrack;
        qDebug() << "// RQST TKS BELOW: " << tracksBelow << " / ABOVE: " << tracksAbove;
        qDebug() << "// EXISTING TKS BELOW: " << projectTracks.second.indexOf(trackId) << ", IX: " << trackId;
        qDebug() << "// EXISTING TKS ABOVE: " << projectTracks.second.size() << " - " << projectTracks.second.indexOf(trackId);
        if (projectTracks.second.indexOf(trackId) < tracksBelow) {
            qDebug() << "// UPDATING BELOW TID IX TO: " << tracksBelow;
            // not enough tracks below, try to paste on upper track
            trackId = projectTracks.second.at(tracksBelow);
        } else if ((projectTracks.second.size() - (projectTracks.second.indexOf(trackId) + 1)) < tracksAbove) {
            // not enough tracks above, try to paste on lower track
            qDebug() << "// UPDATING ABOVE TID IX TO: " << (projectTracks.second.size() - tracksAbove);
            trackId = projectTracks.second.at(projectTracks.second.size() - tracksAbove - 1);
        }
    } else {
        // Audio only
        masterSourceTrack = copiedItems.documentElement().attribute(QStringLiteral("masterAudioTrack")).toInt();
        int tracksBelow = masterSourceTrack - audioTracks.first();
        int tracksAbove = audioTracks.last() - masterSourceTrack;
        if (projectTracks.first.indexOf(trackId) < tracksBelow) {
            qDebug() << "// UPDATING BELOW TID IX TO: " << tracksBelow;
            // not enough tracks below, try to paste on upper track
            trackId = projectTracks.first.at(tracksBelow);
        } else if ((projectTracks.first.size() - (projectTracks.first.indexOf(trackId) + 1)) < tracksAbove) {
            // not enough tracks above, try to paste on lower track
            qDebug() << "// UPDATING ABOVE TID IX TO: " << (projectTracks.first.size() - tracksAbove);
            trackId = projectTracks.first.at(projectTracks.first.size() - tracksAbove - 1);
        }
    }
    QMap<int, int> tracksMap;
    bool audioMaster = false;
    int masterIx = projectTracks.second.indexOf(trackId);
    if (masterIx == -1) {
        masterIx = projectTracks.first.indexOf(trackId);
        audioMaster = true;
    }
    qDebug() << "/// PROJECT VIDEO TKS: " << projectTracks.second << ", MASTER: " << trackId;
    qDebug() << "/// PASTE VIDEO TKS: " << videoTracks << " / MASTER: " << masterSourceTrack;
    qDebug() << "/// MASTER PASTE: " << masterIx;
    for (int tk : videoTracks) {
        int newPos = masterIx + tk - masterSourceTrack;
        if (newPos < 0 || newPos >= projectTracks.second.size()) {
            pCore->displayMessage(i18n("Not enough tracks to paste clipboard"), InformationMessage, 500);
        }
        tracksMap.insert(tk, projectTracks.second.at(newPos));
    }
    bool audioOffsetCalculated = false;
    int audioOffset = 0;
    for (const auto &mirror : audioMirrors) {
        int videoIx = tracksMap.value(mirror.second);
        tracksMap.insert(mirror.first, timeline->getMirrorAudioTrackId(videoIx));
        if (!audioOffsetCalculated) {
            int oldPosition = mirror.first;
            int currentPosition = timeline->getTrackPosition(tracksMap.value(oldPosition));
            audioOffset = currentPosition - oldPosition;
            audioOffsetCalculated = true;
        }
    }
    if (!audioOffsetCalculated && audioMaster) {
        audioOffset = masterIx - masterSourceTrack;
        audioOffsetCalculated = true;
    }

    for (int i = 0; i < singleAudioTracks.size(); i++) {
        int oldPos = singleAudioTracks.at(i);
        if (tracksMap.contains(oldPos)) {
            continue;
        }
        int offsetId = oldPos + audioOffset;
        if (offsetId < 0 || offsetId >= projectTracks.first.size()) {
            pCore->displayMessage(i18n("Not enough tracks to paste clipboard"), InformationMessage, 500);
            return false;
        }
        tracksMap.insert(oldPos, projectTracks.first.at(offsetId));
    }
    if (!docId.isEmpty() && docId != pCore->currentDoc()->getDocumentProperty(QStringLiteral("documentid"))) {
        // paste from another document, import bin clips
        QString folderId = pCore->projectItemModel()->getFolderIdByName(i18n("Pasted clips"));
        if (folderId.isEmpty()) {
            // Folder doe not exist
            const QString rootId = pCore->projectItemModel()->getRootFolder()->clipId();
            folderId = QString::number(pCore->projectItemModel()->getFreeFolderId());
            pCore->projectItemModel()->requestAddFolder(folderId, i18n("Pasted clips"), rootId, undo, redo);
        }
        QDomNodeList binClips = copiedItems.documentElement().elementsByTagName(QStringLiteral("producer"));
        for (int i = 0; i < binClips.count(); ++i) {
            QDomElement currentProd = binClips.item(i).toElement();
            QString clipId = Xml::getXmlProperty(currentProd, QStringLiteral("kdenlive:id"));
            if (!pCore->projectItemModel()->isIdFree(clipId)) {
                QString updatedId = QString::number(pCore->projectItemModel()->getFreeClipId());
                Xml::setXmlProperty(currentProd, QStringLiteral("kdenlive:id"), updatedId);
                mappedIds.insert(clipId, updatedId);
                clipId = updatedId;
            }
            pCore->projectItemModel()->requestAddBinClip(clipId, currentProd, folderId, undo, redo);
        }
    }

    int offset = copiedItems.documentElement().attribute(QStringLiteral("offset")).toInt();

    bool res = true;
    QLocale locale;
    std::unordered_map<int, int> correspondingIds;
    QList<int> waitingIds;
    for (int i = 0; i < clips.count(); i++) {
        waitingIds << i;
    }
    for (int i = 0; res && !waitingIds.isEmpty();) {
        if (i >= waitingIds.size()) {
            i = 0;
        }
        QDomElement prod = clips.at(waitingIds.at(i)).toElement();
        QString originalId = prod.attribute(QStringLiteral("binid"));
        if (mappedIds.contains(originalId)) {
            // Map id
            originalId = mappedIds.value(originalId);
        }
        int in = prod.attribute(QStringLiteral("in")).toInt();
        int out = prod.attribute(QStringLiteral("out")).toInt();
        int curTrackId = tracksMap.value(prod.attribute(QStringLiteral("track")).toInt());
        int pos = prod.attribute(QStringLiteral("position")).toInt() - offset;
        double speed = locale.toDouble(prod.attribute(QStringLiteral("speed")));
        int newId;
        bool created = timeline->requestClipCreation(originalId, newId, timeline->getTrackById_const(curTrackId)->trackType(), speed, undo, redo);
        if (created) {
            // Master producer is ready
            // ids.removeAll(originalId);
            waitingIds.removeAt(i);
        } else {
            i++;
            qApp->processEvents();
            continue;
        }
        if (timeline->m_allClips[newId]->m_endlessResize) {
            out = out - in;
            in = 0;
            timeline->m_allClips[newId]->m_producer->set("length", out + 1);
        }
        timeline->m_allClips[newId]->setInOut(in, out);
        int targetId = prod.attribute(QStringLiteral("id")).toInt();
        correspondingIds[targetId] = newId;
        res = res && timeline->getTrackById(curTrackId)->requestClipInsertion(newId, position + pos, true, true, undo, redo);
        // paste effects
        if (res) {
            std::shared_ptr<EffectStackModel> destStack = timeline->getClipEffectStackModel(newId);
            destStack->fromXml(prod.firstChildElement(QStringLiteral("effects")), undo, redo);
        }
    }

    // Compositions
    for (int i = 0; res && i < compositions.count(); i++) {
        QDomElement prod = compositions.at(i).toElement();
        QString originalId = prod.attribute(QStringLiteral("composition"));
        int in = prod.attribute(QStringLiteral("in")).toInt();
        int out = prod.attribute(QStringLiteral("out")).toInt();
        int curTrackId = tracksMap.value(prod.attribute(QStringLiteral("track")).toInt());
        int aTrackId = prod.attribute(QStringLiteral("a_track")).toInt();
        if (aTrackId > 0) {
            aTrackId = timeline->getTrackPosition(tracksMap.value(aTrackId));
        }
        int pos = prod.attribute(QStringLiteral("position")).toInt() - offset;
        int newId;
        auto transProps = std::make_unique<Mlt::Properties>();
        QDomNodeList props = prod.elementsByTagName(QStringLiteral("property"));
        for (int j = 0; j < props.count(); j++) {
            transProps->set(props.at(j).toElement().attribute(QStringLiteral("name")).toUtf8().constData(),
                            props.at(j).toElement().text().toUtf8().constData());
        }
        res = timeline->requestCompositionInsertion(originalId, curTrackId, aTrackId, position + pos, out - in + 1, std::move(transProps), newId, undo, redo);
    }
    if (!res) {
        undo();
        return false;
    }
    // Rebuild groups
    const QString groupsData = copiedItems.documentElement().firstChildElement(QStringLiteral("groups")).text();
    if (!groupsData.isEmpty()) {
        timeline->m_groups->fromJsonWithOffset(groupsData, tracksMap, position - offset, undo, redo);
    }
    // unsure to clear selection in undo/redo too.
    Fun unselect = [&]() {
        qDebug() << "starting undo or redo. Selection " << timeline->m_currentSelection;
        timeline->requestClearSelection();
        qDebug() << "after Selection " << timeline->m_currentSelection;
        return true;
    };
    PUSH_FRONT_LAMBDA(unselect, undo);
    PUSH_FRONT_LAMBDA(unselect, redo);
    pCore->pushUndo(undo, redo, i18n("Paste clips"));
    return true;
}

bool TimelineFunctions::requestDeleteBlankAt(const std::shared_ptr<TimelineItemModel> &timeline, int trackId, int position, bool affectAllTracks)
{
    // find blank duration
    int spaceDuration = timeline->getTrackById_const(trackId)->getBlankSizeAtPos(position);
    int cid = requestSpacerStartOperation(timeline, affectAllTracks ? -1 : trackId, position);
    if (cid == -1) {
        return false;
    }
    int start = timeline->getItemPosition(cid);
    requestSpacerEndOperation(timeline, cid, start, start - spaceDuration);
    return true;
}
