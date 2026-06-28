-- GrainFieldView — display-only ViewControl hosting the grain-field phosphor
-- scope. One glowing dot per active grain: X = stereo field (Spread/Detune),
-- Y = pitch (Chord/Semitone/Psprd/HoldDetune), brightness = envelope. Freezes
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

  -- Sub display: caption only (display has no editable parameters).
  self.subGraphic = app.Graphic(0, 0, 128, 64)
  local label = app.Label("grain field", 12)
  label:fitToText(3)
  label:setCenter(64, 32)
  self.subGraphic:addChild(label)
end

return GrainFieldView
