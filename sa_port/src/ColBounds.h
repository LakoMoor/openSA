#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sa {

// Bounding sphere of one collision model, as stored in the COL file header (not the triangle data).
// Layout from gta-reversed ColHelpers.h: COL1 = {radius, center, min, max}; COL2/3/4 = {min, max, center, radius}.
struct ColBound {
	float radius = 0;
	float center[3] = {};
};

// Walks the concatenated COL records of one file ("COLL"/"COL2"/"COL3"/"COL4", u32 size, 22-byte name, u16 id, bounds, ...)
// and stores the bound under the lower-case model name. Returns the number of records read.
size_t ParseColBounds(const uint8_t *data, size_t size, std::unordered_map<std::string, ColBound> &out);

// Collision geometry of one model (COL2/COL3/COL4 only; COL1 records keep bounds but no geometry here).
// Layout from CFileLoader::LoadCollisionModelVer3 (0x537CE0): header after the 40-byte bounds is
// u16 nSpheres, u16 nBoxes, u16 nFaces, u8 nLines, u8 pad, u32 flags, u32 offSpheres, offBoxes, offLines, offVerts, offFaces, offPlanes
// (offsets are relative to record start + 4). Vertices are int16[3] / 128, faces {u16 a,b,c; u8 material, light}, boxes {min,max,surface}.
struct ColBox { float min[3], max[3]; };
struct ColSphereT { float center[3], radius; };
struct ColTri { uint16_t a, b, c; uint8_t material; };
struct ColModel {
	ColBound bound;
	std::vector<float> verts; // x,y,z triples
	std::vector<ColTri> tris;
	std::vector<ColBox> boxes;
	std::vector<ColSphereT> spheres;
};

size_t ParseColModels(const uint8_t *data, size_t size, std::unordered_map<std::string, ColModel> &out);

} // namespace sa
