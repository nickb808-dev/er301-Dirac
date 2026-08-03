-- SPDX-License-Identifier: Apache-2.0
-- Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
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
  -- Multi-playhead (v0.2.0). heads = 1 is the classic single-head path.
  param(self, head, "heads",      "Heads",      1.0)   -- playhead count [1,4]
  param(self, head, "headspread", "HeadSpread", 0.0)   -- position offset between heads
  param(self, head, "headtune",   "HeadTune",   0.0)   -- pitch interval between heads (st)
  param(self, head, "snap",      "Snap",      0.0)     -- pull grain starts onto transients
  param(self, head, "color",     "Color",     0.0)     -- per-grain cutoff scatter (v0.2.2)
  param(self, head, "window",    "Window",    0.0)     -- grain shape 0..7 (v0.2.10)
  param(self, head, "tilt",      "Tilt",      0.0)     -- envelope peak position, bipolar (v0.2.10)
  param(self, head, "tiltspread","TiltSpread",0.0)     -- per-grain tilt scatter (v0.2.10)
  param(self, head, "mix",       "Mix",       1.0)     -- wet/dry (live mode only); 1 = all wet
  param(self, head, "feedback",  "Feedback",  0.0)     -- regen: ring reinjection (both modes, v0.2.2)
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

-- Multi-playhead controls (v0.2.0) are OFF by default to keep the default strip
-- short; the Playheads menu option adds/removes them, mirroring showWaveform.
local kHeadControls = { "heads", "headspread", "headtune" }

local function headInView(self, name)
  local view = self:getView("expanded")
  if not view then return nil, nil end
  for i, c in ipairs(view.controls) do
    if c == self.controls[name] then return view, i end
  end
  return view, nil
end

function Dirac:showHeads(show)
  if not self.controls then return end
  local changed = false
  for _, name in ipairs(kHeadControls) do
    if self.controls[name] then
      local view, idx = headInView(self, name)
      if show and view and idx == nil then
        self:placeControl(name, "expanded")     -- append to the strip
        changed = true
      elseif (not show) and view and idx ~= nil then
        table.remove(view.controls, idx)
        changed = true
      end
    end
  end
  if changed and self.currentViewName == "expanded" then
    require("Application").postTrigger(self, "rebuildView")
  end
end

-- ── Mode-dependent controls (v0.2.0) ────────────────────────────────────────
-- Three controls are HARD-GATED in the DSP by source mode, so in the wrong mode
-- they are knobs that visibly move and do nothing — which reads as a bug. Hide
-- them instead. (Dirac.cpp: mix `if (liveMode && mix < 0.99995f)`, speed inside
-- `if (!liveMode)`.) v0.2.2: feedback now regenerates in BOTH modes (sample mode
-- spawns ring-reading grains), so it stays in the strip everywhere.
local kLiveOnlyControls   = { "mix" }               -- dead once a sample is attached
local kSampleOnlyControls = { "speed" }             -- dead in live mode

-- Forward declaration: the canonical strip order is built further down (it needs
-- controlOrder, which is defined with the views). It must be declared HERE so
-- showModeControls closes over the local rather than a nil global.
local kExpandedOrder

-- Generic "is this control currently in the expanded strip?" (headInView with the
-- name generalised — same contract, returns the RAW view.controls index).
local function controlInView(self, name)
  local view = self:getView("expanded")
  if not view then return nil, nil end
  local control = self.controls and self.controls[name]
  if not control then return view, nil end
  for i, c in ipairs(view.controls) do
    if c == control then return view, i end
  end
  return view, nil
end

-- Where should `name` go when we put it BACK? placeControl takes a user-facing
-- position (it adds +2 internally to skip the insert and header entries), so
-- count how many of its canonical predecessors are currently placed. Without
-- this, a re-shown control lands at the end of the strip and the order scrambles.
local function orderedPosition(self, name, canonical)
  local pos = 0
  for _, other in ipairs(canonical) do
    if other == name then break end
    local _, idx = controlInView(self, other)
    if idx then pos = pos + 1 end
  end
  return pos + 1
end

function Dirac:showModeControls(live)
  if not self.controls then return end
  local changed = false
  local function apply(list, want)
    for _, name in ipairs(list) do
      local view, idx = controlInView(self, name)
      if view then
        if want and idx == nil then
          self:placeControl(name, "expanded", orderedPosition(self, name, kExpandedOrder))
          changed = true
        elseif (not want) and idx ~= nil then
          table.remove(view.controls, idx)
          changed = true
        end
      end
    end
  end
  apply(kLiveOnlyControls, live)
  apply(kSampleOnlyControls, not live)
  if changed and self.currentViewName == "expanded" then
    require("Application").postTrigger(self, "rebuildView")
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
  self:showModeControls(self.sample == nil)
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

local menu = { "sampleHeader", "selectFromCard", "selectFromPool", "detachBuffer", "editSample",
               "playheads", "diffuse", "freeze" }

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

  -- Multi-playhead (v0.2.0): off by default. Turning it on reveals the three
  -- head controls in the strip; off hides them AND hard-bypasses the DSP path.
  controls.playheads = OptionControl {
    description = "Playheads",
    option      = objects.head:getOption("Playheads"),
    choices     = { "single", "multi" },
    descriptionWidth = 2,
    onUpdate    = function() self:showHeads(objects.head:getOption("Playheads"):value() >= 2) end,
  }

  -- Diffuse (v0.2.2): wet-path allpass halo, off by default (hard-bypassed).
  controls.diffuse = OptionControl {
    description = "Diffuse",
    option      = objects.head:getOption("Diffuse"),
    choices     = { "off", "on" },
    descriptionWidth = 2,
  }
  -- Freeze (v0.2.2): stops WRITING the capture ring. Hold freezes the CLOUD;
  -- Freeze freezes the SOURCE — all controls stay live over the locked buffer.
  controls.freeze = OptionControl {
    description = "Capture",
    option      = objects.head:getOption("Freeze"),
    choices     = { "run", "freeze" },
    descriptionWidth = 2,
  }

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

-- Sync the head controls to the (preset-restored) Playheads option once the
-- views exist, so reloading a patch saved in "multi" brings the controls back.
function Dirac:onLoadFinished()
  self:showHeads(self.objects.head:getOption("Playheads"):value() >= 2)
  -- deserialize() only calls setSample() when the preset HAS a sample, so a
  -- sample-less preset would never fire the mode hook. Reconcile here.
  self:showModeControls(self.sample == nil)
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
  "psprd", "scale", "window", "texture", "tilt", "tiltspread", "color", "snap", "compress", "revprob", "level", "mix", "feedback", "binaural",
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
-- Dirac starts in LIVE mode (no sample), so the sample-only controls are absent
-- from the default strip; setSample() places them when a sample is attached.
-- They still get a focused view built here, exactly like the always-present ones.
local hiddenAtStart = {}
for _, name in ipairs(kSampleOnlyControls) do hiddenAtStart[name] = true end

local views = { expanded = { "field" }, collapsed = {} }
for _, name in ipairs(controlOrder) do
  if not hiddenAtStart[name] then views.expanded[#views.expanded + 1] = name end
  views[name] = { "wave", name }
end
views.field = { "wave", "field" }

-- Canonical strip order used to re-insert a control at its proper slot.
-- "wave" leads (placed at position 1 on sample attach), then "field", then the
-- control order. Head controls are appended after everything, so they're absent.
kExpandedOrder = { "wave", "field" }
for _, name in ipairs(controlOrder) do kExpandedOrder[#kExpandedOrder + 1] = name end

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
  -- Multi-playhead (v0.2.0): N heads read the same buffer, dealt round-robin.
  -- Heads steps in whole units (1..4) — grains are dealt to discrete heads and
  -- the DSP rounds anyway; same integer-dial pattern as Grains/Semitone below.
  local headsMap = app.LinearDialMap(1, 4);  headsMap:setCoarseRadix(3)
  gb("heads",      "heads", "Playheads (1 = classic)", headsMap, 1.0,
     app.LinearDialMap(-4, 4))
  gb("headspread", "hsprd", "Head position offset", Encoder.getMap("[0,1]"), 0.0)
  gb("headtune",   "htune", "Head interval (st)", app.LinearDialMap(-12, 12), 0.0,
     app.LinearDialMap(-12, 12))
  gb("rate",     "dens",  "0 = trig only", app.LinearDialMap(0, 16), 3.0, app.LinearDialMap(-16, 16))
  gb("posjtr",   "posJtr","Position jitter", Encoder.getMap("[0,1]"), 0.0)
  gb("spread",   "sprd",  "Stereo spread", Encoder.getMap("[0,1]"), 0.5)
  gb("detune",   "detun", "Stereo detune", app.LinearDialMap(0, 0.5), 0.0, app.LinearDialMap(-0.5, 0.5))
  gb("level",    "level", "Output level", Encoder.getMap("[0,1]"), 0.7)
  gb("mix",      "mix",   "Wet/dry (live)", Encoder.getMap("[0,1]"), 1.0)
  gb("feedback", "fdbk",  "Regen (live+sample)", Encoder.getMap("[0,1]"), 0.0)
  gb("compress", "comp",  "Grain leveling", Encoder.getMap("[0,1]"), 0.0)
  gb("binaural", "bin",   "Binaural", Encoder.getMap("[0,1]"), 0.0)
  gb("revprob",  "rev",   "Reverse probability", Encoder.getMap("[0,1]"), 0.0)
  gb("psprd",    "psprd", "Pitch scatter (st)", app.LinearDialMap(0, 12), 0.0, app.LinearDialMap(-12, 12))
  -- Window (v0.2.10): grain shape select. Integer-stepping + CV-able, so an
  -- S&H here scatters shape per grain. 0 normal · 1 bell · 2 sinc · 3 tri ·
  -- 4 decay · 5 ramp · 6 tukey · 7 square. (A menu OptionControl can only
  -- ever display THREE choices — hence a dial.)
  local windowMap = app.LinearDialMap(0, 7);  windowMap:setCoarseRadix(7)
  gb("window",   "shape", "0 nrm 1bel 2snc 3tri 4dcy 5rmp 6tky 7sqr", windowMap, 0.0,
     windowMap)
  -- Texture: in shape 0 the perc>hann>tukey morph; in shapes 1-7 that shape's
  -- own macro (width / ring density / curvature / time / taper / edge).
  gb("texture",  "textr", "Shape morph / shape macro", Encoder.getMap("[0,1]"), 0.5)
  -- Tilt (v0.2.10): envelope peak position, works on every Window shape.
  gb("tilt",     "tilt",  "Peak: attack<>swell", app.LinearDialMap(-1, 1), 0.0,
     app.LinearDialMap(-1, 1))
  gb("tiltspread","tsprd","Tilt scatter per grain", Encoder.getMap("[0,1]"), 0.0)
  gb("snap",     "snap",  "Lock grains to onsets", Encoder.getMap("[0,1]"), 0.0)
  gb("color",    "color", "Cutoff scatter: bright-dark", Encoder.getMap("[0,1]"), 0.0)
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
