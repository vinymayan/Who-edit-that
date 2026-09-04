#include "Manager.h"

#include "RE/A/ActorValueList.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <set>

namespace WhoEditThat
{
    namespace fs = std::filesystem;
    using namespace API;

    namespace
    {
        constexpr int kSchemaVersion = 3;
        constexpr std::uint32_t kFormRecord = 'FRMS';
        constexpr std::uint32_t kFormRecordVersion = 1;
        constexpr char kKeySeparator = '\x1F';
        constexpr float kMinimumActorScale = 0.10F;
        constexpr float kMaximumActorScale = 10.0F;

        enum class ContextState : std::uint32_t
        {
            kNoContext,
            kLoading,
            kReady
        };

        enum class EvaluationBinding : std::uint32_t
        {
            kSnapshot = 0,
            kLive = 1
        };

        // Binding stays internal; consumers trigger Live refreshes through API calls.
        constexpr auto kDefaultBinding = EvaluationBinding::kLive;

        struct Context
        {
            std::uint32_t characterID{ 0 };
            std::uint32_t saveNumber{ 0 };
            std::string saveName;
        };

        struct Client
        {
            std::string id;
            std::string displayName;
        };

        struct ActorValueContribution
        {
            std::uint32_t actorFormID{ 0 };
            std::string actorKey;
            std::string ownerID;
            std::string mutationKey;
            std::string targetActorValue;
            NumericOperation operation{ NumericOperation::kFlat };
            NumericSource source{ NumericSource::kFixed };
            ModifierChannel channel{ ModifierChannel::kPermanent };
            EvaluationBinding binding{ kDefaultBinding };
            float fixedValue{ 0.0F };
            std::uint32_t sourceGlobalFormID{ 0 };
            std::string sourceActorValue;
            float sourceMultiplier{ 1.0F };
            float baselineAtActivation{ 0.0F };
            float resolvedSourceValue{ 0.0F };
            float appliedDelta{ 0.0F };
            std::uint64_t sequence{ 0 };
        };

        struct DisplayNameOverride
        {
            std::uint32_t actorFormID{ 0 };
            std::string actorKey;
            std::string ownerID;
            std::string mutationKey;
            std::string requestedName;
            std::string originalName;
            std::string lastAppliedName;
            std::int32_t priority{ 0 };
            std::uint64_t sequence{ 0 };
        };

        struct ActorScaleContribution
        {
            std::uint32_t actorFormID{ 0 };
            std::string actorKey;
            std::string ownerID;
            std::string mutationKey;
            NumericOperation operation{ NumericOperation::kFlat };
            NumericSource source{ NumericSource::kFixed };
            EvaluationBinding binding{ kDefaultBinding };
            float fixedValue{ 0.0F };
            std::uint32_t sourceGlobalFormID{ 0 };
            std::string sourceActorValue;
            float sourceMultiplier{ 1.0F };
            float baselineAtActivation{ 1.0F };
            float resolvedSourceValue{ 0.0F };
            float appliedDelta{ 0.0F };
            std::uint64_t sequence{ 0 };
        };

        struct AuditEvent
        {
            std::uint64_t sequence{ 0 };
            std::uint64_t generation{ 0 };
            std::uint32_t actorFormID{ 0 };
            std::string actorKey;
            std::string ownerID;
            std::string mutationKey;
            std::string kind;
            std::string action;
            float beforeValue{ 0.0F };
            float afterValue{ 0.0F };
            Status status{ Status::kSuccess };
        };

        struct ExternalActorValueEvent
        {
            std::uint64_t sequence{ 0 };
            std::uint64_t generation{ 0 };
            std::uint32_t actorFormID{ 0 };
            std::string actorKey;
            std::string actorValue;
            ActorValueHooks::MutationKind kind{
                ActorValueHooks::MutationKind::kMod };
            RE::ACTOR_VALUE_MODIFIER modifier{
                RE::ACTOR_VALUE_MODIFIER::kPermanent };
            float requested{ 0.0F };
            ActorValueHooks::Snapshot before;
            ActorValueHooks::Snapshot after;
            bool durable{ false };
        };

        struct ActorValueCheckpoint
        {
            std::uint32_t actorFormID{ 0 };
            std::string actorKey;
            std::string actorValue;
            float externalBase{ 0.0F };
            float externalPermanent{ 0.0F };
            float externalTemporary{ 0.0F };
            float ledgerPermanent{ 0.0F };
            float ledgerTemporary{ 0.0F };
        };

        struct OwnedActorValueRequest
        {
            std::string ownerID;
            std::uint32_t actorFormID{ 0 };
            std::string mutationKey;
            std::string targetActorValue;
            NumericOperation operation{ NumericOperation::kFlat };
            NumericSource source{ NumericSource::kFixed };
            ModifierChannel channel{ ModifierChannel::kPermanent };
            float fixedValue{ 0.0F };
            std::uint32_t sourceGlobalFormID{ 0 };
            std::string sourceActorValue;
            float sourceMultiplier{ 1.0F };
        };

        struct OwnedActorScaleRequest
        {
            std::string ownerID;
            std::uint32_t actorFormID{ 0 };
            std::string mutationKey;
            NumericOperation operation{ NumericOperation::kFlat };
            NumericSource source{ NumericSource::kFixed };
            float fixedValue{ 0.0F };
            std::uint32_t sourceGlobalFormID{ 0 };
            std::string sourceActorValue;
            float sourceMultiplier{ 1.0F };
        };

        struct OwnedContributionRequest
        {
            std::string ownerID;
            std::uint32_t actorFormID{ 0 };
            std::string mutationKey;
        };

        struct OwnedContributionScopeRequest
        {
            std::string ownerID;
            std::uint32_t actorFormID{ 0 };
            std::string mutationKeyPrefix;
        };

        struct OwnedDisplayNameRequest
        {
            std::string ownerID;
            std::uint32_t actorFormID{ 0 };
            std::string mutationKey;
            std::string displayName;
            std::int32_t priority{ 0 };
        };

        struct CallbackTarget
        {
            Callback callback{ nullptr };
            void* userData{ nullptr };

            void Deliver(const Result& result) const noexcept
            {
                if (!callback) {
                    return;
                }
                try {
                    callback(std::addressof(result), userData);
                }
                catch (...) {
                    logger::error("[WhoEditThat] Consumer callback threw an exception");
                }
            }
        };

        struct ListCallbackTarget
        {
            ListCallback callback{ nullptr };
            void* userData{ nullptr };

            void Deliver(const ActorValueListResult& result) const noexcept
            {
                if (!callback) {
                    return;
                }
                try {
                    callback(std::addressof(result), userData);
                }
                catch (...) {
                    logger::error("[WhoEditThat] Consumer list callback threw an exception");
                }
            }
        };

        struct ScaleListCallbackTarget
        {
            ScaleListCallback callback{ nullptr };
            void* userData{ nullptr };

            void Deliver(const ActorScaleListResult& result) const noexcept
            {
                if (!callback) {
                    return;
                }
                try {
                    callback(std::addressof(result), userData);
                }
                catch (...) {
                    logger::error(
                        "[WhoEditThat] Consumer scale list callback threw an exception");
                }
            }
        };

        struct SqliteDb
        {
            sqlite3* handle{ nullptr };
            ~SqliteDb()
            {
                if (handle) {
                    sqlite3_close(handle);
                }
            }
        };

        struct Statement
        {
            sqlite3_stmt* handle{ nullptr };
            ~Statement()
            {
                if (handle) {
                    sqlite3_finalize(handle);
                }
            }
        };

        template <std::size_t N>
        void CopyText(char(&target)[N], std::string_view value)
        {
            const auto count = std::min(value.size(), N - 1);
            std::memcpy(target, value.data(), count);
            target[count] = '\0';
        }

        Result MakeResult(
            Operation operation,
            Status status,
            std::uint32_t actorFormID,
            std::string_view message = {})
        {
            Result result;
            result.operation = operation;
            result.status = status;
            result.actorFormID = actorFormID;
            CopyText(result.message, message);
            return result;
        }

        std::string CompositeKey(
            std::uint32_t actorFormID,
            std::string_view owner,
            std::string_view mutation)
        {
            return std::format(
                "{:08X}{}{}{}{}",
                actorFormID,
                kKeySeparator,
                owner,
                kKeySeparator,
                mutation);
        }

        std::string CheckpointKey(
            std::uint32_t actorFormID,
            std::string_view actorValue)
        {
            return std::format(
                "{:08X}{}{}", actorFormID, kKeySeparator, actorValue);
        }

        bool NearlyEqual(float left, float right)
        {
            const auto scale = std::max({ 1.0F, std::abs(left), std::abs(right) });
            return std::abs(left - right) <= scale * 0.0001F;
        }

        std::string BuildActorKey(RE::Actor* actor)
        {
            if (!actor) {
                return {};
            }
            if (actor->IsDynamicForm()) {
                return std::format("Dynamic|{:08X}", actor->GetFormID());
            }
            const auto* file = actor->GetFile(0);
            return std::format(
                "{}|{:08X}",
                file ? file->GetFilename() : "Unknown",
                actor->GetLocalFormID());
        }

        std::uint32_t ResolveStoredActorID(
            std::uint32_t storedFormID,
            std::string_view actorKey,
            const std::unordered_map<std::uint32_t, std::uint32_t>& resolved)
        {
            if (const auto found = resolved.find(storedFormID);
                found != resolved.end()) {
                return found->second;
            }
            const auto separator = actorKey.find('|');
            if (separator == std::string_view::npos ||
                actorKey.substr(0, separator) == "Dynamic") {
                return 0;
            }
            try {
                const auto plugin = actorKey.substr(0, separator);
                const auto localID = static_cast<std::uint32_t>(std::stoul(
                    std::string(actorKey.substr(separator + 1)), nullptr, 16));
                const auto* actor = RE::TESDataHandler::GetSingleton()->
                    LookupForm<RE::Actor>(localID, plugin);
                return actor ? actor->GetFormID() : 0;
            }
            catch (...) {
                return 0;
            }
        }

        RE::Actor* LookupActor(std::uint32_t formID)
        {
            return RE::TESForm::LookupByID<RE::Actor>(formID);
        }

        bool IsValidActorValue(RE::ActorValue actorValue)
        {
            const auto value = std::to_underlying(actorValue);
            return value >= 0 &&
                value < std::to_underlying(RE::ActorValue::kTotal);
        }

        std::string NormalizeActorValueName(std::string_view name)
        {
            std::string normalized;
            normalized.reserve(name.size());
            for (const auto character : name) {
                const auto value = static_cast<unsigned char>(character);
                if (std::isalnum(value)) {
                    normalized.push_back(static_cast<char>(std::tolower(value)));
                }
            }
            return normalized;
        }

        RE::ActorValue ResolveActorValue(std::string_view name)
        {
            if (name.empty()) {
                return RE::ActorValue::kNone;
            }
            const auto resolved = RE::ActorValueList::LookupActorValueByName(
                std::string(name).c_str());
            if (IsValidActorValue(resolved)) {
                return resolved;
            }

            const auto normalized = NormalizeActorValueName(name);
            for (auto index = 0;
                index < std::to_underlying(RE::ActorValue::kTotal);
                ++index) {
                const auto actorValue = static_cast<RE::ActorValue>(index);
                const auto* canonical =
                    RE::ActorValueList::GetActorValueName(actorValue);
                if (canonical &&
                    NormalizeActorValueName(canonical) == normalized) {
                    return actorValue;
                }
            }
            return RE::ActorValue::kNone;
        }

        RE::ACTOR_VALUE_MODIFIER ResolveChannel(ModifierChannel channel)
        {
            return channel == ModifierChannel::kTemporary ?
                RE::ACTOR_VALUE_MODIFIERS::kTemporary :
                RE::ACTOR_VALUE_MODIFIERS::kPermanent;
        }

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
            }
            catch (...) {
            }
            return 0;
        }

        std::uint32_t ParseSaveNumber(
            std::string_view saveName,
            std::uint32_t fallback)
        {
            try {
                const auto save = saveName.find("Save");
                const auto underscore = saveName.find('_', save);
                if (save != std::string_view::npos &&
                    underscore != std::string_view::npos) {
                    return static_cast<std::uint32_t>(std::stoul(
                        std::string(saveName.substr(
                            save + 4,
                            underscore - save - 4))));
                }
            }
            catch (...) {
            }
            return fallback;
        }

        fs::path DatabasePath(std::uint32_t characterID)
        {
            const fs::path directory = "Data/Viny Mods/WhoEditThat/Saves";
            fs::create_directories(directory);
            return directory / std::format("{:08X}.db", characterID);
        }

        bool Exec(sqlite3* db, const char* sql)
        {
            char* error = nullptr;
            if (sqlite3_exec(db, sql, nullptr, nullptr, &error) == SQLITE_OK) {
                return true;
            }
            logger::error(
                "[WhoEditThat] SQLite error: {}",
                error ? error : sqlite3_errmsg(db));
            sqlite3_free(error);
            return false;
        }

        bool Prepare(sqlite3* db, const char* sql, Statement& statement)
        {
            if (sqlite3_prepare_v2(
                db, sql, -1, std::addressof(statement.handle), nullptr) ==
                SQLITE_OK) {
                return true;
            }
            logger::error("[WhoEditThat] SQLite prepare error: {}", sqlite3_errmsg(db));
            return false;
        }

        void BindText(sqlite3_stmt* statement, int index, std::string_view value)
        {
            sqlite3_bind_text(
                statement,
                index,
                value.data(),
                static_cast<int>(value.size()),
                SQLITE_TRANSIENT);
        }

        std::string ColumnText(sqlite3_stmt* statement, int index)
        {
            const auto* value = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, index));
            return value ? value : "";
        }

        bool ConfigureDatabase(sqlite3* db)
        {
            // These PRAGMAs are connection setup. In particular, journal_mode must
            // be changed while SQLite is in autocommit mode. Keeping them outside
            // EnsureSchema prevents an active schema/version reader from turning
            // the WAL switch into "cannot change into wal mode from within a
            // transaction".
            return Exec(db, "PRAGMA foreign_keys=ON;") &&
                Exec(db, "PRAGMA journal_mode=WAL;") &&
                Exec(db, "PRAGMA synchronous=FULL;");
        }

        bool OpenDatabase(const fs::path& path, SqliteDb& database)
        {
            if (sqlite3_open_v2(
                path.string().c_str(),
                std::addressof(database.handle),
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                nullptr) != SQLITE_OK) {
                logger::error(
                    "[WhoEditThat] Could not open '{}': {}",
                    path.string(),
                    database.handle ? sqlite3_errmsg(database.handle) : "unknown");
                return false;
            }
            sqlite3_busy_timeout(database.handle, 5000);
            if (!ConfigureDatabase(database.handle)) {
                logger::error(
                    "[WhoEditThat] Could not configure SQLite connection for '{}'",
                    path.string());
                return false;
            }
            return true;
        }

        bool EnsureSchema(sqlite3* db)
        {
            int existingVersion = 0;
            {
                // Finalize the PRAGMA reader before executing any schema work.
                // A statement left at SQLITE_ROW can keep a read transaction open
                // until it is finalized.
                Statement version;
                if (!Prepare(db, "PRAGMA user_version;", version) ||
                    sqlite3_step(version.handle) != SQLITE_ROW) {
                    return false;
                }
                existingVersion = sqlite3_column_int(version.handle, 0);
            }

            if (existingVersion > kSchemaVersion) {
                logger::error(
                    "[WhoEditThat] Database schema {} is newer than supported schema {}",
                    existingVersion,
                    kSchemaVersion);
                return false;
            }
            static constexpr auto schema = R"sql(
                CREATE TABLE IF NOT EXISTS metadata(
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS clients(
                    client_id TEXT PRIMARY KEY,
                    display_name TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS save_snapshots(
                    snapshot_id INTEGER PRIMARY KEY,
                    save_number INTEGER NOT NULL,
                    save_name TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    UNIQUE(save_number, save_name)
                );
                CREATE TABLE IF NOT EXISTS actor_value_contributions(
                    snapshot_id INTEGER NOT NULL REFERENCES save_snapshots(snapshot_id) ON DELETE CASCADE,
                    actor_form_id INTEGER NOT NULL,
                    actor_key TEXT NOT NULL,
                    owner_id TEXT NOT NULL,
                    mutation_key TEXT NOT NULL,
                    target_actor_value TEXT NOT NULL,
                    operation INTEGER NOT NULL,
                    source INTEGER NOT NULL,
                    channel INTEGER NOT NULL,
                    binding INTEGER NOT NULL,
                    fixed_value REAL NOT NULL,
                    source_global_form_id INTEGER NOT NULL,
                    source_actor_value TEXT NOT NULL,
                    source_multiplier REAL NOT NULL,
                    baseline REAL NOT NULL,
                    resolved_source REAL NOT NULL,
                    applied_delta REAL NOT NULL,
                    sequence INTEGER NOT NULL,
                    PRIMARY KEY(snapshot_id, actor_key, owner_id, mutation_key)
                );
                CREATE TABLE IF NOT EXISTS name_overrides(
                    snapshot_id INTEGER NOT NULL REFERENCES save_snapshots(snapshot_id) ON DELETE CASCADE,
                    actor_form_id INTEGER NOT NULL,
                    actor_key TEXT NOT NULL,
                    owner_id TEXT NOT NULL,
                    mutation_key TEXT NOT NULL,
                    requested_name TEXT NOT NULL,
                    original_name TEXT NOT NULL,
                    last_applied_name TEXT NOT NULL,
                    priority INTEGER NOT NULL,
                    sequence INTEGER NOT NULL,
                    PRIMARY KEY(snapshot_id, actor_key, owner_id, mutation_key)
                );
                CREATE TABLE IF NOT EXISTS actor_scale_contributions(
                    snapshot_id INTEGER NOT NULL REFERENCES save_snapshots(snapshot_id) ON DELETE CASCADE,
                    actor_form_id INTEGER NOT NULL,
                    actor_key TEXT NOT NULL,
                    owner_id TEXT NOT NULL,
                    mutation_key TEXT NOT NULL,
                    operation INTEGER NOT NULL,
                    source INTEGER NOT NULL,
                    binding INTEGER NOT NULL,
                    fixed_value REAL NOT NULL,
                    source_global_form_id INTEGER NOT NULL,
                    source_actor_value TEXT NOT NULL,
                    source_multiplier REAL NOT NULL,
                    baseline REAL NOT NULL,
                    resolved_source REAL NOT NULL,
                    applied_delta REAL NOT NULL,
                    sequence INTEGER NOT NULL,
                    PRIMARY KEY(snapshot_id, actor_key, owner_id, mutation_key)
                );
                CREATE TABLE IF NOT EXISTS audit_events(
                    event_id INTEGER PRIMARY KEY,
                    snapshot_id INTEGER NOT NULL REFERENCES save_snapshots(snapshot_id) ON DELETE CASCADE,
                    sequence INTEGER NOT NULL,
                    generation INTEGER NOT NULL,
                    actor_form_id INTEGER NOT NULL,
                    actor_key TEXT NOT NULL,
                    owner_id TEXT NOT NULL,
                    mutation_key TEXT NOT NULL,
                    kind TEXT NOT NULL,
                    action TEXT NOT NULL,
                    before_value REAL NOT NULL,
                    after_value REAL NOT NULL,
                    status INTEGER NOT NULL
                );
                CREATE TABLE IF NOT EXISTS external_actor_value_events(
                    event_id INTEGER PRIMARY KEY,
                    snapshot_id INTEGER NOT NULL REFERENCES save_snapshots(snapshot_id) ON DELETE CASCADE,
                    sequence INTEGER NOT NULL,
                    generation INTEGER NOT NULL,
                    actor_form_id INTEGER NOT NULL,
                    actor_key TEXT NOT NULL,
                    actor_value TEXT NOT NULL,
                    mutation_kind INTEGER NOT NULL,
                    modifier INTEGER NOT NULL,
                    requested REAL NOT NULL,
                    before_base REAL NOT NULL,
                    before_permanent REAL NOT NULL,
                    before_temporary REAL NOT NULL,
                    before_maximum REAL NOT NULL,
                    after_base REAL NOT NULL,
                    after_permanent REAL NOT NULL,
                    after_temporary REAL NOT NULL,
                    after_maximum REAL NOT NULL,
                    durable INTEGER NOT NULL
                );
                CREATE TABLE IF NOT EXISTS actor_value_checkpoints(
                    snapshot_id INTEGER NOT NULL REFERENCES save_snapshots(snapshot_id) ON DELETE CASCADE,
                    actor_form_id INTEGER NOT NULL,
                    actor_key TEXT NOT NULL,
                    actor_value TEXT NOT NULL,
                    external_base REAL NOT NULL,
                    external_permanent REAL NOT NULL,
                    external_temporary REAL NOT NULL,
                    ledger_permanent REAL NOT NULL,
                    ledger_temporary REAL NOT NULL,
                    PRIMARY KEY(snapshot_id,actor_key,actor_value)
                );
                PRAGMA user_version=3;
            )sql";
            return Exec(db, schema);
        }

        bool BackupDatabase(const fs::path& sourcePath)
        {
            SqliteDb source;
            SqliteDb destination;
            const auto backupPath = fs::path(sourcePath).concat(".bak");
            if (sqlite3_open_v2(
                sourcePath.string().c_str(),
                std::addressof(source.handle),
                SQLITE_OPEN_READONLY,
                nullptr) != SQLITE_OK ||
                sqlite3_open_v2(
                    backupPath.string().c_str(),
                    std::addressof(destination.handle),
                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                    nullptr) != SQLITE_OK) {
                return false;
            }
            auto* backup = sqlite3_backup_init(
                destination.handle, "main", source.handle, "main");
            if (!backup) {
                return false;
            }
            const auto step = sqlite3_backup_step(backup, -1);
            const auto finish = sqlite3_backup_finish(backup);
            return (step == SQLITE_DONE || step == SQLITE_OK) && finish == SQLITE_OK;
        }

        constexpr float GroupDelta(
            float baseline,
            float flat,
            float percent,
            float multiplier)
        {
            return baseline * ((1.0F + percent) * multiplier - 1.0F) + flat;
        }

        static_assert(GroupDelta(100.0F, 30.0F, 0.25F, 1.5F) == 117.5F);
        static_assert(GroupDelta(100.0F, -10.0F, 0.0F, 1.0F) == -10.0F);
    }

    struct Manager::Impl
    {
        mutable std::mutex mutex;
        std::atomic<ContextState> state{ ContextState::kNoContext };
        std::atomic<std::uint64_t> generation{ 1 };
        Context context;
        std::uint64_t nextClientHandle{ 1 };
        std::uint64_t nextSequence{ 1 };
        std::unordered_map<ClientHandle, Client> clients;
        std::unordered_map<std::string, ClientHandle> clientHandles;
        std::map<std::string, ActorValueContribution> actorValues;
        std::map<std::string, ActorScaleContribution> scales;
        std::map<std::string, DisplayNameOverride> names;
        std::vector<AuditEvent> audit;
        std::vector<ExternalActorValueEvent> externalAudit;
        std::map<std::string, ActorValueCheckpoint> checkpoints;
        std::unordered_map<std::uint32_t, std::uint32_t> resolvedForms;

        [[nodiscard]] std::optional<std::string> Owner(ClientHandle handle) const
        {
            const auto found = clients.find(handle);
            if (found == clients.end()) {
                return std::nullopt;
            }
            return found->second.id;
        }

        void CancelContext(bool preserveLoadingContext)
        {
            generation.fetch_add(1, std::memory_order_acq_rel);
            actorValues.clear();
            scales.clear();
            names.clear();
            audit.clear();
            externalAudit.clear();
            checkpoints.clear();
            resolvedForms.clear();
            if (!preserveLoadingContext) {
                context = {};
                state.store(ContextState::kNoContext, std::memory_order_release);
            }
        }

        void AddAudit(
            const ActorValueContribution& contribution,
            std::string_view action,
            float before,
            float after,
            Status status)
        {
            audit.push_back({
                nextSequence++,
                generation.load(std::memory_order_acquire),
                contribution.actorFormID,
                contribution.actorKey,
                contribution.ownerID,
                contribution.mutationKey,
                "ActorValue",
                std::string(action),
                before,
                after,
                status });
        }

        void AddAudit(
            const DisplayNameOverride & override,
            std::string_view action,
            Status status)
        {
            audit.push_back({
                nextSequence++,
                generation.load(std::memory_order_acquire),
                override.actorFormID,
                override.actorKey,
                override.ownerID,
                override.mutationKey,
                "DisplayName",
                std::string(action),
                0.0F,
                0.0F,
                status });
        }

        void AddAudit(
            const ActorScaleContribution& contribution,
            std::string_view action,
            float before,
            float after,
            Status status)
        {
            audit.push_back({
                nextSequence++,
                generation.load(std::memory_order_acquire),
                contribution.actorFormID,
                contribution.actorKey,
                contribution.ownerID,
                contribution.mutationKey,
                "ActorScale",
                std::string(action),
                before,
                after,
                status });
        }

        auto FindActorValue(
            std::uint32_t actorFormID,
            std::string_view owner,
            std::string_view mutation)
        {
            return std::ranges::find_if(
                actorValues,
                [&](const auto& pair) {
                    const auto& value = pair.second;
                    return value.actorFormID == actorFormID &&
                        value.ownerID == owner &&
                        value.mutationKey == mutation;
                });
        }

        auto FindActorValue(
            std::uint32_t actorFormID,
            std::string_view owner,
            std::string_view mutation) const
        {
            return std::ranges::find_if(
                actorValues,
                [&](const auto& pair) {
                    const auto& value = pair.second;
                    return value.actorFormID == actorFormID &&
                        value.ownerID == owner &&
                        value.mutationKey == mutation;
                });
        }

        auto FindName(
            std::uint32_t actorFormID,
            std::string_view owner,
            std::string_view mutation)
        {
            return std::ranges::find_if(
                names,
                [&](const auto& pair) {
                    const auto& value = pair.second;
                    return value.actorFormID == actorFormID &&
                        value.ownerID == owner &&
                        value.mutationKey == mutation;
                });
        }

        auto FindScale(
            std::uint32_t actorFormID,
            std::string_view owner,
            std::string_view mutation)
        {
            return std::ranges::find_if(
                scales,
                [&](const auto& pair) {
                    const auto& value = pair.second;
                    return value.actorFormID == actorFormID &&
                        value.ownerID == owner &&
                        value.mutationKey == mutation;
                });
        }

        auto FindScale(
            std::uint32_t actorFormID,
            std::string_view owner,
            std::string_view mutation) const
        {
            return std::ranges::find_if(
                scales,
                [&](const auto& pair) {
                    const auto& value = pair.second;
                    return value.actorFormID == actorFormID &&
                        value.ownerID == owner &&
                        value.mutationKey == mutation;
                });
        }

        float ScaleLedgerDelta(std::uint32_t actorFormID) const
        {
            float result = 0.0F;
            for (const auto& [key, contribution] : scales) {
                (void)key;
                if (contribution.actorFormID == actorFormID) {
                    result += contribution.appliedDelta;
                }
            }
            return result;
        }

        std::optional<float> ScaleBaseline(std::uint32_t actorFormID) const
        {
            for (const auto& [key, contribution] : scales) {
                (void)key;
                if (contribution.actorFormID == actorFormID) {
                    return contribution.baselineAtActivation;
                }
            }
            return std::nullopt;
        }

        float LedgerDelta(
            std::uint32_t actorFormID,
            RE::ActorValue target) const
        {
            float result = 0.0F;
            for (const auto& [key, contribution] : actorValues) {
                (void)key;
                if (contribution.actorFormID == actorFormID &&
                    ResolveActorValue(contribution.targetActorValue) == target) {
                    result += contribution.appliedDelta;
                }
            }
            return result;
        }

        float GroupAppliedDelta(
            std::uint32_t actorFormID,
            std::string_view targetName,
            ModifierChannel channel) const
        {
            float result = 0.0F;
            for (const auto& [key, contribution] : actorValues) {
                (void)key;
                if (contribution.actorFormID == actorFormID &&
                    contribution.targetActorValue == targetName &&
                    contribution.channel == channel) {
                    result += contribution.appliedDelta;
                }
            }
            return result;
        }

        float ChannelDelta(
            std::uint32_t actorFormID,
            std::string_view targetName,
            ModifierChannel channel) const
        {
            return GroupAppliedDelta(actorFormID, targetName, channel);
        }

        bool IsTracked(
            std::uint32_t actorFormID,
            std::string_view targetName) const
        {
            return std::ranges::any_of(actorValues, [&](const auto& pair) {
                const auto& value = pair.second;
                return value.actorFormID == actorFormID &&
                    value.targetActorValue == targetName;
                });
        }

        void UpdateCheckpoint(RE::Actor* actor, std::string_view targetName)
        {
            const auto actorValue = ResolveActorValue(targetName);
            if (!actor || actorValue == RE::ActorValue::kNone) {
                return;
            }
            auto* owner = actor->AsActorValueOwner();
            if (!owner) {
                return;
            }
            const auto actorFormID = actor->GetFormID();
            const auto ledgerPermanent = ChannelDelta(
                actorFormID, targetName, ModifierChannel::kPermanent);
            const auto ledgerTemporary = ChannelDelta(
                actorFormID, targetName, ModifierChannel::kTemporary);
            const auto permanent = actor->GetActorValueModifier(
                RE::ACTOR_VALUE_MODIFIER::kPermanent, actorValue);
            const auto temporary = actor->GetActorValueModifier(
                RE::ACTOR_VALUE_MODIFIER::kTemporary, actorValue);
            checkpoints[CheckpointKey(actorFormID, targetName)] = {
                actorFormID,
                BuildActorKey(actor),
                std::string(targetName),
                owner->GetBaseActorValue(actorValue),
                permanent - ledgerPermanent,
                temporary - ledgerTemporary,
                ledgerPermanent,
                ledgerTemporary };
        }

        template <class Contribution>
        std::optional<float> ResolveSourceValue(
            RE::Actor* actor,
            const Contribution& contribution) const
        {
            float sourceValue = 0.0F;
            switch (contribution.source) {
            case NumericSource::kFixed:
                sourceValue = contribution.fixedValue;
                break;
            case NumericSource::kGlobal: {
                const auto* global = RE::TESForm::LookupByID<RE::TESGlobal>(
                    contribution.sourceGlobalFormID);
                if (!global) {
                    return std::nullopt;
                }
                sourceValue = global->value;
                break;
            }
            case NumericSource::kActorValue: {
                const auto actorValue = ResolveActorValue(
                    contribution.sourceActorValue);
                if (actorValue == RE::ActorValue::kNone) {
                    return std::nullopt;
                }
                // Live sources use the maximum value, never damage/wear.
                sourceValue = actor->GetActorValueMax(actorValue);
                break;
            }
            default:
                return std::nullopt;
            }
            sourceValue *= contribution.sourceMultiplier;
            return std::isfinite(sourceValue) ?
                std::optional<float>{ sourceValue } : std::nullopt;
        }

        bool RecalculateGroup(
            RE::Actor* actor,
            std::string_view targetName,
            ModifierChannel channel,
            float previousAllLedgerDelta,
            float previousGroupDelta,
            std::string& error)
        {
            const auto target = ResolveActorValue(targetName);
            auto* owner = actor ? actor->AsActorValueOwner() : nullptr;
            if (!owner || target == RE::ActorValue::kNone) {
                error = "Actor Value is not available";
                return false;
            }

            const auto actorFormID = actor->GetFormID();
            const auto externalBaseline =
                actor->GetActorValueMax(target) - previousAllLedgerDelta;
            float flat = 0.0F;
            float percent = 0.0F;
            float multiplier = 1.0F;
            std::vector<ActorValueContribution*> group;
            for (auto& [key, contribution] : actorValues) {
                (void)key;
                if (contribution.actorFormID != actorFormID ||
                    contribution.targetActorValue != targetName ||
                    contribution.channel != channel) {
                    continue;
                }
                group.push_back(std::addressof(contribution));
                const auto value = contribution.resolvedSourceValue;
                switch (contribution.operation) {
                case NumericOperation::kFlat:
                    flat += value;
                    break;
                case NumericOperation::kPercent:
                    percent += value / 100.0F;
                    break;
                case NumericOperation::kMultiply:
                    multiplier *= value;
                    break;
                }
            }

            const auto desiredTotal = GroupDelta(
                externalBaseline, flat, percent, multiplier);
            const auto adjustment = desiredTotal - previousGroupDelta;
            if (!std::isfinite(desiredTotal) || !std::isfinite(adjustment)) {
                error = "Numeric calculation produced a non-finite value";
                return false;
            }

            if (std::abs(adjustment) > std::numeric_limits<float>::epsilon()) {
                ActorValueHooks::ScopedOrigin origin{
                    ActorValueHooks::Origin::kLedgerAPI };
                owner->ModActorValue(ResolveChannel(channel), target, adjustment);
            }

            std::ranges::sort(group, {}, &ActorValueContribution::sequence);
            float multiplierBase = externalBaseline * (1.0F + percent);
            for (auto* contribution : group) {
                contribution->baselineAtActivation = externalBaseline;
                switch (contribution->operation) {
                case NumericOperation::kFlat:
                    contribution->appliedDelta = contribution->resolvedSourceValue;
                    break;
                case NumericOperation::kPercent:
                    contribution->appliedDelta =
                        externalBaseline * contribution->resolvedSourceValue / 100.0F;
                    break;
                case NumericOperation::kMultiply:
                    contribution->appliedDelta = multiplierBase *
                        (contribution->resolvedSourceValue - 1.0F);
                    multiplierBase *= contribution->resolvedSourceValue;
                    break;
                }
            }
            return true;
        }

        bool LiveOrder(
            std::uint32_t actorFormID,
            std::vector<std::string>& order,
            std::string& error) const
        {
            std::set<std::string> nodes;
            for (const auto& [key, contribution] : actorValues) {
                (void)key;
                if (contribution.actorFormID == actorFormID &&
                    contribution.binding == EvaluationBinding::kLive) {
                    nodes.insert(contribution.targetActorValue);
                }
            }

            std::map<std::string, std::set<std::string>> next;
            std::map<std::string, std::size_t> incoming;
            for (const auto& node : nodes) {
                incoming[node] = 0;
            }
            for (const auto& [key, contribution] : actorValues) {
                (void)key;
                if (contribution.actorFormID != actorFormID ||
                    contribution.binding != EvaluationBinding::kLive ||
                    contribution.source != NumericSource::kActorValue ||
                    !nodes.contains(contribution.sourceActorValue)) {
                    continue;
                }
                if (contribution.sourceActorValue ==
                    contribution.targetActorValue) {
                    error = "A Live Actor Value cannot depend on itself";
                    return false;
                }
                if (next[contribution.sourceActorValue].insert(
                    contribution.targetActorValue).second) {
                    ++incoming[contribution.targetActorValue];
                }
            }

            std::set<std::string> ready;
            for (const auto& [node, count] : incoming) {
                if (count == 0) {
                    ready.insert(node);
                }
            }
            while (!ready.empty()) {
                const auto node = *ready.begin();
                ready.erase(ready.begin());
                order.push_back(node);
                for (const auto& dependent : next[node]) {
                    if (--incoming[dependent] == 0) {
                        ready.insert(dependent);
                    }
                }
            }
            if (order.size() != nodes.size()) {
                error = "Actor Value Live dependency cycle";
                return false;
            }
            return true;
        }

        bool RefreshLiveActor(RE::Actor* actor, std::string& error)
        {
            if (!actor) {
                error = "Actor is not available";
                return false;
            }
            std::vector<std::string> order;
            if (!LiveOrder(actor->GetFormID(), order, error)) {
                return false;
            }

            for (const auto& targetName : order) {
                for (auto& [key, contribution] : actorValues) {
                    (void)key;
                    if (contribution.actorFormID != actor->GetFormID() ||
                        contribution.binding != EvaluationBinding::kLive ||
                        contribution.targetActorValue != targetName) {
                        continue;
                    }
                    const auto source = ResolveSourceValue(actor, contribution);
                    if (!source ||
                        (contribution.operation == NumericOperation::kMultiply &&
                            *source < 0.0F) ||
                        (contribution.operation == NumericOperation::kPercent &&
                            *source < -100.0F)) {
                        logger::warn(
                            "[WhoEditThat] Kept the previous Live value for "
                            "{}.{} because its source is invalid",
                            contribution.ownerID,
                            contribution.mutationKey);
                        continue;
                    }
                    contribution.resolvedSourceValue = *source;
                }

                for (const auto channel : {
                         ModifierChannel::kPermanent,
                         ModifierChannel::kTemporary }) {
                    const auto hasGroup = std::ranges::any_of(
                        actorValues, [&](const auto& pair) {
                            const auto& value = pair.second;
                            return value.actorFormID == actor->GetFormID() &&
                                value.targetActorValue == targetName &&
                                value.channel == channel;
                        });
                    if (!hasGroup) {
                        continue;
                    }
                    const auto target = ResolveActorValue(targetName);
                    const auto previousAll = LedgerDelta(
                        actor->GetFormID(), target);
                    const auto previousGroup = GroupAppliedDelta(
                        actor->GetFormID(), targetName, channel);
                    if (!RecalculateGroup(
                        actor,
                        targetName,
                        channel,
                        previousAll,
                        previousGroup,
                        error)) {
                        return false;
                    }
                }
                UpdateCheckpoint(actor, targetName);
            }
            return true;
        }

        bool RemoveActorValue(
            RE::Actor* actor,
            std::map<std::string, ActorValueContribution>::iterator found,
            ActorValueContribution& removed,
            std::string& error)
        {
            const auto key = found->first;
            removed = found->second;
            const auto targetValue = ResolveActorValue(
                removed.targetActorValue);
            const auto previousAll = LedgerDelta(
                removed.actorFormID, targetValue);
            const auto previousGroup = GroupAppliedDelta(
                removed.actorFormID,
                removed.targetActorValue,
                removed.channel);
            actorValues.erase(found);
            auto success = RecalculateGroup(
                actor,
                removed.targetActorValue,
                removed.channel,
                previousAll,
                previousGroup,
                error);
            if (success) {
                success = RefreshLiveActor(actor, error);
            }
            if (success) {
                std::string scaleError;
                if (!RefreshLiveScale(actor, scaleError)) {
                    logger::warn(
                        "[WhoEditThat] Actor Scale refresh after Actor Value "
                        "removal failed on {:08X}: {}",
                        actor->GetFormID(),
                        scaleError);
                }
            }
            if (!success) {
                actorValues[key] = removed;
                return false;
            }
            if (!IsTracked(
                removed.actorFormID,
                removed.targetActorValue)) {
                checkpoints.erase(CheckpointKey(
                    removed.actorFormID,
                    removed.targetActorValue));
            }
            AddAudit(
                removed,
                "Remove",
                removed.appliedDelta,
                0.0F,
                Status::kSuccess);
            return true;
        }

        bool RecalculateScale(
            RE::Actor* actor,
            float baseline,
            float previousLedgerDelta,
            std::string& error)
        {
            if (!actor || !std::isfinite(baseline)) {
                error = "Actor scale is not available";
                return false;
            }

            const auto observed = actor->GetScale();
            const auto expectedPrevious = baseline + previousLedgerDelta;
            if (!NearlyEqual(observed, expectedPrevious) &&
                !NearlyEqual(observed, baseline)) {
                error = "Actor scale was changed outside the ledger";
                return false;
            }

            float flat = 0.0F;
            float percent = 0.0F;
            float multiplier = 1.0F;
            std::vector<ActorScaleContribution*> group;
            for (auto& [key, contribution] : scales) {
                (void)key;
                if (contribution.actorFormID != actor->GetFormID()) {
                    continue;
                }
                group.push_back(std::addressof(contribution));
                contribution.baselineAtActivation = baseline;
                switch (contribution.operation) {
                case NumericOperation::kFlat:
                    flat += contribution.resolvedSourceValue;
                    break;
                case NumericOperation::kPercent:
                    percent += contribution.resolvedSourceValue / 100.0F;
                    break;
                case NumericOperation::kMultiply:
                    multiplier *= contribution.resolvedSourceValue;
                    break;
                }
            }

            const auto desired = baseline +
                GroupDelta(baseline, flat, percent, multiplier);
            if (!std::isfinite(desired) || desired < kMinimumActorScale ||
                desired > kMaximumActorScale) {
                error = std::format(
                    "Actor scale must be between {:.2f} and {:.2f}",
                    kMinimumActorScale,
                    kMaximumActorScale);
                return false;
            }

            actor->SetScale(desired);
            const auto appliedScale = actor->GetScale();
            if (!std::isfinite(appliedScale)) {
                error = "Actor scale produced a non-finite value";
                return false;
            }

            std::ranges::sort(group, {}, &ActorScaleContribution::sequence);
            float attributed = 0.0F;
            const auto percentBase = baseline;
            auto multiplierBase = baseline * (1.0F + percent);
            for (auto* contribution : group) {
                switch (contribution->operation) {
                case NumericOperation::kFlat:
                    contribution->appliedDelta = contribution->resolvedSourceValue;
                    break;
                case NumericOperation::kPercent:
                    contribution->appliedDelta = percentBase *
                        contribution->resolvedSourceValue / 100.0F;
                    break;
                case NumericOperation::kMultiply:
                    contribution->appliedDelta = multiplierBase *
                        (contribution->resolvedSourceValue - 1.0F);
                    multiplierBase *= contribution->resolvedSourceValue;
                    break;
                }
                attributed += contribution->appliedDelta;
            }
            if (!group.empty()) {
                group.back()->appliedDelta +=
                    appliedScale - baseline - attributed;
            }
            return true;
        }

        bool RefreshLiveScale(RE::Actor* actor, std::string& error)
        {
            if (!actor) {
                error = "Actor is not available";
                return false;
            }
            const auto baseline = ScaleBaseline(actor->GetFormID());
            if (!baseline) {
                return true;
            }
            const auto previousDelta = ScaleLedgerDelta(actor->GetFormID());
            for (auto& [key, contribution] : scales) {
                (void)key;
                if (contribution.actorFormID != actor->GetFormID() ||
                    contribution.binding != EvaluationBinding::kLive) {
                    continue;
                }
                const auto source = ResolveSourceValue(actor, contribution);
                if (!source ||
                    (contribution.operation == NumericOperation::kMultiply &&
                        *source < 0.0F) ||
                    (contribution.operation == NumericOperation::kPercent &&
                        *source < -100.0F)) {
                    logger::warn(
                        "[WhoEditThat] Kept the previous Live scale value for "
                        "{}.{} because its source is invalid",
                        contribution.ownerID,
                        contribution.mutationKey);
                    continue;
                }
                contribution.resolvedSourceValue = *source;
            }
            return RecalculateScale(
                actor, *baseline, previousDelta, error);
        }

        bool RemoveScale(
            RE::Actor* actor,
            std::map<std::string, ActorScaleContribution>::iterator found,
            ActorScaleContribution& removed,
            std::string& error)
        {
            const auto key = found->first;
            removed = found->second;
            const auto previousDelta = ScaleLedgerDelta(removed.actorFormID);
            const auto baseline = removed.baselineAtActivation;
            scales.erase(found);
            if (!RecalculateScale(actor, baseline, previousDelta, error)) {
                scales[key] = removed;
                return false;
            }
            AddAudit(
                removed,
                "Remove",
                removed.appliedDelta,
                0.0F,
                Status::kSuccess);
            return true;
        }

        void RecordExternalMutation(
            RE::Actor* actor,
            const ActorValueHooks::MutationEvent& event)
        {
            const auto* rawName = RE::ActorValueList::GetActorValueName(
                event.actorValue);
            if (!actor || !rawName ||
                !IsTracked(event.actorFormID, rawName)) {
                return;
            }
            const bool durable =
                !NearlyEqual(event.before.base, event.after.base) ||
                !NearlyEqual(event.before.permanent, event.after.permanent);
            const bool transient =
                !NearlyEqual(event.before.temporary, event.after.temporary);
            if (!durable && !transient) {
                return;
            }

            externalAudit.push_back({
                nextSequence++,
                generation.load(std::memory_order_acquire),
                event.actorFormID,
                BuildActorKey(actor),
                rawName,
                event.kind,
                event.modifier,
                event.requested,
                event.before,
                event.after,
                durable });
            logger::debug(
                "[WhoEditThat] Tracked external Actor Value mutation: "
                "actor={:08X} av={} kind={} modifier={} requested={} "
                "durable={} max:{}->{}",
                event.actorFormID,
                rawName,
                std::to_underlying(event.kind),
                static_cast<std::uint32_t>(event.modifier),
                event.requested,
                durable,
                event.before.maximum,
                event.after.maximum);

            const auto ledgerPermanent = ChannelDelta(
                event.actorFormID, rawName, ModifierChannel::kPermanent);
            const auto ledgerTemporary = ChannelDelta(
                event.actorFormID, rawName, ModifierChannel::kTemporary);
            auto& checkpoint = checkpoints[CheckpointKey(
                event.actorFormID, rawName)];
            checkpoint.actorFormID = event.actorFormID;
            checkpoint.actorKey = BuildActorKey(actor);
            checkpoint.actorValue = rawName;
            checkpoint.externalBase = event.after.base;
            checkpoint.externalPermanent =
                event.after.permanent - ledgerPermanent;
            checkpoint.externalTemporary =
                event.after.temporary - ledgerTemporary;
            checkpoint.ledgerPermanent = ledgerPermanent;
            checkpoint.ledgerTemporary = ledgerTemporary;
        }

        void ReconcileActor(RE::Actor* actor)
        {
            if (!actor) {
                return;
            }
            auto* owner = actor->AsActorValueOwner();
            if (!owner) {
                return;
            }
            for (auto& [key, checkpoint] : checkpoints) {
                (void)key;
                if (checkpoint.actorFormID != actor->GetFormID() ||
                    !IsTracked(checkpoint.actorFormID, checkpoint.actorValue)) {
                    continue;
                }
                const auto actorValue = ResolveActorValue(checkpoint.actorValue);
                if (actorValue == RE::ActorValue::kNone) {
                    continue;
                }

                checkpoint.ledgerPermanent = ChannelDelta(
                    checkpoint.actorFormID,
                    checkpoint.actorValue,
                    ModifierChannel::kPermanent);
                checkpoint.ledgerTemporary = ChannelDelta(
                    checkpoint.actorFormID,
                    checkpoint.actorValue,
                    ModifierChannel::kTemporary);
                checkpoint.externalBase = owner->GetBaseActorValue(actorValue);

                const auto reconcileChannel = [&](
                    RE::ACTOR_VALUE_MODIFIER modifier,
                    float& external,
                    float ledger,
                    std::string_view channelName) {
                        const auto observed = actor->GetActorValueModifier(
                            modifier, actorValue);
                        const auto expected = external + ledger;
                        if (NearlyEqual(observed, expected)) {
                            return;
                        }
                        if (!NearlyEqual(ledger, 0.0F) &&
                            NearlyEqual(observed, external)) {
                            ActorValueHooks::ScopedOrigin origin{
                                ActorValueHooks::Origin::kRecovery };
                            owner->ModActorValue(modifier, actorValue, ledger);
                            audit.push_back({
                                nextSequence++,
                                generation.load(std::memory_order_acquire),
                                actor->GetFormID(),
                                BuildActorKey(actor),
                                "__whoeditthat__",
                                checkpoint.actorValue,
                                "ActorValueRecovery",
                                std::string(channelName),
                                observed,
                                observed + ledger,
                                Status::kSuccess });
                            logger::debug(
                                "[WhoEditThat] Recovered {} {} {} on {:08X}",
                                ledger,
                                channelName,
                                checkpoint.actorValue,
                                actor->GetFormID());
                            return;
                        }

                        external = observed - ledger;
                        audit.push_back({
                            nextSequence++,
                            generation.load(std::memory_order_acquire),
                            actor->GetFormID(),
                            BuildActorKey(actor),
                            "External/Unknown",
                            checkpoint.actorValue,
                            "ActorValueRecovery",
                            "Conflict",
                            expected,
                            observed,
                            Status::kConflict });
                        logger::warn(
                            "[WhoEditThat] Ambiguous {} {} state on {:08X}; "
                            "expected {}, observed {}",
                            channelName,
                            checkpoint.actorValue,
                            actor->GetFormID(),
                            expected,
                            observed);
                    };

                reconcileChannel(
                    RE::ACTOR_VALUE_MODIFIER::kPermanent,
                    checkpoint.externalPermanent,
                    checkpoint.ledgerPermanent,
                    "Permanent");
                reconcileChannel(
                    RE::ACTOR_VALUE_MODIFIER::kTemporary,
                    checkpoint.externalTemporary,
                    checkpoint.ledgerTemporary,
                    "Temporary");
            }

            const auto scaleBaseline = ScaleBaseline(actor->GetFormID());
            if (scaleBaseline) {
                const auto ledgerDelta = ScaleLedgerDelta(actor->GetFormID());
                const auto observed = actor->GetScale();
                const auto expected = *scaleBaseline + ledgerDelta;
                if (!NearlyEqual(observed, expected)) {
                    auto baseline = *scaleBaseline;
                    const auto recovered = NearlyEqual(observed, baseline);
                    if (!recovered) {
                        baseline = observed;
                        for (auto& [key, contribution] : scales) {
                            (void)key;
                            if (contribution.actorFormID == actor->GetFormID()) {
                                contribution.baselineAtActivation = baseline;
                            }
                        }
                    }
                    std::string error;
                    if (RefreshLiveScale(actor, error)) {
                        audit.push_back({
                            nextSequence++,
                            generation.load(std::memory_order_acquire),
                            actor->GetFormID(),
                            BuildActorKey(actor),
                            recovered ? "__whoeditthat__" : "External/Unknown",
                            "Scale",
                            "ActorScaleRecovery",
                            recovered ? "Recover" : "Rebase",
                            observed,
                            actor->GetScale(),
                            recovered ? Status::kSuccess : Status::kConflict });
                        logger::debug(
                            "[WhoEditThat] {} Actor Scale on {:08X}: {} -> {}",
                            recovered ? "Recovered" : "Rebased",
                            actor->GetFormID(),
                            observed,
                            actor->GetScale());
                    }
                    else {
                        logger::warn(
                            "[WhoEditThat] Could not reconcile Actor Scale on "
                            "{:08X}: {}",
                            actor->GetFormID(),
                            error);
                    }
                }
            }
        }

        std::optional<DisplayNameOverride*> WinningName(std::uint32_t actorFormID)
        {
            DisplayNameOverride* winner = nullptr;
            for (auto& [key, override] : names) {
                (void)key;
                if (override.actorFormID != actorFormID) {
                    continue;
                }
                if (!winner || override.priority > winner->priority ||
                    (override.priority == winner->priority &&
                        override.sequence > winner->sequence)) {
                    winner = std::addressof(override);
                }
            }
            return winner ? std::optional{ winner } : std::nullopt;
        }

        Status SetReferenceDisplayName(
            RE::Actor* actor,
            std::string_view desiredName)
        {
            if (!actor) {
                return Status::kActorUnavailable;
            }

            const std::string desired{ desiredName };
            if (!actor->SetDisplayName(RE::BSFixedString(desired), true)) {
                logger::error(
                    "[WhoEditThat] SetDisplayName rejected actor {:08X} -> '{}'",
                    actor->GetFormID(),
                    desired);
                return Status::kInternalError;
            }

            // SetDisplayName writes ExtraTextDisplayData on the reference.
            // Mark game-only extra data as changed so Skyrim also treats the
            // reference-level override as dirty for its own save pipeline.
            actor->AddChange(
                static_cast<std::uint32_t>(
                    RE::TESObjectREFR::ChangeFlags::kGameOnlyExtra));

            const auto* rawApplied = actor->GetDisplayFullName();
            const std::string applied = rawApplied ? rawApplied : "";
            if (applied != desired) {
                logger::error(
                    "[WhoEditThat] Display name verification failed on {:08X}: "
                    "requested='{}' observed='{}'",
                    actor->GetFormID(),
                    desired,
                    applied);
                return Status::kInternalError;
            }

            return Status::kSuccess;
        }

        Status ApplyWinningName(
            RE::Actor* actor,
            std::string_view previousLastApplied,
            std::string_view originalName)
        {
            if (!actor) {
                return Status::kActorUnavailable;
            }

            const auto* rawCurrent = actor->GetDisplayFullName();
            const std::string current = rawCurrent ? rawCurrent : "";

            std::string desired = std::string(originalName);
            if (const auto winner = WinningName(actor->GetFormID())) {
                desired = (*winner)->requestedName;
            }

            if (current == desired) {
                for (auto& [key, override] : names) {
                    (void)key;
                    if (override.actorFormID == actor->GetFormID()) {
                        override.originalName = std::string(originalName);
                        override.lastAppliedName = desired;
                    }
                }
                return Status::kSuccess;
            }

            if (!previousLastApplied.empty() && current != previousLastApplied) {
                return Status::kConflict;
            }

            const auto status = SetReferenceDisplayName(actor, desired);
            if (status != Status::kSuccess) {
                return status;
            }

            for (auto& [key, override] : names) {
                (void)key;
                if (override.actorFormID == actor->GetFormID()) {
                    override.originalName = std::string(originalName);
                    override.lastAppliedName = desired;
                }
            }
            return Status::kSuccess;
        }

        void ReconcileDisplayName(RE::Actor* actor)
        {
            if (!actor) {
                return;
            }

            const auto winner = WinningName(actor->GetFormID());
            if (!winner) {
                return;
            }

            auto* winningOverride = *winner;
            const std::string desired = winningOverride->requestedName;
            const auto* rawCurrent = actor->GetDisplayFullName();
            const std::string current = rawCurrent ? rawCurrent : "";

            if (current == desired) {
                for (auto& [key, override] : names) {
                    (void)key;
                    if (override.actorFormID == actor->GetFormID()) {
                        override.lastAppliedName = desired;
                    }
                }
                return;
            }

            // A reset/load can remove ExtraTextDisplayData and expose the
            // original name again. That exact state is safe to recover.
            // Any third-party name is treated as a conflict and left intact.
            const bool canRecover =
                winningOverride->lastAppliedName.empty() ||
                current == winningOverride->originalName;
            if (!canRecover) {
                AddAudit(*winningOverride, "RecoveryConflict", Status::kConflict);
                logger::warn(
                    "[WhoEditThat] Display name conflict on {:08X}: "
                    "expected='{}' original='{}' observed='{}'",
                    actor->GetFormID(),
                    desired,
                    winningOverride->originalName,
                    current);
                return;
            }

            const std::string originalName = winningOverride->originalName;
            const auto status = SetReferenceDisplayName(actor, desired);
            if (status != Status::kSuccess) {
                AddAudit(*winningOverride, "RecoveryFailed", status);
                logger::warn(
                    "[WhoEditThat] Could not recover display name on {:08X}: "
                    "'{}'",
                    actor->GetFormID(),
                    desired);
                return;
            }

            for (auto& [key, override] : names) {
                (void)key;
                if (override.actorFormID == actor->GetFormID()) {
                    override.originalName = originalName;
                    override.lastAppliedName = desired;
                }
            }

            AddAudit(*winningOverride, "Recover", Status::kSuccess);
            logger::debug(
                "[WhoEditThat] Recovered display name on {:08X}: '{}' -> '{}'",
                actor->GetFormID(),
                current,
                desired);
        }
    };

    Manager::Manager() : _impl(std::make_unique<Impl>()) {}
    Manager::~Manager() = default;

    Manager& Manager::GetSingleton()
    {
        static Manager singleton;
        return singleton;
    }

    void Manager::Initialize()
    {
        ActorValueHooks::SetObserver([](
            const ActorValueHooks::MutationEvent& event) {
                Manager::GetSingleton().QueueExternalMutation(event);
            });
        logger::info("[WhoEditThat] API-only ledger initialized");
    }

    bool Manager::IsReady() const noexcept
    {
        return _impl->state.load(std::memory_order_acquire) ==
            ContextState::kReady;
    }

    ClientHandle Manager::RegisterClient(
        const ClientRegistration& registration)
    {
        if (!registration.clientID || !*registration.clientID ||
            std::string_view(registration.clientID).size() > 95) {
            return kInvalidClient;
        }
        std::scoped_lock lock(_impl->mutex);
        const std::string clientID = registration.clientID;
        if (const auto existing = _impl->clientHandles.find(clientID);
            existing != _impl->clientHandles.end()) {
            return existing->second;
        }
        const auto handle = _impl->nextClientHandle++;
        _impl->clients.emplace(
            handle,
            Client{
                clientID,
                registration.displayName && *registration.displayName ?
                    registration.displayName : clientID });
        _impl->clientHandles.emplace(clientID, handle);
        return handle;
    }

    void Manager::BeginLoad(
        std::uint32_t characterID,
        std::uint32_t saveNumber,
        std::string_view saveName)
    {
        std::scoped_lock lock(_impl->mutex);
        _impl->state.store(ContextState::kLoading, std::memory_order_release);
        _impl->CancelContext(true);
        _impl->context.characterID = characterID;
        _impl->context.saveNumber = saveNumber;
        _impl->context.saveName = fs::path(saveName).filename().string();
        logger::info(
            "[WhoEditThat] Begin load generation {}: {:08X} '{}' #{}",
            _impl->generation.load(),
            characterID,
            _impl->context.saveName,
            saveNumber);
    }

    void Manager::StartNewGame()
    {
        std::scoped_lock lock(_impl->mutex);
        _impl->CancelContext(false);
        _impl->context = {};
        _impl->state.store(ContextState::kReady, std::memory_order_release);
        logger::info(
            "[WhoEditThat] New game generation {} ready in memory",
            _impl->generation.load());
    }

    void Manager::Revert()
    {
        std::scoped_lock lock(_impl->mutex);
        const auto loading = _impl->state.load(std::memory_order_acquire) ==
            ContextState::kLoading;
        _impl->CancelContext(loading);
        logger::info("[WhoEditThat] Context reverted; queued work invalidated");
    }

    void Manager::QueueExternalMutation(
        const ActorValueHooks::MutationEvent& event)
    {
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load(std::memory_order_acquire) !=
                ContextState::kReady) {
                return;
            }
            const auto* rawName = RE::ActorValueList::GetActorValueName(
                event.actorValue);
            if (!rawName || !_impl->IsTracked(event.actorFormID, rawName)) {
                return;
            }
            requestGeneration = _impl->generation.load(std::memory_order_acquire);
        }
        SKSE::GetTaskInterface()->AddTask(
            [this, event, requestGeneration] {
                std::scoped_lock lock(_impl->mutex);
                if (_impl->generation.load(std::memory_order_acquire) !=
                    requestGeneration ||
                    _impl->state.load(std::memory_order_acquire) !=
                    ContextState::kReady) {
                    return;
                }
                _impl->RecordExternalMutation(
                    LookupActor(event.actorFormID), event);
            });
    }

    void Manager::QueueReconcileActor(std::uint32_t actorFormID)
    {
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (actorFormID == 0 ||
                _impl->state.load(std::memory_order_acquire) !=
                ContextState::kReady) {
                return;
            }
            requestGeneration = _impl->generation.load(std::memory_order_acquire);
        }
        SKSE::GetTaskInterface()->AddTask(
            [this, actorFormID, requestGeneration] {
                std::scoped_lock lock(_impl->mutex);
                if (_impl->generation.load(std::memory_order_acquire) !=
                    requestGeneration ||
                    _impl->state.load(std::memory_order_acquire) !=
                    ContextState::kReady) {
                    return;
                }
                if (auto* actor = LookupActor(actorFormID)) {
                    _impl->ReconcileActor(actor);
                    _impl->ReconcileDisplayName(actor);
                }
            });
    }
}

namespace WhoEditThat
{
    void Manager::FinishLoad()
    {
        std::scoped_lock lock(_impl->mutex);
        if (_impl->state.load(std::memory_order_acquire) !=
            ContextState::kLoading) {
            return;
        }

        const auto path = DatabasePath(_impl->context.characterID);
        if (_impl->context.characterID == 0 ||
            _impl->context.saveName.empty() || !fs::exists(path)) {
            _impl->state.store(ContextState::kReady, std::memory_order_release);
            logger::info("[WhoEditThat] No exact ledger snapshot; starting empty");
            return;
        }

        SqliteDb database;
        if (!OpenDatabase(path, database) || !EnsureSchema(database.handle)) {
            _impl->state.store(
                ContextState::kNoContext, std::memory_order_release);
            logger::error("[WhoEditThat] Load aborted because the database is invalid");
            return;
        }

        Statement snapshot;
        if (!Prepare(
            database.handle,
            "SELECT snapshot_id FROM save_snapshots "
            "WHERE save_number=?1 AND save_name=?2 "
            "ORDER BY snapshot_id DESC LIMIT 1;",
            snapshot)) {
            _impl->state.store(ContextState::kNoContext);
            return;
        }
        sqlite3_bind_int64(
            snapshot.handle, 1, _impl->context.saveNumber);
        BindText(snapshot.handle, 2, _impl->context.saveName);
        if (sqlite3_step(snapshot.handle) != SQLITE_ROW) {
            _impl->state.store(ContextState::kReady, std::memory_order_release);
            logger::info(
                "[WhoEditThat] No snapshot for '{}' #{}; no prior save was inherited",
                _impl->context.saveName,
                _impl->context.saveNumber);
            return;
        }
        const auto snapshotID = sqlite3_column_int64(snapshot.handle, 0);

        Statement actorValues;
        if (!Prepare(
            database.handle,
            "SELECT actor_form_id,actor_key,owner_id,mutation_key,"
            "target_actor_value,operation,source,channel,binding,fixed_value,"
            "source_global_form_id,source_actor_value,source_multiplier,baseline,"
            "resolved_source,applied_delta,sequence "
            "FROM actor_value_contributions WHERE snapshot_id=?1;",
            actorValues)) {
            _impl->state.store(ContextState::kNoContext);
            return;
        }
        sqlite3_bind_int64(actorValues.handle, 1, snapshotID);
        while (sqlite3_step(actorValues.handle) == SQLITE_ROW) {
            ActorValueContribution value;
            value.actorKey = ColumnText(actorValues.handle, 1);
            const auto storedActorID = static_cast<std::uint32_t>(
                sqlite3_column_int64(actorValues.handle, 0));
            value.actorFormID = ResolveStoredActorID(
                storedActorID, value.actorKey, _impl->resolvedForms);
            value.ownerID = ColumnText(actorValues.handle, 2);
            value.mutationKey = ColumnText(actorValues.handle, 3);
            value.targetActorValue = ColumnText(actorValues.handle, 4);
            value.operation = static_cast<NumericOperation>(
                sqlite3_column_int(actorValues.handle, 5));
            value.source = static_cast<NumericSource>(
                sqlite3_column_int(actorValues.handle, 6));
            value.channel = static_cast<ModifierChannel>(
                sqlite3_column_int(actorValues.handle, 7));
            value.binding = kDefaultBinding;
            value.fixedValue = static_cast<float>(
                sqlite3_column_double(actorValues.handle, 9));
            const auto storedGlobalID = static_cast<std::uint32_t>(
                sqlite3_column_int64(actorValues.handle, 10));
            value.sourceGlobalFormID = storedGlobalID == 0 ? 0 :
                (_impl->resolvedForms.contains(storedGlobalID) ?
                    _impl->resolvedForms.at(storedGlobalID) : 0);
            value.sourceActorValue = ColumnText(actorValues.handle, 11);
            value.sourceMultiplier = static_cast<float>(
                sqlite3_column_double(actorValues.handle, 12));
            value.baselineAtActivation = static_cast<float>(
                sqlite3_column_double(actorValues.handle, 13));
            value.resolvedSourceValue = static_cast<float>(
                sqlite3_column_double(actorValues.handle, 14));
            value.appliedDelta = static_cast<float>(
                sqlite3_column_double(actorValues.handle, 15));
            value.sequence = static_cast<std::uint64_t>(
                sqlite3_column_int64(actorValues.handle, 16));
            _impl->nextSequence = std::max(
                _impl->nextSequence, value.sequence + 1);
            _impl->actorValues.emplace(
                CompositeKey(
                    value.actorFormID, value.ownerID, value.mutationKey),
                std::move(value));
        }

        Statement actorScales;
        if (!Prepare(
            database.handle,
            "SELECT actor_form_id,actor_key,owner_id,mutation_key,"
            "operation,source,binding,fixed_value,source_global_form_id,"
            "source_actor_value,source_multiplier,baseline,resolved_source,"
            "applied_delta,sequence FROM actor_scale_contributions "
            "WHERE snapshot_id=?1;",
            actorScales)) {
            _impl->state.store(ContextState::kNoContext);
            return;
        }
        sqlite3_bind_int64(actorScales.handle, 1, snapshotID);
        while (sqlite3_step(actorScales.handle) == SQLITE_ROW) {
            ActorScaleContribution value;
            value.actorKey = ColumnText(actorScales.handle, 1);
            const auto storedActorID = static_cast<std::uint32_t>(
                sqlite3_column_int64(actorScales.handle, 0));
            value.actorFormID = ResolveStoredActorID(
                storedActorID, value.actorKey, _impl->resolvedForms);
            value.ownerID = ColumnText(actorScales.handle, 2);
            value.mutationKey = ColumnText(actorScales.handle, 3);
            value.operation = static_cast<NumericOperation>(
                sqlite3_column_int(actorScales.handle, 4));
            value.source = static_cast<NumericSource>(
                sqlite3_column_int(actorScales.handle, 5));
            value.binding = kDefaultBinding;
            value.fixedValue = static_cast<float>(
                sqlite3_column_double(actorScales.handle, 7));
            const auto storedGlobalID = static_cast<std::uint32_t>(
                sqlite3_column_int64(actorScales.handle, 8));
            value.sourceGlobalFormID = storedGlobalID == 0 ? 0 :
                (_impl->resolvedForms.contains(storedGlobalID) ?
                    _impl->resolvedForms.at(storedGlobalID) : 0);
            value.sourceActorValue = ColumnText(actorScales.handle, 9);
            value.sourceMultiplier = static_cast<float>(
                sqlite3_column_double(actorScales.handle, 10));
            value.baselineAtActivation = static_cast<float>(
                sqlite3_column_double(actorScales.handle, 11));
            value.resolvedSourceValue = static_cast<float>(
                sqlite3_column_double(actorScales.handle, 12));
            value.appliedDelta = static_cast<float>(
                sqlite3_column_double(actorScales.handle, 13));
            value.sequence = static_cast<std::uint64_t>(
                sqlite3_column_int64(actorScales.handle, 14));
            _impl->nextSequence = std::max(
                _impl->nextSequence, value.sequence + 1);
            _impl->scales.emplace(
                CompositeKey(
                    value.actorFormID, value.ownerID, value.mutationKey),
                std::move(value));
        }

        Statement names;
        if (!Prepare(
            database.handle,
            "SELECT actor_form_id,actor_key,owner_id,mutation_key,"
            "requested_name,original_name,last_applied_name,priority,sequence "
            "FROM name_overrides WHERE snapshot_id=?1;",
            names)) {
            _impl->state.store(ContextState::kNoContext);
            return;
        }
        sqlite3_bind_int64(names.handle, 1, snapshotID);
        while (sqlite3_step(names.handle) == SQLITE_ROW) {
            DisplayNameOverride value;
            value.actorKey = ColumnText(names.handle, 1);
            const auto storedActorID = static_cast<std::uint32_t>(
                sqlite3_column_int64(names.handle, 0));
            value.actorFormID = ResolveStoredActorID(
                storedActorID, value.actorKey, _impl->resolvedForms);
            value.ownerID = ColumnText(names.handle, 2);
            value.mutationKey = ColumnText(names.handle, 3);
            value.requestedName = ColumnText(names.handle, 4);
            value.originalName = ColumnText(names.handle, 5);
            value.lastAppliedName = ColumnText(names.handle, 6);
            value.priority = sqlite3_column_int(names.handle, 7);
            value.sequence = static_cast<std::uint64_t>(
                sqlite3_column_int64(names.handle, 8));
            _impl->nextSequence = std::max(
                _impl->nextSequence, value.sequence + 1);
            _impl->names.emplace(
                CompositeKey(
                    value.actorFormID, value.ownerID, value.mutationKey),
                std::move(value));
        }

        Statement checkpointRows;
        if (!Prepare(
            database.handle,
            "SELECT actor_form_id,actor_key,actor_value,external_base,"
            "external_permanent,external_temporary,ledger_permanent,"
            "ledger_temporary FROM actor_value_checkpoints "
            "WHERE snapshot_id=?1;",
            checkpointRows)) {
            _impl->state.store(ContextState::kNoContext);
            return;
        }
        sqlite3_bind_int64(checkpointRows.handle, 1, snapshotID);
        while (sqlite3_step(checkpointRows.handle) == SQLITE_ROW) {
            ActorValueCheckpoint value;
            value.actorKey = ColumnText(checkpointRows.handle, 1);
            const auto storedActorID = static_cast<std::uint32_t>(
                sqlite3_column_int64(checkpointRows.handle, 0));
            value.actorFormID = ResolveStoredActorID(
                storedActorID, value.actorKey, _impl->resolvedForms);
            value.actorValue = ColumnText(checkpointRows.handle, 2);
            value.externalBase = static_cast<float>(
                sqlite3_column_double(checkpointRows.handle, 3));
            value.externalPermanent = static_cast<float>(
                sqlite3_column_double(checkpointRows.handle, 4));
            value.externalTemporary = static_cast<float>(
                sqlite3_column_double(checkpointRows.handle, 5));
            value.ledgerPermanent = static_cast<float>(
                sqlite3_column_double(checkpointRows.handle, 6));
            value.ledgerTemporary = static_cast<float>(
                sqlite3_column_double(checkpointRows.handle, 7));
            if (value.actorFormID != 0 &&
                _impl->IsTracked(value.actorFormID, value.actorValue)) {
                _impl->checkpoints.emplace(
                    CheckpointKey(value.actorFormID, value.actorValue),
                    std::move(value));
            }
        }

        for (const auto& [key, contribution] : _impl->actorValues) {
            (void)key;
            const auto checkpointKey = CheckpointKey(
                contribution.actorFormID, contribution.targetActorValue);
            if (!_impl->checkpoints.contains(checkpointKey)) {
                _impl->UpdateCheckpoint(
                    LookupActor(contribution.actorFormID),
                    contribution.targetActorValue);
            }
        }

        _impl->state.store(ContextState::kReady, std::memory_order_release);
        std::set<std::uint32_t> trackedActors;
        for (const auto& [key, checkpoint] : _impl->checkpoints) {
            (void)key;
            trackedActors.insert(checkpoint.actorFormID);
        }
        for (const auto& [key, contribution] : _impl->scales) {
            (void)key;
            trackedActors.insert(contribution.actorFormID);
        }
        for (const auto& [key, override] : _impl->names) {
            (void)key;
            if (override.actorFormID != 0) {
                trackedActors.insert(override.actorFormID);
            }
        }
        for (const auto actorFormID : trackedActors) {
            if (auto* actor = LookupActor(actorFormID)) {
                _impl->ReconcileActor(actor);
                _impl->ReconcileDisplayName(actor);
            }
        }
        logger::info(
            "[WhoEditThat] Exact snapshot loaded: {} Actor Values, {} scales, "
            "{} names, {} checkpoints",
            _impl->actorValues.size(),
            _impl->scales.size(),
            _impl->names.size(),
            _impl->checkpoints.size());
    }

    void Manager::PersistCurrentSave(std::string_view saveName)
    {
        std::scoped_lock lock(_impl->mutex);
        if (_impl->state.load(std::memory_order_acquire) !=
            ContextState::kReady) {
            logger::warn("[WhoEditThat] Save ignored without a ready context");
            return;
        }

        const auto cleanName = fs::path(saveName).filename().string();
        RE::BGSSaveLoadFileEntry saveEntry{};
        saveEntry.fileName = cleanName.c_str();
        const auto populated = saveEntry.PopulateFileEntryData();
        if (populated && saveEntry.characterID != 0) {
            _impl->context.characterID = saveEntry.characterID;
        }
        if (_impl->context.characterID == 0) {
            _impl->context.characterID = ParseCharacterID(cleanName);
        }
        if (_impl->context.characterID == 0) {
            logger::error("[WhoEditThat] Could not identify the character for '{}'.", cleanName);
            return;
        }
        _impl->context.saveNumber = populated ?
            saveEntry.saveNumber :
            ParseSaveNumber(cleanName, _impl->context.saveNumber);
        _impl->context.saveName = cleanName;

        const auto path = DatabasePath(_impl->context.characterID);
        SqliteDb database;
        if (!OpenDatabase(path, database) || !EnsureSchema(database.handle) ||
            !Exec(database.handle, "BEGIN IMMEDIATE;")) {
            return;
        }

        const auto rollback = [&] {
            Exec(database.handle, "ROLLBACK;");
            };

        Statement client;
        if (!Prepare(
            database.handle,
            "INSERT INTO clients(client_id,display_name) VALUES(?1,?2) "
            "ON CONFLICT(client_id) DO UPDATE SET display_name=excluded.display_name;",
            client)) {
            rollback();
            return;
        }
        for (const auto& [handle, value] : _impl->clients) {
            (void)handle;
            sqlite3_reset(client.handle);
            sqlite3_clear_bindings(client.handle);
            BindText(client.handle, 1, value.id);
            BindText(client.handle, 2, value.displayName);
            if (sqlite3_step(client.handle) != SQLITE_DONE) {
                rollback();
                return;
            }
        }

        Statement upsertSnapshot;
        if (!Prepare(
            database.handle,
            "INSERT INTO save_snapshots(save_number,save_name,created_at) "
            "VALUES(?1,?2,?3) ON CONFLICT(save_number,save_name) "
            "DO UPDATE SET created_at=excluded.created_at;",
            upsertSnapshot)) {
            rollback();
            return;
        }
        sqlite3_bind_int64(
            upsertSnapshot.handle, 1, _impl->context.saveNumber);
        BindText(upsertSnapshot.handle, 2, _impl->context.saveName);
        sqlite3_bind_int64(
            upsertSnapshot.handle,
            3,
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (sqlite3_step(upsertSnapshot.handle) != SQLITE_DONE) {
            rollback();
            return;
        }

        Statement selectSnapshot;
        if (!Prepare(
            database.handle,
            "SELECT snapshot_id FROM save_snapshots "
            "WHERE save_number=?1 AND save_name=?2;",
            selectSnapshot)) {
            rollback();
            return;
        }
        sqlite3_bind_int64(
            selectSnapshot.handle, 1, _impl->context.saveNumber);
        BindText(selectSnapshot.handle, 2, _impl->context.saveName);
        if (sqlite3_step(selectSnapshot.handle) != SQLITE_ROW) {
            rollback();
            return;
        }
        const auto snapshotID = sqlite3_column_int64(selectSnapshot.handle, 0);

        Statement clearActorValues;
        Statement clearScales;
        Statement clearNames;
        Statement clearCheckpoints;
        if (!Prepare(
            database.handle,
            "DELETE FROM actor_value_contributions WHERE snapshot_id=?1;",
            clearActorValues) ||
            !Prepare(
                database.handle,
                "DELETE FROM actor_scale_contributions WHERE snapshot_id=?1;",
                clearScales) ||
            !Prepare(
                database.handle,
                "DELETE FROM name_overrides WHERE snapshot_id=?1;",
                clearNames) ||
            !Prepare(
                database.handle,
                "DELETE FROM actor_value_checkpoints WHERE snapshot_id=?1;",
                clearCheckpoints)) {
            rollback();
            return;
        }
        for (auto* statement : {
                 clearActorValues.handle,
                 clearScales.handle,
                 clearNames.handle,
                 clearCheckpoints.handle }) {
            sqlite3_bind_int64(statement, 1, snapshotID);
            if (sqlite3_step(statement) != SQLITE_DONE) {
                rollback();
                return;
            }
        }

        Statement insertActorValue;
        if (!Prepare(
            database.handle,
            "INSERT INTO actor_value_contributions VALUES("
            "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);",
            insertActorValue)) {
            rollback();
            return;
        }
        for (const auto& [key, value] : _impl->actorValues) {
            (void)key;
            auto* statement = insertActorValue.handle;
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            sqlite3_bind_int64(statement, 1, snapshotID);
            sqlite3_bind_int64(statement, 2, value.actorFormID);
            BindText(statement, 3, value.actorKey);
            BindText(statement, 4, value.ownerID);
            BindText(statement, 5, value.mutationKey);
            BindText(statement, 6, value.targetActorValue);
            sqlite3_bind_int(statement, 7, std::to_underlying(value.operation));
            sqlite3_bind_int(statement, 8, std::to_underlying(value.source));
            sqlite3_bind_int(statement, 9, std::to_underlying(value.channel));
            sqlite3_bind_int(statement, 10, std::to_underlying(value.binding));
            sqlite3_bind_double(statement, 11, value.fixedValue);
            sqlite3_bind_int64(statement, 12, value.sourceGlobalFormID);
            BindText(statement, 13, value.sourceActorValue);
            sqlite3_bind_double(statement, 14, value.sourceMultiplier);
            sqlite3_bind_double(statement, 15, value.baselineAtActivation);
            sqlite3_bind_double(statement, 16, value.resolvedSourceValue);
            sqlite3_bind_double(statement, 17, value.appliedDelta);
            sqlite3_bind_int64(statement, 18, value.sequence);
            if (sqlite3_step(statement) != SQLITE_DONE) {
                rollback();
                return;
            }
        }

        Statement insertScale;
        if (!Prepare(
            database.handle,
            "INSERT INTO actor_scale_contributions VALUES("
            "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16);",
            insertScale)) {
            rollback();
            return;
        }
        for (const auto& [key, value] : _impl->scales) {
            (void)key;
            auto* statement = insertScale.handle;
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            sqlite3_bind_int64(statement, 1, snapshotID);
            sqlite3_bind_int64(statement, 2, value.actorFormID);
            BindText(statement, 3, value.actorKey);
            BindText(statement, 4, value.ownerID);
            BindText(statement, 5, value.mutationKey);
            sqlite3_bind_int(statement, 6, std::to_underlying(value.operation));
            sqlite3_bind_int(statement, 7, std::to_underlying(value.source));
            sqlite3_bind_int(statement, 8, std::to_underlying(value.binding));
            sqlite3_bind_double(statement, 9, value.fixedValue);
            sqlite3_bind_int64(statement, 10, value.sourceGlobalFormID);
            BindText(statement, 11, value.sourceActorValue);
            sqlite3_bind_double(statement, 12, value.sourceMultiplier);
            sqlite3_bind_double(statement, 13, value.baselineAtActivation);
            sqlite3_bind_double(statement, 14, value.resolvedSourceValue);
            sqlite3_bind_double(statement, 15, value.appliedDelta);
            sqlite3_bind_int64(statement, 16, value.sequence);
            if (sqlite3_step(statement) != SQLITE_DONE) {
                rollback();
                return;
            }
        }

        Statement insertName;
        if (!Prepare(
            database.handle,
            "INSERT INTO name_overrides VALUES("
            "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);",
            insertName)) {
            rollback();
            return;
        }
        for (const auto& [key, value] : _impl->names) {
            (void)key;
            auto* statement = insertName.handle;
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            sqlite3_bind_int64(statement, 1, snapshotID);
            sqlite3_bind_int64(statement, 2, value.actorFormID);
            BindText(statement, 3, value.actorKey);
            BindText(statement, 4, value.ownerID);
            BindText(statement, 5, value.mutationKey);
            BindText(statement, 6, value.requestedName);
            BindText(statement, 7, value.originalName);
            BindText(statement, 8, value.lastAppliedName);
            sqlite3_bind_int(statement, 9, value.priority);
            sqlite3_bind_int64(statement, 10, value.sequence);
            if (sqlite3_step(statement) != SQLITE_DONE) {
                rollback();
                return;
            }
        }

        Statement insertAudit;
        if (!Prepare(
            database.handle,
            "INSERT INTO audit_events("
            "snapshot_id,sequence,generation,actor_form_id,actor_key,owner_id,"
            "mutation_key,kind,action,before_value,after_value,status) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12);",
            insertAudit)) {
            rollback();
            return;
        }
        for (const auto& event : _impl->audit) {
            auto* statement = insertAudit.handle;
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            sqlite3_bind_int64(statement, 1, snapshotID);
            sqlite3_bind_int64(statement, 2, event.sequence);
            sqlite3_bind_int64(statement, 3, event.generation);
            sqlite3_bind_int64(statement, 4, event.actorFormID);
            BindText(statement, 5, event.actorKey);
            BindText(statement, 6, event.ownerID);
            BindText(statement, 7, event.mutationKey);
            BindText(statement, 8, event.kind);
            BindText(statement, 9, event.action);
            sqlite3_bind_double(statement, 10, event.beforeValue);
            sqlite3_bind_double(statement, 11, event.afterValue);
            sqlite3_bind_int(statement, 12, std::to_underlying(event.status));
            if (sqlite3_step(statement) != SQLITE_DONE) {
                rollback();
                return;
            }
        }

        Statement insertExternal;
        if (!Prepare(
            database.handle,
            "INSERT INTO external_actor_value_events("
            "snapshot_id,sequence,generation,actor_form_id,actor_key,"
            "actor_value,mutation_kind,modifier,requested,before_base,"
            "before_permanent,before_temporary,before_maximum,after_base,"
            "after_permanent,after_temporary,after_maximum,durable) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,"
            "?15,?16,?17,?18);",
            insertExternal)) {
            rollback();
            return;
        }
        for (const auto& event : _impl->externalAudit) {
            auto* statement = insertExternal.handle;
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            sqlite3_bind_int64(statement, 1, snapshotID);
            sqlite3_bind_int64(statement, 2, event.sequence);
            sqlite3_bind_int64(statement, 3, event.generation);
            sqlite3_bind_int64(statement, 4, event.actorFormID);
            BindText(statement, 5, event.actorKey);
            BindText(statement, 6, event.actorValue);
            sqlite3_bind_int(statement, 7, std::to_underlying(event.kind));
            sqlite3_bind_int(
                statement, 8, static_cast<int>(event.modifier));
            sqlite3_bind_double(statement, 9, event.requested);
            sqlite3_bind_double(statement, 10, event.before.base);
            sqlite3_bind_double(statement, 11, event.before.permanent);
            sqlite3_bind_double(statement, 12, event.before.temporary);
            sqlite3_bind_double(statement, 13, event.before.maximum);
            sqlite3_bind_double(statement, 14, event.after.base);
            sqlite3_bind_double(statement, 15, event.after.permanent);
            sqlite3_bind_double(statement, 16, event.after.temporary);
            sqlite3_bind_double(statement, 17, event.after.maximum);
            sqlite3_bind_int(statement, 18, event.durable ? 1 : 0);
            if (sqlite3_step(statement) != SQLITE_DONE) {
                rollback();
                return;
            }
        }

        Statement insertCheckpoint;
        if (!Prepare(
            database.handle,
            "INSERT INTO actor_value_checkpoints VALUES("
            "?1,?2,?3,?4,?5,?6,?7,?8,?9);",
            insertCheckpoint)) {
            rollback();
            return;
        }
        for (const auto& [key, checkpoint] : _impl->checkpoints) {
            (void)key;
            auto* statement = insertCheckpoint.handle;
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            sqlite3_bind_int64(statement, 1, snapshotID);
            sqlite3_bind_int64(statement, 2, checkpoint.actorFormID);
            BindText(statement, 3, checkpoint.actorKey);
            BindText(statement, 4, checkpoint.actorValue);
            sqlite3_bind_double(statement, 5, checkpoint.externalBase);
            sqlite3_bind_double(statement, 6, checkpoint.externalPermanent);
            sqlite3_bind_double(statement, 7, checkpoint.externalTemporary);
            sqlite3_bind_double(statement, 8, checkpoint.ledgerPermanent);
            sqlite3_bind_double(statement, 9, checkpoint.ledgerTemporary);
            if (sqlite3_step(statement) != SQLITE_DONE) {
                rollback();
                return;
            }
        }

        if (!Exec(database.handle, "COMMIT;")) {
            rollback();
            return;
        }
        Exec(database.handle, "PRAGMA wal_checkpoint(TRUNCATE);");
        _impl->audit.clear();
        _impl->externalAudit.clear();
        logger::info(
            "[WhoEditThat] Snapshot '{}' #{} persisted to '{}'",
            _impl->context.saveName,
            _impl->context.saveNumber,
            path.string());
        if (!BackupDatabase(path)) {
            logger::warn("[WhoEditThat] Could not update database backup");
        }
    }

    bool Manager::QueueUpsertActorValue(
        const ActorValueContributionRequest& request,
        Callback callback,
        void* userData)
    {
        if (!callback || request.actorFormID == 0 ||
            !request.mutationKey || !*request.mutationKey ||
            !request.targetActorValue || !*request.targetActorValue ||
            std::string_view(request.mutationKey).size() > 127 ||
            std::string_view(request.targetActorValue).size() > 63 ||
            !std::isfinite(request.fixedValue) ||
            !std::isfinite(request.sourceMultiplier) ||
            std::to_underlying(request.operation) >
            std::to_underlying(NumericOperation::kMultiply) ||
            std::to_underlying(request.source) >
            std::to_underlying(NumericSource::kActorValue) ||
            std::to_underlying(request.channel) >
            std::to_underlying(ModifierChannel::kTemporary) ||
            (request.source == NumericSource::kGlobal &&
                request.sourceGlobalFormID == 0) ||
            (request.source == NumericSource::kActorValue &&
                (!request.sourceActorValue || !*request.sourceActorValue ||
                    std::string_view(request.sourceActorValue).size() > 63))) {
            return false;
        }

        OwnedActorValueRequest owned;
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load(std::memory_order_acquire) !=
                ContextState::kReady) {
                return false;
            }
            const auto owner = _impl->Owner(request.client);
            if (!owner) {
                return false;
            }
            owned.ownerID = *owner;
            owned.actorFormID = request.actorFormID;
            owned.mutationKey = request.mutationKey;
            owned.targetActorValue = request.targetActorValue;
            owned.operation = request.operation;
            owned.source = request.source;
            owned.channel = request.channel;
            owned.fixedValue = request.fixedValue;
            owned.sourceGlobalFormID = request.sourceGlobalFormID;
            owned.sourceActorValue = request.sourceActorValue ?
                request.sourceActorValue : "";
            owned.sourceMultiplier = request.sourceMultiplier;
            requestGeneration = _impl->generation.load(std::memory_order_acquire);
        }

        CallbackTarget target{ callback, userData };
        SKSE::GetTaskInterface()->AddTask(
            [this,
            request = std::move(owned),
            requestGeneration,
            target] {
                auto result = MakeResult(
                    Operation::kUpsertActorValue,
                    Status::kInternalError,
                    request.actorFormID);
                bool unchangedRequest = false;
                if (_impl->generation.load(std::memory_order_acquire) !=
                    requestGeneration ||
                    _impl->state.load(std::memory_order_acquire) !=
                    ContextState::kReady) {
                    result.status = Status::kContextChanged;
                    CopyText(result.message, "Save context changed before execution");
                    target.Deliver(result);
                    return;
                }

                {
                    std::scoped_lock lock(_impl->mutex);
                    if (_impl->generation.load(std::memory_order_acquire) !=
                        requestGeneration ||
                        _impl->state.load(std::memory_order_acquire) !=
                        ContextState::kReady) {
                        result.status = Status::kContextChanged;
                    }
                    else if (auto* actor = LookupActor(request.actorFormID)) {
                        const auto targetValue = ResolveActorValue(
                            request.targetActorValue);
                        const auto* targetName = IsValidActorValue(targetValue) ?
                            RE::ActorValueList::GetActorValueName(targetValue) :
                            nullptr;
                        if (!targetName || !*targetName) {
                            result.status = Status::kUnsupportedActorValue;
                            CopyText(result.message, "Unknown target Actor Value");
                        }
                        else if (
                            request.operation == NumericOperation::kMultiply &&
                            request.source == NumericSource::kFixed &&
                            request.fixedValue * request.sourceMultiplier < 0.0F) {
                            result.status = Status::kInvalidArgument;
                            CopyText(result.message, "A multiplier cannot be negative");
                        }
                        else {
                            ActorValueContribution contribution;
                            contribution.actorFormID = request.actorFormID;
                            contribution.actorKey = BuildActorKey(actor);
                            contribution.ownerID = request.ownerID;
                            contribution.mutationKey = request.mutationKey;
                            contribution.targetActorValue = targetName;
                            contribution.operation = request.operation;
                            contribution.source = request.source;
                            contribution.channel = request.channel;
                            contribution.binding = kDefaultBinding;
                            contribution.fixedValue = request.fixedValue;
                            contribution.sourceGlobalFormID =
                                request.sourceGlobalFormID;
                            contribution.sourceActorValue = request.sourceActorValue;
                            contribution.sourceMultiplier = request.sourceMultiplier;
                            if (request.source == NumericSource::kActorValue) {
                                const auto sourceValue = ResolveActorValue(
                                    request.sourceActorValue);
                                const auto* sourceName =
                                    IsValidActorValue(sourceValue) ?
                                    RE::ActorValueList::GetActorValueName(
                                        sourceValue) :
                                    nullptr;
                                if (sourceName && *sourceName) {
                                    contribution.sourceActorValue =
                                        sourceName;
                                }
                            }
                            const auto source = _impl->ResolveSourceValue(
                                actor, contribution);
                            if (!source) {
                                result.status = Status::kInvalidArgument;
                                CopyText(result.message, "Numeric source could not be resolved");
                            }
                            else if (
                                contribution.operation == NumericOperation::kMultiply &&
                                *source < 0.0F) {
                                result.status = Status::kInvalidArgument;
                                CopyText(result.message, "A multiplier cannot be negative");
                            }
                            else if (
                                contribution.operation == NumericOperation::kPercent &&
                                *source < -100.0F) {
                                result.status = Status::kInvalidArgument;
                                CopyText(result.message, "A percentage cannot be below -100");
                            }
                            else {
                                contribution.resolvedSourceValue = *source;
                                auto existing = _impl->FindActorValue(
                                    request.actorFormID,
                                    request.ownerID,
                                    request.mutationKey);
                                const auto existed = existing != _impl->actorValues.end();
                                std::optional<ActorValueContribution> previous;
                                std::string mapKey = CompositeKey(
                                    request.actorFormID,
                                    request.ownerID,
                                    request.mutationKey);
                                if (existed) {
                                    previous = existing->second;
                                    mapKey = existing->first;
                                    contribution.appliedDelta =
                                        existing->second.appliedDelta;
                                }

                                const auto unchanged = previous &&
                                    previous->targetActorValue ==
                                    contribution.targetActorValue &&
                                    previous->operation == contribution.operation &&
                                    previous->source == contribution.source &&
                                    previous->channel == contribution.channel &&
                                    NearlyEqual(
                                        previous->fixedValue,
                                        contribution.fixedValue) &&
                                    previous->sourceGlobalFormID ==
                                    contribution.sourceGlobalFormID &&
                                    previous->sourceActorValue ==
                                    contribution.sourceActorValue &&
                                    NearlyEqual(
                                        previous->sourceMultiplier,
                                        contribution.sourceMultiplier) &&
                                    NearlyEqual(
                                        previous->resolvedSourceValue,
                                        contribution.resolvedSourceValue);
                                if (unchanged) {
                                    unchangedRequest = true;
                                    result.status = Status::kSuccess;
                                    result.numericOperation = previous->operation;
                                    result.channel = previous->channel;
                                    result.previousDelta = previous->appliedDelta;
                                    result.appliedDelta = previous->appliedDelta;
                                    CopyText(result.ownerID, previous->ownerID);
                                    CopyText(result.mutationKey, previous->mutationKey);
                                    CopyText(
                                        result.actorValue,
                                        previous->targetActorValue);
                                }
                                else {
                                    contribution.sequence = _impl->nextSequence++;
                                    logger::debug(
                                        "[WhoEditThat] Actor Value upsert apply: "
                                        "owner='{}' key='{}' actor={:08X} target='{}'",
                                        request.ownerID,
                                        request.mutationKey,
                                        request.actorFormID,
                                        contribution.targetActorValue);
                                    const auto sameGroup = previous &&
                                        previous->targetActorValue ==
                                        contribution.targetActorValue &&
                                        previous->channel == contribution.channel;
                                    std::string error;
                                    bool success = true;
                                    const auto newTarget = ResolveActorValue(
                                        contribution.targetActorValue);
                                    if (previous && !sameGroup) {
                                        const auto oldTarget = ResolveActorValue(
                                            previous->targetActorValue);
                                        const auto oldAll = _impl->LedgerDelta(
                                            request.actorFormID, oldTarget);
                                        const auto oldGroup = _impl->GroupAppliedDelta(
                                            request.actorFormID,
                                            previous->targetActorValue,
                                            previous->channel);
                                        _impl->actorValues.erase(mapKey);
                                        success = _impl->RecalculateGroup(
                                            actor,
                                            previous->targetActorValue,
                                            previous->channel,
                                            oldAll,
                                            oldGroup,
                                            error);
                                        contribution.appliedDelta = 0.0F;
                                    }
                                    if (success) {
                                        const auto previousAll = _impl->LedgerDelta(
                                            request.actorFormID, newTarget);
                                        const auto previousGroup =
                                            _impl->GroupAppliedDelta(
                                                request.actorFormID,
                                                contribution.targetActorValue,
                                                contribution.channel);
                                        _impl->actorValues[mapKey] = contribution;
                                        std::vector<std::string> liveOrder;
                                        success = _impl->LiveOrder(
                                            request.actorFormID, liveOrder, error);
                                        if (success) {
                                            success = _impl->RecalculateGroup(
                                                actor,
                                                contribution.targetActorValue,
                                                contribution.channel,
                                                previousAll,
                                                previousGroup,
                                                error);
                                        }
                                        if (success) {
                                            success = _impl->RefreshLiveActor(
                                                actor, error);
                                        }
                                        if (success) {
                                            std::string scaleError;
                                            if (!_impl->RefreshLiveScale(
                                                actor, scaleError)) {
                                                logger::warn(
                                                    "[WhoEditThat] Actor Scale refresh "
                                                    "after Actor Value upsert failed on "
                                                    "{:08X}: {}",
                                                    actor->GetFormID(),
                                                    scaleError);
                                            }
                                        }
                                    }
                                    if (!success) {
                                        _impl->actorValues.erase(mapKey);
                                        if (previous) {
                                            _impl->actorValues[mapKey] = *previous;
                                            if (!sameGroup) {
                                                const auto oldTarget = ResolveActorValue(
                                                    previous->targetActorValue);
                                                const auto currentAll =
                                                    _impl->LedgerDelta(
                                                        request.actorFormID,
                                                        oldTarget) -
                                                    previous->appliedDelta;
                                                const auto currentGroup =
                                                    _impl->GroupAppliedDelta(
                                                        request.actorFormID,
                                                        previous->targetActorValue,
                                                        previous->channel) -
                                                    previous->appliedDelta;
                                                std::string rollbackError;
                                                _impl->RecalculateGroup(
                                                    actor,
                                                    previous->targetActorValue,
                                                    previous->channel,
                                                    currentAll,
                                                    currentGroup,
                                                    rollbackError);
                                            }
                                        }
                                        result.status =
                                            error.find("depend") != std::string::npos ?
                                            Status::kInvalidArgument :
                                            Status::kInternalError;
                                        CopyText(result.message, error);
                                    }
                                    else {
                                        if (previous &&
                                            !_impl->IsTracked(
                                                request.actorFormID,
                                                previous->targetActorValue)) {
                                            _impl->checkpoints.erase(CheckpointKey(
                                                request.actorFormID,
                                                previous->targetActorValue));
                                        }
                                        const auto stored = _impl->actorValues.find(mapKey);
                                        result.status = Status::kSuccess;
                                        result.numericOperation = stored->second.operation;
                                        result.channel = stored->second.channel;
                                        result.previousDelta = previous ?
                                            previous->appliedDelta : 0.0F;
                                        result.appliedDelta = stored->second.appliedDelta;
                                        CopyText(result.ownerID, stored->second.ownerID);
                                        CopyText(result.mutationKey, stored->second.mutationKey);
                                        CopyText(result.actorValue, stored->second.targetActorValue);
                                        _impl->AddAudit(
                                            stored->second,
                                            existed ? "Update" : "Create",
                                            previous ? previous->appliedDelta : 0.0F,
                                            stored->second.appliedDelta,
                                            Status::kSuccess);
                                    }
                                }
                            }
                        }
                    }
                    else {
                        result.status = Status::kActorUnavailable;
                        CopyText(result.message, "Actor is not available");
                    }
                }
                if (unchangedRequest) {
                    logger::debug(
                        "[WhoEditThat] Actor Value upsert ignored: "
                        "actor={:08X} key='{}' delta={}",
                        result.actorFormID,
                        result.mutationKey,
                        result.appliedDelta);
                }
                else {
                    logger::debug(
                        "[WhoEditThat] Actor Value upsert end: actor={:08X} "
                        "status={} delta={} message='{}'",
                        result.actorFormID,
                        std::to_underlying(result.status),
                        result.appliedDelta,
                        result.message);
                }
                target.Deliver(result);
            });
        return true;
    }

    bool Manager::QueueRemoveActorValue(
        const ContributionRequest& request,
        Callback callback,
        void* userData)
    {
        if (!callback || request.actorFormID == 0 ||
            !request.mutationKey || !*request.mutationKey ||
            std::string_view(request.mutationKey).size() > 127) {
            return false;
        }
        OwnedContributionRequest owned;
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load() != ContextState::kReady) {
                return false;
            }
            const auto owner = _impl->Owner(request.client);
            if (!owner) {
                return false;
            }
            owned = { *owner, request.actorFormID, request.mutationKey };
            requestGeneration = _impl->generation.load();
        }
        CallbackTarget target{ callback, userData };
        SKSE::GetTaskInterface()->AddTask(
            [this, request = std::move(owned), requestGeneration, target] {
                logger::debug(
                    "[WhoEditThat] Actor Value remove begin: owner='{}' "
                    "key='{}' actor={:08X}",
                    request.ownerID,
                    request.mutationKey,
                    request.actorFormID);
                auto result = MakeResult(
                    Operation::kRemoveActorValue,
                    Status::kInternalError,
                    request.actorFormID);
                {
                    std::scoped_lock lock(_impl->mutex);
                    if (_impl->generation.load() != requestGeneration ||
                        _impl->state.load() != ContextState::kReady) {
                        result.status = Status::kContextChanged;
                    }
                    else if (auto* actor = LookupActor(request.actorFormID)) {
                        const auto found = _impl->FindActorValue(
                            request.actorFormID,
                            request.ownerID,
                            request.mutationKey);
                        if (found == _impl->actorValues.end()) {
                            result.status = Status::kNotFound;
                        }
                        else {
                            std::string error;
                            ActorValueContribution removed;
                            if (!_impl->RemoveActorValue(
                                actor, found, removed, error)) {
                                result.status = Status::kInternalError;
                                CopyText(result.message, error);
                            }
                            else {
                                result.status = Status::kSuccess;
                                result.numericOperation = removed.operation;
                                result.channel = removed.channel;
                                result.previousDelta = removed.appliedDelta;
                                result.appliedDelta = removed.appliedDelta;
                                result.affectedCount = 1;
                                CopyText(result.ownerID, removed.ownerID);
                                CopyText(result.mutationKey, removed.mutationKey);
                                CopyText(result.actorValue, removed.targetActorValue);
                            }
                        }
                    }
                    else {
                        result.status = Status::kActorUnavailable;
                    }
                }
                logger::debug(
                    "[WhoEditThat] Actor Value remove end: actor={:08X} "
                    "status={} affected={} delta={} message='{}'",
                    result.actorFormID,
                    std::to_underlying(result.status),
                    result.affectedCount,
                    result.appliedDelta,
                    result.message);
                target.Deliver(result);
            });
        return true;
    }

    bool Manager::QueueLookupActorValue(
        const ContributionRequest& request,
        Callback callback,
        void* userData)
    {
        if (!callback || request.actorFormID == 0 ||
            !request.mutationKey || !*request.mutationKey ||
            std::string_view(request.mutationKey).size() > 127) {
            return false;
        }
        OwnedContributionRequest owned;
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load() != ContextState::kReady) {
                return false;
            }
            const auto owner = _impl->Owner(request.client);
            if (!owner) {
                return false;
            }
            owned = { *owner, request.actorFormID, request.mutationKey };
            requestGeneration = _impl->generation.load();
        }
        CallbackTarget target{ callback, userData };
        SKSE::GetTaskInterface()->AddTask(
            [this, request = std::move(owned), requestGeneration, target] {
                auto result = MakeResult(
                    Operation::kLookupActorValue,
                    Status::kInternalError,
                    request.actorFormID);
                {
                    std::scoped_lock lock(_impl->mutex);
                    if (_impl->generation.load() != requestGeneration ||
                        _impl->state.load() != ContextState::kReady) {
                        result.status = Status::kContextChanged;
                    }
                    else {
                        const auto found = _impl->FindActorValue(
                            request.actorFormID,
                            request.ownerID,
                            request.mutationKey);
                        if (found == _impl->actorValues.end()) {
                            result.status = Status::kNotFound;
                        }
                        else {
                            const auto& value = found->second;
                            result.status = Status::kSuccess;
                            result.numericOperation = value.operation;
                            result.channel = value.channel;
                            result.appliedDelta = value.appliedDelta;
                            CopyText(result.ownerID, value.ownerID);
                            CopyText(result.mutationKey, value.mutationKey);
                            CopyText(result.actorValue, value.targetActorValue);
                        }
                    }
                }
                target.Deliver(result);
            });
        return true;
    }

    bool Manager::QueueListActorValues(
        const ContributionScopeRequest& request,
        ListCallback callback,
        void* userData)
    {
        if (!callback || request.actorFormID == 0 ||
            !request.mutationKeyPrefix || !*request.mutationKeyPrefix ||
            std::string_view(request.mutationKeyPrefix).size() > 127) {
            return false;
        }
        OwnedContributionScopeRequest owned;
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load() != ContextState::kReady) {
                return false;
            }
            const auto owner = _impl->Owner(request.client);
            if (!owner) {
                return false;
            }
            owned = { *owner, request.actorFormID,
                request.mutationKeyPrefix };
            requestGeneration = _impl->generation.load();
        }
        ListCallbackTarget target{ callback, userData };
        SKSE::GetTaskInterface()->AddTask(
            [this, request = std::move(owned), requestGeneration, target] {
                std::vector<ActorValueEntry> entries;
                ActorValueListResult result;
                result.actorFormID = request.actorFormID;
                {
                    std::scoped_lock lock(_impl->mutex);
                    if (_impl->generation.load() != requestGeneration ||
                        _impl->state.load() != ContextState::kReady) {
                        result.status = Status::kContextChanged;
                    }
                    else {
                        for (const auto& [key, value] : _impl->actorValues) {
                            (void)key;
                            if (value.actorFormID != request.actorFormID ||
                                value.ownerID != request.ownerID ||
                                !value.mutationKey.starts_with(
                                    request.mutationKeyPrefix)) {
                                continue;
                            }
                            ActorValueEntry entry;
                            entry.actorFormID = value.actorFormID;
                            entry.numericOperation = value.operation;
                            entry.channel = value.channel;
                            entry.appliedDelta = value.appliedDelta;
                            CopyText(entry.mutationKey, value.mutationKey);
                            CopyText(entry.actorValue, value.targetActorValue);
                            entries.push_back(entry);
                        }
                        result.status = Status::kSuccess;
                    }
                }
                result.entryCount = static_cast<std::uint32_t>(entries.size());
                result.entries = entries.data();
                logger::debug(
                    "[WhoEditThat] Actor Value list: owner='{}' "
                    "prefix='{}' actor={:08X} status={} entries={}",
                    request.ownerID,
                    request.mutationKeyPrefix,
                    request.actorFormID,
                    std::to_underlying(result.status),
                    result.entryCount);
                target.Deliver(result);
            });
        return true;
    }

    bool Manager::QueueRemoveActorValuesByPrefix(
        const ContributionScopeRequest& request,
        Callback callback,
        void* userData)
    {
        if (!callback || request.actorFormID == 0 ||
            !request.mutationKeyPrefix || !*request.mutationKeyPrefix ||
            std::string_view(request.mutationKeyPrefix).size() > 127) {
            return false;
        }
        OwnedContributionScopeRequest owned;
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load() != ContextState::kReady) {
                return false;
            }
            const auto owner = _impl->Owner(request.client);
            if (!owner) {
                return false;
            }
            owned = { *owner, request.actorFormID,
                request.mutationKeyPrefix };
            requestGeneration = _impl->generation.load();
        }
        CallbackTarget target{ callback, userData };
        SKSE::GetTaskInterface()->AddTask(
            [this, request = std::move(owned), requestGeneration, target] {
                logger::debug(
                    "[WhoEditThat] Actor Value prefix remove begin: "
                    "owner='{}' prefix='{}' actor={:08X}",
                    request.ownerID,
                    request.mutationKeyPrefix,
                    request.actorFormID);
                auto result = MakeResult(
                    Operation::kRemoveActorValuesByPrefix,
                    Status::kInternalError,
                    request.actorFormID);
                CopyText(result.mutationKey, request.mutationKeyPrefix);
                {
                    std::scoped_lock lock(_impl->mutex);
                    if (_impl->generation.load() != requestGeneration ||
                        _impl->state.load() != ContextState::kReady) {
                        result.status = Status::kContextChanged;
                    }
                    else if (auto* actor = LookupActor(request.actorFormID)) {
                        std::vector<std::string> mutations;
                        for (const auto& [key, value] : _impl->actorValues) {
                            (void)key;
                            if (value.actorFormID == request.actorFormID &&
                                value.ownerID == request.ownerID &&
                                value.mutationKey.starts_with(
                                    request.mutationKeyPrefix)) {
                                mutations.push_back(value.mutationKey);
                            }
                        }
                        result.status = mutations.empty() ?
                            Status::kNotFound : Status::kSuccess;
                        for (const auto& mutation : mutations) {
                            const auto found = _impl->FindActorValue(
                                request.actorFormID,
                                request.ownerID,
                                mutation);
                            if (found == _impl->actorValues.end()) {
                                continue;
                            }
                            ActorValueContribution removed;
                            std::string error;
                            if (!_impl->RemoveActorValue(
                                actor, found, removed, error)) {
                                result.status = Status::kInternalError;
                                CopyText(result.message, error);
                                break;
                            }
                            ++result.affectedCount;
                        }
                    }
                    else {
                        result.status = Status::kActorUnavailable;
                    }
                }
                logger::debug(
                    "[WhoEditThat] Actor Value prefix remove end: "
                    "actor={:08X} status={} affected={} message='{}'",
                    result.actorFormID,
                    std::to_underlying(result.status),
                    result.affectedCount,
                    result.message);
                target.Deliver(result);
            });
        return true;
    }

    bool Manager::QueueUpsertActorScale(
        const ActorScaleContributionRequest& request,
        Callback callback,
        void* userData)
    {
        if (!callback || request.actorFormID == 0 ||
            !request.mutationKey || !*request.mutationKey ||
            std::string_view(request.mutationKey).size() > 127 ||
            !std::isfinite(request.fixedValue) ||
            !std::isfinite(request.sourceMultiplier) ||
            std::to_underlying(request.operation) >
            std::to_underlying(NumericOperation::kMultiply) ||
            std::to_underlying(request.source) >
            std::to_underlying(NumericSource::kActorValue) ||
            (request.source == NumericSource::kGlobal &&
                request.sourceGlobalFormID == 0) ||
            (request.source == NumericSource::kActorValue &&
                (!request.sourceActorValue || !*request.sourceActorValue ||
                    std::string_view(request.sourceActorValue).size() > 63))) {
            return false;
        }

        OwnedActorScaleRequest owned;
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load(std::memory_order_acquire) !=
                ContextState::kReady) {
                return false;
            }
            const auto owner = _impl->Owner(request.client);
            if (!owner) {
                return false;
            }
            owned.ownerID = *owner;
            owned.actorFormID = request.actorFormID;
            owned.mutationKey = request.mutationKey;
            owned.operation = request.operation;
            owned.source = request.source;
            owned.fixedValue = request.fixedValue;
            owned.sourceGlobalFormID = request.sourceGlobalFormID;
            owned.sourceActorValue = request.sourceActorValue ?
                request.sourceActorValue : "";
            owned.sourceMultiplier = request.sourceMultiplier;
            requestGeneration = _impl->generation.load(std::memory_order_acquire);
        }

        CallbackTarget target{ callback, userData };
        SKSE::GetTaskInterface()->AddTask(
            [this, request = std::move(owned), requestGeneration, target] {
                auto result = MakeResult(
                    Operation::kUpsertActorScale,
                    Status::kInternalError,
                    request.actorFormID);
                bool unchangedRequest = false;
                {
                    std::scoped_lock lock(_impl->mutex);
                    if (_impl->generation.load(std::memory_order_acquire) !=
                        requestGeneration ||
                        _impl->state.load(std::memory_order_acquire) !=
                        ContextState::kReady) {
                        result.status = Status::kContextChanged;
                    }
                    else if (auto* actor = LookupActor(request.actorFormID)) {
                        ActorScaleContribution contribution;
                        contribution.actorFormID = request.actorFormID;
                        contribution.actorKey = BuildActorKey(actor);
                        contribution.ownerID = request.ownerID;
                        contribution.mutationKey = request.mutationKey;
                        contribution.operation = request.operation;
                        contribution.source = request.source;
                        contribution.binding = kDefaultBinding;
                        contribution.fixedValue = request.fixedValue;
                        contribution.sourceGlobalFormID =
                            request.sourceGlobalFormID;
                        contribution.sourceActorValue = request.sourceActorValue;
                        contribution.sourceMultiplier = request.sourceMultiplier;
                        if (request.source == NumericSource::kActorValue) {
                            const auto sourceValue = ResolveActorValue(
                                request.sourceActorValue);
                            const auto* sourceName = IsValidActorValue(sourceValue) ?
                                RE::ActorValueList::GetActorValueName(sourceValue) :
                                nullptr;
                            if (sourceName && *sourceName) {
                                contribution.sourceActorValue = sourceName;
                            }
                        }

                        const auto source = _impl->ResolveSourceValue(
                            actor, contribution);
                        if (!source) {
                            result.status = Status::kInvalidArgument;
                            CopyText(result.message,
                                "Numeric source could not be resolved");
                        }
                        else if (
                            contribution.operation == NumericOperation::kMultiply &&
                            *source < 0.0F) {
                            result.status = Status::kInvalidArgument;
                            CopyText(result.message,
                                "A scale multiplier cannot be negative");
                        }
                        else if (
                            contribution.operation == NumericOperation::kPercent &&
                            *source < -100.0F) {
                            result.status = Status::kInvalidArgument;
                            CopyText(result.message,
                                "A scale percentage cannot be below -100");
                        }
                        else {
                            contribution.resolvedSourceValue = *source;
                            auto existing = _impl->FindScale(
                                request.actorFormID,
                                request.ownerID,
                                request.mutationKey);
                            const auto existed = existing != _impl->scales.end();
                            std::optional<ActorScaleContribution> previous;
                            auto mapKey = CompositeKey(
                                request.actorFormID,
                                request.ownerID,
                                request.mutationKey);
                            if (existed) {
                                previous = existing->second;
                                mapKey = existing->first;
                                contribution.baselineAtActivation =
                                    previous->baselineAtActivation;
                                contribution.appliedDelta = previous->appliedDelta;
                            }
                            else {
                                contribution.baselineAtActivation =
                                    _impl->ScaleBaseline(request.actorFormID)
                                    .value_or(actor->GetScale());
                            }

                            const auto unchanged = previous &&
                                previous->operation == contribution.operation &&
                                previous->source == contribution.source &&
                                NearlyEqual(previous->fixedValue,
                                    contribution.fixedValue) &&
                                previous->sourceGlobalFormID ==
                                contribution.sourceGlobalFormID &&
                                previous->sourceActorValue ==
                                contribution.sourceActorValue &&
                                NearlyEqual(previous->sourceMultiplier,
                                    contribution.sourceMultiplier) &&
                                NearlyEqual(previous->resolvedSourceValue,
                                    contribution.resolvedSourceValue);
                            if (unchanged) {
                                unchangedRequest = true;
                                result.status = Status::kSuccess;
                                result.numericOperation = previous->operation;
                                result.previousDelta = previous->appliedDelta;
                                result.appliedDelta = previous->appliedDelta;
                                CopyText(result.ownerID, previous->ownerID);
                                CopyText(result.mutationKey, previous->mutationKey);
                                CopyText(result.actorValue, "Scale");
                            }
                            else {
                                const auto previousObserved = actor->GetScale();
                                contribution.sequence = _impl->nextSequence++;
                                _impl->scales[mapKey] = contribution;
                                std::string error;
                                if (!_impl->RefreshLiveScale(actor, error)) {
                                    _impl->scales.erase(mapKey);
                                    if (previous) {
                                        _impl->scales[mapKey] = *previous;
                                    }
                                    actor->SetScale(previousObserved);
                                    result.status = error.find("outside") !=
                                        std::string::npos ?
                                        Status::kConflict :
                                        Status::kInvalidArgument;
                                    CopyText(result.message, error);
                                }
                                else {
                                    const auto stored = _impl->scales.find(mapKey);
                                    result.status = Status::kSuccess;
                                    result.numericOperation = stored->second.operation;
                                    result.previousDelta = previous ?
                                        previous->appliedDelta : 0.0F;
                                    result.appliedDelta = stored->second.appliedDelta;
                                    CopyText(result.ownerID, stored->second.ownerID);
                                    CopyText(result.mutationKey,
                                        stored->second.mutationKey);
                                    CopyText(result.actorValue, "Scale");
                                    _impl->AddAudit(
                                        stored->second,
                                        existed ? "Update" : "Create",
                                        previous ? previous->appliedDelta : 0.0F,
                                        stored->second.appliedDelta,
                                        Status::kSuccess);
                                }
                            }
                        }
                    }
                    else {
                        result.status = Status::kActorUnavailable;
                        CopyText(result.message, "Actor is not available");
                    }
                }
                if (unchangedRequest) {
                    logger::debug(
                        "[WhoEditThat] Actor Scale upsert ignored: "
                        "actor={:08X} key='{}' delta={}",
                        result.actorFormID,
                        result.mutationKey,
                        result.appliedDelta);
                }
                else {
                    logger::debug(
                        "[WhoEditThat] Actor Scale upsert end: actor={:08X} "
                        "status={} delta={} message='{}'",
                        result.actorFormID,
                        std::to_underlying(result.status),
                        result.appliedDelta,
                        result.message);
                }
                target.Deliver(result);
            });
        return true;
    }

    bool Manager::QueueRemoveActorScale(
        const ContributionRequest& request,
        Callback callback,
        void* userData)
    {
        if (!callback || request.actorFormID == 0 ||
            !request.mutationKey || !*request.mutationKey ||
            std::string_view(request.mutationKey).size() > 127) {
            return false;
        }
        OwnedContributionRequest owned;
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load() != ContextState::kReady) {
                return false;
            }
            const auto owner = _impl->Owner(request.client);
            if (!owner) {
                return false;
            }
            owned = { *owner, request.actorFormID, request.mutationKey };
            requestGeneration = _impl->generation.load();
        }
        CallbackTarget target{ callback, userData };
        SKSE::GetTaskInterface()->AddTask(
            [this, request = std::move(owned), requestGeneration, target] {
                auto result = MakeResult(
                    Operation::kRemoveActorScale,
                    Status::kInternalError,
                    request.actorFormID);
                {
                    std::scoped_lock lock(_impl->mutex);
                    if (_impl->generation.load() != requestGeneration ||
                        _impl->state.load() != ContextState::kReady) {
                        result.status = Status::kContextChanged;
                    }
                    else if (auto* actor = LookupActor(request.actorFormID)) {
                        const auto found = _impl->FindScale(
                            request.actorFormID,
                            request.ownerID,
                            request.mutationKey);
                        if (found == _impl->scales.end()) {
                            result.status = Status::kNotFound;
                        }
                        else {
                            ActorScaleContribution removed;
                            std::string error;
                            if (!_impl->RemoveScale(actor, found, removed, error)) {
                                result.status = error.find("outside") !=
                                    std::string::npos ?
                                    Status::kConflict : Status::kInternalError;
                                CopyText(result.message, error);
                            }
                            else {
                                result.status = Status::kSuccess;
                                result.numericOperation = removed.operation;
                                result.previousDelta = removed.appliedDelta;
                                result.appliedDelta = removed.appliedDelta;
                                result.affectedCount = 1;
                                CopyText(result.ownerID, removed.ownerID);
                                CopyText(result.mutationKey, removed.mutationKey);
                                CopyText(result.actorValue, "Scale");
                            }
                        }
                    }
                    else {
                        result.status = Status::kActorUnavailable;
                    }
                }
                target.Deliver(result);
            });
        return true;
    }

    bool Manager::QueueLookupActorScale(
        const ContributionRequest& request,
        Callback callback,
        void* userData)
    {
        if (!callback || request.actorFormID == 0 ||
            !request.mutationKey || !*request.mutationKey ||
            std::string_view(request.mutationKey).size() > 127) {
            return false;
        }
        OwnedContributionRequest owned;
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load() != ContextState::kReady) {
                return false;
            }
            const auto owner = _impl->Owner(request.client);
            if (!owner) {
                return false;
            }
            owned = { *owner, request.actorFormID, request.mutationKey };
            requestGeneration = _impl->generation.load();
        }
        CallbackTarget target{ callback, userData };
        SKSE::GetTaskInterface()->AddTask(
            [this, request = std::move(owned), requestGeneration, target] {
                auto result = MakeResult(
                    Operation::kLookupActorScale,
                    Status::kInternalError,
                    request.actorFormID);
                {
                    std::scoped_lock lock(_impl->mutex);
                    if (_impl->generation.load() != requestGeneration ||
                        _impl->state.load() != ContextState::kReady) {
                        result.status = Status::kContextChanged;
                    }
                    else {
                        const auto found = _impl->FindScale(
                            request.actorFormID,
                            request.ownerID,
                            request.mutationKey);
                        if (found == _impl->scales.end()) {
                            result.status = Status::kNotFound;
                        }
                        else {
                            const auto& value = found->second;
                            result.status = Status::kSuccess;
                            result.numericOperation = value.operation;
                            result.appliedDelta = value.appliedDelta;
                            CopyText(result.ownerID, value.ownerID);
                            CopyText(result.mutationKey, value.mutationKey);
                            CopyText(result.actorValue, "Scale");
                        }
                    }
                }
                target.Deliver(result);
            });
        return true;
    }

    bool Manager::QueueListActorScales(
        const ContributionScopeRequest& request,
        ScaleListCallback callback,
        void* userData)
    {
        if (!callback || request.actorFormID == 0 ||
            !request.mutationKeyPrefix || !*request.mutationKeyPrefix ||
            std::string_view(request.mutationKeyPrefix).size() > 127) {
            return false;
        }
        OwnedContributionScopeRequest owned;
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load() != ContextState::kReady) {
                return false;
            }
            const auto owner = _impl->Owner(request.client);
            if (!owner) {
                return false;
            }
            owned = { *owner, request.actorFormID,
                request.mutationKeyPrefix };
            requestGeneration = _impl->generation.load();
        }
        ScaleListCallbackTarget target{ callback, userData };
        SKSE::GetTaskInterface()->AddTask(
            [this, request = std::move(owned), requestGeneration, target] {
                std::vector<ActorScaleEntry> entries;
                ActorScaleListResult result;
                result.actorFormID = request.actorFormID;
                {
                    std::scoped_lock lock(_impl->mutex);
                    if (_impl->generation.load() != requestGeneration ||
                        _impl->state.load() != ContextState::kReady) {
                        result.status = Status::kContextChanged;
                    }
                    else {
                        for (const auto& [key, value] : _impl->scales) {
                            (void)key;
                            if (value.actorFormID != request.actorFormID ||
                                value.ownerID != request.ownerID ||
                                !value.mutationKey.starts_with(
                                    request.mutationKeyPrefix)) {
                                continue;
                            }
                            ActorScaleEntry entry;
                            entry.actorFormID = value.actorFormID;
                            entry.numericOperation = value.operation;
                            entry.appliedDelta = value.appliedDelta;
                            CopyText(entry.mutationKey, value.mutationKey);
                            entries.push_back(entry);
                        }
                        result.status = Status::kSuccess;
                    }
                }
                result.entryCount = static_cast<std::uint32_t>(entries.size());
                result.entries = entries.data();
                logger::debug(
                    "[WhoEditThat] Actor Scale list: owner='{}' prefix='{}' "
                    "actor={:08X} status={} entries={}",
                    request.ownerID,
                    request.mutationKeyPrefix,
                    request.actorFormID,
                    std::to_underlying(result.status),
                    result.entryCount);
                target.Deliver(result);
            });
        return true;
    }

    bool Manager::QueueRemoveActorScalesByPrefix(
        const ContributionScopeRequest& request,
        Callback callback,
        void* userData)
    {
        if (!callback || request.actorFormID == 0 ||
            !request.mutationKeyPrefix || !*request.mutationKeyPrefix ||
            std::string_view(request.mutationKeyPrefix).size() > 127) {
            return false;
        }
        OwnedContributionScopeRequest owned;
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load() != ContextState::kReady) {
                return false;
            }
            const auto owner = _impl->Owner(request.client);
            if (!owner) {
                return false;
            }
            owned = { *owner, request.actorFormID,
                request.mutationKeyPrefix };
            requestGeneration = _impl->generation.load();
        }
        CallbackTarget target{ callback, userData };
        SKSE::GetTaskInterface()->AddTask(
            [this, request = std::move(owned), requestGeneration, target] {
                auto result = MakeResult(
                    Operation::kRemoveActorScalesByPrefix,
                    Status::kInternalError,
                    request.actorFormID);
                CopyText(result.mutationKey, request.mutationKeyPrefix);
                {
                    std::scoped_lock lock(_impl->mutex);
                    if (_impl->generation.load() != requestGeneration ||
                        _impl->state.load() != ContextState::kReady) {
                        result.status = Status::kContextChanged;
                    }
                    else if (auto* actor = LookupActor(request.actorFormID)) {
                        std::vector<std::string> mutations;
                        for (const auto& [key, value] : _impl->scales) {
                            (void)key;
                            if (value.actorFormID == request.actorFormID &&
                                value.ownerID == request.ownerID &&
                                value.mutationKey.starts_with(
                                    request.mutationKeyPrefix)) {
                                mutations.push_back(value.mutationKey);
                            }
                        }
                        result.status = mutations.empty() ?
                            Status::kNotFound : Status::kSuccess;
                        for (const auto& mutation : mutations) {
                            const auto found = _impl->FindScale(
                                request.actorFormID,
                                request.ownerID,
                                mutation);
                            if (found == _impl->scales.end()) {
                                continue;
                            }
                            ActorScaleContribution removed;
                            std::string error;
                            if (!_impl->RemoveScale(actor, found, removed, error)) {
                                result.status = error.find("outside") !=
                                    std::string::npos ?
                                    Status::kConflict : Status::kInternalError;
                                CopyText(result.message, error);
                                break;
                            }
                            ++result.affectedCount;
                        }
                    }
                    else {
                        result.status = Status::kActorUnavailable;
                    }
                }
                logger::debug(
                    "[WhoEditThat] Actor Scale prefix remove: actor={:08X} "
                    "status={} affected={} message='{}'",
                    result.actorFormID,
                    std::to_underlying(result.status),
                    result.affectedCount,
                    result.message);
                target.Deliver(result);
            });
        return true;
    }

    bool Manager::QueueUpsertDisplayName(
        const DisplayNameRequest& request,
        Callback callback,
        void* userData)
    {
        if (!callback || request.actorFormID == 0 ||
            !request.mutationKey || !*request.mutationKey ||
            !request.displayName || !*request.displayName ||
            std::string_view(request.mutationKey).size() > 127 ||
            std::string_view(request.displayName).size() > 1024) {
            return false;
        }
        OwnedDisplayNameRequest owned;
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load() != ContextState::kReady) {
                return false;
            }
            const auto owner = _impl->Owner(request.client);
            if (!owner) {
                return false;
            }
            owned = {
                *owner,
                request.actorFormID,
                request.mutationKey,
                request.displayName,
                request.priority };
            requestGeneration = _impl->generation.load();
        }
        CallbackTarget target{ callback, userData };
        SKSE::GetTaskInterface()->AddTask(
            [this, request = std::move(owned), requestGeneration, target] {
                auto result = MakeResult(
                    Operation::kUpsertDisplayName,
                    Status::kInternalError,
                    request.actorFormID);
                {
                    std::scoped_lock lock(_impl->mutex);
                    if (_impl->generation.load() != requestGeneration ||
                        _impl->state.load() != ContextState::kReady) {
                        result.status = Status::kContextChanged;
                    }
                    else if (auto* actor = LookupActor(request.actorFormID)) {
                        auto existing = _impl->FindName(
                            request.actorFormID,
                            request.ownerID,
                            request.mutationKey);
                        const auto mapKey = existing == _impl->names.end() ?
                            CompositeKey(
                                request.actorFormID,
                                request.ownerID,
                                request.mutationKey) :
                            existing->first;
                        std::optional<DisplayNameOverride> previous;
                        if (existing != _impl->names.end()) {
                            previous = existing->second;
                        }

                        const auto* rawOriginal = actor->GetDisplayFullName();
                        std::string originalName = rawOriginal ? rawOriginal : "";
                        std::string lastAppliedName;
                        for (const auto& [key, value] : _impl->names) {
                            (void)key;
                            if (value.actorFormID == request.actorFormID) {
                                originalName = value.originalName;
                                lastAppliedName = value.lastAppliedName;
                                break;
                            }
                        }
                        DisplayNameOverride override{
                            request.actorFormID,
                            BuildActorKey(actor),
                            request.ownerID,
                            request.mutationKey,
                            request.displayName,
                            originalName,
                            lastAppliedName,
                            request.priority,
                            _impl->nextSequence++ };
                        _impl->names[mapKey] = override;
                        const auto status = _impl->ApplyWinningName(
                            actor, lastAppliedName, originalName);
                        if (status != Status::kSuccess) {
                            if (previous) {
                                _impl->names[mapKey] = *previous;
                            }
                            else {
                                _impl->names.erase(mapKey);
                            }
                            result.status = status;
                            CopyText(
                                result.message,
                                status == Status::kConflict ?
                                "Display name was changed outside the ledger" :
                                "Display name could not be applied");
                        }
                        else {
                            result.status = Status::kSuccess;
                            CopyText(result.ownerID, request.ownerID);
                            CopyText(result.mutationKey, request.mutationKey);
                            _impl->AddAudit(
                                _impl->names[mapKey],
                                previous ? "Update" : "Create",
                                Status::kSuccess);
                        }
                    }
                    else {
                        result.status = Status::kActorUnavailable;
                    }
                }
                target.Deliver(result);
            });
        return true;
    }

    bool Manager::QueueRemoveDisplayName(
        const ContributionRequest& request,
        Callback callback,
        void* userData)
    {
        if (!callback || request.actorFormID == 0 ||
            !request.mutationKey || !*request.mutationKey ||
            std::string_view(request.mutationKey).size() > 127) {
            return false;
        }
        OwnedContributionRequest owned;
        std::uint64_t requestGeneration = 0;
        {
            std::scoped_lock lock(_impl->mutex);
            if (_impl->state.load() != ContextState::kReady) {
                return false;
            }
            const auto owner = _impl->Owner(request.client);
            if (!owner) {
                return false;
            }
            owned = { *owner, request.actorFormID, request.mutationKey };
            requestGeneration = _impl->generation.load();
        }
        CallbackTarget target{ callback, userData };
        SKSE::GetTaskInterface()->AddTask(
            [this, request = std::move(owned), requestGeneration, target] {
                auto result = MakeResult(
                    Operation::kRemoveDisplayName,
                    Status::kInternalError,
                    request.actorFormID);
                {
                    std::scoped_lock lock(_impl->mutex);
                    if (_impl->generation.load() != requestGeneration ||
                        _impl->state.load() != ContextState::kReady) {
                        result.status = Status::kContextChanged;
                    }
                    else if (auto* actor = LookupActor(request.actorFormID)) {
                        const auto found = _impl->FindName(
                            request.actorFormID,
                            request.ownerID,
                            request.mutationKey);
                        if (found == _impl->names.end()) {
                            result.status = Status::kNotFound;
                        }
                        else {
                            const auto mapKey = found->first;
                            const auto removed = found->second;
                            _impl->names.erase(found);
                            const auto status = _impl->ApplyWinningName(
                                actor,
                                removed.lastAppliedName,
                                removed.originalName);
                            if (status != Status::kSuccess) {
                                _impl->names[mapKey] = removed;
                                result.status = status;
                                CopyText(
                                    result.message,
                                    status == Status::kConflict ?
                                    "Display name was changed outside the ledger" :
                                    "Display name could not be restored");
                            }
                            else {
                                result.status = Status::kSuccess;
                                CopyText(result.ownerID, removed.ownerID);
                                CopyText(result.mutationKey, removed.mutationKey);
                                _impl->AddAudit(
                                    removed, "Remove", Status::kSuccess);
                            }
                        }
                    }
                    else {
                        result.status = Status::kActorUnavailable;
                    }
                }
                target.Deliver(result);
            });
        return true;
    }

    bool Manager::SaveSerialization(
        SKSE::SerializationInterface* serialization)
    {
        if (!serialization) {
            return false;
        }
        std::scoped_lock lock(_impl->mutex);
        std::set<std::uint32_t> forms;
        for (const auto& [key, value] : _impl->actorValues) {
            (void)key;
            if (value.actorFormID != 0) {
                forms.insert(value.actorFormID);
            }
            if (value.sourceGlobalFormID != 0) {
                forms.insert(value.sourceGlobalFormID);
            }
        }
        for (const auto& [key, value] : _impl->scales) {
            (void)key;
            if (value.actorFormID != 0) {
                forms.insert(value.actorFormID);
            }
            if (value.sourceGlobalFormID != 0) {
                forms.insert(value.sourceGlobalFormID);
            }
        }
        for (const auto& [key, value] : _impl->names) {
            (void)key;
            if (value.actorFormID != 0) {
                forms.insert(value.actorFormID);
            }
        }

        if (!serialization->OpenRecord(kFormRecord, kFormRecordVersion)) {
            return false;
        }
        const auto count = static_cast<std::uint32_t>(forms.size());
        if (!serialization->WriteRecordData(
            std::addressof(count), sizeof(count))) {
            return false;
        }
        for (const auto formID : forms) {
            if (!serialization->WriteRecordData(
                std::addressof(formID), sizeof(formID))) {
                return false;
            }
        }
        return true;
    }

    void Manager::LoadSerialization(
        SKSE::SerializationInterface* serialization)
    {
        if (!serialization) {
            return;
        }
        std::scoped_lock lock(_impl->mutex);
        _impl->resolvedForms.clear();

        std::uint32_t type = 0;
        std::uint32_t version = 0;
        std::uint32_t length = 0;
        while (serialization->GetNextRecordInfo(type, version, length)) {
            if (type != kFormRecord || version != kFormRecordVersion) {
                logger::warn(
                    "[WhoEditThat] Ignoring serialization record {:08X} v{}",
                    type,
                    version);
                continue;
            }
            std::uint32_t count = 0;
            if (serialization->ReadRecordData(
                std::addressof(count), sizeof(count)) != sizeof(count)) {
                logger::error("[WhoEditThat] Invalid form mapping record");
                return;
            }
            if (count > 100000) {
                logger::error("[WhoEditThat] Form mapping count is unreasonable");
                return;
            }
            for (std::uint32_t index = 0; index < count; ++index) {
                std::uint32_t oldFormID = 0;
                if (serialization->ReadRecordData(
                    std::addressof(oldFormID),
                    sizeof(oldFormID)) != sizeof(oldFormID)) {
                    logger::error("[WhoEditThat] Truncated form mapping record");
                    return;
                }
                std::uint32_t newFormID = 0;
                if (serialization->ResolveFormID(oldFormID, newFormID)) {
                    _impl->resolvedForms.emplace(oldFormID, newFormID);
                }
            }
        }
    }
}
