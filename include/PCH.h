#pragma once
#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <atomic>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace logger = SKSE::log;
using namespace std::literals;
