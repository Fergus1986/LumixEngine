#include "engine/lumix.h"
#include "engine/iplugin.h"


namespace Lumix
{


class NavigationScene : public IScene
{
public:
	virtual bool generateNavmesh() = 0;
	virtual bool generateTile(int x, int z, bool keep_data) = 0;
	virtual bool generateTileAt(const Vec3& pos, bool keep_data) = 0;
	virtual bool load(const char* path) = 0;
	virtual bool save(const char* path) = 0;
	virtual void debugDrawNavmesh() = 0;
	virtual void debugDrawCompactHeightfield() = 0;
	virtual void debugDrawHeightfield() = 0;
	virtual void debugDrawContours() = 0;
	virtual bool isNavmeshReady() const = 0;
	virtual void debugDrawPath(Entity entity) = 0;
};


} // namespace Lumix
