#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>

namespace hook_sync {

constexpr DWORD kDefaultLockTimeoutMs = 15000;
constexpr uint32_t kRegistryMagic = 0x4B4F4F48;  // "HOOK"
constexpr uint32_t kRegistryVersion = 1;
constexpr int kMaxRegisteredHooks = 32;

using SuspendFn = void (*)(void* context);
using ResumeFn = void (*)(void* context);

struct RegistryEntry {
    void* target;
    SuspendFn suspend;
    ResumeFn resume;
    void* context;
    uint64_t sequence;
};

struct Registry {
    uint32_t magic;
    uint32_t version;
    uint64_t next_sequence;
    RegistryEntry entries[kMaxRegisteredHooks];
};

inline HANDLE lock_handle() {
    static HANDLE handle = [] {
        wchar_t name[64];
        swprintf_s(name, L"Local\\ets2la_hook_lock_%lu", GetCurrentProcessId());
        return CreateMutexW(nullptr, FALSE, name);
    }();
    return handle;
}

inline bool lock(DWORD timeout_ms = kDefaultLockTimeoutMs) {
    HANDLE handle = lock_handle();
    if (!handle) {
        return false;
    }
    const DWORD result = WaitForSingleObject(handle, timeout_ms);
    return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
}

inline void unlock() {
    ReleaseMutex(lock_handle());
}

class HookLockGuard {
public:
    explicit HookLockGuard(DWORD timeout_ms = kDefaultLockTimeoutMs) : owns_(lock(timeout_ms)) {}
    ~HookLockGuard() {
        if (owns_) {
            unlock();
        }
    }
    HookLockGuard(const HookLockGuard&) = delete;
    HookLockGuard& operator=(const HookLockGuard&) = delete;

    bool owns_lock() const { return owns_; }

private:
    bool owns_;
};

inline Registry* registry() {
    static Registry* view = []() -> Registry* {
        wchar_t name[64];
        swprintf_s(name, L"Local\\ets2la_hook_registry_%lu", GetCurrentProcessId());
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Registry), name);
        if (!mapping) {
            return nullptr;
        }
        void* memory = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Registry));
        if (!memory) {
            CloseHandle(mapping);
            return nullptr;
        }
        auto* result = static_cast<Registry*>(memory);
        if (result->magic == 0) {
            result->magic = kRegistryMagic;
            result->version = kRegistryVersion;
            result->next_sequence = 0;
        }
        if (result->magic != kRegistryMagic || result->version != kRegistryVersion) {
            UnmapViewOfFile(memory);
            CloseHandle(mapping);
            return nullptr;
        }
        return result;
    }();
    return view;
}

inline bool owner_loaded(const RegistryEntry& entry) {
    HMODULE module = nullptr;
    return GetModuleHandleExW(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
               reinterpret_cast<LPCWSTR>(entry.suspend), &module
           ) && module;
}

inline void purge_stale_entries(Registry* reg) {
    for (RegistryEntry& entry : reg->entries) {
        if (entry.target && !owner_loaded(entry)) {
            std::memset(&entry, 0, sizeof(entry));
        }
    }
}

inline RegistryEntry* find_entry(Registry* reg, void* target, SuspendFn suspend) {
    for (RegistryEntry& entry : reg->entries) {
        if (entry.target == target && entry.suspend == suspend) {
            return &entry;
        }
    }
    return nullptr;
}

inline bool register_hook(void* target, SuspendFn suspend, ResumeFn resume, void* context) {
    Registry* reg = registry();
    if (!reg || !target || !suspend || !resume) {
        return false;
    }
    purge_stale_entries(reg);
    RegistryEntry* entry = find_entry(reg, target, suspend);
    if (!entry) {
        entry = find_entry(reg, nullptr, nullptr);
    }
    if (!entry) {
        return false;
    }
    entry->target = target;
    entry->suspend = suspend;
    entry->resume = resume;
    entry->context = context;
    entry->sequence = ++reg->next_sequence;
    return true;
}

template <typename RemoveFn>
inline void remove_hook_ordered(void* target, SuspendFn suspend, RemoveFn&& remove_own_hook) {
    Registry* reg = registry();
    RegistryEntry* own = reg ? find_entry(reg, target, suspend) : nullptr;
    if (!own) {
        remove_own_hook();
        return;
    }
    purge_stale_entries(reg);
    own = find_entry(reg, target, suspend);

    RegistryEntry above[kMaxRegisteredHooks];
    int count = 0;
    if (own) {
        for (const RegistryEntry& entry : reg->entries) {
            if (entry.target == target && entry.sequence > own->sequence) {
                above[count++] = entry;
            }
        }
    }

    for (int i = 1; i < count; ++i) {
        const RegistryEntry key = above[i];
        int j = i - 1;
        while (j >= 0 && above[j].sequence < key.sequence) {
            above[j + 1] = above[j];
            --j;
        }
        above[j + 1] = key;
    }

    for (int i = 0; i < count; ++i) {
        above[i].suspend(above[i].context);
    }

    remove_own_hook();
    if (own) {
        std::memset(own, 0, sizeof(*own));
    }

    for (int i = count - 1; i >= 0; --i) {
        above[i].resume(above[i].context);
        if (RegistryEntry* entry = find_entry(reg, above[i].target, above[i].suspend)) {
            entry->sequence = ++reg->next_sequence;
        }
    }
}

}