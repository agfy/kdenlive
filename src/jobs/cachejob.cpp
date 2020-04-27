/***************************************************************************
 *   Copyright (C) 2019 by Jean-Baptiste Mardelle                          *
 *   This file is part of Kdenlive. See www.kdenlive.org.                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3 or any later version accepted by the       *
 *   membership of KDE e.V. (or its successor approved  by the membership  *
 *   of KDE e.V.), which shall act as a proxy defined in Section 14 of     *
 *   version 3 of the license.                                             *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#include "cachejob.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "bin/projectsubclip.h"
#include "core.h"
#include "doc/kthumb.h"
#include "klocalizedstring.h"
#include "macros.hpp"
#include "utils/thumbnailcache.hpp"
#include <QImage>
#include <QScopedPointer>
#include <mlt++/MltProducer.h>

#include <set>

CacheJob::CacheJob(const QString &binId, int thumbsCount, int inPoint, int outPoint)
    : AbstractClipJob(CACHEJOB, binId)
    , m_imageHeight(pCore->thumbProfile()->height())
    , m_imageWidth(pCore->thumbProfile()->width())
    , m_fullWidth(m_imageHeight * pCore->getCurrentDar() + 0.5)
    , m_done(false)
    , m_thumbsCount(thumbsCount)
    , m_inPoint(inPoint)
    , m_outPoint(outPoint)

{
    if (m_fullWidth % 2 > 0) {
        m_fullWidth ++;
    }
    m_imageHeight += m_imageHeight % 2;
    auto item = pCore->projectItemModel()->getItemByBinId(binId);
    Q_ASSERT(item != nullptr && item->itemType() == AbstractProjectItem::ClipItem);
    connect(this, &CacheJob::jobCanceled, [&] () {
        m_done = true;
        m_clipId.clear();
    });
}

const QString CacheJob::getDescription() const
{
    return i18n("Extracting thumbs from clip %1", m_clipId);
}

bool CacheJob::startJob()
{
    // We reload here, because things may have changed since creation of this job
    if (m_done) {
        // Job aborted
        return false;
    }
    m_binClip = pCore->projectItemModel()->getClipByBinID(m_clipId);
    if (m_binClip->clipType() != ClipType::Video && m_binClip->clipType() != ClipType::AV && m_binClip->clipType() != ClipType::Playlist) {
        // Don't create thumbnail for audio clips
        m_done = true;
        return true;
    }
    m_prod = m_binClip->thumbProducer();
    if ((m_prod == nullptr) || !m_prod->is_valid()) {
        qDebug() << "********\nCOULD NOT READ THUMB PRODUCER\n********";
        return false;
    }
    int duration = m_outPoint > 0 ? m_outPoint - m_inPoint : (int)m_binClip->frameDuration();
    if (m_thumbsCount * 5 > duration) {
        m_thumbsCount = duration / 10;
    }
    std::set<int> frames;
    for (int i = 1; i <= m_thumbsCount; ++i) {
        frames.insert(m_inPoint + (duration * i / m_thumbsCount));
    }
    int size = (int)frames.size();
    int count = 0;
    for (int i : frames) {
        if (m_done) {
            break;
        }
        emit jobProgress(100 * count / size);
        count++;
        if (ThumbnailCache::get()->hasThumbnail(m_clipId, i)) {
            continue;
        }
        m_prod->seek(i);
        QScopedPointer<Mlt::Frame> frame(m_prod->get_frame());
        frame->set("deinterlace_method", "onefield");
        frame->set("top_field_first", -1);
        frame->set("rescale.interp", "nearest");
        if (frame != nullptr && frame->is_valid()) {
            QImage result = KThumb::getFrame(frame.data(), m_imageWidth, m_imageHeight, m_fullWidth);
            ThumbnailCache::get()->storeThumbnail(m_clipId, i, result, true);
        }
    }
    m_done = true;
    return true;
}

bool CacheJob::commitResult(Fun &undo, Fun &redo)
{
    Q_UNUSED(undo)
    Q_UNUSED(redo)
    Q_ASSERT(!m_resultConsumed);
    m_resultConsumed = true;
    return m_done;
}
