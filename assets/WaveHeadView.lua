-- WaveHeadView.lua — waveform + playhead view for Dirac.
--
-- Wraps the platform's app.HeadDisplay (SampleView + head position marker),
-- driven by our Dirac object (an od::Head subclass). The head's
-- mCurrentIndex — which the DSP sets to the Playhead sample index every block —
-- is drawn as the moving marker, so the display literally shows where grains
-- are being pulled from. Modeled on core.Player.GrainView.

local app = app
local Class = require "Base.Class"
local Zoomable = require "Unit.ViewControl.Zoomable"
local Channels = require "Channels"
local Signal = require "Signal"
local libmgs = require "dirac.libdirac"
local ply = app.SECTION_PLY

local WaveHeadView = Class {}
WaveHeadView:include(Zoomable)

function WaveHeadView:init(args)
  Zoomable.init(self)
  self:setClassName("Dirac.WaveHeadView")
  local head = args.head or app.logError("%s.init: head is missing.", self)
  local width = args.width or (4 * ply)
  self.head = head

  local graphic = app.Graphic(0, 0, width, 64)
  -- Custom head display: waveform + playhead marker + a mini tick per active
  -- grain at its source position (height = envelope). Falls back gracefully to
  -- the base waveform when no sample is attached.
  self.mainDisplay = libmgs.DiracHeadDisplay(head, 0, 0, width, 64)
  graphic:addChild(self.mainDisplay)
  self:setMainCursorController(self.mainDisplay)
  self:setControlGraphic(graphic)

  for i = 1, (width // ply) do
    self:addSpotDescriptor{ center = (i - 0.5) * ply }
  end
  self.verticalDivider = width

  self.subGraphic = app.Graphic(0, 0, 128, 64)
  self.subDisplay = app.HeadSubDisplay(head)
  self.subGraphic:addChild(self.subDisplay)
  self.subButton1 = app.SubButton("", 1); self.subGraphic:addChild(self.subButton1)
  self.subButton2 = app.SubButton("", 2); self.subGraphic:addChild(self.subButton2)
  self.subButton3 = app.SubButton("", 3); self.subGraphic:addChild(self.subButton3)

  Signal.weakRegister("selectReleased", self)
end

function WaveHeadView:setSample(sample)
  if sample then self.subDisplay:setName(sample.name) end
end

function WaveHeadView:selectReleased(i, shifted)
  self.mainDisplay:setChannel(Channels.getSide(i) - 1)
  return true
end

function WaveHeadView:getFloatingMenuItems()
  local choices = Zoomable.getFloatingMenuItems(self)
  choices[#choices + 1] = "collapse"
  choices[#choices + 1] = "open editor"
  return choices
end

function WaveHeadView:onFloatingMenuSelection(choice)
  if choice == "open editor" then
    self:callUp("showSampleEditor")
    return true
  elseif choice == "collapse" then
    self:callUp("switchView", "expanded")
  else
    return Zoomable.onFloatingMenuSelection(self, choice)
  end
end

function WaveHeadView:subReleased(i, shifted)
  return true
end

return WaveHeadView
