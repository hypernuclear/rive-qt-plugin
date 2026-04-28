#ifndef RIVE_RENDER_BACKEND_HELPERS_H
#define RIVE_RENDER_BACKEND_HELPERS_H

// Shared inline mappings from the public RiveRenderBackend enum surface
// (FitMode / AlignmentMode) to rive::Fit / rive::Alignment. Backend
// .cpp/.mm files include this so the mapping lives in one place — adding
// a new fit or alignment only needs editing here + the public enum in
// rive_render_backend.h.
//
// PRIVATE — included only from backend implementations. Not part of the
// plugin's public surface; consumers don't need to compile against
// rive/layout.hpp.

#include "rive_render_backend.h"

#include <rive/layout.hpp>

namespace rive_qt {

inline rive::Fit toRiveFit(RiveRenderBackend::FitMode f)
{
    switch (f)
    {
    case RiveRenderBackend::FitMode::Fill:      return rive::Fit::fill;
    case RiveRenderBackend::FitMode::Contain:   return rive::Fit::contain;
    case RiveRenderBackend::FitMode::Cover:     return rive::Fit::cover;
    case RiveRenderBackend::FitMode::FitWidth:  return rive::Fit::fitWidth;
    case RiveRenderBackend::FitMode::FitHeight: return rive::Fit::fitHeight;
    case RiveRenderBackend::FitMode::None:      return rive::Fit::none;
    case RiveRenderBackend::FitMode::ScaleDown: return rive::Fit::scaleDown;
    case RiveRenderBackend::FitMode::Layout:    return rive::Fit::layout;
    }
    return rive::Fit::contain;
}

inline rive::Alignment toRiveAlignment(RiveRenderBackend::AlignmentMode a)
{
    switch (a)
    {
    case RiveRenderBackend::AlignmentMode::TopLeft:      return rive::Alignment::topLeft;
    case RiveRenderBackend::AlignmentMode::TopCenter:    return rive::Alignment::topCenter;
    case RiveRenderBackend::AlignmentMode::TopRight:     return rive::Alignment::topRight;
    case RiveRenderBackend::AlignmentMode::CenterLeft:   return rive::Alignment::centerLeft;
    case RiveRenderBackend::AlignmentMode::Center:       return rive::Alignment::center;
    case RiveRenderBackend::AlignmentMode::CenterRight:  return rive::Alignment::centerRight;
    case RiveRenderBackend::AlignmentMode::BottomLeft:   return rive::Alignment::bottomLeft;
    case RiveRenderBackend::AlignmentMode::BottomCenter: return rive::Alignment::bottomCenter;
    case RiveRenderBackend::AlignmentMode::BottomRight:  return rive::Alignment::bottomRight;
    }
    return rive::Alignment::center;
}

} // namespace rive_qt

#endif // RIVE_RENDER_BACKEND_HELPERS_H
