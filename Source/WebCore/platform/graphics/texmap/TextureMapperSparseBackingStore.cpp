/*
 * Copyright (C) 2022 Sony Interactive Entertainment Inc.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "TextureMapperSparseBackingStore.h"
#include <wtf/TZoneMallocInlines.h>

#if USE(GRAPHICS_LAYER_WC)
#include "Color.h"
#include "FloatQuad.h"
#include "IntRect.h"
#include "TextureMapper.h"
#include <cmath>
#include <numbers>

#if PLATFORM(WIN)
#include "MediaTraceWin.h"
#endif

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TextureMapperSparseBackingStore);

// Lyra: placeholders and fade-in (see the header).
static constexpr Seconds placeholderShowDelay = 70_ms; // a tile late by a frame or two shows nothing
static constexpr Seconds placeholderAppear = 180_ms;
static constexpr Seconds placeholderPulsePeriod = 1400_ms;
static constexpr Seconds placeholderPulseLimit = 10_s; // then it stays still (nothing to animate)
static constexpr Seconds tileFadeDuration = 160_ms;
static constexpr Seconds pulseFrameInterval = 33_ms;
static constexpr Seconds fadeFrameInterval = 7_ms;
static Seconds s_animationFrameDelay;

static void requestAnimationFrame(Seconds delay)
{
    if (!s_animationFrameDelay || delay < s_animationFrameDelay)
        s_animationFrameDelay = std::max(delay, 1_ms);
}

Seconds TextureMapperSparseBackingStore::takeAnimationFrameDelay()
{
    return std::exchange(s_animationFrameDelay, 0_s);
}

static void drawPlaceholder(TextureMapper& textureMapper, const FloatRect& rect, const TransformationMatrix& transform, float opacity, MonotonicTime now)
{
    double phase = std::fmod(now.secondsSinceEpoch().seconds(), placeholderPulsePeriod.seconds()) / placeholderPulsePeriod.seconds();
    float alpha = (0.05f + 0.045f * (0.5f - 0.5f * static_cast<float>(std::cos(2 * std::numbers::pi * phase)))) * opacity;
    if (alpha <= 0.002f)
        return;
    textureMapper.drawSolidColor(rect, transform, Color(SRGBA<float> { 0.5f, 0.5f, 0.5f, alpha }), true);
}

void TextureMapperSparseBackingStore::setSize(const IntSize& size)
{
    if (m_size == size)
        return;
    m_size = size;
    m_tiles.clear();
    m_missingSince.clear();
    m_placeholderShown.clear();
    m_fadeStart.clear();
    m_cellSize = { };
}

// The web process tiles layers in a fixed grid (WCTileGrid, 512 CSS pixels scaled to the backing
// store). A tile's index times the cell size is its origin: learn the cell size from a tile that is
// not in the first row or column. Until then, the largest tile is a lower bound.
IntSize TextureMapperSparseBackingStore::cellSize()
{
    if (!m_cellSize.isEmpty())
        return m_cellSize;
    IntSize size;
    IntSize largest;
    for (auto& iterator : m_tiles) {
        auto rect = enclosingIntRect(iterator.value->rect());
        if (iterator.key.x() > 0 && !size.width())
            size.setWidth(rect.x() / iterator.key.x());
        if (iterator.key.y() > 0 && !size.height())
            size.setHeight(rect.y() / iterator.key.y());
        largest = largest.expandedTo(rect.size());
    }
    if (size.width() > 0 && size.height() > 0)
        return m_cellSize = size;
    // A layer one tile wide (or high) so far: that tile spans it. The largest tile can be a pixel
    // narrower than the layer (rounding of a scaled layer, e.g. a 32 px scrollbar with a 31 px
    // tile), and taking it as the cell made up a 1 px column that never gets a tile: its
    // placeholder pulsed for 10 s, drawn as a thin line, and repainted the scene 30 times a second.
    return { size.width() > 0 ? size.width() : std::max(largest.width(), m_size.width()),
             size.height() > 0 ? size.height() : std::max(largest.height(), m_size.height()) };
}

// Whether the tiles there already cover (nearly) all of `cell`.
static bool coveredByTiles(const HashMap<TextureMapperSparseBackingStore::TileIndex, std::unique_ptr<TextureMapperTile>>& tiles, const IntRect& cell)
{
    long long covered = 0;
    for (auto& iterator : tiles) {
        IntRect overlap = intersection(enclosingIntRect(iterator.value->rect()), cell);
        covered += static_cast<long long>(overlap.width()) * overlap.height();
    }
    return covered * 100 >= static_cast<long long>(cell.width()) * cell.height() * 98;
}

void TextureMapperSparseBackingStore::willAddTile(const TileIndex& index)
{
    if (m_tiles.contains(index))
        return;
    m_missingSince.remove(index);
    if (m_placeholderShown.remove(index))
        m_fadeStart.set(index, MonotonicTime::now());
}

void TextureMapperSparseBackingStore::paintPlaceholders(TextureMapper& textureMapper, const TransformationMatrix& transform, float opacity, MonotonicTime now)
{
    IntRect layerRect = { { }, m_size };
    auto inverse = transform.inverse();
    auto cell = cellSize();
    if (!inverse || layerRect.isEmpty() || cell.width() <= 0 || cell.height() <= 0) {
        m_missingSince.clear();
        return;
    }
    FloatRect visible = inverse->mapQuad(FloatQuad(FloatRect(textureMapper.clipBounds()))).boundingBox();
    visible.intersect(layerRect);
    if (visible.isEmpty()) {
        m_missingSince.clear();
        return;
    }
    int x0 = static_cast<int>(visible.x()) / cell.width();
    int y0 = static_cast<int>(visible.y()) / cell.height();
    int x1 = (static_cast<int>(std::ceil(visible.maxX())) + cell.width() - 1) / cell.width();
    int y1 = (static_cast<int>(std::ceil(visible.maxY())) + cell.height() - 1) / cell.height();
    if ((x1 - x0) * (y1 - y0) > 400)
        return; // a huge transformed layer; not worth it

    HashMap<TileIndex, MonotonicTime> missing;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            TileIndex index { x, y };
            if (m_tiles.contains(index))
                continue;
            IntRect cellRect = intersection(layerRect, { x * cell.width(), y * cell.height(), cell.width(), cell.height() });
            // (a sliver at the layer's edge, or a cell the neighbouring tiles already cover)
            if (cellRect.width() < 4 || cellRect.height() < 4 || coveredByTiles(m_tiles, cellRect))
                continue;
            auto iterator = m_missingSince.find(index);
            MonotonicTime since = iterator != m_missingSince.end() ? iterator->value : now;
            missing.add(index, since);
            Seconds missingFor = now - since;
            if (missingFor < placeholderShowDelay) {
                requestAnimationFrame(placeholderShowDelay - missingFor);
                continue;
            }
            m_placeholderShown.add(index);
            float appear = std::min(1.0, (missingFor - placeholderShowDelay) / placeholderAppear);
            drawPlaceholder(textureMapper, cellRect, transform, opacity * appear, now);
            if (missingFor < placeholderPulseLimit)
                requestAnimationFrame(appear < 1 ? fadeFrameInterval : pulseFrameInterval);
        }
    }
    m_missingSince = WTF::move(missing);
}

TransformationMatrix TextureMapperSparseBackingStore::adjustedTransformForRect(const FloatRect& targetRect)
{
    return TransformationMatrix::rectToRect({ { }, m_size }, targetRect);
}

// Lyra: a layer drawn 1:1 (no scaling or rotation, the usual case) lands on whole device pixels.
// At a fractional offset - scrolling at 125 % / 150 % display scale, position: fixed layers moved
// by the scrolling thread - every tile is resampled between pixels: the page goes slightly soft
// and the tiles' shared edges show as thin lines where two half-covered pixels are blended.
static TransformationMatrix snappedToDevicePixels(const TransformationMatrix& matrix)
{
    if (!matrix.isAffine())
        return matrix;
    constexpr double epsilon = 0.001;
    if (std::abs(matrix.a() - 1) > epsilon || std::abs(matrix.d() - 1) > epsilon || std::abs(matrix.b()) > epsilon || std::abs(matrix.c()) > epsilon)
        return matrix;
    TransformationMatrix snapped = matrix;
    snapped.setA(1);
    snapped.setD(1);
    snapped.setB(0);
    snapped.setC(0);
    snapped.setE(std::round(matrix.e()));
    snapped.setF(std::round(matrix.f()));
    return snapped;
}

void TextureMapperSparseBackingStore::paintToTextureMapper(TextureMapper& textureMapper, const FloatRect& targetRect, const TransformationMatrix& transform, float opacity)
{
    IntRect rect = { { }, m_size };
    TransformationMatrix adjustedTransform = snappedToDevicePixels(transform * adjustedTransformForRect(targetRect));
    auto now = MonotonicTime::now();
    for (auto& iterator : m_tiles) {
        float fade = 1;
        auto fadeIterator = m_fadeStart.find(iterator.key);
        if (fadeIterator != m_fadeStart.end()) {
            fade = std::clamp<float>((now - fadeIterator->value) / tileFadeDuration, 0, 1);
            if (fade >= 1)
                m_fadeStart.remove(fadeIterator);
            else {
                // Ease out; the placeholder fades out underneath.
                fade = 1 - (1 - fade) * (1 - fade);
                drawPlaceholder(textureMapper, iterator.value->rect(), adjustedTransform, opacity * (1 - fade), now);
                requestAnimationFrame(fadeFrameInterval);
            }
        }
        iterator.value->paint(textureMapper, adjustedTransform, opacity * fade, allTileEdgesExposed(rect, iterator.value->rect()));
    }
    paintPlaceholders(textureMapper, adjustedTransform, opacity, now);
}

void TextureMapperSparseBackingStore::drawBorder(TextureMapper& textureMapper, const Color& borderColor, float borderWidth, const FloatRect& targetRect, const TransformationMatrix& transform)
{
    TransformationMatrix adjustedTransform = transform * adjustedTransformForRect(targetRect);
    for (auto& iterator : m_tiles)
        textureMapper.drawBorder(borderColor, borderWidth, iterator.value->rect(), adjustedTransform);
}

void TextureMapperSparseBackingStore::drawRepaintCounter(TextureMapper& textureMapper, int repaintCount, const Color& borderColor, const FloatRect& targetRect, const TransformationMatrix& transform)
{
    TransformationMatrix adjustedTransform = transform * adjustedTransformForRect(targetRect);
    for (auto& iterator : m_tiles)
        textureMapper.drawNumber(repaintCount, borderColor, iterator.value->rect().location(), adjustedTransform);
}

// A tile is created with the rect of its first update, which the web process sends in full.
// If an update covers more than that (the tile was created from a partial repaint), start the
// tile over at the larger rect rather than writing outside its texture.
static TextureMapperTile& ensureTileForUpdate(HashMap<TextureMapperSparseBackingStore::TileIndex, std::unique_ptr<TextureMapperTile>>& tiles, const TextureMapperSparseBackingStore::TileIndex& index, const IntRect& dirtyRect)
{
    auto addResult = tiles.ensure(index, [&]() {
        return makeUnique<TextureMapperTile>(dirtyRect);
    });
    auto& tile = addResult.iterator->value;
    if (!addResult.isNewEntry) {
        IntRect tileRect = enclosingIntRect(tile->rect());
        if (!tileRect.contains(dirtyRect) && dirtyRect.contains(tileRect)) {
#if PLATFORM(WIN)
            MEDIA_TRACE("tile %d,%d %dx%d grown to %d,%d %dx%d", tileRect.x(), tileRect.y(), tileRect.width(), tileRect.height(),
                dirtyRect.x(), dirtyRect.y(), dirtyRect.width(), dirtyRect.height());
#endif
            tile = makeUnique<TextureMapperTile>(dirtyRect);
        } else if (!tileRect.contains(dirtyRect)) {
#if PLATFORM(WIN)
            MEDIA_TRACE("tile %d,%d %dx%d does not contain update %d,%d %dx%d", tileRect.x(), tileRect.y(), tileRect.width(), tileRect.height(),
                dirtyRect.x(), dirtyRect.y(), dirtyRect.width(), dirtyRect.height());
#endif
        }
    }
    return *tile;
}

void TextureMapperSparseBackingStore::updateContents(const TileIndex& index, NativeImage& image, const IntRect& dirtyRect)
{
    willAddTile(index);
    ensureTileForUpdate(m_tiles, index, dirtyRect).updateContents(image, dirtyRect);
}

void TextureMapperSparseBackingStore::updateContentsFromTexture(const TileIndex& index, unsigned textureID, const IntRect& dirtyRect)
{
    willAddTile(index);
    ensureTileForUpdate(m_tiles, index, dirtyRect).updateContentsFromTexture(textureID, dirtyRect);
}

void TextureMapperSparseBackingStore::removeTile(const TileIndex& index)
{
    m_tiles.remove(index);
    m_fadeStart.remove(index);
    m_placeholderShown.remove(index);
}

} // namespace WebCore

#endif // USE(GRAPHICS_LAYER_WC)
