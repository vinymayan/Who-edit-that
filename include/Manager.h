#pragma once

#include "Hooks.h"
#include "WhoEditThatAPI.h"

namespace WhoEditThat
{
    class Manager
    {
    public:
        static Manager& GetSingleton();

        void Initialize();
        void BeginLoad(
            std::uint32_t characterID,
            std::uint32_t saveNumber,
            std::string_view saveName);
        void FinishLoad();
        void StartNewGame();
        void PersistCurrentSave(std::string_view saveName);
        void Revert();
        void QueueExternalMutation(
            const ActorValueHooks::MutationEvent& event);
        void QueueReconcileActor(std::uint32_t actorFormID);

        [[nodiscard]] bool IsReady() const noexcept;
        [[nodiscard]] API::ClientHandle RegisterClient(
            const API::ClientRegistration& registration);

        bool QueueUpsertActorValue(
            const API::ActorValueContributionRequest& request,
            API::Callback callback,
            void* userData);
        bool QueueRemoveActorValue(
            const API::ContributionRequest& request,
            API::Callback callback,
            void* userData);
        bool QueueLookupActorValue(
            const API::ContributionRequest& request,
            API::Callback callback,
            void* userData);
        bool QueueListActorValues(
            const API::ContributionScopeRequest& request,
            API::ListCallback callback,
            void* userData);
        bool QueueRemoveActorValuesByPrefix(
            const API::ContributionScopeRequest& request,
            API::Callback callback,
            void* userData);
        bool QueueUpsertActorScale(
            const API::ActorScaleContributionRequest& request,
            API::Callback callback,
            void* userData);
        bool QueueRemoveActorScale(
            const API::ContributionRequest& request,
            API::Callback callback,
            void* userData);
        bool QueueLookupActorScale(
            const API::ContributionRequest& request,
            API::Callback callback,
            void* userData);
        bool QueueListActorScales(
            const API::ContributionScopeRequest& request,
            API::ScaleListCallback callback,
            void* userData);
        bool QueueRemoveActorScalesByPrefix(
            const API::ContributionScopeRequest& request,
            API::Callback callback,
            void* userData);
        bool QueueUpsertDisplayName(
            const API::DisplayNameRequest& request,
            API::Callback callback,
            void* userData);
        bool QueueRemoveDisplayName(
            const API::ContributionRequest& request,
            API::Callback callback,
            void* userData);

        bool SaveSerialization(SKSE::SerializationInterface* serialization);
        void LoadSerialization(SKSE::SerializationInterface* serialization);

    private:
        Manager();
        ~Manager();
        Manager(const Manager&) = delete;
        Manager& operator=(const Manager&) = delete;

        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
