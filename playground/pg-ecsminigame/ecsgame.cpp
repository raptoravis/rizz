#include <cstdint>
#include <entt/entt.hpp>
#include <iostream>
#include <string>

#include "./buffering.h"

#include "./game.h"

const int kObjectCount = 10000;
const int kAvoidCount = 20;


static float RandomFloat01()
{
    return (float)rand() / (float)RAND_MAX;
}
static float RandomFloat(float from, float to)
{
    return RandomFloat01() * (to - from) + from;
}


// -------------------------------------------------------------------------------------------------
// components we use in our "game". these are all just simple structs with some data.


// 2D position: just x,y coordinates
struct PositionComponent {
    float x, y;
};


// Sprite: color, sprite index (in the sprite atlas), and scale for rendering it
struct SpriteComponent {
    float colorR, colorG, colorB;
    int spriteIndex;
    float scale;
};


// World bounds for our "game" logic: x,y minimum & maximum values
struct WorldBoundsComponent {
    float xMin, xMax, yMin, yMax;
};


// Move around with constant velocity. When reached world bounds, reflect back from them.
struct MoveComponent {
    float velx, vely;

    static void Initialize(float minSpeed, float maxSpeed, float& velx, float& vely)
    {
        // random angle
        float angle = RandomFloat01() * 3.1415926f * 2;
        // random movement speed between given min & max
        float speed = RandomFloat(minSpeed, maxSpeed);
        // velocity x & y components
        velx = cosf(angle) * speed;
        vely = sinf(angle) * speed;
    }
};

entt::registry registry;

// -------------------------------------------------------------------------------------------------
// "systems" that we have; they operate on components of game objects
static entt::entity boundsID;


// "Avoidance system" works out interactions between objects that "avoid" and "should be avoided".
// Objects that avoid:
// - when they get closer to things that should be avoided than the given distance, they bounce
// back,
// - also they take sprite color from the object they just bumped into
struct AvoidanceSystem {
    // things to be avoided: distances to them, and their IDs
    std::vector<float> avoidDistanceList;
    std::vector<entt::entity> avoidList;

    // objects that avoid: their IDs
    std::vector<entt::entity> objectList;

    void AddAvoidThisObjectToSystem(entt::entity id, float distance)
    {
        avoidList.emplace_back(id);
        avoidDistanceList.emplace_back(distance * distance);
    }

    void AddObjectToSystem(entt::entity id) { objectList.emplace_back(id); }

    static float DistanceSq(const PositionComponent& a, const PositionComponent& b)
    {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        return dx * dx + dy * dy;
    }

    void ResolveCollision(entt::entity id, float deltaTime)
    {
        PositionComponent& pos = registry.get<PositionComponent>(id);
        MoveComponent& move = registry.get<MoveComponent>(id);

        // flip velocity
        move.velx = -move.velx;
        move.vely = -move.vely;

        // move us out of collision, by moving just a tiny bit more than we'd normally move during a
        // frame
        pos.x += move.velx * deltaTime * 1.1f;
        pos.y += move.vely * deltaTime * 1.1f;
    }

    void UpdateSystem(entt::registry& registry, double time, float deltaTime)
    {
        // go through all the objects
        for (size_t io = 0, no = objectList.size(); io != no; ++io) {
            entt::entity go = objectList[io];
            const PositionComponent& myposition = registry.get<PositionComponent>(go);

            // check each thing in avoid list
            for (size_t ia = 0, na = avoidList.size(); ia != na; ++ia) {
                float avDistance = avoidDistanceList[ia];
                entt::entity avoid = avoidList[ia];
                const PositionComponent& avoidposition = registry.get<PositionComponent>(avoid);

                // is our position closer to "thing to avoid" position than the avoid distance?
                if (DistanceSq(myposition, avoidposition) < avDistance) {
                    ResolveCollision(go, deltaTime);

                    // also make our sprite take the color of the thing we just bumped into
                    SpriteComponent& avoidSprite = registry.get<SpriteComponent>(avoid);
                    SpriteComponent& mySprite = registry.get<SpriteComponent>(go);
                    mySprite.colorR = avoidSprite.colorR;
                    mySprite.colorG = avoidSprite.colorG;
                    mySprite.colorB = avoidSprite.colorB;
                }
            }
        }
    }
};

static AvoidanceSystem s_AvoidanceSystem;

extern "C" void game_initialize(void)
{
    // create "world bounds" object
    WorldBoundsComponent bounds;

    {
        auto entity = registry.create();

        registry.assign<WorldBoundsComponent>(entity, -80.0f, 80.0f, -50.0f, 50.f);
        boundsID = entity;

        bounds = registry.get<WorldBoundsComponent>(entity);
    }

    // create regular objects that move
    for (auto i = 0; i < kObjectCount; ++i) {
        auto entity = registry.create();

        // position it within world bounds
        auto x = RandomFloat(bounds.xMin, bounds.xMax);
        auto y = RandomFloat(bounds.yMin, bounds.yMax);

        registry.assign<PositionComponent>(entity, x, y);

        // setup a sprite for it (random sprite index from first 5), and initial white color
        auto colorR = 1.0f;
        auto colorG = 1.0f;
        auto colorB = 1.0f;
        auto spriteIndex = rand() % 5;
        auto scale = 1.0f;

        registry.assign<SpriteComponent>(entity, colorR, colorG, colorB, spriteIndex, scale);

        // make it move
        float velx, vely;
        MoveComponent::Initialize(0.5f, 0.7f, velx, vely);
        registry.assign<MoveComponent>(entity, velx, vely);

		s_AvoidanceSystem.AddObjectToSystem(entity);
    }

    // create objects that should be avoided
    for (auto i = 0; i < kAvoidCount; ++i) {
        auto entity = registry.create();

        // position it within world bounds
        auto x = RandomFloat(bounds.xMin, bounds.xMax) * 0.2f;
        auto y = RandomFloat(bounds.yMin, bounds.yMax) * 0.2f;

        registry.assign<PositionComponent>(entity, x, y);

        // setup a sprite for it (random sprite index from first 5), and initial white color
        auto colorR = RandomFloat(0.5f, 1.0f);
        auto colorG = RandomFloat(0.5f, 1.0f);
        auto colorB = RandomFloat(0.5f, 1.0f);
        auto spriteIndex = 5;
        auto scale = 2.0f;

        registry.assign<SpriteComponent>(entity, colorR, colorG, colorB, spriteIndex, scale);

        // make it move
        float velx, vely;
        MoveComponent::Initialize(0.1f, 0.2f, velx, vely);
        registry.assign<MoveComponent>(entity, velx, vely);

        // add to avoidance this as "Avoid This" object
        s_AvoidanceSystem.AddAvoidThisObjectToSystem(entity, 1.3f);
        s_AvoidanceSystem.AddObjectToSystem(entity);
    }
}


extern "C" void game_destroy(void)
{
    //
}

void updateMovement(entt::registry& registry, double time, float deltaTime)
{
    auto view = registry.view<PositionComponent, MoveComponent>();

	const WorldBoundsComponent& bounds = registry.get<WorldBoundsComponent>(boundsID);

    for (auto entity : view) {
        // gets only the components that are going to be used ...
        auto& pos = view.get<PositionComponent>(entity);
        auto& move = view.get<MoveComponent>(entity);

		// update position based on movement velocity & delta time
        pos.x += move.velx * deltaTime;
        pos.y += move.vely * deltaTime;

        // check against world bounds; put back onto bounds and mirror the velocity component to
        // "bounce" back
        if (pos.x < bounds.xMin) {
            move.velx = -move.velx;
            pos.x = bounds.xMin;
        }
        if (pos.x > bounds.xMax) {
            move.velx = -move.velx;
            pos.x = bounds.xMax;
        }
        if (pos.y < bounds.yMin) {
            move.vely = -move.vely;
            pos.y = bounds.yMin;
        }
        if (pos.y > bounds.yMax) {
            move.vely = -move.vely;
            pos.y = bounds.yMax;
        }
    }
}

void updateAvoidance(entt::registry& registry, double time, float deltaTime)
{
    s_AvoidanceSystem.UpdateSystem(registry, time, deltaTime);
}

int updateRender(entt::registry& registry, sprite_data_t* data)
{
    int objectCount = 0;
    auto view = registry.view<PositionComponent, SpriteComponent>();

    // For objects that have a Position & Sprite on them: write out
    // their data into destination buffer that will be rendered later on.
    //
    // Using a smaller global scale "zooms out" the rendering, so to speak.
    float globalScale = 0.05f;

    for (auto entity : view) {
        sprite_data_t& spr = data[objectCount++];

        // gets only the components that are going to be used ...
        const auto& pos = view.get<PositionComponent>(entity);
        const auto& sprite = view.get<SpriteComponent>(entity);

        spr.posX = pos.x * globalScale;
        spr.posY = pos.y * globalScale;

        spr.scale = sprite.scale * globalScale;
        spr.colR = sprite.colorR;
        spr.colG = sprite.colorG;
        spr.colB = sprite.colorB;

        spr.sprite = (float)sprite.spriteIndex;
    }


	return objectCount;
}


extern "C" int game_update(sprite_data_t* data, double time, float deltaTime)
{
    // update object systems
    updateMovement(registry, time, deltaTime);
    updateAvoidance(registry, time, deltaTime);

	int objectCount = updateRender(registry, data);

    return objectCount;
}

