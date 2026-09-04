#include "Manager.h"

namespace WhoEditThat::API
{
    class Interface final : public IWhoEditThatAPI
    {
    public:
        std::uint32_t GetVersion() const noexcept override
        {
            return kInterfaceVersion;
        }

        bool IsReady() const noexcept override
        {
            return Manager::GetSingleton().IsReady();
        }

        ClientHandle RegisterClient(
            const ClientRegistration* registration) noexcept override
        {
            if (!registration ||
                registration->structSize < sizeof(ClientRegistration)) {
                return kInvalidClient;
            }
            try {
                return Manager::GetSingleton().RegisterClient(*registration);
            } catch (...) {
                return kInvalidClient;
            }
        }

        bool QueueUpsertActorValue(
            const ActorValueContributionRequest* request,
            Callback callback,
            void* userData) noexcept override
        {
            return Guard(request, callback, [&] {
                return Manager::GetSingleton().QueueUpsertActorValue(
                    *request, callback, userData);
            });
        }

        bool QueueRemoveActorValue(
            const ContributionRequest* request,
            Callback callback,
            void* userData) noexcept override
        {
            return Guard(request, callback, [&] {
                return Manager::GetSingleton().QueueRemoveActorValue(
                    *request, callback, userData);
            });
        }

        bool QueueLookupActorValue(
            const ContributionRequest* request,
            Callback callback,
            void* userData) noexcept override
        {
            return Guard(request, callback, [&] {
                return Manager::GetSingleton().QueueLookupActorValue(
                    *request, callback, userData);
            });
        }

        bool QueueUpsertDisplayName(
            const DisplayNameRequest* request,
            Callback callback,
            void* userData) noexcept override
        {
            return Guard(request, callback, [&] {
                return Manager::GetSingleton().QueueUpsertDisplayName(
                    *request, callback, userData);
            });
        }

        bool QueueRemoveDisplayName(
            const ContributionRequest* request,
            Callback callback,
            void* userData) noexcept override
        {
            return Guard(request, callback, [&] {
                return Manager::GetSingleton().QueueRemoveDisplayName(
                    *request, callback, userData);
            });
        }

        bool QueueListActorValues(
            const ContributionScopeRequest* request,
            ListCallback callback,
            void* userData) noexcept override
        {
            return Guard(request, callback, [&] {
                return Manager::GetSingleton().QueueListActorValues(
                    *request, callback, userData);
            });
        }

        bool QueueRemoveActorValuesByPrefix(
            const ContributionScopeRequest* request,
            Callback callback,
            void* userData) noexcept override
        {
            return Guard(request, callback, [&] {
                return Manager::GetSingleton().QueueRemoveActorValuesByPrefix(
                    *request, callback, userData);
            });
        }

        bool QueueUpsertActorScale(
            const ActorScaleContributionRequest* request,
            Callback callback,
            void* userData) noexcept override
        {
            return Guard(request, callback, [&] {
                return Manager::GetSingleton().QueueUpsertActorScale(
                    *request, callback, userData);
            });
        }

        bool QueueRemoveActorScale(
            const ContributionRequest* request,
            Callback callback,
            void* userData) noexcept override
        {
            return Guard(request, callback, [&] {
                return Manager::GetSingleton().QueueRemoveActorScale(
                    *request, callback, userData);
            });
        }

        bool QueueLookupActorScale(
            const ContributionRequest* request,
            Callback callback,
            void* userData) noexcept override
        {
            return Guard(request, callback, [&] {
                return Manager::GetSingleton().QueueLookupActorScale(
                    *request, callback, userData);
            });
        }

        bool QueueListActorScales(
            const ContributionScopeRequest* request,
            ScaleListCallback callback,
            void* userData) noexcept override
        {
            return Guard(request, callback, [&] {
                return Manager::GetSingleton().QueueListActorScales(
                    *request, callback, userData);
            });
        }

        bool QueueRemoveActorScalesByPrefix(
            const ContributionScopeRequest* request,
            Callback callback,
            void* userData) noexcept override
        {
            return Guard(request, callback, [&] {
                return Manager::GetSingleton().QueueRemoveActorScalesByPrefix(
                    *request, callback, userData);
            });
        }

    private:
        template <class Request, class CallbackType, class Fn>
        static bool Guard(
            const Request* request,
            CallbackType callback,
            Fn&& fn) noexcept
        {
            if (!request || request->structSize < sizeof(Request) || !callback) {
                return false;
            }
            try {
                return std::forward<Fn>(fn)();
            } catch (...) {
                return false;
            }
        }
    };
}

extern "C" __declspec(dllexport) void* GetWhoEditThatAPI()
{
    static WhoEditThat::API::Interface api;
    return std::addressof(api);
}
