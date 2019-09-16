#ifndef _BUFFER_H_
#define _BUFFER_H_


template <typename... Types>
class buffering
{
    using ident = entt::identifier<Types...>;

    template <typename Type, typename... Tail, typename... Other, typename Func>
    void execute(entt::type_list<Other...>, entt::registry& registry, Func&& func)
    {
        if (next == ident::template type<Type>) {
            registry.view<Type, Other...>().each(std::forward<Func>(func));
        } else {
            if constexpr (sizeof...(Tail) == 0) {
                assert(false);
            } else {
                execute<Tail...>(entt::type_list<Other...>{}, registry, std::forward<Func>(func));
            }
        }
    }

  public:
    template <typename... Other, typename Func>
    void run(entt::registry& registry, Func&& func)
    {
        execute<Types...>(entt::type_list<Other...>{}, registry, std::forward<Func>(func));
    }

    //void next() { curr = (curr + 1) % sizeof...(Types); }

    void swap() { next = (next + 1) % sizeof...(Types); }

  private:
    std::size_t next{};
};


#endif //