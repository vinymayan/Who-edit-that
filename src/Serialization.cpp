#include "Manager.h"
#include "Serialization.h"

namespace WhoEditThat
{
    namespace
    {
        constexpr std::uint32_t kSerializationID = 'WIET';
    }

    void InstallSerialization()
    {
        auto* serialization = SKSE::GetSerializationInterface();
        serialization->SetUniqueID(kSerializationID);
        serialization->SetSaveCallback([](SKSE::SerializationInterface* serialization) {
            Manager::GetSingleton().SaveSerialization(serialization);
        });
        serialization->SetLoadCallback([](SKSE::SerializationInterface* serialization) {
            Manager::GetSingleton().LoadSerialization(serialization);
        });
        serialization->SetRevertCallback([](SKSE::SerializationInterface*) {
            Manager::GetSingleton().Revert();
        });
    }
}
