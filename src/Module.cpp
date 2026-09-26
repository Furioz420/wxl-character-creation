#include "ExtensionApi.hpp"

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info{
        sizeof(WXL_PluginInfo), WXL_API_VERSION,
        "wxl-character-creation-preview", 1, WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;
    wxl_character_creation_preview::g_api = api;

    if (!wxl_character_creation_preview::FrameScript())
    {
        api->Log(WXL_LOG_ERROR, "wxl-character-creation-preview",
                 "required wxl.framescript v1 is unavailable");
        return 0;
    }
    return wxl_character_creation_preview::InstallCharacterCreationPreview() ? 1 : 0;
}
