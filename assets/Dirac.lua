-- Dirac.lua — ER-301 unit wrapper for Dirac.
--
-- Sample-based sibling of MorphGrain: attach a sample from the pool/card, and
-- the morph grain engine granulates it at a visible PLAYHEAD. Spray/jitter and
-- scan drift are all relative to the playhead, and the waveform view draws the
-- playhead marker where the grains are pulled from.

local app = app
local Class = require "Base.Class"
local Unit = require "Unit"
local Encoder = require "Encoder"
local GainBias = require "Unit.ViewControl.GainBias"
local Gate = require "Unit.ViewControl.Gate"
local Pitch = require "Unit.ViewControl.Pitch"
local SamplePool = require "Sample.Pool"
local SamplePoolInterface = require "Sample.Pool.Interface"
local SampleEditor = require "Sample.Editor"
local Task = require "Unit.MenuControl.Task"
local MenuHeader = require "Unit.MenuControl.Header"
local OptionControl = require "Unit.MenuControl.OptionControl"
local WaveHeadView = require "dirac.WaveHeadView"
local GrainFieldView = require "dirac.GrainFieldView"

local libmgs = require "dirac.libdirac"

local Dirac = Class {}
Dirac:include(Unit)

function Dirac:init(args)
  args.title = "Dirac"
  args.mnemonic = "Di"
  Unit.init(self, args)
end

-- GainBias param + range + mono mod branch, connected to a head inlet.
local function param(self, head, name, port, bias)
  local p = self:addObject(name .. "Param", app.GainBias())
  local r = self:addObject(name .. "Range", app.MinMax())
  p:hardSet("Bias", bias)
  connect(p, "Out", r, "In")
  connect(p, "Out", head, port)
  self:addMonoBranch(name, p, "In", p, "Out")
end

function Dirac:onLoadGraph(channelCount)
  local head = self:addObject("head", libmgs.Dirac())

  -- Chain audio → live capture input. Granulated when NO sample is attached;
  -- once a sample is selected from the card, the engine reads the sample instead.
  connect(self, "In1", head, "In")

  -- Fire gate (optional external trigger) — one grain per rising edge
  local fire = self:addObject("fire", app.Comparator())
  connect(fire, "Out", head, "Fire")
  self:addMonoBranch("fire", fire, "In", fire, "Out")

  -- Hold: latched toggle (a held version of trig). Push on = grains loop as a
  -- frozen cloud; push off = they release. Binary in the DSP (no in-between).
  local holdGate = self:addObject("holdGate", app.Comparator())
  holdGate:setToggleMode()
  connect(holdGate, "Out", head, "Hold")
  self:addMonoBranch("hold", holdGate, "In", holdGate, "Out")

  param(self, head, "playhead",  "Playhead",  0.0)
  param(self, head, "rate",      "Rate",      3.0)   -- now DENSITY (grain overlap)
  param(self, head, "posjtr",    "PosJtr",    0.0)
  param(self, head, "spread",    "Spread",    0.5)
  param(self, head, "detune",    "Detune",    0.0)
  param(self, head, "level",     "Level",     0.7)
  param(self, head, "revprob",   "RevProb",   0.0)
  param(self, head, "psprd",     "Psprd",     0.0)
  param(self, head, "texture",   "Texture",   0.5)   -- 0=perc, 0.5=Hann, 1=Tukey
  param(self, head, "grains",    "Grains",    12.0)
  param(self, head, "semishift", "SemiShift", 0.0)

  -- V/Oct via a Pitch control (ConstantOffset), same value convention as the
  -- Variable Speed Player. Feeds the head's V/Oct inlet; the DSP scales it by
  -- 12 st/oct (1 V = 1 octave), summed with SemiShift.
  local tune      = self:addObject("tune",      app.ConstantOffset())
  local tuneRange = self:addObject("tuneRange", app.MinMax())
  connect(tune, "Out", tuneRange, "In")
  connect(tune, "Out", head, "V/Oct")
  self:addMonoBranch("voct", tune, "In", tune, "Out")
  param(self, head, "grainlen",  "GrainLen",  0.050)   -- seconds (50 ms)
  param(self, head, "speed",     "Speed",     0.0)     -- playhead scan rate; 0 = parked (sample mode)
  param(self, head, "mix",       "Mix",       1.0)     -- wet/dry (live mode only); 1 = all wet
  param(self, head, "feedback",  "Feedback",  0.0)     -- output→capture reinjection (live mode)
  param(self, head, "compress",  "Compress",  0.0)     -- per-grain leveling toward a target
  param(self, head, "binaural",  "Binaural",  0.0)     -- 3D depth: ITD + head-shadow
  param(self, head, "scale",     "Scale",     0.0)     -- quantize Psprd to a scale (0=off)

  connect(head, "OutL", self, "Out1")
  if channelCount > 1 then connect(head, "OutR", self, "Out2") end
end

-- ── Sample pool management (ManualGrains template) ──────────────────────────

function Dirac:serialize()
  local t = Unit.serialize(self)
  if self.sample then t.sample = SamplePool.serializeSample(self.sample) end
  return t
end

function Dirac:deserialize(t)
  Unit.deserialize(self, t)
  if t.sample then
    local sample = SamplePool.deserializeSample(t.sample, self.chain)
    if sample then self:setSample(sample)
    else app.logError("%s:deserialize: failed to load sample.", self) end
  end
end

-- The waveform display stays closed until a sample is attached. Place/remove
-- the "wave" control in the expanded strip on sample changes. Guarded by the
-- actual view state so it survives serialize/reload without duplicating.
local function waveInView(self)
  local view = self:getView("expanded")
  if not view then return nil, nil end
  for i, c in ipairs(view.controls) do
    if c == self.controls.wave then return view, i end
  end
  return view, nil
end

function Dirac:showWaveform(show)
  if not (self.controls and self.controls.wave) then return end
  local view, idx = waveInView(self)
  if show and view and idx == nil then
    self:placeControl("wave", "expanded", 1)   -- front of the strip
  elseif (not show) and view and idx ~= nil then
    table.remove(view.controls, idx)
    if self.currentViewName == "expanded" then
      require("Application").postTrigger(self, "rebuildView")
    end
  end
end

function Dirac:setSample(sample)
  if self.sample then self.sample:release(self); self.sample = nil end
  if sample == nil or sample:getChannelCount() == 0 then
    self.objects.head:setSample(nil)
  else
    self.objects.head:setSample(sample.pSample)
    self.sample = sample
    self.sample:claim(self)
  end
  if self.sampleEditor then self.sampleEditor:setSample(sample) end
  self:showWaveform(self.sample ~= nil)
  self:notifyControls("setSample", sample)
end

function Dirac:doDetachSample()
  local Overlay = require "Overlay"
  Overlay.flashMainMessage("Sample detached.")
  self:setSample()
end

function Dirac:doAttachSampleFromCard()
  local Pool = require "Sample.Pool"
  Pool.chooseFileFromCard(self.loadInfo.id, function(sample)
    if sample then
      require("Overlay").flashMainMessage("Attached sample: %s", sample.name)
      self:setSample(sample)
    end
  end)
end

function Dirac:doAttachSampleFromPool()
  local chooser = SamplePoolInterface(self.loadInfo.id, "choose")
  chooser:setDefaultChannelCount(self.channelCount)
  chooser:highlight(self.sample)
  chooser:subscribe("done", function(sample)
    if sample then
      require("Overlay").flashMainMessage("Attached sample: %s", sample.name)
      self:setSample(sample)
    end
  end)
  chooser:show()
end

function Dirac:showSampleEditor()
  if self.sample then
    if self.sampleEditor == nil then
      self.sampleEditor = SampleEditor(self, self.objects.head)
      self.sampleEditor:setSample(self.sample)
      self.sampleEditor:setPointerLabel("P")
    end
    self.sampleEditor:show()
  else
    require("Overlay").flashMainMessage("You must first select a sample.")
  end
end

local menu = { "sampleHeader", "selectFromCard", "selectFromPool", "detachBuffer", "editSample" }

function Dirac:onShowMenu(objects, branches)
  local controls = {}
  controls.sampleHeader = MenuHeader { description = "Sample Menu" }
  controls.selectFromCard = Task { description = "Select from Card",
    task = function() self:doAttachSampleFromCard() end }
  controls.selectFromPool = Task { description = "Select from Pool",
    task = function() self:doAttachSampleFromPool() end }
  controls.detachBuffer = Task { description = "Detach Buffer",
    task = function() self:doDetachSample() end }
  controls.editSample = Task { description = "Edit Buffer",
    task = function() self:showSampleEditor() end }

  local sub = {}
  if self.sample then
    sub[1] = { position = app.GRID5_LINE1, justify = app.justifyLeft, text = "Attached Sample:" }
    sub[2] = { position = app.GRID5_LINE2, justify = app.justifyLeft,
               text = "+ " .. self.sample:getFilenameForDisplay(24) }
    sub[3] = { position = app.GRID5_LINE3, justify = app.justifyLeft,
               text = "+ " .. self.sample:getDurationText() }
  else
    sub[1] = { position = app.GRID5_LINE3, justify = app.justifyCenter, text = "No sample attached." }
  end
  return controls, menu, sub
end

function Dirac:onRemove()
  self:setSample(nil)
  Unit.onRemove(self)
end

-- ── Views ───────────────────────────────────────────────────────────────────

-- Control button order. Leads with the Manual-Grains-familiar controls so the
-- muscle memory carries over (this is a pure spectral sample granulator, so the
-- mapping is by role):
--   trig      ≈ Manual Grains "trigger"
--   semishift ≈ "V/oct"     (semitone transpose, V/oct-patchable)
--   playhead  ≈ "start"     (read position in the sample)
--   grainlen  ≈ "duration"
-- then the granulation controls, with level last.
local controlOrder = {
  "trig", "hold", "semishift", "voct", "playhead", "speed", "grainlen", "grains", "rate",
  "posjtr", "spread", "detune",
  "psprd", "scale", "texture", "compress", "revprob", "level", "mix", "feedback", "binaural",
}

-- "wave" LEADS the expanded strip so the waveform is the default main display
-- (this is what showed it in v0.1.2 — removing it in v0.1.3 hid it on load).
-- Additionally, every control's FOCUSED view pairs it with the waveform, so
-- drilling into any control (Fire, etc.) keeps the waveform on the main display —
-- like Manual Grains pairing gview with every control. Without an explicit entry
-- the framework falls back to { "scope", <control> } (output scope, not waveform).
-- The grain-field phosphor is a compact, one-control-width viz that lives
-- persistently in the expanded strip. The waveform ("wave") is NOT in the
-- default expanded view — it stays closed until a sample is attached, at which
-- point setSample() places it at the front of the strip (and removes it again
-- on detach). Focused control views still reference "wave" so it appears when
-- you drill into a position control.
local views = { expanded = { "field" }, collapsed = {} }
for _, name in ipairs(controlOrder) do
  views.expanded[#views.expanded + 1] = name
  views[name] = { "wave", name }
end
views.field = { "wave", "field" }

function Dirac:onLoadViews(objects, branches)
  local controls = {}

  controls.wave  = WaveHeadView  { head = objects.head, width = 4 * app.SECTION_PLY }
  controls.field = GrainFieldView { head = objects.head, width = app.SECTION_PLY }

  local function gb(name, btn, desc, map, bias, gainMap, units)
    controls[name] = GainBias {
      button = btn, description = desc, branch = branches[name],
      gainbias = objects[name .. "Param"], range = objects[name .. "Range"],
      biasMap = map, initialBias = bias,
      gainMap = gainMap or Encoder.getMap("[-1,1]"), biasUnits = units,
    }
  end

  gb("playhead", "playh", "Read pos; live: fb delay", Encoder.getMap("[0,1]"), 0.0)
  -- Speed (v0.1.18): playhead scan rate — decouples TIME from PITCH (sample mode).
  -- 0 = parked (playh is the position, exactly as before); 1 = original tempo;
  -- negative = reverse; wraps at the sample ends. Touching playh re-seats the scan.
  gb("speed",    "speed", "Scan rate (0=park 1=tempo)", app.LinearDialMap(-4, 4), 0.0,
     app.LinearDialMap(-4, 4))
  gb("rate",     "dens",  "0 = trig only", app.LinearDialMap(0, 16), 3.0, app.LinearDialMap(-16, 16))
  gb("posjtr",   "posJtr","Position jitter", Encoder.getMap("[0,1]"), 0.0)
  gb("spread",   "sprd",  "Stereo spread", Encoder.getMap("[0,1]"), 0.5)
  gb("detune",   "detun", "Stereo detune", app.LinearDialMap(0, 0.5), 0.0, app.LinearDialMap(-0.5, 0.5))
  gb("level",    "level", "Output level", Encoder.getMap("[0,1]"), 0.7)
  gb("mix",      "mix",   "Wet/dry (live)", Encoder.getMap("[0,1]"), 1.0)
  gb("feedback", "fdbk",  "Regen (live)", Encoder.getMap("[0,1]"), 0.0)
  gb("compress", "comp",  "Grain leveling", Encoder.getMap("[0,1]"), 0.0)
  gb("binaural", "bin",   "Binaural", Encoder.getMap("[0,1]"), 0.0)
  gb("revprob",  "rev",   "Reverse probability", Encoder.getMap("[0,1]"), 0.0)
  gb("psprd",    "psprd", "Pitch scatter (st)", app.LinearDialMap(0, 12), 0.0, app.LinearDialMap(-12, 12))
  gb("texture",  "textr", "Perc>Hann>Tukey", Encoder.getMap("[0,1]"), 0.5)
  -- Integer-stepping dials: Grains (1..16) and Semitone (−24..24) move in whole units.
  local grainsMap = app.LinearDialMap(1, 16);   grainsMap:setCoarseRadix(15)
  local semiMap   = app.LinearDialMap(-24, 24);  semiMap:setCoarseRadix(48)
  local scaleMap  = app.LinearDialMap(0, 6);     scaleMap:setCoarseRadix(6)
  gb("grains",   "grains","Polyphony cap", grainsMap, 12.0, app.LinearDialMap(-16, 16))
  gb("semishift","semi",  "Transpose (st)", semiMap, 0.0, semiMap)

  controls.voct = Pitch {
    button      = "V/oct",
    description = "1v/oct",
    branch      = branches.voct,
    offset      = objects.tune,
    range       = objects.tuneRange,
  }
  gb("scale",    "scale", "Psprd quant: 0off 1chr 2Maj 3min 4penMaj 5penMin 6whole", scaleMap, 0.0, scaleMap)
  gb("grainlen", "gLen",  "Grain length (s)", app.LinearDialMap(0.001, 1.0), 0.050, app.LinearDialMap(-1.0, 1.0), app.unitSecs)

  controls.trig = Gate { button = "trig", description = "1 grain per trig",
                         branch = branches.fire, comparator = objects.fire }

  controls.hold = Gate { button = "hold", description = "latched looping",
                         branch = branches.hold, comparator = objects.holdGate }

  return controls, views
end

return Dirac
