#include <cstdint>
#include <entt/entt.hpp>

#include "./buffering.h"

struct position {
    float x;
    float y;
};

struct velocity {
    float dx;
    float dy;
};

void update(entt::registry& registry)
{
    auto view = registry.view<position, velocity>();

    for (auto entity : view) {
        // gets only the components that are going to be used ...

        auto& vel = view.get<velocity>(entity);

        vel.dx = 0.;
        vel.dy = 0.;

        // ...
    }
}

void update(std::uint64_t dt, entt::registry& registry)
{
    registry.view<position, velocity>().each([dt](auto& pos, auto& vel) {
        // gets all the components of the view at once ...

        pos.x += vel.dx * dt;
        pos.y += vel.dy * dt;

        // ...
    });
}

extern "C" int ecsstuff();

struct my_type
{
    int m1;
    float m2;
};

struct position_1 : position {
};

struct position_2 : position {
};


void bufferingTest(entt::registry& registry)
{
    buffering<position_1, position_2> executor;

    const auto entity = registry.create();
    registry.assign<position_1>(entity, 1.0f, 2.0f);
    registry.assign<position_2>(entity, 2.0f, 3.0f);
    registry.assign<my_type>(entity, 1, 1.0f);

    executor.run<my_type>(registry, [](position& pos, my_type& instance) { 
		//
	});
    executor.swap();
    executor.run<my_type>(registry, [](position& pos, my_type& instance) { 
		//	
	});
}

int ecsstuff()
{
    entt::registry registry;
    std::uint64_t dt = 16;

    for (auto i = 0; i < 10; ++i) {
        auto entity = registry.create();
        registry.assign<position>(entity, i * 1.f, i * 1.f);
        if (i % 2 == 0) {
            registry.assign<velocity>(entity, i * .1f, i * .1f);
        }
    }

    update(dt, registry);
    update(registry);

	bufferingTest(registry);

    // ...
    return 0;
}