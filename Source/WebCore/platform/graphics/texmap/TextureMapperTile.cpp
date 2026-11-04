/*
 Copyright (C) 2013 Nokia Corporation and/or its subsidiary(-ies)

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License as published by the Free Software Foundation; either
 version 2 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.

 You should have received a copy of the GNU Library General Public License
 along with this library; see the file COPYING.LIB.  If not, write to
 the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 Boston, MA 02110-1301, USA.
 */
#include "config.h"
#include "TextureMapperTile.h"

#if PLATFORM(WIN)
#include "MediaTraceWin.h"
#endif

#include "BitmapTexture.h"
#include "NativeImage.h"
#include "TextureMapper.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TextureMapperTile);

class GraphicsLayer;

TextureMapperTile::TextureMapperTile(const FloatRect& rect)
    : m_rect(rect)
{
}

TextureMapperTile::~TextureMapperTile() = default;

RefPtr<BitmapTexture> TextureMapperTile::texture() const
{
    return m_texture;
}

void TextureMapperTile::setTexture(BitmapTexture* texture)
{
    m_texture = texture;
}

void TextureMapperTile::updateContents(NativeImage& image, const IntRect& dirtyRect)
{
    IntRect targetRect = enclosingIntRect(m_rect);
    targetRect.intersect(dirtyRect);
    if (targetRect.isEmpty())
        return;
    IntPoint sourceOffset = targetRect.location();

    // Normalize sourceRect to the buffer's coordinates.
    sourceOffset.move(-dirtyRect.x(), -dirtyRect.y());

    // Normalize targetRect to the texture's coordinates.
    targetRect.move(-m_rect.x(), -m_rect.y());
    if (!m_texture) {
        OptionSet<BitmapTexture::Flags> flags;
        if (image.hasAlpha())
            flags.add(BitmapTexture::Flags::SupportsAlpha);
        m_texture = BitmapTexture::create(targetRect.size(), flags);
    }

    m_texture->updateContents(&image, targetRect, sourceOffset);
}

void TextureMapperTile::updateContentsFromTexture(unsigned textureID, const IntRect& dirtyRect)
{
    IntRect targetRect = enclosingIntRect(m_rect);
    targetRect.intersect(dirtyRect);
    if (targetRect.isEmpty())
        return;
    IntPoint sourceOffset = targetRect.location();
    sourceOffset.move(-dirtyRect.x(), -dirtyRect.y());
    targetRect.move(-m_rect.x(), -m_rect.y());
    if (!m_texture)
        m_texture = BitmapTexture::create(targetRect.size(), { BitmapTexture::Flags::SupportsAlpha });
    // copyFromExternalTexture() requires the copy to fit the texture on both sides.
    auto textureSize = m_texture->size();
    if (targetRect.maxX() > textureSize.width() || targetRect.maxY() > textureSize.height()
        || sourceOffset.x() + targetRect.width() > textureSize.width() || sourceOffset.y() + targetRect.height() > textureSize.height()) {
#if PLATFORM(WIN)
        MEDIA_TRACE("tile copy skipped: tile %d,%d %dx%d texture %dx%d dirty %d,%d %dx%d",
            static_cast<int>(m_rect.x()), static_cast<int>(m_rect.y()), static_cast<int>(m_rect.width()), static_cast<int>(m_rect.height()),
            textureSize.width(), textureSize.height(), dirtyRect.x(), dirtyRect.y(), dirtyRect.width(), dirtyRect.height());
#endif
        return;
    }
    m_texture->copyFromExternalTexture(textureID, targetRect, IntSize(sourceOffset.x(), sourceOffset.y()));
}

void TextureMapperTile::updateContents(GraphicsLayer* sourceLayer, const IntRect& dirtyRect, float scale)
{
    IntRect targetRect = enclosingIntRect(m_rect);
    targetRect.intersect(dirtyRect);
    if (targetRect.isEmpty())
        return;
    IntPoint sourceOffset = targetRect.location();

    // Normalize targetRect to the texture's coordinates.
    targetRect.move(-m_rect.x(), -m_rect.y());

    if (!m_texture)
        m_texture = BitmapTexture::create(targetRect.size(), { BitmapTexture::Flags::SupportsAlpha });

    m_texture->updateContents(sourceLayer, targetRect, sourceOffset, scale);
}

void TextureMapperTile::paint(TextureMapper& textureMapper, const TransformationMatrix& transform, float opacity, bool allEdgesExposed)
{
    if (texture().get())
        textureMapper.drawTexture(*texture().get(), rect(), transform, opacity, allEdgesExposed ? TextureMapper::AllEdgesExposed::Yes : TextureMapper::AllEdgesExposed::No);
}

} // namespace WebCore
