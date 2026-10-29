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
| 6 | Audio: weapons, speech, ambience, radio streams | partial (key X on foot plays the Desert Eagle shot from bank 143 sound 6; builds and runs, never triggered or heard in a test; no weapon system, layering, speech, ambience, radio) |
| 7 | Water: waves, foam, flow shading, seam, brightness | partial (water.dat polygons drawn in two textured layers with the exe's alpha/UV-scroll/wind logic and timecyc colour; wave height grid near the camera done; no foam, colour multiplier / glare, original block-scan renderer or seam handling) |
| 8 | Particles (sandstorm, splashes, rain haze) | partial (rain streaks; ground splash rings as an INVENTED stand-in, not seen in a screenshot; no particle system, sandstorm) |
| 9 | CJ clothes (`ConstructTextures`, clothes builder) | partial (starting outfit from main.scm's GIVE_PLAYER_CLOTHES_OUTSIDE_SHOP: vest, denim jeans, black sneakers as separate skinned parts; checked on screen; no texture composition/blending, no fat/muscle blend, no outfit changes, outfit hard-coded from the recorded script calls) |
| 10 | More script commands / missions (most commands are stubs) | partial |
| 11 | Real vehicle physics (CAutomobile) | partial (suspension + tyre + transmission core ported from reVC/gta-reversed, flat-ground numbers sane; surface adhesion from surface.dat/surfinfo.dat done; static-world collision impulses done; wall hit + VehicleDamage health loss in `CarPhysics::hitStatic`, headless crash test in physics_check: collForceMult corrected to 1 (collision path VehicleDamage(0): gta-reversed Automobile.cpp:2816); Elegant now loses ~100 of 1000 health at 100 km/h, 60 at 60 km/h. Whether that matches the real game was not compared; only a single ApplyCollision impulse per hit is modelled; no visual damage, no vehicle/ped/object collisions) |
| 12 | Car streaming for generator models (`m_AppropriateLoadedCars`) | partial (StreamOneNewCar / appropriate-zone logic ported from gta-reversed; pool of up to 22 model ids with the original 64/350-frame cadence, no real loading, no taxi/boats/cops/gang cars; run-tested via log) |

Steps 5-12 are each a subsystem of weeks of work in the original; they are not expected to finish in a single session.
