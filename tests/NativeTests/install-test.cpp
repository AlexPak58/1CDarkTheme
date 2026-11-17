#include "platform.hpp"
#include <cstdio>
#include <cstdlib>

namespace {
bool denyRestore{};
bool failFlush{};

BOOL WINAPI fixtureProtect(LPVOID address, SIZE_T size, DWORD protection, PDWORD previous)
{
    if (denyRestore && protection == PAGE_EXECUTE_READ) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return VirtualProtect(address, size, protection, previous);
}

BOOL WINAPI fixtureFlush(HANDLE process, LPCVOID address, SIZE_T size)
{
    if (failFlush) {
        failFlush = false;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return FlushInstructionCache(process, address, size);
}
}

#define VirtualProtect fixtureProtect
#define FlushInstructionCache fixtureFlush
#include "theme.cpp"
#undef VirtualProtect
#undef FlushInstructionCache

void check(bool ok, const char* details)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", details);
        std::exit(1);
    }
}

int main()
{
    auto* page = static_cast<BYTE*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(page != nullptr, "allocate synthetic patch page");
    for (size_t i = 0; i < installedSiteCount; ++i) {
        sites[i].address = page + i * 8;
        *sites[i].address = sites[i].instruction.front();
    }
    DWORD ignored{};
    check(VirtualProtect(page, 1, PAGE_EXECUTE_READ, &ignored), "set initial executable protection");
    failFlush = true;
    check(patchSites() == ERROR_ACCESS_DENIED, "initial flush failure reports clean failure after rollback");
    MEMORY_BASIC_INFORMATION info{};
    check(VirtualQuery(page, &info, sizeof(info)) && info.Protect == PAGE_EXECUTE_READ,
          "restore shared executable page");
    for (size_t i = 0; i < installedSiteCount; ++i)
        check(*sites[i].address == sites[i].instruction.front(), "rollback restores original opcode");

    denyRestore = true;
    installationResult = patchSites();
    check(installationResult == themeStateUncertain, "partial install cannot claim clean rejection");
    check(*sites[0].address == 0xCC, "partial install really retained a breakpoint");
    check(applyProfile(defaultProfile, 0) == themeStateUncertain,
          "uncertain INT3 state cannot claim later disable");
    check(VirtualFree(page, 0, MEM_RELEASE), "release synthetic patch page");
    std::puts("PASS: initial breakpoint rollback and uncertain protection restoration");
}
