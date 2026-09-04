#include "Events.h"
#include "Hooks.h"
#include "Manager.h"
#include "Serialization.h"
#include "logger.h"

namespace
{
    std::uint32_t ParseCharacterID(std::string_view saveName)
    {
        try {
            const auto first = saveName.find('_');
            const auto second = saveName.find('_', first + 1);
            if (first != std::string_view::npos &&
                second != std::string_view::npos) {
                return static_cast<std::uint32_t>(std::stoul(
                    std::string(saveName.substr(first + 1, second - first - 1)),
                    nullptr,
                    16));
            }
        } catch (...) {
        }
        return 0;
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        auto& manager = WhoEditThat::Manager::GetSingleton();
        switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            manager.Initialize();
            ActorValueHooks::Install();
            WhoEditThat::Events::Install();
            break;
        case SKSE::MessagingInterface::kPreLoadGame: {
            const auto* rawName = static_cast<const char*>(message->data);
            if (!rawName) {
                manager.Revert();
                break;
            }

            RE::BGSSaveLoadFileEntry entry{};
            entry.fileName = rawName;
            bool populated = false;
            for (int attempt = 0; attempt < 5 && !populated; ++attempt) {
                populated = entry.PopulateFileEntryData();
                if (!populated) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            auto characterID = ParseCharacterID(rawName);
            if (characterID == 0 && populated) {
                characterID = entry.characterID;
            }
            manager.BeginLoad(
                characterID,
                populated ? entry.saveNumber : 0,
                std::filesystem::path(rawName).filename().string());
            break;
        }
        case SKSE::MessagingInterface::kPostLoadGame:
            manager.FinishLoad();
            break;
        case SKSE::MessagingInterface::kSaveGame:
            if (const auto* rawName = static_cast<const char*>(message->data)) {
                manager.PersistCurrentSave(rawName);
            }
            break;
        case SKSE::MessagingInterface::kNewGame:
            manager.StartNewGame();
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SetupLog();
    logger::info("WhoEditThat loaded");
    WhoEditThat::InstallSerialization();
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
