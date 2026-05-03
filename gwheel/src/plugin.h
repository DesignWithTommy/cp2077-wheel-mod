#pragma once

#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>

namespace gwheel
{
    inline constexpr const wchar_t* kPluginName = L"gwheel";
    inline constexpr const wchar_t* kPluginAuthor = L"Grant Perdue";
    inline constexpr int kVersionMajor = 2;
    inline constexpr int kVersionMinor = 31;
    inline constexpr int kVersionPatch = 1;
    inline constexpr const char* kVersionString = "2.31.1";

    struct PluginContext
    {
        RED4ext::v1::PluginHandle handle{};
        const RED4ext::v1::Sdk* sdk{};
    };

    PluginContext& Ctx();

    void OnLoad(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
    void OnUnload();
}
