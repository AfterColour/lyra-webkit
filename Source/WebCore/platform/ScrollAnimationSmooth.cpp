/*
 * Copyright (C) 2016 Igalia S.L.
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
 * Copyright (c) 2011 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ScrollAnimationSmooth.h"

#include "FloatPoint.h"
#include "GeometryUtilities.h"
#include "KeyboardScroll.h"
#include "ScrollExtents.h"
#include "ScrollableArea.h"
#include "SpringSolver.h"
#include "TimingFunction.h"
#include "UnitBezier.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/TextStream.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ScrollAnimationSmooth);

#if PLATFORM(WIN)
// Lyra: mouse-wheel scrolling with inertia, close to Chromium's: every notch starts moving at once
// and glides out over ~0.25-0.45 s (a strong ease-out), instead of a 0.1 s ease-in-out step.
static const float animationSpeed { 650.0f };
static const Seconds minAnimationDuration { 200_ms };
static const Seconds maxAnimationDuration { 420_ms };
// An ease-out whose initial speed is `startSpeedRatio` times the average speed of the animation.
// The first notch starts steeply; later notches start at the speed the scroll already has, so a
// series of notches is one continuous glide instead of a burst per notch.
static Ref<CubicBezierTimingFunction> scrollTimingFunction(double startSpeedRatio = 7)
{
    constexpr double x1 = 0.1;
    double y1 = std::clamp(startSpeedRatio * x1, 0.05, 1.0);
    return CubicBezierTimingFunction::create(x1, y1, 0.2, 1.0);
}
#else
static const float animationSpeed { 1000.0f };
static const Seconds maxAnimationDuration { 200_ms };
#endif

// The end of the spring is imperceptible, so finish once the remaining distance is a fraction of a pixel.
static constexpr float springAnimationCompletionDistance { 0.25 };

ScrollAnimationSmooth::ScrollAnimationSmooth(ScrollAnimationClient& client)
    : ScrollAnimation(Type::Smooth, client)
    , m_timingFunction(CubicBezierTimingFunction::create())
{
}

ScrollAnimationSmooth::~ScrollAnimationSmooth() = default;

bool ScrollAnimationSmooth::startAnimatedScrollToDestination(const FloatPoint& fromOffset, const FloatPoint& destinationOffset, ScrollAnimationCurve curve)
{
    auto extents = m_client.scrollExtentsForAnimation(*this);

    m_curve = curve;
    m_currentOffset = m_startOffset = fromOffset;
    m_destinationOffset = destinationOffset.constrainedBetween(extents.minimumScrollOffset(), extents.maximumScrollOffset());

    if (!isActive() && fromOffset == m_destinationOffset)
        return false;

    m_duration = durationFromDistance(m_destinationOffset - m_startOffset);
    if (!m_duration)
        return false;

#if PLATFORM(WIN)
    m_timingFunction = scrollTimingFunction();
#else
    downcast<CubicBezierTimingFunction>(*m_timingFunction).setTimingFunctionPreset(CubicBezierTimingFunction::TimingFunctionPreset::EaseInOut);
#endif

    if (!isActive())
        didStart(MonotonicTime::now());

    return true;
}

bool ScrollAnimationSmooth::retargetActiveAnimation(const FloatPoint& newOffset)
{
    return retargetActiveAnimation(newOffset, m_curve);
}

bool ScrollAnimationSmooth::retargetActiveAnimation(const FloatPoint& newOffset, ScrollAnimationCurve curve)
{
    if (!isActive())
        return false;

    auto extents = m_client.scrollExtentsForAnimation(*this);
#if PLATFORM(WIN)
    // Current speed along the scroll (px/s), from the running curve, to continue from it.
    double currentSpeed = 0;
    if (m_duration > 0_s) {
        auto now = MonotonicTime::now();
        double fraction = std::clamp((now - m_startTime) / m_duration, 0.0, 1.0);
        constexpr double step = 0.01;
        Ref timing = *m_timingFunction;
        double progressNow = timing->transformProgress(fraction, m_duration.value());
        double progressNext = timing->transformProgress(std::min(1.0, fraction + step), m_duration.value());
        double distance = euclidianDistance(m_destinationOffset - m_startOffset);
        currentSpeed = (progressNext - progressNow) / step * distance / m_duration.value();
    }
#endif

    m_curve = curve;
    m_startTime = MonotonicTime::now();
    m_startOffset = m_currentOffset;
    m_destinationOffset = newOffset.constrainedBetween(extents.minimumScrollOffset(), extents.maximumScrollOffset());
    m_duration = durationFromDistance(m_destinationOffset - m_startOffset);
#if PLATFORM(WIN)
    {
        double distance = euclidianDistance(m_destinationOffset - m_startOffset);
        double averageSpeed = m_duration > 0_s ? distance / m_duration.value() : 0;
        // Never slower than the scroll already is, and a little push for the new notch.
        double ratio = averageSpeed > 0 ? std::max(currentSpeed / averageSpeed, 2.0) : 7.0;
        m_timingFunction = scrollTimingFunction(ratio);
    }
#else
    downcast<CubicBezierTimingFunction>(*m_timingFunction).setTimingFunctionPreset(CubicBezierTimingFunction::TimingFunctionPreset::EaseOut);
    m_timingFunction = CubicBezierTimingFunction::create(CubicBezierTimingFunction::TimingFunctionPreset::EaseOut);
#endif

    if (m_currentOffset == m_destinationOffset || !m_duration)
        return false;

    return true;
}

void ScrollAnimationSmooth::updateScrollExtents()
{
    auto extents = m_client.scrollExtentsForAnimation(*this);
    // FIXME: Ideally fix up m_startOffset so m_currentOffset doesn't go backwards.
    m_destinationOffset = m_destinationOffset.constrainedBetween(extents.minimumScrollOffset(), extents.maximumScrollOffset());
}

Seconds ScrollAnimationSmooth::durationFromDistance(const FloatSize& delta) const
{
    float distance = euclidianDistance(delta);
    if (m_curve == ScrollAnimationCurve::Spring)
        return distance ? DocumentEdgeScrollSpring::duration : 0_s;
#if PLATFORM(WIN)
    if (!distance)
        return 0_s;
    return std::clamp(Seconds(distance / animationSpeed), minAnimationDuration, maxAnimationDuration);
#else
    return std::min(Seconds(distance / animationSpeed), maxAnimationDuration);
#endif
}

double ScrollAnimationSmooth::springProgress(Seconds elapsedTime)
{
    // Computed the same way SpringSolver computes the damping ratio, so that the ratio is exactly 1.
    double damping = 2 * std::sqrt(DocumentEdgeScrollSpring::stiffness * DocumentEdgeScrollSpring::mass);

    // Like a CAMediaTimingFunction, the bezier warps time before the spring is evaluated. It removes the
    // initial ease-in of the spring, so that the curve resembles exponential decay.
    auto& controlPoints = DocumentEdgeScrollSpring::timingFunctionControlPoints;
    double fractionComplete = std::clamp(elapsedTime / DocumentEdgeScrollSpring::duration, 0.0, 1.0);
    double warpedTime = UnitBezier(controlPoints[0], controlPoints[1], controlPoints[2], controlPoints[3]).solve(fractionComplete, UnitBezier::kBezierEpsilon) * DocumentEdgeScrollSpring::duration.value();

    return SpringSolver(DocumentEdgeScrollSpring::mass, DocumentEdgeScrollSpring::stiffness, damping, 0).solve(warpedTime);
}

void ScrollAnimationSmooth::serviceAnimation(MonotonicTime currentTime)
{
    bool animationActive = animateScroll(currentTime);
    m_client.scrollAnimationDidUpdate(*this, m_currentOffset);
    if (!animationActive)
        didEnd();
}

bool ScrollAnimationSmooth::animateScroll(MonotonicTime currentTime)
{
    MonotonicTime endTime = m_startTime + m_duration;
    currentTime = std::min(currentTime, endTime);

    float progress;
    if (m_curve == ScrollAnimationCurve::Spring)
        progress = springProgress(currentTime - m_startTime);
    else {
        double fractionComplete = (currentTime - m_startTime) / m_duration;
        progress = protect(m_timingFunction)->transformProgress(fractionComplete, m_duration.value());
    }

    m_currentOffset = {
        std::lerp(m_startOffset.x(), m_destinationOffset.x(), progress),
        std::lerp(m_startOffset.y(), m_destinationOffset.y(), progress),
    };

    if (m_curve == ScrollAnimationCurve::Spring && (currentTime == endTime || euclidianDistance(m_destinationOffset - m_currentOffset) < springAnimationCompletionDistance)) {
        m_currentOffset = m_destinationOffset;
        return false;
    }

    return currentTime < endTime;
}

String ScrollAnimationSmooth::debugDescription() const
{
    TextStream textStream;
    textStream << "ScrollAnimationSmooth " << this << " active " << isActive() << " curve " << m_curve << " from " << m_startOffset << " to " << m_destinationOffset << " current offset " << currentOffset();
    return textStream.release();
}

} // namespace WebCore
