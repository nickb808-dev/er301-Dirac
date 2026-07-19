-- GrainFieldView — display-only ViewControl hosting the grain-field phosphor
-- scope. One glowing dot per active grain: Y = pitch (Semitone/V-Oct/Psprd),
-- brightness = envelope. Freezes on Hold. Subclasses the base ViewControl
-- (no Zoomable — nothing to zoom).
--
-- SHIFT-TAP toggles the X axis (v0.2.0):
--   STEREO   — X = pan position (Spread/Detune). The original field.
--   POSITION — X = where each grain reads in the buffer/sample. This is how you
--              SEE Snap and multi-playhead: the cloud collects onto transients as
--              Snap comes up, and splits into columns as HeadSpread opens.
-- The sub display swaps between the Paul Dirac epigraph and an axis legend.
--
-- Why shift is free here: this control has no gain/bias readouts, so it doesn't
-- collide with the firmware's shift+sub (keyboard entry) or shift+encoder (fine
-- adjust) bindings the way a GainBias control would. We still distinguish a shift
-- TAP from shift-as-a-modifier (the AnamSubControl pattern) so holding shift for
-- something else never flips the axis.

local app = app
local Class = require "Base.Class"
local ViewControl = require "Unit.ViewControl"
local libmgs = require "dirac.libdirac"
local ply = app.SECTION_PLY

local GrainFieldView = Class {}
GrainFieldView:include(ViewControl)

-- Build a centered multi-line text panel for the sub display.
-- Each line is { text, yCenter, [fontSize] }; valid sizes are 8/10/12/16/48.
local function textPanel(lines)
  local g = app.Graphic(0, 0, 128, 64)
  for _, ln in ipairs(lines) do
    local label = app.Label(ln[1], ln[3] or 10)
    label:fitToText(0)
    label:setCenter(64, ln[2])
    g:addChild(label)
  end
  return g
end

function GrainFieldView:init(args)
  ViewControl.init(self, args.name or "field")
  self:setClassName("dirac.GrainFieldView")
  local head = args.head or app.logError("%s.init: head is missing.", self)
  local width = args.width or ply   -- one control-width slot, like Mirror's phosphor

  self.field = libmgs.DiracFieldGraphic(0, 0, width, 64)
  self.field:follow(head)

  local graphic = app.Graphic(0, 0, width, 64)
  graphic:addChild(self.field)
  self:setControlGraphic(graphic)
  self:setMainCursorController(self.field)

  for i = 1, (width // ply) do
    self:addSpotDescriptor{ center = (i - 0.5) * ply }
  end

  -- Sub display A: Paul Dirac epigraph (default, STEREO mode).
  self.epigraphGraphic = textPanel {
    { '"Pick a flower on Earth', 51 },   -- y: top (GRID5_LINE1) descending
    { 'and you move the',        38 },
    { 'farthest star"',          25 },
    { '- Paul Dirac',            12 },
  }

  -- Sub display B: axis legend for POSITION mode.
  self.positionGraphic = textPanel {
    { 'X = read position',   51 },
    { 'Y = pitch',           38 },
    { 'old  <---->  now',    25 },
    { 'shift: stereo view',  12, 8 },
  }

  self.axisMode  = 0
  self.shiftHeld = false
  self.shiftUsed = false
  self.subGraphic = self.epigraphGraphic
end

function GrainFieldView:setAxisMode(mode)
  if mode == self.axisMode then return end
  self.axisMode = mode
  self.field:setAxisMode(mode)
  self:removeSubGraphic(self.subGraphic)
  self.subGraphic = (mode == 1) and self.positionGraphic or self.epigraphGraphic
  self:addSubGraphic(self.subGraphic)
end

function GrainFieldView:onCursorEnter(spot)
  ViewControl.onCursorEnter(self, spot)
  self:grabFocus("shiftPressed", "shiftReleased")
end

function GrainFieldView:onCursorLeave(spot)
  self:releaseFocus("shiftPressed", "shiftReleased")
  self.shiftHeld = false
  ViewControl.onCursorLeave(self, spot)
end

function GrainFieldView:shiftPressed()
  self.shiftHeld = true
  self.shiftUsed = false      -- set if shift gets used as a modifier instead
  return true
end

function GrainFieldView:shiftReleased()
  if self.shiftHeld and not self.shiftUsed then
    self:setAxisMode(self.axisMode == 1 and 0 or 1)
  end
  self.shiftHeld = false
  return true
end

-- If shift gets used as a MODIFIER (shift+dial, shift+spot) it wasn't a tap, so
-- releasing it must not flip the axis. These two are the only shift-taking methods
-- the base ViewControl actually defines — it has no `encoder` or `subReleased`,
-- so overriding those would be a nil call.
function GrainFieldView:dialPressed(shifted)
  if shifted and self.shiftHeld then self.shiftUsed = true end
  return ViewControl.dialPressed(self, shifted)
end

function GrainFieldView:spotReleased(spot, shifted)
  if shifted and self.shiftHeld then self.shiftUsed = true end
  return ViewControl.spotReleased(self, spot, shifted)
end

return GrainFieldView
