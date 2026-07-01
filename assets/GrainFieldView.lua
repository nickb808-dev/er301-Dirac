-- GrainFieldView — display-only ViewControl hosting the grain-field phosphor
-- scope. One glowing dot per active grain: X = stereo field (Spread/Detune),
-- Y = pitch (Semitone/V-Oct/Psprd), brightness = envelope. Freezes
-- on Hold. Subclasses the base ViewControl (no Zoomable — nothing to zoom).

local app = app
local Class = require "Base.Class"
local ViewControl = require "Unit.ViewControl"
local libmgs = require "dirac.libdirac"
local ply = app.SECTION_PLY

local GrainFieldView = Class {}
GrainFieldView:include(ViewControl)

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

  -- Sub display: Paul Dirac epigraph (display has no editable parameters).
  self.subGraphic = app.Graphic(0, 0, 128, 64)
  local lines = {
    { '"Pick a flower on Earth', 51 },   -- y: top (GRID5_LINE1) descending
    { 'and you move the',        38 },
    { 'farthest star"',          25 },
    { '- Paul Dirac',            12 },
  }
  for _, ln in ipairs(lines) do
    local label = app.Label(ln[1], 10)
    label:fitToText(0)
    label:setCenter(64, ln[2])
    self.subGraphic:addChild(label)
  end
end

return GrainFieldView
