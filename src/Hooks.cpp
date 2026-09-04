#include "Hooks.h"

#include <atomic>
#include <cmath>

namespace ActorValueHooks
{
    namespace
    {
        thread_local bool hookActive = false;
        thread_local Origin currentOrigin = Origin::kExternal;
        std::atomic<Observer> observer = nullptr;

        class HookGuard
        {
        public:
            HookGuard() : previous_(hookActive) { hookActive = true; }
            ~HookGuard() { hookActive = previous_; }

        private:
            bool previous_;
        };

        Snapshot Capture(
            const RE::ActorValueOwner* owner,
            RE::ActorValue actorValue)
        {
            Snapshot result;
            result.base = owner->GetBaseActorValue(actorValue);
            result.current = owner->GetActorValue(actorValue);
            if (const auto* actor = skyrim_cast<const RE::Actor*>(owner)) {
                result.permanent = actor->GetActorValueModifier(
                    RE::ACTOR_VALUE_MODIFIER::kPermanent, actorValue);
                result.temporary = actor->GetActorValueModifier(
                    RE::ACTOR_VALUE_MODIFIER::kTemporary, actorValue);
                result.damage = actor->GetActorValueModifier(
                    RE::ACTOR_VALUE_MODIFIER::kDamage, actorValue);
                result.maximum = actor->GetActorValueMax(actorValue);
            } else {
                result.maximum = result.current;
            }
            return result;
        }

        RE::Actor* ActorFrom(RE::ActorValueOwner* owner)
        {
            return skyrim_cast<RE::Actor*>(owner);
        }

        void Notify(const MutationEvent& event)
        {
            if (event.origin != Origin::kExternal || event.actorFormID == 0) {
                return;
            }
            constexpr auto epsilon = 0.0001F;
            if (std::abs(event.after.base - event.before.base) <= epsilon &&
                std::abs(event.after.permanent - event.before.permanent) <= epsilon &&
                std::abs(event.after.temporary - event.before.temporary) <= epsilon) {
                return;
            }

            if (const auto callback = observer.load(std::memory_order_acquire)) {
                callback(event);
            }
        }

        template <class Tag>
        struct SetBaseActorValueHook
        {
            static void Thunk(
                RE::ActorValueOwner* self,
                RE::ActorValue actorValue,
                float value)
            {
                auto* actor = ActorFrom(self);
                if (hookActive || currentOrigin != Origin::kExternal || !actor) {
                    Original(self, actorValue, value);
                    return;
                }
                MutationEvent event{ actor->GetFormID(), actorValue,
                    RE::ACTOR_VALUE_MODIFIER::kPermanent,
                    MutationKind::kSetBase, currentOrigin, value, Capture(self, actorValue) };
                {
                    HookGuard guard;
                    Original(self, actorValue, value);
                    event.after = Capture(self, actorValue);
                }
                Notify(event);
            }

            static inline REL::Relocation<decltype(Thunk)> Original;
        };

        template <class Tag>
        struct ModBaseActorValueHook
        {
            static void Thunk(
                RE::ActorValueOwner* self,
                RE::ActorValue actorValue,
                float delta)
            {
                auto* actor = ActorFrom(self);
                if (hookActive || currentOrigin != Origin::kExternal || !actor) {
                    Original(self, actorValue, delta);
                    return;
                }
                MutationEvent event{ actor->GetFormID(), actorValue,
                    RE::ACTOR_VALUE_MODIFIER::kPermanent,
                    MutationKind::kModBase, currentOrigin, delta, Capture(self, actorValue) };
                {
                    HookGuard guard;
                    Original(self, actorValue, delta);
                    event.after = Capture(self, actorValue);
                }
                Notify(event);
            }

            static inline REL::Relocation<decltype(Thunk)> Original;
        };

        template <class Tag>
        struct ModActorValueHook
        {
            static void Thunk(
                RE::ActorValueOwner* self,
                RE::ACTOR_VALUE_MODIFIER modifier,
                RE::ActorValue actorValue,
                float delta)
            {
                auto* actor = ActorFrom(self);
                if (hookActive || currentOrigin != Origin::kExternal || !actor ||
                    modifier == RE::ACTOR_VALUE_MODIFIER::kDamage) {
                    Original(self, modifier, actorValue, delta);
                    return;
                }
                MutationEvent event{ actor->GetFormID(), actorValue, modifier,
                    MutationKind::kMod, currentOrigin, delta, Capture(self, actorValue) };
                {
                    HookGuard guard;
                    Original(self, modifier, actorValue, delta);
                    event.after = Capture(self, actorValue);
                }
                Notify(event);
            }

            static inline REL::Relocation<decltype(Thunk)> Original;
        };

        template <class Tag>
        struct SetActorValueHook
        {
            static void Thunk(
                RE::ActorValueOwner* self,
                RE::ActorValue actorValue,
                float value)
            {
                auto* actor = ActorFrom(self);
                if (hookActive || currentOrigin != Origin::kExternal || !actor) {
                    Original(self, actorValue, value);
                    return;
                }
                MutationEvent event{ actor->GetFormID(), actorValue,
                    RE::ACTOR_VALUE_MODIFIER::kPermanent,
                    MutationKind::kSet, currentOrigin, value, Capture(self, actorValue) };
                {
                    HookGuard guard;
                    Original(self, actorValue, value);
                    event.after = Capture(self, actorValue);
                }
                Notify(event);
            }

            static inline REL::Relocation<decltype(Thunk)> Original;
        };

        template <class Tag>
        void InstallOne(const REL::VariantID& vtableID, std::string_view ownerType)
        {
            REL::Relocation<std::uintptr_t> vtable{ vtableID };
            SetBaseActorValueHook<Tag>::Original =
                vtable.write_vfunc(0x4, SetBaseActorValueHook<Tag>::Thunk);
            ModBaseActorValueHook<Tag>::Original =
                vtable.write_vfunc(0x5, ModBaseActorValueHook<Tag>::Thunk);
            ModActorValueHook<Tag>::Original =
                vtable.write_vfunc(0x6, ModActorValueHook<Tag>::Thunk);
            SetActorValueHook<Tag>::Original =
                vtable.write_vfunc(0x7, SetActorValueHook<Tag>::Thunk);
            logger::info("[ActorValueHooks] Installed mutation hooks for {}", ownerType);
        }
    }

    ScopedOrigin::ScopedOrigin(Origin origin) noexcept : previous_(currentOrigin)
    {
        currentOrigin = origin;
    }

    ScopedOrigin::~ScopedOrigin()
    {
        currentOrigin = previous_;
    }

    void SetObserver(Observer value) noexcept
    {
        observer.store(value, std::memory_order_release);
    }

    void Install()
    {
        static std::atomic_bool installed = false;
        if (installed.exchange(true)) {
            return;
        }
        InstallOne<RE::Character>(RE::VTABLE_Character[5], "Character");
        InstallOne<RE::PlayerCharacter>(
            RE::VTABLE_PlayerCharacter[5], "PlayerCharacter");
    }
}
