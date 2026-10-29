# Porting plan

Written down from the README status table and the project notes; it is the plan the work has followed. Status is as of the last commit. "Done" means implemented and checked as far as the README states for that row, with the listed deviations; nothing here is a complete 1:1 port.

| # | Step | Status |
|---|------|--------|
| 1 | Path nodes and links (`nodes0..63.dat`) | done (parsed, all links valid) |
| 2 | Ambient pedestrians on ped nodes (popcycle group, `pedgrp.dat` model, `animgrp.dat` walk clip) | done, with deviations (no stop/cross/avoid logic, LS variant only, weekday only) |
| 3 | Ambient traffic along car path links | done, with deviations (constant speed, guessed lane offset, no lights/lane changes/reactions) |
| 4a | Audio: SFX bank parser | done (710 banks parse) |
| 4b | Audio: footsteps (surface banks, swimming, ambient peds) | done, run-tested only, not heard |
| 5 | Audio: vehicle engine / horn / doors | not started |
| 6 | Audio: weapons, speech, ambience, radio streams | not started |
| 7 | Water: waves, foam, flow shading, seam, brightness | not started |
| 8 | Particles (sandstorm, splashes, rain haze) | not started |
| 9 | CJ clothes (`ConstructTextures`, clothes builder) | not started |
| 10 | More script commands / missions (most commands are stubs) | partial |
| 11 | Real vehicle physics (CAutomobile) | not started (driving prototype is a fitted kinematic model) |
| 12 | Car streaming for generator models (`m_AppropriateLoadedCars`) | not started |

Steps 5-12 are each a subsystem of weeks of work in the original; they are not expected to finish in a single session.
