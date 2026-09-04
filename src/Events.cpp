#include "Events.h"

#include "Manager.h"

namespace WhoEditThat::Events
{
    namespace
    {
        class Sink final :
            public RE::BSTEventSink<RE::TESResetEvent>,
            public RE::BSTEventSink<RE::TESCellAttachDetachEvent>
        {
        public:
            static Sink& GetSingleton()
            {
                static Sink singleton;
                return singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESResetEvent* event,
                RE::BSTEventSource<RE::TESResetEvent>*) override
            {
                if (event && event->object) {
                    if (const auto* actor = event->object->As<RE::Actor>()) {
                        Manager::GetSingleton().QueueReconcileActor(
                            actor->GetFormID());
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESCellAttachDetachEvent* event,
                RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
            {
                if (event && event->attached && event->reference) {
                    if (const auto* actor = event->reference->As<RE::Actor>()) {
                        Manager::GetSingleton().QueueReconcileActor(
                            actor->GetFormID());
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Install()
    {
        static bool installed = false;
        if (installed) {
            return;
        }
        auto* source = RE::ScriptEventSourceHolder::GetSingleton();
        if (!source) {
            logger::error("[WhoEditThat] Script event source is unavailable");
            return;
        }
        auto* sink = std::addressof(Sink::GetSingleton());
        source->AddEventSink<RE::TESResetEvent>(sink);
        source->AddEventSink<RE::TESCellAttachDetachEvent>(sink);
        installed = true;
        logger::info("[WhoEditThat] Reset and cell-attach observers installed");
    }
}
