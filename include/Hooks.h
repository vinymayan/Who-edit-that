#pragma once

namespace ActorValueHooks
{
    enum class Origin : std::uint8_t
    {
        kExternal,
        kLedgerAPI,
        kRecovery
    };

    enum class MutationKind : std::uint8_t
    {
        kSetBase,
        kModBase,
        kMod,
        kSet
    };

    struct Snapshot
    {
        float base{ 0.0F };
        float permanent{ 0.0F };
        float temporary{ 0.0F };
        float damage{ 0.0F };
        float current{ 0.0F };
        float maximum{ 0.0F };
    };

    struct MutationEvent
    {
        std::uint32_t actorFormID{ 0 };
        RE::ActorValue actorValue{ RE::ActorValue::kNone };
        RE::ACTOR_VALUE_MODIFIER modifier{
            RE::ACTOR_VALUE_MODIFIER::kPermanent };
        MutationKind kind{ MutationKind::kMod };
        Origin origin{ Origin::kExternal };
        float requested{ 0.0F };
        Snapshot before;
        Snapshot after;
    };

    using Observer = void (*)(const MutationEvent&);

    class ScopedOrigin
    {
    public:
        explicit ScopedOrigin(Origin origin) noexcept;
        ~ScopedOrigin();
        ScopedOrigin(const ScopedOrigin&) = delete;
        ScopedOrigin& operator=(const ScopedOrigin&) = delete;

    private:
        Origin previous_;
    };

    void SetObserver(Observer observer) noexcept;
    void Install();
}
