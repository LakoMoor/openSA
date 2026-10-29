<p align="center"><img src="branding/logo_512.png" width="160" alt="openSA logo"></p>

# openSA

An open-source reimplementation of the **GTA San Andreas** runtime, written in C++ on top of
[librw](https://github.com/aap/librw) and checked against the 1.0 US executable you already own.

> **No game files are included and none are needed in this repository.** You need your own copy of
> GTA San Andreas (the viewers only read its `data/` and `models/` folders). This project is not affiliated
> with Rockstar Games or Take-Two Interactive.

Website: <https://lakomoor.github.io/openSA/>

## Status

Early stage. This is a data and streaming layer plus viewers, not a playable game.

| Area | State |
|---|---|
| IMG v2, `gta.dat`/`default.dat`, all IDE sections, text + binary IPL, DFF/TXD loading | done; the whole stock game parses without errors |
| Streaming slot layout (`CStreaming::LoadCdDirectory`), pool sizes (`CPools::Initialise`), TXD slots | done, taken from the decompiled executable |
| Streamer (request / load / remove, model → TXD → parent TXD chain) | done (synchronous, no loading channels or memory budget yet) |
| `sa_stream`: whole-map viewer that streams models and textures around a free camera, with LOD | done; distance/LOD rule follows `SetupMapEntityVisibility` (colModel radius from COL headers; no occlusion, tunnels or fade yet) |
| Vehicles: palette paint, wheels | partial: stock wheel per car and right-wheel orientation unverified |
| Real `CRenderer` visibility/LOD rules | distance part done; culling pending; big-building rule from `CFileLoader`; LOD multiplier follows `CCamera::Process` and the rendering FOV (`SA_FOV`, default 70; during cutscenes the original uses 1.0, not applicable here) |
| Time of day: `timecyc.dat` parsed, sky colour, fog, far clip and ambient follow `SA_HOUR` / `SA_WEATHER` (day/night vertex colours from the Extra Vert Colour plugin, balance curve verified in the exe; game clock runs 1 minute per second (`SA_CLOCK=0` freezes it); hourly weather changes follow the exe forecast tables and blend the timecyc keys; sky follows CClouds::RenderSkyPolys (banded gradient 0.5 / 0 / -0.1 / -0.3 of a 30 m wall, below-horizon grey table from the exe) with a additive coronastar sun sprite (particle.txd, outer corona fades by line of sight to the sun as CRegisteredCorona::Update does, but the LOS ray only reaches the 3x3 cells around the camera; no SUN flare sprites) at the exe's vector-to-sun and the directional light follows it, low clouds (12 additive cloud1 sprites after CClouds::Render_RenderLowClouds), moon disc (no crescent mask) and Rockstar logo (layout unverified), rain streaks (CWeather::RenderRainStreaks: 32 lines, Rain step from the exe; wind from CWeather::Update tables read in the exe, FogReduction 0, no particles/puddles/wet roads; streaks confirmed drawn on screen with SA_RAINDEBUG=1 red test colour); lightning flash (white sky/fog/ambient, per-frame roll as in CWeather::Update; no thunder sound or bolt; SA_LIGHTNING=1 forces it for tests); no rainbow, fog banks or sandstorm; water.dat polygons drawn as two textured layers (waterclear256, UV scale 25 / 12.5 m, alpha 160 / 90 from the exe, colour = raw timecyc water rgb, layer alphas derived from the timecyc alpha as the exe does; UV scroll follows the nearest flow plus the layer-1 sin/cos wobble scaled by CWeather::Wavyness (exe-derived: min(min(wind,1)+0.3,1), wind table read at 0x8D5E50); no waves, foam or flow shading, and the result still looks darker than the original); CJ treads water at waterLevel-0.55 with swim_tread, entry rule derived from CPed::ProcessBuoyancy (origin < waterLevel-0.6; swim exit follows m_fSwimStopTime (15-tick threshold read in the exe at 0x8D2FC0; the buoyancy-force reset branch is not modelled), height approach velocity-based (clamps 0.1*ts and 0.02*ts read in the exe))) | partial |
| Collision: COL2/3/4 geometry parsed (1.06 M triangles in gta3.img); walk mode (`G`): ground via vertical ray tests (origin line of CPed::ProcessEntityCollision 0x5E2530: starts at the ped origin, reaches 1.0 below + 0.15*timeStep while on ground, snaps to hit+1.0; read in the exe), walls via the exe's three ped spheres with the CPhysical::ProcessShiftSectorList shift (sum of contact normals, ped step clamp(1.5 x depth, 0.005, 0.3) read in the exe at 0x546A62; horizontal part only; ceiling-bump branch of 0x5E2530 (top sphere, flat ceiling: push back against velocity, -0.05 height; untested in play); no vehicle/ped/object contact sources), third-person camera (look-at height 0.6 m from the exe table at 0x8CC548; follow distance min(2.0 + zoomSmoothed, 4.0) from the exe, zoomSmoothed rising from 0 to the CCam target 3.6 for ped zoom 2 (targets -0.55 / 1.5 / 3.6 read at 0x5BC5BB; default ped zoom 2 read in CCamera::Init at 0x5BC6C2, no zoom change keys) so 4 m in steady state) with a COL ray test (ray cast checked against ground and forward hits; objects without COL do not block it), CJ (base body parts from player.img, no clothes) animated from `ped.ifp` (idle/walk/run/jump/swim clips chosen by key and cross-faded with BlendAnimation semantics: fade-in delta 1.0 for walk/run/sprint and 4.0 for idle (read in CPed::SetMoveAnim, exe 0x5E4A00; jump launch/glide 8.0, land 1000, swim tread 8.0 and breast/crawl 2.0 read in the exe; swim height 0.55 / 0.4 / 0.2 follows the live clip amounts); per-bone weighted nlerp; `SA_MALE01=1` selects the generic male01 ped) | partial |
| Parked vehicles in the streaming viewer: the 293 IPL car generators with a fixed vehicle model (angle stored in radians; 742 more use modelId -1 = random zone model and are skipped) are shown as static, intact, painted cars with wheels within 150 m of the camera (placed on the COL ground; verified on screen with a street sweeper). Spawn rule implemented: parked-car population limit 5 by day / 10 at 21-07 unless the generator ignores it (read in gta-reversed DoInternalProcessing, not re-read in the exe). Deviations: no spawn delays/probability, no blockage check, no random models, generator colour indices applied to paint markers 0 and 1 (random -1 and markers 2-3 use the first carcols pair; marker mapping not verified), the stock wheel is the car's own main wheel atomic cloned onto the four wheel dummies (as CVehicleModelInfo::PreprocessHierarchy does in gta-reversed; not re-read in the exe; checked on screen, the sweeper shows black tyres on all visible wheel dummies; wheels.dff is only for upgrades), player collision with the car's own embedded COL3 (spheres/boxes/triangles through the ped-sphere shift; the player is held at the side of the car (checked 18 s walking into the door side) and at the rear bumper, held at the rear bumper for 28 s with a simplified solid-obstacle response (full horizontal depth, capped 0.3 per tick) instead of the original's ApplyCollision impulses; front checked (player stops at the bumper after fixing the COL sphere field order: centre xyz then radius); a car never moves), no physics | partial |
| Zones: `info.zon` parsed (380 zones), the innermost box under the player is looked up and logged (PLS at Playa del Seville, matches the map); popcycle / car groups / random models not built on it yet | partial |
| Script VM (main.scm): core commands, mission launch, zone population types; random-model car generators | started: zone pop type from main.scm and popcycle/cargrp pick the model (approximation of PickRandomCar, no car streaming); most game commands unhandled |
| Driving a parked car (F, WASD, space) | PROTOTYPE: kinematic model from handling.cfg, no suspension/damage/enter animation; world collision = 5 pushed-out spheres; verified on screen |
| Path nodes (`nodes0..63.dat`): 68237 nodes (30587 vehicle, 37650 ped) parsed, areaId matches file for all | nodes, node links (143622), car path links (31466), link lengths and intersection flags parsed, every link target valid; routing not yet |
| Ambient pedestrians in the viewer: spawn on ped path nodes 30-70 m from the camera, ped group from the zone's popcycle row (weekday only), model from `pedgrp.dat` (Los Santos variant only), dff/txd from gta3.img, walk along node links (random next node, not back) at the walk clip's average speed, ground from COL, despawn at 110 m; checked on screen near Grove Street. Deviations: walk cycle and speed come from the model's anim group (`peds.ide` column -> first clip of that group in `animgrp.dat`, clip root-motion speed; falls back to `walk_civi` when the clip is missing; groups defined in other IFP files are not loaded), no stopping/crossing/avoidance/collision, no cars or traffic reaction, spawn ring/despawn/cap (30) are guesses, `SA_NOPEDS=1` disables | partial |
| Ambient traffic in the viewer: cars spawn on car path nodes 80-140 m from the camera (cap min(popcycle maxCars, 20)), random model as for random generators, drive node to node along the links at a constant 10 m/s shifted 1.8 m right of travel, stop when another traffic car is within 8 m ahead, despawn at 170 m; checked on screen on the highway near Idlewood. Deviations: lane offset guessed, car path link lanes / curve slowdown / traffic lights / lane changes not modelled, no reaction to player or peds, no physics, random carcols.dat colour pair per car (builds and runs; colours not confirmed on a screenshot, cars were too far away), ring/cap are guesses, `SA_NOTRAFFIC=1` disables | partial |
| Audio, first slice: all 710 SFX banks parse from `BankLkup.dat` + `audio/sfx/*` (layout from gta-reversed `CAEBankLoader`; consistent with file sizes, probe `audio_probe`), player footsteps in walk mode through waveOut (generic bank 0, sounds 1-5). Not verified by ear (no audio output available to the author). Footstep bank follows the COL surface under the feet via `surfaud.dat` (checked in the log: asphalt -> generic, grass -> grass bank; surface id = row order of surfaud.dat is assumed; box colliders count as surface 0; water plays nothing). Ambient peds also make generic-bank footsteps (0.56 volume, quadratic falloff to silence at 30 m; run-tested, not heard). Swimming plays the GENRL_SWIMMING bank (128, sounds 0-4) twice per stroke clip loop (bank loads, run-tested in the sea off Santa Maria, not heard). Deviations: steps timed as two per clip loop instead of animation events, flat volume, no 3D, no other sounds, no radio / streams / ambience / vehicles / weapons / speech; `SA_MUTE=1` silences | partial |
| Rigid-body car physics for the driven car (`VehiclePhysics.*`, default; `SA_KINEMATIC=1` selects the old kinematic prototype): port of CPhysical force application, CAutomobile suspension springs + damping, CVehicle::ProcessWheel traction and cTransmission drive acceleration, structure from reVC and SA formulas from gta-reversed; constants checked in the exe (gravity 0.008 at 0x863984, damping limits 0.25 / 0.998 at 0x8CD7A0, wheel spin limit 0.3 / spinning loss 0.2 at 0x8D34C0). Headless check `physics_check` on flat ground, ELEGANT: 0-100 km/h in ~11.5 s, top speed ~165-178 km/h (cfg 165), braking ~6.6 m/s^2 (cfg 6.0). Run on screen in Angel Pine (car sits on the road, drives and steers). Deviations: wheel probes are vertical rays against the COL ground with a 3-sample normal, tyre adhesion 1.0 on every surface (surfinfo.dat not ported), world collision is the old 5-sphere push-out plus a speed loss (no CPhysical::ProcessCollision impulses), steering smoothing 0.1/tick assumed, no 1G/2G boost flags, no wheel rotation/steering visuals, no damage, no other cars' physics, speed above cfg top speed by up to ~8 % (not investigated) | partial |
| Driven car attitude: pitch and roll follow the terrain from ground samples under the axles / sides (+-1.4 m / +-0.8 m), smoothed. Run-tested on a flat road (no regression: car stays on the ground), slope behaviour NOT confirmed on screen; geometric stand-in for the CAutomobile suspension | partial |
| Engine of the driven car: model's player bank from the VehicleAudioSettings table (EngineTable.inc, copied from gta-reversed 0x860AF0, not re-read in the exe; bank layout checked with `audio_probe`: 3 sounds = acceleration / cruise with loop point / deceleration), cruise loop streamed through waveOut at a pitch from speed. Run-tested 30 s of driving, not heard. Deviations: only the cruise loop, pitch = enginePitch*(0.55+speed/vmax) instead of gear RPM, no acceleration/deceleration sounds, no other cars' engines, flat volume, 29 models without a bank stay silent | partial |
| Horn of the driven car (key `H`, assumed binding): GENRL_HORN bank 74, sound id and pitch per model from the VehicleAudioSettings table (copied from gta-reversed 0x860AF0, 232 rows, not re-read in the exe); run-tested (log shows the sound/pitch chosen), not heard. Deviations: retriggered instead of looped, flat volume, no 3D; engine, doors, sirens not done | partial |
| Vehicle reflections, vehicle physics (real) | not started |

The goal is behaviour identical to the original game. Facts read from the executable are marked as such in
the source comments (function names and addresses refer to the 1.0 US executable); everything else is an
assumption and is called out as one.

## Build (Windows, Visual Studio 2022, CMake 3.15+)

```bat
git clone https://github.com/LakoMoor/openSA
cd openSA
git clone https://github.com/aap/librw third_party/librw

cmake -S sa_view -B build/view
cmake --build build/view --config Release --target sa_stream sa_world sa_view

build\view\Release\sa_stream.exe "C:\Games\GTA San Andreas" 2495 -1690
```

Controls: `G` toggles walk mode (gravity, ground collision), `W A S D` move (run; `Left Alt` walks, `Space` sprints, `Left Shift` jumps; speeds from the ped.ifp root motion, jump from CTaskSimpleJump::Launch), `Q`/`E` down/up, `Shift` faster, left mouse drag to look, `Esc` quits.
`sa_world.exe <gamedir> [x y radius]` shows a static slice of the map, `sa_view.exe <gamedir> [model]` shows one
model (try `infernus`, `cheetah`, `banshee`).

Data-layer tools (no GPU needed):

```bat
cmake -S sa_port -B build/port
cmake --build build/port --config Release
build\port\Release\load_all.exe "C:\Games\GTA San Andreas"   :: parse every IDE/IPL, report problems
build\port\Release\stream_info.exe "C:\Games\GTA San Andreas" :: slot assignment + streamer self-test
```

## Layout

| Path | Contents |
|---|---|
| `sa_port/src` | data library: archives, IDE/IPL, RenderWare chunk walker, streaming slots, streamer |
| `sa_view` | librw (D3D9) viewers: `sa_view`, `sa_world`, `sa_stream` |
| `tools` | helpers for mapping function addresses to names from the public gta-reversed project |
| `branding` | logo and icon (original artwork) and the script that draws them |
| `docs` | project website (GitHub Pages) |

## Licence

Source code: MIT, see `LICENSE`. librw is a separate project (MIT) and is not part of this repository.
"Grand Theft Auto" and "San Andreas" are trademarks of their respective owners.
