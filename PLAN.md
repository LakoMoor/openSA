# Porting plan

reVC (E:\openSAeVC) is the structural reference for most remaining steps; SA-specific values come from the exe / gta-reversed.

Written down from the README status table and the project notes; it is the plan the work has followed. Status is as of the last commit. "Done" means implemented and checked as far as the README states for that row, with the listed deviations; nothing here is a complete 1:1 port.

| # | Step | Status |
|---|------|--------|
| 1 | Path nodes and links (`nodes0..63.dat`) | done (parsed, all links valid) |
| 2 | Ambient pedestrians on ped nodes (popcycle group, `pedgrp.dat` model, `animgrp.dat` walk clip) | done, with deviations (no stop/cross/avoid logic, LS variant only, weekday only) |
| 3 | Ambient traffic along car path links | done, with deviations (constant speed, guessed lane offset, no lights/lane changes/reactions) |
| 4a | Audio: SFX bank parser | done (710 banks parse) |
| 4b | Audio: footsteps (surface banks, swimming, ambient peds) | done, run-tested only, not heard |
| 5 | Audio: vehicle engine / horn / doors | partial (horn + cruise-loop engine of the driven car, run-tested, not heard) |
| 6 | Audio: weapons, speech, ambience, radio streams | not started |
| 7 | Water: waves, foam, flow shading, seam, brightness | partial (water.dat polygons drawn in two textured layers with the exe's alpha/UV-scroll/wind logic and timecyc colour; no vertex wave animation, foam, 2x2 m renderer grid or seam handling) |
| 8 | Particles (sandstorm, splashes, rain haze) | partial (rain streaks only; no particle system, splashes, sandstorm) |
| 9 | CJ clothes (`ConstructTextures`, clothes builder) | not started |
| 10 | More script commands / missions (most commands are stubs) | partial |
| 11 | Real vehicle physics (CAutomobile) | partial (suspension + tyre + transmission core ported from reVC/gta-reversed, flat-ground numbers sane; surface adhesion from surface.dat/surfinfo.dat done; static-world collision impulses done; no damage, no vehicle/ped/object collisions) |
| 12 | Car streaming for generator models (`m_AppropriateLoadedCars`) | partial (StreamOneNewCar / appropriate-zone logic ported from gta-reversed; pool of up to 22 model ids with the original 64/350-frame cadence, no real loading, no taxi/boats/cops/gang cars; run-tested via log) |

Steps 5-12 are each a subsystem of weeks of work in the original; they are not expected to finish in a single session.
