//
//  Colors.h
//  NeuralRig
//
//  Originally from NeuralAmpModelerPlugin, Copyright (c) 2022 Steven Atkinson.
//  Repalette for NeuralRig.
//
// One place for the plugin's colours.

#ifndef Colors_h
#define Colors_h

#include "IGraphicsStructs.h"

/**
    NeuralRig's palette: warm amber on near-black.

    Amber rather than blue because the thing being modelled glows. Valve light
    is around 2000K, so the accent sits in the orange end and every highlight
    in the UI is a tint of it -- knob arcs, meter caps, section rules, the
    signal-flow connectors. Nothing is a second accent colour competing with it.

    Greys are warm rather than neutral (a few points more red than blue). Next
    to an amber accent, a truly neutral grey reads as faintly blue and fights
    it; warming them slightly makes the whole panel feel lit rather than tinted.

    The names NAM_1..NAM_3 are kept because upstream's controls reference them.
*/
namespace PluginColors
{
// HINT: ARGB

// --- Surfaces ---------------------------------------------------------------
// Three steps, dark to light, so panels can sit above the background and
// controls above panels without any borders being needed to separate them.
const iplug::igraphics::IColor CHASSIS(255, 10, 10, 12); // deepest, behind everything
const iplug::igraphics::IColor PANEL(255, 23, 24, 28); // raised section panels
const iplug::igraphics::IColor PANEL_HI(255, 42, 44, 51); // 1px top edge: catches the light
const iplug::igraphics::IColor WELL(255, 14, 14, 17); // recessed, for file rows and meters

// --- Accent -----------------------------------------------------------------
const iplug::igraphics::IColor AMBER(255, 255, 163, 64); // valve glow
const iplug::igraphics::IColor AMBER_DIM(255, 138, 88, 36); // unlit state of the same
const iplug::igraphics::IColor AMBER_GLOW(90, 255, 163, 64); // halo, used at low opacity

// --- Type -------------------------------------------------------------------
const iplug::igraphics::IColor INK(255, 232, 228, 220); // warm off-white
const iplug::igraphics::IColor INK_MUTED(255, 138, 133, 125); // captions, section labels
const iplug::igraphics::IColor INK_DIM(255, 92, 89, 84); // placeholders, empty slots

// --- Meters -----------------------------------------------------------------
const iplug::igraphics::IColor METER_OK(255, 118, 184, 108);
const iplug::igraphics::IColor METER_HOT(255, 255, 163, 64);
const iplug::igraphics::IColor METER_CLIP(255, 224, 76, 62);

// --- Names upstream's controls expect --------------------------------------
const iplug::igraphics::IColor OFF_WHITE = INK;
const iplug::igraphics::IColor NAM_0(0, 10, 10, 12); // transparent
const iplug::igraphics::IColor NAM_1 = CHASSIS;
const iplug::igraphics::IColor NAM_2 = AMBER;
const iplug::igraphics::IColor NAM_3 = INK_MUTED;
const iplug::igraphics::IColor NAM_THEMECOLOR = AMBER;
const iplug::igraphics::IColor NAM_THEMEFONTCOLOR = INK;

const iplug::igraphics::IColor MOUSEOVER = AMBER.WithOpacity(0.14f);
const iplug::igraphics::IColor HELP_TEXT = INK_MUTED;
const iplug::igraphics::IColor HELP_TEXT_MO = INK;
const iplug::igraphics::IColor HELP_TEXT_CLICKED = INK.WithOpacity(0.8f);

}; // namespace PluginColors

#endif /* Colors_h */
