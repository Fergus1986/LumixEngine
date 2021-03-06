#include "engine/lumix.h"
#include "engine/array.h"
#include "engine/base_proxy_allocator.h"
#include "engine/blob.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/fs/os_file.h"
#include "engine/iallocator.h"
#include "engine/log.h"
#include "engine/lua_wrapper.h"
#include "engine/profiler.h"
#include "engine/property_descriptor.h"
#include "engine/property_register.h"
#include "engine/vec.h"
#include "engine/universe/universe.h"
#include "lua_script/lua_script_system.h"
#include "navigation_system.h"
#include "physics/physics_scene.h"
#include "renderer/model.h"
#include "renderer/material.h"
#include "renderer/render_scene.h"
#include "renderer/texture.h"
#include <cmath>
#include <DetourCrowd.h>
#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourNavMeshBuilder.h>
#include <Recast.h>
#include <RecastAlloc.h>


namespace Lumix
{


static const ComponentType NAVMESH_AGENT_TYPE = PropertyRegister::getComponentType("navmesh_agent");
static const int CELLS_PER_TILE_SIDE = 256;
static const float CELL_SIZE = 0.3f;
static void registerLuaAPI(lua_State* L);


struct Agent
{
	Entity entity;
	float radius;
	float height;
	int agent;
	bool is_finished;
};


struct NavigationSystem : public IPlugin
{
	NavigationSystem(Engine& engine)
		: m_engine(engine)
		, m_allocator(engine.getAllocator())
	{
		ASSERT(s_instance == nullptr);
		s_instance = this;
		dtAllocSetCustom(&detourAlloc, &detourFree);
		rcAllocSetCustom(&recastAlloc, &recastFree);
		registerLuaAPI(m_engine.getState());
		registerProperties();
	}


	~NavigationSystem()
	{
		s_instance = nullptr;
	}


	static void detourFree(void* ptr)
	{
		s_instance->m_allocator.deallocate(ptr);
	}


	static void* detourAlloc(size_t size, dtAllocHint hint)
	{
		return s_instance->m_allocator.allocate(size);
	}


	static void recastFree(void* ptr)
	{
		s_instance->m_allocator.deallocate(ptr);
	}


	static void* recastAlloc(size_t size, rcAllocHint hint)
	{
		return s_instance->m_allocator.allocate(size);
	}


	static NavigationSystem* s_instance;


	void registerProperties();
	bool create() override { return true; }
	void destroy() override {}
	const char* getName() const override { return "navigation"; }
	IScene* createScene(Universe& universe) override;
	void destroyScene(IScene* scene) override;

	BaseProxyAllocator m_allocator;
	Engine& m_engine;
};


NavigationSystem* NavigationSystem::s_instance = nullptr;


struct NavigationSceneImpl : public NavigationScene
{
	enum class Version
	{
		AGENTS = 0,

		LATEST,
	};


	NavigationSceneImpl(NavigationSystem& system, Universe& universe, IAllocator& allocator)
		: m_allocator(allocator)
		, m_universe(universe)
		, m_system(system)
		, m_detail_mesh(nullptr)
		, m_polymesh(nullptr)
		, m_navquery(nullptr)
		, m_navmesh(nullptr)
		, m_debug_compact_heightfield(nullptr)
		, m_debug_heightfield(nullptr)
		, m_debug_contours(nullptr)
		, m_num_tiles_x(0)
		, m_num_tiles_z(0)
		, m_agents(m_allocator)
		, m_crowd(nullptr)
	{
		setGeneratorParams(0.3f, 0.1f, 0.3f, 2.0f, 60.0f, 1.5f);
		m_universe.entityTransformed().bind<NavigationSceneImpl, &NavigationSceneImpl::onEntityMoved>(this);
	}


	~NavigationSceneImpl()
	{
		m_universe.entityTransformed().unbind<NavigationSceneImpl, &NavigationSceneImpl::onEntityMoved>(this);
		clear();
		for (auto* agent : m_agents) LUMIX_DELETE(m_allocator, agent);
		m_agents.clear();
	}


	void onEntityMoved(Entity entity)
	{
		auto iter = m_agents.find(entity);
		if (m_agents.end() == iter) return;
		Vec3 pos = m_universe.getPosition(iter.key());
		const dtCrowdAgent* dt_agent = m_crowd->getAgent(iter.value()->agent);
		if ((pos - *(Vec3*)dt_agent->npos).squaredLength() > 0.1f)
		{
			m_crowd->removeAgent(iter.value()->agent);
			addCrowdAgent(iter.value());
		}
	}


	int getVersion() const override { return (int)Version::LATEST; }


	void clear()
	{
		rcFreePolyMeshDetail(m_detail_mesh);
		rcFreePolyMesh(m_polymesh);
		dtFreeNavMeshQuery(m_navquery);
		dtFreeNavMesh(m_navmesh);
		dtFreeCrowd(m_crowd);
		rcFreeCompactHeightfield(m_debug_compact_heightfield);
		rcFreeHeightField(m_debug_heightfield);
		rcFreeContourSet(m_debug_contours);
		m_detail_mesh = nullptr;
		m_polymesh = nullptr;
		m_navquery = nullptr;
		m_navmesh = nullptr;
		m_crowd = nullptr;
		m_debug_compact_heightfield = nullptr;
		m_debug_heightfield = nullptr;
		m_debug_contours = nullptr;
	}


	void rasterizeGeometry(const AABB& aabb, rcContext& ctx, rcConfig& cfg, rcHeightfield& solid)
	{
		rasterizeMeshes(aabb, ctx, cfg, solid);
		rasterizeTerrains(aabb, ctx, cfg, solid);
	}


	AABB getTerrainSpaceAABB(const Vec3& terrain_pos, const Quat& terrain_rot, const AABB& aabb_world_space)
	{
		Matrix mtx;
		terrain_rot.toMatrix(mtx);
		mtx.setTranslation(terrain_pos);
		mtx.fastInverse();
		AABB ret = aabb_world_space;
		ret.transform(mtx);
		return ret;
	}


	void rasterizeTerrains(const AABB& aabb, rcContext& ctx, rcConfig& cfg, rcHeightfield& solid)
	{
		PROFILE_FUNCTION();
		const float walkable_threshold = cosf(Math::degreesToRadians(60));

		auto render_scene = static_cast<RenderScene*>(m_universe.getScene(crc32("renderer")));
		if (!render_scene) return;

		ComponentHandle cmp = render_scene->getFirstTerrain();
		while (cmp != INVALID_COMPONENT)
		{
			Entity entity = render_scene->getTerrainEntity(cmp);
			Vec3 pos = m_universe.getPosition(entity);
			Quat rot = m_universe.getRotation(entity);
			Vec2 res = render_scene->getTerrainResolution(cmp);
			float scaleXZ = render_scene->getTerrainXZScale(cmp);
			AABB terrain_space_aabb = getTerrainSpaceAABB(pos, rot, aabb);
			int from_z = (int)Math::clamp(terrain_space_aabb.min.z / scaleXZ - 1, 0.0f, res.y - 1);
			int to_z = (int)Math::clamp(terrain_space_aabb.max.z / scaleXZ + 1, 0.0f, res.y - 1);
			int from_x = (int)Math::clamp(terrain_space_aabb.min.x / scaleXZ - 1, 0.0f, res.x - 1);
			int to_x = (int)Math::clamp(terrain_space_aabb.max.x / scaleXZ + 1, 0.0f, res.x - 1);
			for (int j = from_z; j < to_z; ++j)
			{
				for (int i = from_x; i < to_x; ++i)
				{
					float x = i * scaleXZ;
					float z = j * scaleXZ;
					float h0 = render_scene->getTerrainHeightAt(cmp, x, z);
					Vec3 p0 = pos + rot * Vec3(x, h0, z);

					x = (i + 1) * scaleXZ;
					z = j * scaleXZ;
					float h1 = render_scene->getTerrainHeightAt(cmp, x, z);
					Vec3 p1 = pos + rot * Vec3(x, h1, z);

					x = (i + 1) * scaleXZ;
					z = (j + 1) * scaleXZ;
					float h2 = render_scene->getTerrainHeightAt(cmp, x, z);
					Vec3 p2 = pos + rot * Vec3(x, h2, z);

					x = i * scaleXZ;
					z = (j + 1) * scaleXZ;
					float h3 = render_scene->getTerrainHeightAt(cmp, x, z);
					Vec3 p3 = pos + rot * Vec3(x, h3, z);

					Vec3 n = crossProduct(p1 - p0, p0 - p2).normalized();
					uint8 area = n.y > walkable_threshold ? RC_WALKABLE_AREA : 0;
					rcRasterizeTriangle(&ctx, &p0.x, &p1.x, &p2.x, area, solid);

					n = crossProduct(p2 - p0, p0 - p3).normalized();
					area = n.y > walkable_threshold ? RC_WALKABLE_AREA : 0;
					rcRasterizeTriangle(&ctx, &p0.x, &p2.x, &p3.x, area, solid);
				}
			}

			cmp = render_scene->getNextTerrain(cmp);
		}
	}


	void rasterizeMeshes(const AABB& aabb, rcContext& ctx, rcConfig& cfg, rcHeightfield& solid)
	{
		PROFILE_FUNCTION();
		const float walkable_threshold = cosf(Math::degreesToRadians(45));

		auto render_scene = static_cast<RenderScene*>(m_universe.getScene(crc32("renderer")));
		if (!render_scene) return;

		uint32 no_navigation_flag = Material::getCustomFlag("no_navigation");
		uint32 nonwalkable_flag = Material::getCustomFlag("nonwalkable");
		for (auto renderable = render_scene->getFirstRenderable(); renderable != INVALID_COMPONENT;
			 renderable = render_scene->getNextRenderable(renderable))
		{
			auto* model = render_scene->getRenderableModel(renderable);
			if (!model) return;
			ASSERT(model->isReady());

			auto& indices = model->getIndices();
			bool is16 = model->getFlags() & (uint32)Model::Flags::INDICES_16BIT;

			Entity entity = render_scene->getRenderableEntity(renderable);
			Matrix mtx = m_universe.getMatrix(entity);
			AABB model_aabb = model->getAABB();
			model_aabb.transform(mtx);
			if (!model_aabb.overlaps(aabb)) continue;

			auto lod = model->getLODMeshIndices(0);
			for (int mesh_idx = lod.from; mesh_idx <= lod.to; ++mesh_idx)
			{
				auto& mesh = model->getMesh(mesh_idx);
				if (mesh.material->isCustomFlag(no_navigation_flag)) continue;
				bool is_walkable = !mesh.material->isCustomFlag(nonwalkable_flag);
				auto* vertices =
					&model->getVertices()[mesh.attribute_array_offset / model->getVertexDecl().getStride()];
				if (is16)
				{
					uint16* indices16 = (uint16*)&model->getIndices()[0];
					for (int i = 0; i < mesh.indices_count; i += 3)
					{
						Vec3 a = mtx.multiplyPosition(vertices[indices16[mesh.indices_offset + i]]);
						Vec3 b = mtx.multiplyPosition(vertices[indices16[mesh.indices_offset + i + 1]]);
						Vec3 c = mtx.multiplyPosition(vertices[indices16[mesh.indices_offset + i + 2]]);

						Vec3 n = crossProduct(a - b, a - c).normalized();
						uint8 area = n.y > walkable_threshold && is_walkable ? RC_WALKABLE_AREA : 0;
						rcRasterizeTriangle(&ctx, &a.x, &b.x, &c.x, area, solid);
					}
				}
				else
				{
					uint32* indices32 = (uint32*)&model->getIndices()[0];
					for (int i = 0; i < mesh.indices_count; i += 3)
					{
						Vec3 a = mtx.multiplyPosition(vertices[indices32[mesh.indices_offset + i]]);
						Vec3 b = mtx.multiplyPosition(vertices[indices32[mesh.indices_offset + i + 1]]);
						Vec3 c = mtx.multiplyPosition(vertices[indices32[mesh.indices_offset + i + 2]]);

						Vec3 n = crossProduct(a - b, a - c).normalized();
						uint8 area = n.y > walkable_threshold && is_walkable ? RC_WALKABLE_AREA : 0;
						rcRasterizeTriangle(&ctx, &a.x, &b.x, &c.x, area, solid);
					}
				}
			}
		}
	}


	void onPathFinished(Agent* agent)
	{
		if (!m_script_scene) return;
		
		auto cmp = m_script_scene->getComponent(agent->entity);
		if (cmp == INVALID_COMPONENT) return;

		for (int i = 0, c = m_script_scene->getScriptCount(cmp); i < c; ++i)
		{
			auto* call = m_script_scene->beginFunctionCall(cmp, i, "onPathFinished");
			if (!call) continue;

			m_script_scene->endFunctionCall(*call);
		}
	}


	void update(float time_delta, bool paused) override
	{
		PROFILE_FUNCTION();
		if (!m_crowd) return;
		m_crowd->update(time_delta, nullptr);

		for (auto* agent : m_agents)
		{
			const dtCrowdAgent* dt_agent = m_crowd->getAgent(agent->agent);
			m_universe.setPosition(agent->entity, *(Vec3*)dt_agent->npos);
			Vec3 velocity = *(Vec3*)dt_agent->vel;
			float speed = velocity.length();
			if (speed > 0)
			{
				velocity *= 1 / speed;
				float yaw = atan2(velocity.x, velocity.z);
				Quat rot(Vec3(0, 1, 0), yaw);
				m_universe.setRotation(agent->entity, rot);
			}

			if (dt_agent->ncorners == 0)
			{
				if (!agent->is_finished)
				{
					m_crowd->resetMoveTarget(agent->agent);
					agent->is_finished = true;
					onPathFinished(agent);
				}
			}
			else
			{
				agent->is_finished = false;
			}
		}
	}


	void debugDrawPath(Entity entity)
	{
		auto render_scene = static_cast<RenderScene*>(m_universe.getScene(crc32("renderer")));
		if (!render_scene) return;
		if (!m_crowd) return;

		auto iter = m_agents.find(entity);
		if (iter == m_agents.end()) return;
		Agent* agent = iter.value();

		const dtCrowdAgent* dt_agent = m_crowd->getAgent(agent->agent);
		const dtPolyRef* path = dt_agent->corridor.getPath();
		const int npath = dt_agent->corridor.getPathCount();
		for (int j = 0; j < npath; ++j)
		{
			dtPolyRef ref = path[j];
			const dtMeshTile* tile = 0;
			const dtPoly* poly = 0;
			if (dtStatusFailed(m_navmesh->getTileAndPolyByRef(ref, &tile, &poly))) continue;

			const unsigned int ip = (unsigned int)(poly - tile->polys);

			const dtPolyDetail* pd = &tile->detailMeshes[ip];

			for (int i = 0; i < pd->triCount; ++i)
			{
				Vec3 v[3];
				const unsigned char* t = &tile->detailTris[(pd->triBase + i) * 4];
				for (int k = 0; k < 3; ++k)
				{
					if (t[k] < poly->vertCount)
					{
						v[k] = *(Vec3*)&tile->verts[poly->verts[t[k]] * 3];
					}
					else
					{
						v[k] = *(Vec3*)&tile->detailVerts[(pd->vertBase + t[k] - poly->vertCount) * 3];
					}
				}
				render_scene->addDebugTriangle(v[0], v[1], v[2], 0xffff00ff, 0);
				render_scene->addDebugLine(v[0], v[1], 0x0000ffff, 0);
				render_scene->addDebugLine(v[1], v[2], 0x0000ffff, 0);
				render_scene->addDebugLine(v[2], v[0], 0x0000ffff, 0);
			}
		}

		render_scene->addDebugCross(*(Vec3*)dt_agent->targetPos, 1.0f, 0xffffffff, 0);
	}


	void debugDrawContours() override
	{
		auto render_scene = static_cast<RenderScene*>(m_universe.getScene(crc32("renderer")));
		if (!render_scene) return;
		if (!m_debug_contours) return;

		Vec3 orig = m_debug_tile_origin;
		float cs = m_debug_contours->cs;
		float ch = m_debug_contours->ch;
		for (int i = 0; i < m_debug_contours->nconts; ++i)
		{
			const rcContour& c = m_debug_contours->conts[i];

			if (c.nverts < 2) continue;

			Vec3 first =
				orig + Vec3((float)c.verts[0] * cs, (float)c.verts[1] * ch, (float)c.verts[2] * cs);
			Vec3 prev = first;
			for (int j = 1; j < c.nverts; ++j)
			{
				const int* v = &c.verts[j * 4];
				Vec3 cur = orig + Vec3((float)v[0] * cs, (float)v[1] * ch, (float)v[2] * cs);
				render_scene->addDebugLine(prev, cur, i & 1 ? 0xffff00ff : 0xffff0000, 0);
				prev = cur;
			}

			render_scene->addDebugLine(prev, first, i & 1 ? 0xffff00ff : 0xffff0000, 0);
		}
	}


	bool isNavmeshReady() const override
	{
		return m_navmesh != nullptr;
	}


	bool load(const char* path) override
	{
		clear();

		FS::OsFile file;
		if (!file.open(path, FS::Mode::OPEN_AND_READ, m_allocator)) return false;

		if (!initNavmesh()) return false;

		file.read(&m_aabb, sizeof(m_aabb));
		file.read(&m_num_tiles_x, sizeof(m_num_tiles_x));
		file.read(&m_num_tiles_z, sizeof(m_num_tiles_z));
		dtNavMeshParams params;
		file.read(&params, sizeof(params));
		if (dtStatusFailed(m_navmesh->init(&params)))
		{
			g_log_error.log("Navigation") << "Could not init Detour navmesh";
			return false;
		}
		for (int j = 0; j < m_num_tiles_z; ++j)
		{
			for (int i = 0; i < m_num_tiles_x; ++i)
			{
				int data_size;
				file.read(&data_size, sizeof(data_size));
				uint8* data = (uint8*)dtAlloc(data_size, DT_ALLOC_PERM);
				file.read(data, data_size);
				if (dtStatusFailed(m_navmesh->addTile(data, data_size, DT_TILE_FREE_DATA, 0, 0)))
				{
					file.close();
					dtFree(data);
					return false;
				}
			}
		}

		file.close();
		if (!m_crowd) return initCrowd();
		return true;
	}

	
	bool save(const char* path) override
	{
		if (!m_navmesh) return false;

		FS::OsFile file;
		if (!file.open(path, FS::Mode::CREATE_AND_WRITE, m_allocator)) return false;

		file.write(&m_aabb, sizeof(m_aabb));
		file.write(&m_num_tiles_x, sizeof(m_num_tiles_x));
		file.write(&m_num_tiles_z, sizeof(m_num_tiles_z));
		auto params = m_navmesh->getParams();
		file.write(params, sizeof(*params));
		for (int j = 0; j < m_num_tiles_z; ++j)
		{
			for (int i = 0; i < m_num_tiles_x; ++i)
			{
				const auto* tile = m_navmesh->getTileAt(i, j, 0);
				file.write(&tile->dataSize, sizeof(tile->dataSize));
				file.write(tile->data, tile->dataSize);
			}
		}

		file.close();
		return true;
	}


	void debugDrawHeightfield() override
	{
		auto render_scene = static_cast<RenderScene*>(m_universe.getScene(crc32("renderer")));
		if (!render_scene) return;
		if (!m_debug_heightfield) return;

		Vec3 orig = m_debug_tile_origin;
		int width = m_debug_heightfield->width;
		float cell_height = 0.1f;
		int rendered_cubes = 0;
		for(int z = 0; z < m_debug_heightfield->height; ++z)
		{
			for(int x = 0; x < width; ++x)
			{
				float fx = orig.x + x * CELL_SIZE;
				float fz = orig.z + z * CELL_SIZE;
				const rcSpan* span = m_debug_heightfield->spans[x + z * width];
				while(span)
				{
					Vec3 mins(fx, orig.y + span->smin * cell_height, fz);
					Vec3 maxs(fx + CELL_SIZE, orig.y + span->smax * cell_height, fz + CELL_SIZE);
					render_scene->addDebugCubeSolid(mins, maxs, 0xffff00ff, 0);
					render_scene->addDebugCube(mins, maxs, 0xff00aaff, 0);
					span = span->next;
				}
			}
		}
	}


	void debugDrawCompactHeightfield() override
	{
		static const int MAX_CUBES = 0xffFF;
		
		auto render_scene = static_cast<RenderScene*>(m_universe.getScene(crc32("renderer")));
		if (!render_scene) return;
		if (!m_debug_compact_heightfield) return;

		auto& chf = *m_debug_compact_heightfield;
		const float cs = chf.cs;
		const float ch = chf.ch;

		Vec3 orig = m_debug_tile_origin;

		int rendered_cubes = 0;
		for (int y = 0; y < chf.height; ++y)
		{
			for (int x = 0; x < chf.width; ++x)
			{
				float vx = orig.x + (float)x * cs;
				float vz = orig.z + (float)y * cs;

				const rcCompactCell& c = chf.cells[x + y * chf.width];

				for (uint32 i = c.index, ni = c.index + c.count; i < ni; ++i)
				{
					float vy = orig.y + float(chf.spans[i].y) * ch;
					render_scene->addDebugTriangle(
						Vec3(vx, vy, vz), Vec3(vx + cs, vy, vz + cs), Vec3(vx + cs, vy, vz), 0xffff00FF, 0);
					render_scene->addDebugTriangle(
						Vec3(vx, vy, vz), Vec3(vx, vy, vz + cs), Vec3(vx + cs, vy, vz + cs), 0xffff00FF, 0);
					++rendered_cubes;
					if (rendered_cubes > MAX_CUBES) return;
				}
			}
		}
	}


	void debugDrawNavmesh() override
	{
		if (!m_polymesh) return;
		auto& mesh = *m_polymesh;
		auto render_scene = static_cast<RenderScene*>(m_universe.getScene(crc32("renderer")));
		if (!render_scene) return;

		const int nvp = mesh.nvp;
		const float cs = mesh.cs;
		const float ch = mesh.ch;

		Vec3 color(0, 0, 0);

		for (int idx = 0; idx < mesh.npolys; ++idx)
		{
			const auto* p = &mesh.polys[idx * nvp * 2];

			if (mesh.areas[idx] == RC_WALKABLE_AREA) color.set(0, 0.8f, 1.0f);

			Vec3 vertices[6];
			Vec3* vert = vertices;
			for (int j = 0; j < nvp; ++j)
			{
				if (p[j] == RC_MESH_NULL_IDX) break;

				const auto* v = &mesh.verts[p[j] * 3];
				vert->set(v[0] * cs + mesh.bmin[0], (v[1] + 1) * ch + mesh.bmin[1], v[2] * cs + mesh.bmin[2]);
				++vert;
			}
			for(int i = 2; i < vert - vertices; ++i)
			{
				render_scene->addDebugTriangle(vertices[0], vertices[i-1], vertices[i], 0xff00aaff, 0);
			}
			for(int i = 1; i < vert - vertices; ++i)
			{
				render_scene->addDebugLine(vertices[i], vertices[i-1], 0xff0000ff, 0);
			}
			render_scene->addDebugLine(vertices[0], vertices[vert - vertices - 1], 0xff0000ff, 0);
		}
	}


	void stopGame() override
	{
		if (m_crowd)
		{
			for (auto* agent : m_agents)
			{
				m_crowd->removeAgent(agent->agent);
				agent->agent = -1;
			}
			dtFreeCrowd(m_crowd);
			m_crowd = nullptr;
		}
	}


	void startGame() override
	{
		auto* scene = m_universe.getScene(crc32("lua_script"));
		m_script_scene = static_cast<LuaScriptScene*>(scene);
		
		scene = m_universe.getScene(crc32("physics"));
		m_physics_scene = static_cast<PhysicsScene*>(scene);

		if (m_navmesh && !m_crowd) initCrowd();
	}


	bool initCrowd()
	{
		ASSERT(!m_crowd);

		m_crowd = dtAllocCrowd();
		if (!m_crowd->init(1000, 4.0f, m_navmesh))
		{
			dtFreeCrowd(m_crowd);
			m_crowd = nullptr;
			return false;
		}
		for (auto iter = m_agents.begin(), end = m_agents.end(); iter != end; ++iter)
		{
			Agent* agent = iter.value();
			addCrowdAgent(agent);
		}

		return true;
	}


	bool navigate(Entity entity, const Vec3& dest, float speed)
	{
		if (!m_navquery) return false;
		if (!m_crowd) return false;
		if (entity == INVALID_ENTITY) return false;
		auto iter = m_agents.find(entity);
		if (iter == m_agents.end()) return false;
		Agent& agent = *iter.value();
		dtPolyRef end_poly_ref;
		dtQueryFilter filter;
		static const float ext[] = { 1.0f, 2.0f, 1.0f };
		m_navquery->findNearestPoly(&dest.x, ext, &filter, &end_poly_ref, 0);
		dtCrowdAgentParams params = m_crowd->getAgent(agent.agent)->params;
		params.maxSpeed = speed;
		m_crowd->updateAgentParameters(agent.agent, &params);
		return m_crowd->requestMoveTarget(agent.agent, end_poly_ref, &dest.x);
	}


	int getPolygonCount()
	{
		if (!m_polymesh) return 0;
		return m_polymesh->npolys;
	}


	void setGeneratorParams(float cell_size,
		float cell_height,
		float agent_radius,
		float agent_height,
		float walkable_angle,
		float max_climb)
	{
		static const float DETAIL_SAMPLE_DIST = 6;
		static const float DETAIL_SAMPLE_MAX_ERROR = 1;

		m_config.cs = cell_size;
		m_config.ch = cell_height;
		m_config.walkableSlopeAngle = walkable_angle;
		m_config.walkableHeight = (int)(agent_height / m_config.ch + 0.99f);
		m_config.walkableClimb = (int)(max_climb / m_config.ch);
		m_config.walkableRadius = (int)(agent_radius / m_config.cs + 0.99f);
		m_config.maxEdgeLen = (int)(12 / m_config.cs);
		m_config.maxSimplificationError = 1.3f;
		m_config.minRegionArea = 8 * 8;
		m_config.mergeRegionArea = 20 * 20;
		m_config.maxVertsPerPoly = 6;
		m_config.detailSampleDist = DETAIL_SAMPLE_DIST < 0.9f ? 0 : CELL_SIZE * DETAIL_SAMPLE_DIST;
		m_config.detailSampleMaxError = m_config.ch * DETAIL_SAMPLE_MAX_ERROR;
		m_config.borderSize = m_config.walkableRadius + 3;
		m_config.tileSize = CELLS_PER_TILE_SIDE;
		m_config.width = m_config.tileSize + m_config.borderSize * 2;
		m_config.height = m_config.tileSize + m_config.borderSize * 2;
	}


	bool generateTileAt(const Vec3& pos, bool keep_data) override
	{
		int x = int((pos.x - m_aabb.min.x + (1 + m_config.borderSize) * m_config.cs) / (CELLS_PER_TILE_SIDE * CELL_SIZE));
		int z = int((pos.z - m_aabb.min.z + (1 + m_config.borderSize) * m_config.cs) / (CELLS_PER_TILE_SIDE * CELL_SIZE));
		return generateTile(x, z, keep_data);
	}


	bool generateTile(int x, int z, bool keep_data) override
	{
		PROFILE_FUNCTION();
		if (!m_navmesh) return false;
		m_navmesh->removeTile(m_navmesh->getTileRefAt(x, z, 0), 0, 0);

		rcContext ctx;

		Vec3 bmin(m_aabb.min.x + x * CELLS_PER_TILE_SIDE * CELL_SIZE - (1 + m_config.borderSize) * m_config.cs,
			m_aabb.min.y,
			m_aabb.min.z + z * CELLS_PER_TILE_SIDE * CELL_SIZE - (1 + m_config.borderSize) * m_config.cs);
		Vec3 bmax(bmin.x + CELLS_PER_TILE_SIDE * CELL_SIZE + (1 + m_config.borderSize) * m_config.cs,
			m_aabb.max.y,
			bmin.z + CELLS_PER_TILE_SIDE * CELL_SIZE + (1 + m_config.borderSize) * m_config.cs);
		if (keep_data) m_debug_tile_origin = bmin;
		rcVcopy(m_config.bmin, &bmin.x);
		rcVcopy(m_config.bmax, &bmax.x);
		rcHeightfield* solid = rcAllocHeightfield();
		m_debug_heightfield = keep_data ? solid : nullptr;
		if (!solid)
		{
			g_log_error.log("Navigation") << "Could not generate navmesh: Out of memory 'solid'.";
			return false;
		}
		if (!rcCreateHeightfield(
				&ctx, *solid, m_config.width, m_config.height, m_config.bmin, m_config.bmax, m_config.cs, m_config.ch))
		{
			g_log_error.log("Navigation") << "Could not generate navmesh: Could not create solid heightfield.";
			return false;
		}
		rasterizeGeometry(AABB(bmin, bmax), ctx, m_config, *solid);

		rcFilterLowHangingWalkableObstacles(&ctx, m_config.walkableClimb, *solid);
		rcFilterLedgeSpans(&ctx, m_config.walkableHeight, m_config.walkableClimb, *solid);
		rcFilterWalkableLowHeightSpans(&ctx, m_config.walkableHeight, *solid);

		rcCompactHeightfield* chf = rcAllocCompactHeightfield();
		m_debug_compact_heightfield = keep_data ? chf : nullptr;
		if (!chf)
		{
			g_log_error.log("Navigation") << "Could not generate navmesh: Out of memory 'chf'.";
			return false;
		}

		if (!rcBuildCompactHeightfield(&ctx, m_config.walkableHeight, m_config.walkableClimb, *solid, *chf))
		{
			g_log_error.log("Navigation") << "Could not generate navmesh: Could not build compact data.";
			return false;
		}

		if (!m_debug_heightfield) rcFreeHeightField(solid);

		if (!rcErodeWalkableArea(&ctx, m_config.walkableRadius, *chf))
		{
			g_log_error.log("Navigation") << "Could not generate navmesh: Could not erode.";
			return false;
		}

		if (!rcBuildDistanceField(&ctx, *chf))
		{
			g_log_error.log("Navigation") << "Could not generate navmesh: Could not build distance field.";
			return false;
		}

		if (!rcBuildRegions(&ctx, *chf, m_config.borderSize, m_config.minRegionArea, m_config.mergeRegionArea))
		{
			g_log_error.log("Navigation") << "Could not generate navmesh: Could not build regions.";
			return false;
		}

		rcContourSet* cset = rcAllocContourSet();
		m_debug_contours = keep_data ? cset : nullptr;
		if (!cset)
		{
			ctx.log(RC_LOG_ERROR, "Could not generate navmesh: Out of memory 'cset'.");
			return false;
		}
		if (!rcBuildContours(&ctx, *chf, m_config.maxSimplificationError, m_config.maxEdgeLen, *cset))
		{
			g_log_error.log("Navigation") << "Could not generate navmesh: Could not create contours.";
			return false;
		}

		m_polymesh = rcAllocPolyMesh();
		if (!m_polymesh)
		{
			g_log_error.log("Navigation") << "Could not generate navmesh: Out of memory 'm_polymesh'.";
			return false;
		}
		if (!rcBuildPolyMesh(&ctx, *cset, m_config.maxVertsPerPoly, *m_polymesh))
		{
			g_log_error.log("Navigation") << "Could not generate navmesh: Could not triangulate contours.";
			return false;
		}

		m_detail_mesh = rcAllocPolyMeshDetail();
		if (!m_detail_mesh)
		{
			g_log_error.log("Navigation") << "Could not generate navmesh: Out of memory 'pmdtl'.";
			return false;
		}

		if (!rcBuildPolyMeshDetail(
				&ctx, *m_polymesh, *chf, m_config.detailSampleDist, m_config.detailSampleMaxError, *m_detail_mesh))
		{
			g_log_error.log("Navigation") << "Could not generate navmesh: Could not build detail mesh.";
			return false;
		}

		if (!m_debug_compact_heightfield) rcFreeCompactHeightfield(chf);
		if (!m_debug_contours) rcFreeContourSet(cset);

		unsigned char* nav_data = 0;
		int nav_data_size = 0;

		for (int i = 0; i < m_polymesh->npolys; ++i)
		{
			m_polymesh->flags[i] = m_polymesh->areas[i] == RC_WALKABLE_AREA ? 1 : 0;
		}

		dtNavMeshCreateParams params = {};
		params.verts = m_polymesh->verts;
		params.vertCount = m_polymesh->nverts;
		params.polys = m_polymesh->polys;
		params.polyAreas = m_polymesh->areas;
		params.polyFlags = m_polymesh->flags;
		params.polyCount = m_polymesh->npolys;
		params.nvp = m_polymesh->nvp;
		params.detailMeshes = m_detail_mesh->meshes;
		params.detailVerts = m_detail_mesh->verts;
		params.detailVertsCount = m_detail_mesh->nverts;
		params.detailTris = m_detail_mesh->tris;
		params.detailTriCount = m_detail_mesh->ntris;
		params.walkableHeight = (float)m_config.walkableHeight;
		params.walkableRadius = (float)m_config.walkableRadius;
		params.walkableClimb = (float)m_config.walkableClimb;
		params.tileX = x;
		params.tileY = z;
		rcVcopy(params.bmin, m_polymesh->bmin);
		rcVcopy(params.bmax, m_polymesh->bmax);
		params.cs = m_config.cs;
		params.ch = m_config.ch;
		params.buildBvTree = false;

		if (!dtCreateNavMeshData(&params, &nav_data, &nav_data_size))
		{
			g_log_error.log("Navigation") << "Could not build Detour navmesh.";
			return false;
		}
		if (dtStatusFailed(m_navmesh->addTile(nav_data, nav_data_size, DT_TILE_FREE_DATA, 0, nullptr)))
		{
			g_log_error.log("Navigation") << "Could not add Detour tile.";
			return false;
		}
		return true;
	}


	void computeAABB()
	{
		m_aabb.set(Vec3(0, 0, 0), Vec3(0, 0, 0));
		auto* render_scene = static_cast<RenderScene*>(m_universe.getScene(crc32("renderer")));
		if (!render_scene) return;

		for (auto renderable = render_scene->getFirstRenderable(); renderable != INVALID_COMPONENT;
			renderable = render_scene->getNextRenderable(renderable))
		{
			auto* model = render_scene->getRenderableModel(renderable);
			if (!model) continue;
			ASSERT(model->isReady());

			AABB model_bb = model->getAABB();
			Matrix mtx = m_universe.getMatrix(render_scene->getRenderableEntity(renderable));
			model_bb.transform(mtx);
			m_aabb.merge(model_bb);
		}

		ComponentHandle cmp = render_scene->getFirstTerrain();
		while (cmp != INVALID_COMPONENT)
		{
			AABB terrain_aabb = render_scene->getTerrainAABB(cmp);
			Matrix mtx = m_universe.getMatrix(render_scene->getTerrainEntity(cmp));
			terrain_aabb.transform(mtx);
			m_aabb.merge(terrain_aabb);

			cmp = render_scene->getNextTerrain(cmp);
		}
	}


	bool initNavmesh()
	{
		m_navmesh = dtAllocNavMesh();
		if (!m_navmesh)
		{
			g_log_error.log("Navigation") << "Could not create Detour navmesh";
			return false;
		}

		m_navquery = dtAllocNavMeshQuery();
		if (!m_navquery)
		{
			g_log_error.log("Navigation") << "Could not create Detour navmesh query";
			return false;
		}
		if (dtStatusFailed(m_navquery->init(m_navmesh, 2048)))
		{
			g_log_error.log("Navigation") << "Could not init Detour navmesh query";
			return false;
		}
		return true;
	}


	bool generateNavmesh() override
	{
		PROFILE_FUNCTION();
		clear();

		if (!initNavmesh()) return false;

		computeAABB();
		dtNavMeshParams params;
		rcVcopy(params.orig, &m_aabb.min.x);
		params.tileWidth = float(CELLS_PER_TILE_SIDE * CELL_SIZE);
		params.tileHeight = float(CELLS_PER_TILE_SIDE * CELL_SIZE);
		int grid_width, grid_height;
		rcCalcGridSize(&m_aabb.min.x, &m_aabb.max.x, CELL_SIZE, &grid_width, &grid_height);
		m_num_tiles_x = (grid_width + CELLS_PER_TILE_SIDE - 1) / CELLS_PER_TILE_SIDE;
		m_num_tiles_z = (grid_height + CELLS_PER_TILE_SIDE - 1) / CELLS_PER_TILE_SIDE;
		params.maxTiles = m_num_tiles_x * m_num_tiles_z;
		int tiles_bits = Math::log2(Math::nextPow2(params.maxTiles));
		params.maxPolys = 1 << (22 - tiles_bits); // keep 10 bits for salt

		if (dtStatusFailed(m_navmesh->init(&params)))
		{
			g_log_error.log("Navigation") << "Could not init Detour navmesh";
			return false;
		}

		for (int j = 0; j < m_num_tiles_z; ++j)
		{
			for (int i = 0; i < m_num_tiles_x; ++i)
			{
				if (!generateTile(i, j, false))
				{
					return false;
				}
			}
		}
		return true;
	}


	void addCrowdAgent(Agent* agent)
	{
		ASSERT(m_crowd);

		Vec3 pos = m_universe.getPosition(agent->entity);
		dtCrowdAgentParams params = {};
		params.radius = agent->radius;
		params.height = agent->height;
		params.maxAcceleration = 10.0f;
		params.maxSpeed = 10.0f;
		params.collisionQueryRange = params.radius * 12.0f;
		params.pathOptimizationRange = params.radius * 30.0f;
		params.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_SEPARATION | DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_OPTIMIZE_TOPO | DT_CROWD_OPTIMIZE_VIS;
		agent->agent = m_crowd->addAgent(&pos.x, &params);
	}


	ComponentHandle createComponent(ComponentType type, Entity entity) override
	{
		if (type == NAVMESH_AGENT_TYPE)
		{
			Agent* agent = LUMIX_NEW(m_allocator, Agent);
			agent->entity = entity;
			agent->radius = 0.5f;
			agent->height = 2.0f;
			agent->agent = -1;
			agent->is_finished = true;
			if (m_crowd) addCrowdAgent(agent);
			m_agents.insert(entity, agent);
			ComponentHandle cmp = {entity.index};
			m_universe.addComponent(entity, type, this, cmp);
			return cmp;
		}
		return INVALID_COMPONENT;
	}


	void destroyComponent(ComponentHandle component, ComponentType type) override
	{
		if (type == NAVMESH_AGENT_TYPE)
		{
			Entity entity = { component.index };
			auto iter = m_agents.find(entity);
			Agent* agent = iter.value();
			if (m_crowd && agent->agent >= 0) m_crowd->removeAgent(agent->agent);
			LUMIX_DELETE(m_allocator, iter.value());
			m_agents.erase(iter);
			m_universe.destroyComponent(entity, type, this, component);
		}
		else
		{
			ASSERT(false);
		}
	}


	void serialize(OutputBlob& serializer) override
	{
		int count = m_agents.size();
		serializer.write(count);
		for (auto iter = m_agents.begin(), end = m_agents.end(); iter != end; ++iter)
		{
			serializer.write(iter.key());
			serializer.write(iter.value()->radius);
			serializer.write(iter.value()->height);
		}
	}


	void deserialize(InputBlob& serializer, int version) override
	{
		for (auto* agent : m_agents) LUMIX_DELETE(m_allocator, agent);
		m_agents.clear();
		if (version > (int)Version::AGENTS)
		{
			int count = 0;
			serializer.read(count);
			for (int i = 0; i < count; ++i)
			{
				Agent* agent = LUMIX_NEW(m_allocator, Agent);
				Entity entity;
				serializer.read(entity);
				serializer.read(agent->radius);
				serializer.read(agent->height);
				agent->agent = -1;
				m_agents.insert(entity, agent);
				ComponentHandle cmp = { entity.index };
				m_universe.addComponent(entity, NAVMESH_AGENT_TYPE, this, cmp);
			}
		}
	}


	void setAgentRadius(ComponentHandle cmp, float radius)
	{
		Entity entity = {cmp.index};
		m_agents[entity]->radius = radius;
	}


	float getAgentRadius(ComponentHandle cmp)
	{
		Entity entity = { cmp.index };
		return m_agents[entity]->radius;
	}


	void setAgentHeight(ComponentHandle cmp, float height)
	{
		Entity entity = { cmp.index };
		m_agents[entity]->height = height;
	}


	float getAgentHeight(ComponentHandle cmp)
	{
		Entity entity = {cmp.index};
		return m_agents[entity]->height;
	}


	IPlugin& getPlugin() const override { return m_system; }
	bool ownComponentType(ComponentType type) const override { return type == NAVMESH_AGENT_TYPE; }
	ComponentHandle getComponent(Entity entity, ComponentType type) override
	{
		if (type == NAVMESH_AGENT_TYPE) return {entity.index};
		return INVALID_COMPONENT;
	}
	Universe& getUniverse() override { return m_universe; }

	IAllocator& m_allocator;
	Universe& m_universe;
	NavigationSystem& m_system;
	rcPolyMesh* m_polymesh;
	dtNavMesh* m_navmesh;
	dtNavMeshQuery* m_navquery;
	rcPolyMeshDetail* m_detail_mesh;
	HashMap<Entity, Agent*> m_agents;
	int m_first_free_agent;
	rcCompactHeightfield* m_debug_compact_heightfield;
	rcHeightfield* m_debug_heightfield;
	rcContourSet* m_debug_contours;
	Vec3 m_debug_tile_origin;
	AABB m_aabb;
	rcConfig m_config;
	int m_num_tiles_x;
	int m_num_tiles_z;
	LuaScriptScene* m_script_scene;
	PhysicsScene* m_physics_scene;
	dtCrowd* m_crowd;
};


void NavigationSystem::registerProperties()
{
	auto& allocator = m_engine.getAllocator();
	PropertyRegister::add("navmesh_agent",
		LUMIX_NEW(allocator, DecimalPropertyDescriptor<NavigationSceneImpl>)(
			"radius", &NavigationSceneImpl::getAgentRadius, &NavigationSceneImpl::setAgentRadius, 0, 999.0f, 0.1f, allocator));
	PropertyRegister::add("navmesh_agent",
		LUMIX_NEW(allocator, DecimalPropertyDescriptor<NavigationSceneImpl>)(
			"height", &NavigationSceneImpl::getAgentHeight, &NavigationSceneImpl::setAgentHeight, 0, 999.0f, 0.1f, allocator));
}


IScene* NavigationSystem::createScene(Universe& universe)
{
	return LUMIX_NEW(m_allocator, NavigationSceneImpl)(*this, universe, m_allocator);
}


void NavigationSystem::destroyScene(IScene* scene)
{
	LUMIX_DELETE(m_allocator, scene);
}



LUMIX_PLUGIN_ENTRY(navigation)
{
	return LUMIX_NEW(engine.getAllocator(), NavigationSystem)(engine);
}


static void registerLuaAPI(lua_State* L)
{
	#define REGISTER_FUNCTION(name) \
		do {\
			auto f = &LuaWrapper::wrapMethod<NavigationSceneImpl, decltype(&NavigationSceneImpl::name), &NavigationSceneImpl::name>; \
			LuaWrapper::createSystemFunction(L, "Navigation", #name, f); \
		} while(false) \

	REGISTER_FUNCTION(generateNavmesh);
	REGISTER_FUNCTION(navigate);
	REGISTER_FUNCTION(debugDrawNavmesh);
	REGISTER_FUNCTION(debugDrawCompactHeightfield);
	REGISTER_FUNCTION(debugDrawHeightfield);
	REGISTER_FUNCTION(debugDrawPath);
	REGISTER_FUNCTION(getPolygonCount);
	REGISTER_FUNCTION(debugDrawContours);
	REGISTER_FUNCTION(generateTile);
	REGISTER_FUNCTION(save);
	REGISTER_FUNCTION(load);
	REGISTER_FUNCTION(setGeneratorParams);

	#undef REGISTER_FUNCTION
}


} // namespace Lumix
