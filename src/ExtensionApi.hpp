// Shared extension/runtime access for the character-creation preview module.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include "wxl/FrameScriptApi.h"
#include "wxl/PluginApi.h"

namespace wxl_character_creation_preview
{
    extern const WXL_Api* g_api;
    extern const WXL_FrameScriptApi* g_framescript;

    inline const WXL_FrameScriptApi* FrameScript()
    {
        if (!g_framescript && g_api)
            g_framescript = static_cast<const WXL_FrameScriptApi*>(
                g_api->GetInterface("wxl.framescript", WXL_FRAME_SCRIPT_API_VERSION));
        return g_framescript;
    }

    bool InstallCharacterCreationPreview();
}

#define WLOG_INFO(...)  ::wxl_character_creation_preview::g_api->Log( \
    WXL_LOG_INFO, "wxl-character-creation-preview", __VA_ARGS__)
#define WLOG_WARN(...)  ::wxl_character_creation_preview::g_api->Log( \
    WXL_LOG_WARN, "wxl-character-creation-preview", __VA_ARGS__)
#define WLOG_ERROR(...) ::wxl_character_creation_preview::g_api->Log( \
    WXL_LOG_ERROR, "wxl-character-creation-preview", __VA_ARGS__)
