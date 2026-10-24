"""World Heritage Engine — faceplate layout, shared by the Blender scene and the app.

All coordinates are in layout pixels of the 1920x1080 window (origin top-left).
The art is rendered at 2x (3840x2160). The C++ app reads the generated
layout.json so controls land exactly on the rendered pockets.
"""

W, H = 1920, 1080
BEZEL = 34  # brushed-metal frame around the obsidian plate

# Rounded pockets routed into the plate: name -> (x, y, w, h, radius, depth_px)
POCKETS = {
    "status":    (44, 150, 1832, 74, 10, 10),
    "library":   (44, 238, 316, 560, 14, 12),
    "keymap":    (376, 324, 1168, 104, 10, 10),
    "hammer":    (376, 442, 280, 356, 14, 12),
    "strings":   (672, 442, 556, 356, 14, 12),
    "body":      (1244, 442, 300, 356, 14, 12),
    "blendpane": (1560, 238, 316, 560, 14, 12),
    "keyboard":  (44, 812, 1832, 236, 14, 14),
}

# Dark glass screens set into pockets: name -> (x, y, w, h, radius)
GLASS = {
    "status":  (52, 158, 1816, 58, 6),
    "keymap":  (384, 332, 1152, 88, 6),
    "meters":  (1578, 640, 280, 150, 8),
    "engine":  (376, 240, 1168, 74, 8),
}

# Knob wells (circular recesses) and knob sizes: name -> (cx, cy, knob_diameter, ring_radius)
def _row(pocket, names, cy):
    x, y, w, h, *_ = POCKETS[pocket]
    n = len(names)
    return {nm: (x + w * (i + 0.5) / n, cy, 84, 54) for i, nm in enumerate(names)}

KNOBS = {}
KNOBS.update(_row("hammer", ["hardness", "strike"], 622))
KNOBS.update(_row("strings", ["resonance", "detune", "stiffness", "decay"], 622))
KNOBS.update(_row("body", ["sympathetic", "release"], 622))
KNOBS["blend"] = (1718, 360, 120, 76)

# Round illuminated push button (physics on/bypass)
ROUND_BUTTONS = {"physics": (1718, 566, 76)}

# Velocity fader slot inside the keyboard pocket: (x, y, w, h)
FADER = (1092, 826, 300, 36)

SCREWS = [(BEZEL / 2, BEZEL / 2), (W - BEZEL / 2, BEZEL / 2), (BEZEL / 2, H - BEZEL / 2), (W - BEZEL / 2, H - BEZEL / 2)]

# Neon inlays: (x0, y0, x1, y1, width)
NEON = [(64, 138, 1856, 138, 3), (64, 805, 1856, 805, 2)]

# Logo badges: recess circles under the crown (left) and ghost (right)
BADGES = {"crown": (100, 84, 48), "ghost": (1820, 84, 48)}

# Keyboard key area inside the keyboard pocket
KEYS = (56, 876, 1808, 162)
