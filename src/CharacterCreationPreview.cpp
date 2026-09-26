// Class-specific animation choreography for the Glue character-creation model.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "ExtensionApi.hpp"

#include "engine/events/Event.hpp"
#include "game/Binding.hpp"
#include "game/M2.hpp"
#include "game/M2Animation.hpp"
#include "game/Script.hpp"
#include "offsets/game/M2.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
    namespace ev = wxl::events;
    namespace gm2 = wxl::game::m2;
    namespace m2animation = wxl::game::m2animation;
    namespace m2off = wxl::offsets::game::m2;
    namespace script = wxl::game::script;

    // Build 12340 globals recovered from CCharacterCreation::RebuildCharacterModel.
    // m_character is a CCharacterComponent*. Runtime inspection on the live Glue scene confirms
    // the displayed CM2Model* at +0x38; +0x20 is the selected class ID, not a model pointer.
    constexpr uintptr_t kCharacterCreationComponent = 0x00B6B1A0;
    constexpr uintptr_t kUpdateCharacterDisplayEquipment = 0x004E0FD0;
    constexpr size_t kCharacterComponentModel = 0x38;
    constexpr uint32_t kAllBoneSlots = 0xFFFFFFFFu;
    constexpr uint32_t kNoPreviousSequence = 0xFFFFFFFFu;
    // Back-link in the build-12340 M2 attached-child list. Public offsets expose the
    // forward links; this is needed to unlink one owned effect without emptying its slot.
    constexpr size_t kAttachedChildPrev = 0x5C;
    constexpr size_t kMaxExtraVisualTextures = 4;

    // Attachment IDs recovered from CGlueMgr::ApplySpellVisualKitAttachments.
    constexpr uint32_t kAttachBase = 0x13;
    constexpr uint32_t kAttachHead = 0x14;
    constexpr uint32_t kAttachChest = 0x22;
    constexpr uint32_t kAttachLeftHand = 0x15;
    constexpr uint32_t kAttachRightHand = 0x16;
    constexpr uint32_t kAttachRightWeapon = 0x18;

    struct ExtraVisualTexture
    {
        const char* path;
        uint32_t type;
    };

    struct VisualEffect
    {
        const char* path;
        uint32_t attachment;
        float scale;
        float travel;
        float travelZ;
        uint32_t travelMs;
        float offsetX;
        float offsetY;
        float offsetZ;
        const char* texturePath;
        uint32_t textureType = 2;
        const char* secondaryTexturePath = nullptr;
        uint32_t secondaryTextureType = 0;
        uint32_t modelAnimation = UINT32_MAX;
        float modelAnimationBlend = 1.0f;
        uint32_t modelAnimationLoop = 1;
        bool useAttachmentOrigin = false;
        bool persistAcrossPhases = false;
        const ExtraVisualTexture* extraTextures = nullptr;
        uint32_t extraTextureCount = 0;
        uint32_t animationHoldFromMs = 0;
        uint32_t animationHoldAtMs = 0;
        uint32_t animationReleaseAtMs = 0;
        bool useRootRelativeTarget = false;
        bool forceVisible = false;
    };

    struct ActiveVisual
    {
        const char* path;
        void* root;
        void* renderContext;
        void* texture;
        void* secondaryTexture;
        uint32_t textureType;
        uint32_t secondaryTextureType;
        void* extraTextures[kMaxExtraVisualTextures];
        uint32_t extraTextureTypes[kMaxExtraVisualTextures];
        uint32_t extraTextureCount;
        uint32_t attachment;
        float scale;
        float travel;
        float travelZ;
        uint32_t travelMs;
        float offsetX;
        float offsetY;
        float offsetZ;
        uint32_t launchMs;
        bool useAttachmentOrigin;
        bool persistAcrossPhases;
        bool animationConfigured;
        bool originCaptured;
        uint32_t animationHoldFromMs;
        uint32_t animationHoldAtMs;
        uint32_t animationReleaseAtMs;
        bool useRootRelativeTarget;
        bool forceVisible;
        bool placementReported;
        float origin[3];
    };

    std::vector<ActiveVisual> g_activeVisuals;

    struct BlizzardEmitter
    {
        bool active = false;
        uint32_t nextMs = 0;
        uint32_t emitted = 0;
    };
    BlizzardEmitter g_blizzardEmitter;

    struct PriestChannelEmitter
    {
        bool active = false;
        uint32_t nextMs = 0;
        uint32_t emitted = 0;
    };
    PriestChannelEmitter g_priestChannelEmitter;
    constexpr uint32_t kMaxDruidStage = 17;
    uint32_t g_druidMorphStart = 0;
    uint32_t g_druidTranquilityStart = 0;
    const char* g_druidFormPath = nullptr;
    uint32_t g_druidFormAnimation = 0;

    struct WarlockImpEmitter
    {
        bool active = false;
        bool preparing = false;
        uint32_t nextMs = 0;
        uint32_t emitted = 0;
    };
    WarlockImpEmitter g_warlockImpEmitter;

    struct WarlockRainEmitter
    {
        bool active = false;
        uint32_t nextMs = 0;
        uint32_t emitted = 0;
    };
    WarlockRainEmitter g_warlockRainEmitter;

    bool g_characterHidden = false;
    void* g_hiddenCharacter = nullptr;
    float g_hiddenAlphaBase = 1.0f;
    float g_hiddenAlphaStage = 1.0f;


    // CCharModel::AddHandItem(model, visibleItem, equipmentSlot, sheatheType,
    //                         sheathed, alternateHand, twoHanded, textureOverride).
    // The Glue equipment refresh always supplies sheathed=0, even after its model has been switched
    // to the non-combat Stand sequence. That leaves a two-handed weapon on a hand attachment while
    // the hands return to the sides, making the handle look like a long rigid spike through the
    // character. Intercept only our own synchronous refresh and preserve every other argument.
    using CharAddHandItemFn = int32_t (__cdecl*)(
        void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void*);
    CharAddHandItemFn g_originalCharAddHandItem = nullptr;
    thread_local bool g_forceCustomizationSheathed = false;

    void* CharacterCreationModel() noexcept
    {
        __try
        {
            void* component = *reinterpret_cast<void**>(kCharacterCreationComponent);
            if (!component) return nullptr;
            return *reinterpret_cast<void**>(
                static_cast<uint8_t*>(component) + kCharacterComponentModel);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    bool SetCharacterVisible(bool visible) noexcept
    {
        void* model = CharacterCreationModel();
        if (!model) return false;
        __try
        {
            auto* instance = static_cast<m2off::M2Instance*>(model);
            if (!visible)
            {
                if (!g_characterHidden || g_hiddenCharacter != model)
                {
                    g_hiddenCharacter = model;
                    g_hiddenAlphaBase = instance->alphaBase;
                    g_hiddenAlphaStage = instance->alphaStage;
                }
                g_characterHidden = true;
                instance->alphaBase = 0.0f;
                instance->alphaStage = 0.0f;
            }
            else
            {
                g_characterHidden = false;
                if (g_hiddenCharacter == model)
                {
                    instance->alphaBase = std::isfinite(g_hiddenAlphaBase)
                        ? g_hiddenAlphaBase : 1.0f;
                    instance->alphaStage = std::isfinite(g_hiddenAlphaStage)
                        ? g_hiddenAlphaStage : 1.0f;
                }
                g_hiddenCharacter = nullptr;
            }
            WLOG_INFO("character render visibility=%s model=%p alpha=(%.3f,%.3f)",
                      visible ? "shown" : "hidden", model,
                      instance->alphaBase, instance->alphaStage);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    int32_t __cdecl hkCharAddHandItem(
        void* model, void* visibleItem, uint32_t equipmentSlot,
        uint32_t sheatheType, uint32_t sheathed, uint32_t alternateHand,
        uint32_t twoHanded, void* textureOverride)
    {
        if (g_forceCustomizationSheathed && model == CharacterCreationModel() &&
            equipmentSlot >= 15 && equipmentSlot <= 17)
            sheathed = 1;
        return g_originalCharAddHandItem(
            model, visibleItem, equipmentSlot, sheatheType, sheathed,
            alternateHand, twoHanded, textureOverride);
    }

    void* SafePointer(void* base, size_t offset) noexcept
    {
        if (!base) return nullptr;
        __try
        {
            return *reinterpret_cast<void**>(static_cast<uint8_t*>(base) + offset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void* SafeModelData(void* model) noexcept
    {
        __try { return m2animation::ModelData(model); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void ApplyVisualPlacement(ActiveVisual& visual) noexcept
    {
        if (!visual.renderContext) return;
        const bool travels = (visual.travel != 0.0f || visual.travelZ != 0.0f) &&
                             visual.travelMs;
        const bool spatial = travels || visual.offsetX != 0.0f ||
                             visual.offsetY != 0.0f || visual.offsetZ != 0.0f;
        __try
        {
            // The native child update mirrors texture bindings from the parent. Reapply any
            // CreatureDisplayInfo skin afterwards so standalone creature visuals do not turn
            // white on the next frame.
            if (visual.texture)
                gm2::BindTexSlotType(
                    visual.renderContext, visual.textureType, visual.texture);
            if (visual.secondaryTexture)
                gm2::BindTexSlotType(
                    visual.renderContext, visual.secondaryTextureType,
                    visual.secondaryTexture);
            for (uint32_t i = 0; i < visual.extraTextureCount; ++i)
                if (visual.extraTextures[i])
                    gm2::BindTexSlotType(
                        visual.renderContext, visual.extraTextureTypes[i],
                        visual.extraTextures[i]);
            auto* instance = static_cast<uint8_t*>(visual.renderContext);
            if (visual.forceVisible)
            {
                auto* m2Instance = reinterpret_cast<m2off::M2Instance*>(instance);
                m2Instance->alphaBase = 1.0f;
                m2Instance->alphaStage = 1.0f;
            }
            float* placement = reinterpret_cast<float*>(
                instance + m2off::kOffInstPlacement);
            const uint32_t age = GetTickCount() - visual.launchMs;
            float progress = travels
                ? static_cast<float>(age) / static_cast<float>(visual.travelMs)
                : 0.0f;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            const float eased = progress * progress * (3.0f - 2.0f * progress);
            const float distance = visual.travel * eased;
            const float arc = travels
                ? std::sin(progress * 3.14159265f) * 0.18f : 0.0f;
            const float height = visual.travelZ * eased;
            if (spatial && visual.root)
            {
                const float* rootPlacement = reinterpret_cast<const float*>(
                    static_cast<const uint8_t*>(visual.root) + m2off::kOffInstPlacement);
                if (visual.useAttachmentOrigin && !visual.originCaptured)
                {
                    visual.origin[0] = placement[12];
                    visual.origin[1] = placement[13];
                    visual.origin[2] = placement[14];
                    visual.originCaptured =
                        std::isfinite(visual.origin[0]) &&
                        std::isfinite(visual.origin[1]) &&
                        std::isfinite(visual.origin[2]);
                }
                float translated[3] = {
                    visual.useAttachmentOrigin && visual.originCaptured
                        ? visual.origin[0] : rootPlacement[12],
                    visual.useAttachmentOrigin && visual.originCaptured
                        ? visual.origin[1] : rootPlacement[13],
                    visual.useAttachmentOrigin && visual.originCaptured
                        ? visual.origin[2] : rootPlacement[14]
                };
                const auto addDirection = [&](int start, float amount) noexcept {
                    if (amount == 0.0f) return;
                    const float x = rootPlacement[start];
                    const float y = rootPlacement[start + 1];
                    const float z = rootPlacement[start + 2];
                    const float length = std::sqrt(x * x + y * y + z * z);
                    if (!std::isfinite(length) || length <= 0.0001f) return;
                    translated[0] += amount * x / length;
                    translated[1] += amount * y / length;
                    translated[2] += amount * z / length;
                };
                // WoW model space is X-forward, Y-left, Z-up. offsetX/travel describe screen-side
                // motion, offsetY describes depth, and offsetZ/travelZ describe height.
                if (visual.useRootRelativeTarget && visual.originCaptured && travels)
                {
                    float target[3] = {
                        rootPlacement[12], rootPlacement[13], rootPlacement[14]
                    };
                    const auto addTargetDirection = [&](int start, float amount) noexcept {
                        if (amount == 0.0f) return;
                        const float x = rootPlacement[start];
                        const float y = rootPlacement[start + 1];
                        const float z = rootPlacement[start + 2];
                        const float length = std::sqrt(x * x + y * y + z * z);
                        if (!std::isfinite(length) || length <= 0.0001f) return;
                        target[0] += amount * x / length;
                        target[1] += amount * y / length;
                        target[2] += amount * z / length;
                    };
                    addTargetDirection(4, visual.offsetX + visual.travel);
                    addTargetDirection(0, visual.offsetY);
                    addTargetDirection(8, visual.offsetZ + visual.travelZ);
                    translated[0] = visual.origin[0] + (target[0] - visual.origin[0]) * eased;
                    translated[1] = visual.origin[1] + (target[1] - visual.origin[1]) * eased;
                    translated[2] = visual.origin[2] + (target[2] - visual.origin[2]) * eased;
                    addDirection(8, arc);
                }
                else
                {
                    addDirection(4, visual.offsetX + distance);
                    addDirection(0, visual.offsetY);
                    addDirection(8, visual.offsetZ + height + arc);
                }
                placement[12] = translated[0];
                placement[13] = translated[1];
                placement[14] = translated[2];
            }
            for (int basis = 0; basis < 3; ++basis)
            {
                const int start = basis * 4;
                const float x = placement[start];
                const float y = placement[start + 1];
                const float z = placement[start + 2];
                const float length = std::sqrt(x * x + y * y + z * z);
                if (!std::isfinite(length) || length <= 0.0001f) continue;
                const float factor = visual.scale / length;
                placement[start] *= factor;
                placement[start + 1] *= factor;
                placement[start + 2] *= factor;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    bool DetachOwnedVisual(void* parent, void* child) noexcept
    {
        if (!parent || !child) return false;
        __try
        {
            void** link = reinterpret_cast<void**>(
                static_cast<uint8_t*>(parent) + m2off::kOffInstAttachedHead);
            void* current = *link;
            for (uint32_t depth = 0; current && depth < 256; ++depth)
            {
                void* next = *reinterpret_cast<void**>(
                    static_cast<uint8_t*>(current) + m2off::kOffInstAttachedNext);
                if (current == child)
                {
                    *link = next;
                    if (next)
                        *reinterpret_cast<void***>(
                            static_cast<uint8_t*>(next) + kAttachedChildPrev) = link;
                    *reinterpret_cast<void**>(
                        static_cast<uint8_t*>(child) + m2off::kOffInstParent) = nullptr;
                    *reinterpret_cast<uint32_t*>(
                        static_cast<uint8_t*>(child) + m2off::kOffInstAttachSlot) =
                        0xFFFFu;
                    *reinterpret_cast<void**>(
                        static_cast<uint8_t*>(child) + kAttachedChildPrev) = nullptr;
                    *reinterpret_cast<void**>(
                        static_cast<uint8_t*>(child) + m2off::kOffInstAttachedNext) =
                        nullptr;
                    gm2::ReleaseRenderCtx(child);
                    return true;
                }
                link = reinterpret_cast<void**>(
                    static_cast<uint8_t*>(current) + m2off::kOffInstAttachedNext);
                current = next;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    void ClearVisuals(bool includePersistent = true) noexcept
    {
        g_blizzardEmitter.active = false;
        g_priestChannelEmitter.active = false;
        if (includePersistent)
        {
            g_druidFormPath = nullptr;
            g_druidMorphStart = 0;
            g_druidTranquilityStart = 0;
            g_warlockImpEmitter.active = false;
            g_warlockRainEmitter.active = false;
        }
        void* current = CharacterCreationModel();
        const size_t before = g_activeVisuals.size();
        for (auto it = g_activeVisuals.begin(); it != g_activeVisuals.end();)
        {
            if (!includePersistent && it->persistAcrossPhases)
            {
                ++it;
                continue;
            }
            if (current && current == it->root)
                DetachOwnedVisual(it->root, it->renderContext);
            it = g_activeVisuals.erase(it);
        }
        const size_t cleared = before - g_activeVisuals.size();
        if (cleared)
            WLOG_INFO("cleared visuals=%zu retained=%zu root=%p", cleared,
                      g_activeVisuals.size(), current);
    }

    bool HasActiveVisual(const char* path) noexcept
    {
        if (!path) return false;
        for (const ActiveVisual& visual : g_activeVisuals)
            if (visual.path && _stricmp(visual.path, path) == 0)
                return true;
        return false;
    }

    void RemoveActiveVisualsByPath(const char* path) noexcept
    {
        if (!path) return;
        void* current = CharacterCreationModel();
        for (auto it = g_activeVisuals.begin(); it != g_activeVisuals.end();)
        {
            if (!it->path || _stricmp(it->path, path) != 0)
            {
                ++it;
                continue;
            }
            if (current && current == it->root)
                DetachOwnedVisual(it->root, it->renderContext);
            it = g_activeVisuals.erase(it);
        }
    }

    bool TryConfigureVisualAnimation(void* renderContext, uint32_t animation,
                                     float blend, uint32_t loop) noexcept
    {
        if (!renderContext || animation == UINT32_MAX) return true;
        __try
        {
            if (!m2animation::ModelHasSequence(renderContext, animation)) return false;
            wxl::game::Native<m2off::M2_SetBoneSequenceFn>(m2off::kSetBoneSequence)(
                renderContext, nullptr, kAllBoneSlots, animation,
                kNoPreviousSequence, 0, blend, loop, 1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WLOG_WARN("visual animation setup deferred animation=%u context=%p",
                      animation, renderContext);
            return false;
        }
    }

    void HoldVisualAnimation(ActiveVisual& visual, uint32_t age) noexcept
    {
        if (!visual.animationConfigured || !visual.animationHoldFromMs ||
            age < visual.animationHoldFromMs ||
            (visual.animationReleaseAtMs && age >= visual.animationReleaseAtMs))
            return;
        __try
        {
            using SetBoneSequenceTimeFn = void(__fastcall*)(
                void*, void*, uint32_t, uint32_t);
            wxl::game::Native<SetBoneSequenceTimeFn>(m2off::kSetBoneSequenceTime)(
                visual.renderContext, nullptr, kNoPreviousSequence,
                visual.animationHoldAtMs);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    bool EnsureActiveVisualAnimation(const char* path, uint32_t animation,
                                     float blend, uint32_t loop) noexcept
    {
        if (!path) return false;
        for (ActiveVisual& visual : g_activeVisuals)
        {
            if (!visual.path || _stricmp(visual.path, path) != 0) continue;
            if (visual.animationConfigured) return true;
            visual.animationConfigured = TryConfigureVisualAnimation(
                visual.renderContext, animation, blend, loop);
            return visual.animationConfigured;
        }
        return false;
    }

    bool RestartActiveVisualAnimation(const char* path, uint32_t animation,
                                      float blend, uint32_t loop) noexcept
    {
        if (!path) return false;
        for (ActiveVisual& visual : g_activeVisuals)
        {
            if (!visual.path || _stricmp(visual.path, path) != 0) continue;
            visual.animationConfigured = TryConfigureVisualAnimation(
                visual.renderContext, animation, blend, loop);
            return visual.animationConfigured;
        }
        return false;
    }

    void PreloadVisual(const char* path) noexcept
    {
        void* root = CharacterCreationModel();
        void* owner = SafePointer(root, m2off::kOffSceneNodeOwner);
        if (!owner || !path || !*path) return;
        void* renderContext = nullptr;
        __try
        {
            renderContext = gm2::GetRenderCtx(owner, const_cast<char*>(path));
            if (renderContext) gm2::ReleaseRenderCtx(renderContext);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (renderContext)
            {
                __try { gm2::ReleaseRenderCtx(renderContext); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
    }

    bool RestoreEquipment() noexcept
    {
        if (!CharacterCreationModel()) return false;
        __try
        {
            // Refresh the selected CharStartOutfit after the asynchronous model rebuild has
            // settled. This never detaches equipment and does not rebuild customization data.
            wxl::game::Native<void(__cdecl*)()>(kUpdateCharacterDisplayEquipment)();
            WLOG_INFO("refreshed character-creation equipment");
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SetWeaponsSheathed(bool sheathed) noexcept
    {
        if (!CharacterCreationModel()) return false;
        __try
        {
            g_forceCustomizationSheathed = sheathed;
            wxl::game::Native<void(__cdecl*)()>(kUpdateCharacterDisplayEquipment)();
            g_forceCustomizationSheathed = false;
            WLOG_INFO("refreshed character-creation weapons mode=%s",
                      sheathed ? "sheathed" : "unsheathed");
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_forceCustomizationSheathed = false;
            return false;
        }
    }

    bool AttachVisual(const VisualEffect& effect) noexcept
    {
        void* root = CharacterCreationModel();
        void* owner = SafePointer(root, m2off::kOffSceneNodeOwner);
        if (!root || !owner || !effect.path || !*effect.path) return false;

        void* renderContext = nullptr;
        void* texture = nullptr;
        void* secondaryTexture = nullptr;
        void* extraTextures[kMaxExtraVisualTextures]{};
        __try
        {
            renderContext = gm2::GetRenderCtx(owner, const_cast<char*>(effect.path));
            if (!renderContext)
            {
                WLOG_WARN("visual load failed slot=%u path=%s",
                          effect.attachment, effect.path);
                return false;
            }
            if (effect.texturePath && *effect.texturePath)
            {
                texture = gm2::LoadResource(effect.texturePath);
                if (!texture)
                    WLOG_WARN("visual texture load failed path=%s model=%s",
                              effect.texturePath, effect.path);
            }
            if (effect.secondaryTexturePath && *effect.secondaryTexturePath)
            {
                secondaryTexture = gm2::LoadResource(effect.secondaryTexturePath);
                if (!secondaryTexture)
                    WLOG_WARN("visual secondary texture load failed path=%s model=%s",
                              effect.secondaryTexturePath, effect.path);
            }
            const uint32_t extraTextureCount = effect.extraTextures
                ? (effect.extraTextureCount < kMaxExtraVisualTextures
                    ? effect.extraTextureCount
                    : static_cast<uint32_t>(kMaxExtraVisualTextures))
                : 0;
            for (uint32_t i = 0; i < extraTextureCount; ++i)
            {
                const char* path = effect.extraTextures[i].path;
                if (!path || !*path) continue;
                extraTextures[i] = gm2::LoadResource(path);
                if (!extraTextures[i])
                    WLOG_WARN("visual extra texture load failed path=%s model=%s",
                              path, effect.path);
            }
            // Never force an effect into a slot already owned by outfit/collection geometry.
            // Forcing here can replace that model and later slot cleanup can remove it entirely.
            gm2::AttachToScene(renderContext, root, effect.attachment, false);
            const bool attached =
                SafePointer(renderContext, m2off::kOffInstParent) == root;
            gm2::ReleaseRenderCtx(renderContext);
            if (!attached)
            {
                WLOG_WARN("visual attachment rejected slot=%u path=%s",
                          effect.attachment, effect.path);
                return false;
            }
            // AttachToScene may mirror the parent's texture binding into a new child. Apply the
            // CreatureDisplayInfo variation after attachment so the Elemental keeps its own skin.
            if (texture)
                gm2::BindTexSlotType(renderContext, effect.textureType, texture);
            if (secondaryTexture)
                gm2::BindTexSlotType(
                    renderContext, effect.secondaryTextureType, secondaryTexture);
            for (uint32_t i = 0; i < extraTextureCount; ++i)
                if (extraTextures[i])
                    gm2::BindTexSlotType(
                        renderContext, effect.extraTextures[i].type, extraTextures[i]);
            const bool animationConfigured = TryConfigureVisualAnimation(
                renderContext, effect.modelAnimation, effect.modelAnimationBlend,
                effect.modelAnimationLoop);
            ActiveVisual active{};
            active.path = effect.path;
            active.root = root;
            active.renderContext = renderContext;
            active.texture = texture;
            active.secondaryTexture = secondaryTexture;
            active.textureType = effect.textureType;
            active.secondaryTextureType = effect.secondaryTextureType;
            active.extraTextureCount = extraTextureCount;
            for (uint32_t i = 0; i < extraTextureCount; ++i)
            {
                active.extraTextures[i] = extraTextures[i];
                active.extraTextureTypes[i] = effect.extraTextures[i].type;
            }
            active.attachment = effect.attachment;
            active.scale = effect.scale;
            active.travel = effect.travel;
            active.travelZ = effect.travelZ;
            active.travelMs = effect.travelMs;
            active.offsetX = effect.offsetX;
            active.offsetY = effect.offsetY;
            active.offsetZ = effect.offsetZ;
            active.launchMs = GetTickCount();
            active.useAttachmentOrigin = effect.useAttachmentOrigin;
            active.persistAcrossPhases = effect.persistAcrossPhases;
            active.animationConfigured = animationConfigured;
            active.originCaptured = false;
            active.animationHoldFromMs = effect.animationHoldFromMs;
            active.animationHoldAtMs = effect.animationHoldAtMs;
            active.animationReleaseAtMs = effect.animationReleaseAtMs;
            active.useRootRelativeTarget = effect.useRootRelativeTarget;
            active.forceVisible = effect.forceVisible;
            g_activeVisuals.push_back(active);
            WLOG_INFO(
                "attached visual slot=%u scale=%.2f travel=(%.2f,%.2f)/%ums "
                "offset=(%.2f,%.2f,%.2f) path=%s texture=%s type=%u "
                "secondary=%s secondaryType=%u extraTextures=%u",
                effect.attachment, effect.scale, effect.travel, effect.travelZ,
                effect.travelMs, effect.offsetX, effect.offsetY, effect.offsetZ,
                effect.path, effect.texturePath ? effect.texturePath : "<native>",
                effect.textureType,
                effect.secondaryTexturePath ? effect.secondaryTexturePath : "<none>",
                effect.secondaryTextureType, extraTextureCount);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (renderContext)
            {
                __try { gm2::ReleaseRenderCtx(renderContext); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            WLOG_WARN("visual attachment raised exception slot=%u path=%s",
                      effect.attachment, effect.path ? effect.path : "<null>");
            return false;
        }
    }

    void StartWotlkBlizzardEmitter() noexcept
    {
        g_blizzardEmitter.active = true;
        g_blizzardEmitter.nextMs = GetTickCount();
        g_blizzardEmitter.emitted = 0;
        WLOG_INFO("started WotLK Blizzard procedure=9 rate=5.00/s");
    }

    void UpdateWotlkBlizzardEmitter() noexcept
    {
        if (!g_blizzardEmitter.active) return;
        static constexpr float kOffsets[][3] = {
            {-1.60f,  0.00f, 0.0f},
            {-0.75f,  0.35f, 0.0f},
            { 0.10f, -0.20f, 0.0f},
            { 0.90f,  0.30f, 0.0f},
            { 1.65f, -0.10f, 0.0f},
        };
        constexpr uint32_t kEmissionIntervalMs = 200; // Authored WotLK rate: 5 per second.
        constexpr uint32_t kEmissionCount = 16;       // The preview channels for 3.2 seconds.

        const uint32_t now = GetTickCount();
        while (g_blizzardEmitter.active &&
               static_cast<int32_t>(now - g_blizzardEmitter.nextMs) >= 0)
        {
            const uint32_t emitted = g_blizzardEmitter.emitted;
            const auto& offset = kOffsets[emitted % _countof(kOffsets)];
            const float scale = (emitted % 3 == 1) ? 1.05f : 0.92f;
            AttachVisual(VisualEffect{
                "Spells\\Blizzard_Impact_Base.mdx", kAttachBase, scale,
                0.0f, 0.0f, 0, offset[0], offset[1], offset[2]});
            ++g_blizzardEmitter.emitted;
            g_blizzardEmitter.nextMs += kEmissionIntervalMs;
            if (g_blizzardEmitter.emitted >= kEmissionCount)
            {
                g_blizzardEmitter.active = false;
                WLOG_INFO("completed WotLK Blizzard emissions=%u",
                          g_blizzardEmitter.emitted);
            }
        }
    }

    void StartWotlkPriestChannelEmitter() noexcept
    {
        g_priestChannelEmitter.active = true;
        g_priestChannelEmitter.nextMs = GetTickCount();
        g_priestChannelEmitter.emitted = 0;
        WLOG_INFO("started WotLK Priest shadow channel missile stream");
    }

    void UpdateWotlkPriestChannelEmitter() noexcept
    {
        if (!g_priestChannelEmitter.active) return;
        constexpr uint32_t kEmissionIntervalMs = 800;
        constexpr uint32_t kEmissionCount = 3;
        const uint32_t now = GetTickCount();
        while (g_priestChannelEmitter.active &&
               static_cast<int32_t>(now - g_priestChannelEmitter.nextMs) >= 0)
        {
            AttachVisual(VisualEffect{
                "Spells\\ShadowBolt_Missile.m2", kAttachBase, 1.0f,
                -7.2f, 4.3f, 1050, 0.35f, 0.0f, 1.15f,
                nullptr, 2, nullptr, 0, 0, 1.0f, 1});
            ++g_priestChannelEmitter.emitted;
            g_priestChannelEmitter.nextMs += kEmissionIntervalMs;
            if (g_priestChannelEmitter.emitted >= kEmissionCount)
            {
                g_priestChannelEmitter.active = false;
                WLOG_INFO("completed WotLK Priest shadow channel missiles=%u",
                          g_priestChannelEmitter.emitted);
            }
        }
    }

    void StartWotlkWarlockImpEmitter() noexcept
    {
        g_warlockImpEmitter.active = true;
        g_warlockImpEmitter.preparing = true;
        g_warlockImpEmitter.nextMs = GetTickCount() + 500;
        g_warlockImpEmitter.emitted = 0;
        WLOG_INFO("started slow WotLK Imp Firebolt prepare/release cycles");
    }

    void UpdateWotlkWarlockImpEmitter() noexcept
    {
        if (!g_warlockImpEmitter.active) return;
        constexpr uint32_t kPrepareMs = 1000;
        constexpr uint32_t kBetweenCastsMs = 1400;
        constexpr uint32_t kEmissionCount = 3;
        const uint32_t now = GetTickCount();
        while (g_warlockImpEmitter.active &&
               static_cast<int32_t>(now - g_warlockImpEmitter.nextMs) >= 0)
        {
            if (g_warlockImpEmitter.preparing)
            {
                RestartActiveVisualAnimation("Creature\\Imp\\Imp.m2", 52, 0.22f, 1);
                RemoveActiveVisualsByPath("Spells\\Fire_Precast_Hand.mdx");
                AttachVisual(VisualEffect{
                    "Spells\\Fire_Precast_Hand.mdx", kAttachBase, 0.66f,
                    0.0f, 0.0f, 0, 1.45f, 0.15f, 0.82f,
                    nullptr, 2, nullptr, 0, UINT32_MAX, 1.0f, 1, false, true});
                g_warlockImpEmitter.preparing = false;
                g_warlockImpEmitter.nextMs += kPrepareMs;
                continue;
            }

            RemoveActiveVisualsByPath("Spells\\Fire_Precast_Hand.mdx");
            RestartActiveVisualAnimation("Creature\\Imp\\Imp.m2", 53, 0.18f, 1);
            AttachVisual(VisualEffect{
                "Spells\\FireBolt_Missile_Low.m2", kAttachBase, 0.72f,
                -8.30f, 4.55f, 1050, 1.45f, 0.15f, 0.90f});
            ++g_warlockImpEmitter.emitted;
            if (g_warlockImpEmitter.emitted >= kEmissionCount)
            {
                g_warlockImpEmitter.active = false;
                WLOG_INFO("completed WotLK Imp Firebolt emissions=%u",
                          g_warlockImpEmitter.emitted);
            }
            else
            {
                g_warlockImpEmitter.preparing = true;
                g_warlockImpEmitter.nextMs += kBetweenCastsMs;
            }
        }
    }

    void StartWotlkWarlockRainEmitter() noexcept
    {
        g_warlockRainEmitter.active = true;
        g_warlockRainEmitter.nextMs = GetTickCount();
        g_warlockRainEmitter.emitted = 0;
        WLOG_INFO("started WotLK Rain of Fire falling-missile stream");
    }

    void UpdateWotlkWarlockRainEmitter() noexcept
    {
        if (!g_warlockRainEmitter.active) return;
        static constexpr float kOffsets[][2] = {
            {-1.35f, -0.35f}, { 0.10f, 0.45f}, { 1.25f, -0.15f},
            {-0.65f,  0.15f}, { 0.75f, 0.35f}, {-1.10f,  0.50f},
        };
        constexpr uint32_t kEmissionIntervalMs = 260;
        constexpr uint32_t kEmissionCount = 12;
        const uint32_t now = GetTickCount();
        while (g_warlockRainEmitter.active &&
               static_cast<int32_t>(now - g_warlockRainEmitter.nextMs) >= 0)
        {
            const auto& offset = kOffsets[g_warlockRainEmitter.emitted % _countof(kOffsets)];
            AttachVisual(VisualEffect{
                "Spells\\RainOfFire_Missile.m2", kAttachBase, 0.88f,
                0.0f, -5.05f, 780, offset[0], offset[1], 5.25f});
            ++g_warlockRainEmitter.emitted;
            g_warlockRainEmitter.nextMs += kEmissionIntervalMs;
            if (g_warlockRainEmitter.emitted >= kEmissionCount)
            {
                g_warlockRainEmitter.active = false;
                WLOG_INFO("completed WotLK Rain of Fire emissions=%u",
                          g_warlockRainEmitter.emitted);
            }
        }
    }

    bool PlayWotlkMageVisuals(uint32_t stage) noexcept
    {
        if (stage < 1 || stage > 9)
            return false;
        const auto playFixed = [](const auto& effects) noexcept {
            bool fixedOk = true;
            for (const VisualEffect& effect : effects)
                fixedOk &= AttachVisual(effect);
            return fixedOk;
        };

        if (stage == 1)
        {
            static constexpr VisualEffect kPyroblastPrecast[] = {
                {"Spells\\Fire_Precast_Hand.mdx", kAttachLeftHand, 1.0f},
                {"Spells\\Fire_Precast_Hand.mdx", kAttachRightHand, 1.0f},
            };
            return playFixed(kPyroblastPrecast);
        }
        if (stage == 2)
        {
            static constexpr VisualEffect kPyroblastRelease[] = {
                {"Spells\\Fire_Cast_Hand.mdx", kAttachLeftHand, 1.0f},
                {"Spells\\Fire_Cast_Hand.mdx", kAttachRightHand, 1.0f},
                // SpellVisual 2253's WotLK missile. Anchor it to the model base so the casting
                // animation cannot bend its trajectory, then move it toward the upper-left.
                {"Spells\\PyroBlast_Missile.mdx", kAttachBase, 1.0f,
                 -7.2f, 4.3f, 900, 0.35f, 0.0f, 1.15f},
            };
            return playFixed(kPyroblastRelease);
        }

        if (stage == 3)
        {
            static constexpr VisualEffect kFrostboltPrecast[] = {
                {"Spells\\Ice_Precast_Low_Hand.mdx", kAttachLeftHand, 1.0f},
                {"Spells\\Ice_Precast_Low_Hand.mdx", kAttachRightHand, 1.0f},
            };
            return playFixed(kFrostboltPrecast);
        }
        if (stage == 4)
        {
            static constexpr VisualEffect kFrostboltRelease[] = {
                {"Spells\\Ice_Precast_Low_Hand.mdx", kAttachLeftHand, 1.0f},
                {"Spells\\Ice_Precast_Low_Hand.mdx", kAttachRightHand, 1.0f},
                {"Spells\\Frostbolt.mdx", kAttachBase, 1.20f,
                 -8.5f, 4.5f, 900, 0.35f, 0.0f, 1.15f},
            };
            return playFixed(kFrostboltRelease);
        }

        // The third Mage beat is a native summon. The Elemental remains beside the character
        // through Blizzard and is cleared when Arcane Explosion begins.
        if (stage == 5)
        {
            static constexpr VisualEffect kWaterElementalPrecast[] = {
                // Frostbolt visual 13 impact kit 4991 at the missile's top-left destination.
                {"Spells\\Ice_ImpactDD_Med_Chest.mdx", kAttachBase, 1.0f,
                 0.0f, 0.0f, 0, -8.15f, 0.0f, 5.65f},
                {"Spells\\Ice_Precast_Uber_Head.mdx", kAttachHead, 1.0f},
                {"Spells\\Ice_Precast_Uber_Base.mdx", kAttachBase, 1.0f},
                {"Spells\\Ice_Precast_Uber_Hand.mdx", kAttachLeftHand, 1.0f},
                {"Spells\\Ice_Precast_Uber_Hand.mdx", kAttachRightHand, 1.0f},
            };
            return playFixed(kWaterElementalPrecast);
        }
        if (stage == 6)
        {
            static constexpr VisualEffect kWaterElementalSummon[] = {
                {"Spells\\Ice_Precast_Med_Hand.mdx", kAttachLeftHand, 1.0f},
                {"Spells\\Ice_Precast_Med_Hand.mdx", kAttachRightHand, 1.0f},
                {"Spells\\WaterElemental_Impact_Base.mdx", kAttachBase, 1.0f,
                 0.0f, 0.0f, 0, 1.85f, 0.10f, 0.0f},
                {"Creature\\WaterElemental2\\WaterElemental2.m2", kAttachBase, 0.62f,
                 0.0f, 0.0f, 0, 1.85f, 0.10f, 0.0f,
                 "Creature\\WaterElemental2\\WaterElemental2_Blue.blp", 11,
                 "Creature\\WaterElemental2\\WaterElemental2_Asset_7.blp", 12},
            };
            return playFixed(kWaterElementalSummon);
        }
        if (stage == 7)
        {
            static constexpr VisualEffect kWotlkBlizzardPrecast[] = {
                {"Spells\\Ice_Precast_Low_Hand.mdx", kAttachLeftHand, 1.0f},
                {"Spells\\Ice_Precast_Med_Hand.mdx", kAttachRightHand, 1.0f},
                {"Creature\\WaterElemental2\\WaterElemental2.m2", kAttachBase, 0.62f,
                 0.0f, 0.0f, 0, 1.85f, 0.10f, 0.0f,
                 "Creature\\WaterElemental2\\WaterElemental2_Blue.blp", 11,
                 "Creature\\WaterElemental2\\WaterElemental2_Asset_7.blp", 12},
            };
            return playFixed(kWotlkBlizzardPrecast);
        }
        if (stage == 8)
        {
            // Spell 10's WotLK channel kit uses low ice effects on both hands. Its impact-area kit
            // adds Blizzard_Spawn once; persistent kit 9350 then runs character procedure 9 at five
            // Blizzard_Impact_Base emissions per second. Mirror that controller below.
            static constexpr VisualEffect kWotlkBlizzard[] = {
                {"Spells\\Ice_Precast_Low_Hand.mdx", kAttachLeftHand, 1.0f},
                {"Spells\\Ice_Precast_Low_Hand.mdx", kAttachRightHand, 1.0f},
                {"Spells\\Blizzard_Spawn.mdx", kAttachBase, 1.0f},
                {"Creature\\WaterElemental2\\WaterElemental2.m2", kAttachBase, 0.62f,
                 0.0f, 0.0f, 0, 1.85f, 0.10f, 0.0f,
                 "Creature\\WaterElemental2\\WaterElemental2_Blue.blp", 11,
                 "Creature\\WaterElemental2\\WaterElemental2_Asset_7.blp", 12},
            };
            const bool fixedOk = playFixed(kWotlkBlizzard);
            StartWotlkBlizzardEmitter();
            return fixedOk;
        }

        static constexpr VisualEffect kArcaneExplosion[] = {
            {"Spells\\ArcaneExplosion_Base.mdx", kAttachBase, 1.0f},
        };
        return playFixed(kArcaneExplosion);
    }

    bool PlayWotlkWarriorVisuals(uint32_t stage) noexcept
    {
        if (stage < 1 || stage > 4)
            return false;

        static constexpr VisualEffect kBattleShout[] = {
            {"Spells\\BattleShout_Cast_Base.mdx", kAttachBase, 1.0f},
        };
        static constexpr VisualEffect kThunderClap[] = {
            {"Spells\\ThunderClap_Cast_Base.mdx", kAttachBase, 1.0f},
        };
        static constexpr VisualEffect kSlam[] = {
            {"Spells\\Cleave_Cast_Base.mdx", kAttachBase, 1.05f},
            {"Spells\\DustCloud_Land.mdx", kAttachBase, 1.05f,
             0.0f, 0.0f, 0, 0.0f, 0.35f, 0.0f},
            {"Spells\\Slam_Impact_Chest.mdx", kAttachChest, 1.10f},
            {"Spells\\Cleave_Impact_Chest.mdx", kAttachChest, 0.90f},
        };
        static constexpr VisualEffect kBladestorm[] = {
            {"Spells\\Warrior_BladeStorm.m2", kAttachBase, 1.0f},
        };

        const VisualEffect* effects = nullptr;
        size_t count = 0;
        if (stage == 1) { effects = kBattleShout; count = _countof(kBattleShout); }
        else if (stage == 2) { effects = kThunderClap; count = _countof(kThunderClap); }
        else if (stage == 3) { effects = kSlam; count = _countof(kSlam); }
        else { effects = kBladestorm; count = _countof(kBladestorm); }

        bool ok = true;
        for (size_t i = 0; i < count; ++i) ok &= AttachVisual(effects[i]);
        return ok;
    }

    bool PlayWotlkHunterVisuals(uint32_t stage) noexcept
    {
        if (stage < 1 || stage > 7)
            return false;

        // CreatureDisplayInfo 52598: Draenor HD wolf with its Frost display variation in monster
        // skin slot 1. Recreate the pet at every stage because transitions clear the previous
        // owned visual set before attaching the next one.
        static constexpr VisualEffect kWolf = {
            "Creature\\WolfDraenor\\WolfDraenor.m2", kAttachBase, 0.62f,
            0.0f, 0.0f, 0, 1.65f, 0.05f, 0.0f,
            "Creature\\WolfDraenor\\WolfDraenorFrost.blp", 11,
        };
        static constexpr VisualEffect kTrapClosed = {
            "World\\Goober\\G_ImmolationTrap.mdx", kAttachBase, 0.72f,
            0.0f, 0.0f, 0, -0.70f, 0.70f, 0.0f,
            nullptr, 2, nullptr, 0, 147, 0.0f, 1,
        };
        static constexpr VisualEffect kTrapOpening = {
            "World\\Goober\\G_ImmolationTrap.mdx", kAttachBase, 0.72f,
            0.0f, 0.0f, 0, -0.70f, 0.70f, 0.0f,
            nullptr, 2, nullptr, 0, 148, 0.0f, 0,
        };
        static constexpr VisualEffect kTrapOpened = {
            "World\\Goober\\G_ImmolationTrap.mdx", kAttachBase, 0.72f,
            0.0f, 0.0f, 0, -0.70f, 0.70f, 0.0f,
            nullptr, 2, nullptr, 0, 0, 0.0f, 1,
        };

        bool ok = AttachVisual(kWolf);
        if (stage == 2)
            ok &= AttachVisual(kTrapClosed);
        else if (stage == 3)
            ok &= AttachVisual(kTrapOpening);
        else if (stage >= 4)
            ok &= AttachVisual(kTrapOpened);
        if (stage == 5)
        {
            // WotLK Aimed Shot has no dedicated travelling model; use its stock arrow missile.
            ok &= AttachVisual(VisualEffect{
                "Spells\\MultiShot_Missile.mdx", kAttachLeftHand, 1.25f,
                -7.2f, 3.8f, 800, 0.0f, 0.0f, 0.0f,
                nullptr, 2, nullptr, 0, UINT32_MAX, 1.0f, 1, true});
        }
        else if (stage == 6)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\Explosive_Shot_Missile.m2", kAttachLeftHand, 1.05f,
                -8.5f, 4.6f, 850, 0.0f, 0.0f, 0.0f,
                nullptr, 2, nullptr, 0, UINT32_MAX, 1.0f, 1, true});
        }
        else if (stage == 7)
        {
            // Spell 1543 / visual 10387 / effect 2262 from the 3.3.5a DBC chain.
            ok &= AttachVisual(VisualEffect{
                "Spells\\missile_flare.mdx", kAttachBase, 1.0f,
                0.0f, 4.0f, 1800, 0.0f, 0.0f, 0.80f});
        }
        return ok;
    }

    bool PlayWotlkRogueVisuals(uint32_t stage) noexcept
    {
        if (stage < 1 || stage > 6)
            return false;

        if (stage == 1)
        {
            return AttachVisual(VisualEffect{
                "Spells\\BackStab_Cast_Base.mdx", kAttachBase, 1.0f});
        }
        if (stage == 2)
        {
            bool ok = AttachVisual(VisualEffect{
                "Spells\\Eviscerate_Cast_Hands.mdx", kAttachLeftHand, 1.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Eviscerate_Cast_Hands.mdx", kAttachRightHand, 1.0f});
            return ok;
        }
        if (stage == 3)
        {
            return AttachVisual(VisualEffect{
                "Spells\\Vanish_Cast_Base.mdx", kAttachBase, 1.05f});
        }
        if (stage == 4)
        {
            // The character animation itself holds StealthStand; Vanish has cleared.
            return true;
        }

        if (stage == 5)
        {
            bool ok = AttachVisual(VisualEffect{
                "Spells\\ShadowSteps_FX.m2", kAttachBase, 1.05f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\BackStab_Cast_Base.mdx", kAttachBase, 1.0f});
            // Place the impact just in front of the Rogue as the implied invisible target.
            ok &= AttachVisual(VisualEffect{
                "Spells\\BackStab_Impact_Chest.mdx", kAttachBase, 1.0f,
                0.0f, 0.0f, 0, -0.45f, 0.75f, 1.05f});
            return ok;
        }

        bool ok = AttachVisual(VisualEffect{
            "Spells\\FanOfKnives_Precast.m2", kAttachBase, 1.0f});
        ok &= AttachVisual(VisualEffect{
            "Spells\\FanOfKnives.m2", kAttachBase, 1.08f});
        ok &= AttachVisual(VisualEffect{
            "Spells\\FanOfKnives_Missile.m2", kAttachBase, 1.0f});
        return ok;
    }

    bool PlayWotlkPaladinVisuals(uint32_t stage) noexcept
    {
        if (stage < 1 || stage > 6)
            return false;

        static constexpr VisualEffect kConsecration = {
            "Spells\\Consecration_Impact_Base.mdx", kAttachBase, 1.08f,
            0.0f, 0.0f, 0, 0.0f, 0.55f, 0.0f,
        };
        static constexpr VisualEffect kWings[] = {
            {"Spells\\AvengingWrath_Impact_Base.m2", kAttachBase, 1.0f},
            {"Spells\\AvengingWrath_State_Chest.m2", kAttachChest, 1.0f},
        };

        bool ok = true;
        if (stage <= 3)
            ok &= AttachVisual(kConsecration);
        if (stage >= 2 && stage <= 5)
        {
            for (const VisualEffect& wing : kWings)
                ok &= AttachVisual(wing);
        }

        if (stage == 3)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\SealOfCrusader_Impact.m2", kAttachRightWeapon, 1.12f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Holy_ImpactDD_Med_Chest.mdx", kAttachChest, 0.90f});
        }
        else if (stage == 4)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\Holy_Precast_High_Base.mdx", kAttachBase, 1.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Holy_Precast_High_Hand.mdx", kAttachLeftHand, 1.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Holy_Precast_High_Hand.mdx", kAttachRightHand, 1.0f});
        }
        else if (stage == 5)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\HolyLight_Impact_Head.mdx", kAttachHead, 1.05f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Holy_ImpactDD_High_Base.mdx", kAttachBase, 0.90f});
        }
        else if (stage == 6)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\BlessingOfKings_Base.m2", kAttachBase, 1.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\HolyDivineShield_State_Base.mdx", kAttachBase, 1.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\DivineShield_Low_Chest.mdx", kAttachChest, 1.0f});
        }
        return ok;
    }

    bool PlayWotlkPriestVisuals(uint32_t stage) noexcept
    {
        if (stage < 1 || stage > 7)
            return false;

        if (stage <= 3)
        {
            if (stage == 1)
            {
                PreloadVisual("Spells\\HolyDivineShield_State_Base.mdx");
                // The stock Shadow Bolt has a 667 ms Stand sequence. Warm it before
                // the finisher and explicitly loop Stand for its 1050 ms flight.
                PreloadVisual("Spells\\ShadowBolt_Missile.m2");
            }
            bool ok = AttachVisual(VisualEffect{
                "Spells\\CFX_Priest_PenanceDamage_CastHand.mdx",
                kAttachLeftHand, 1.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\CFX_Priest_PenanceDamage_CastHand.mdx",
                kAttachRightHand, 1.0f});
            // Capture the real hand attachment on the first rendered frame, then continue along
            // the accepted fixed top-left trajectory. Alternating hands keeps all three bolts tied
            // to the cast pose without making their destination inherit later hand motion.
            // Use the exact stable base-relative trajectory accepted for Mage Pyroblast.
            // The hand effects preserve the cast origin visually without letting race-specific
            // hand bones skew the missile's path or destination.
            ok &= AttachVisual(VisualEffect{
                "Spells\\CFX_Priest_Penance_Missile.mdx", kAttachBase, 1.0f,
                -7.2f, 4.3f, 900, 0.35f, 0.0f, 1.15f});
            return ok;
        }

        // Spell 17 -> SpellVisual 784 -> state kit 847 -> effect 674. Play sequence zero
        // once, scrub it to the fully open pose while the spell is active, then release
        // it shortly before cleanup so its native closing segment runs exactly once.
        static constexpr VisualEffect kShield = {
            "Spells\\HolyDivineShield_State_Base.mdx", kAttachBase, 1.08f,
            0.0f, 0.0f, 0, 0.0f, 0.0f, 0.0f,
            nullptr, 2, nullptr, 0, 0, 1.0f, 0, false, true,
            nullptr, 0, 450, 450, 5850,
        };

        if (stage == 4)
        {
            bool ok = AttachVisual(kShield);
            ok &= AttachVisual(VisualEffect{
                "Spells\\Holy_Precast_Low_Hand.mdx", kAttachLeftHand, 1.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Holy_Precast_Low_Hand.mdx", kAttachRightHand, 1.0f});
            return ok;
        }

        static constexpr VisualEffect kShadowform = {
            "Spells\\CFX_Priest_Shadowform_StateBase.mdx", kAttachBase, 1.0f,
        };
        static constexpr VisualEffect kPsyfiend = {
            "Spells\\Priest_Psyfiend.m2", kAttachBase, 0.78f,
            0.0f, 0.0f, 0, 1.45f, 0.15f, 0.0f,
        };

        // Normally stage four owns this persistent visual. If its first asynchronous load was not
        // ready, retry on the next phase without duplicating an already active bubble.
        bool ok = HasActiveVisual(kShield.path) ? true : AttachVisual(kShield);
        if (HasActiveVisual(kShield.path))
            EnsureActiveVisualAnimation(kShield.path, 0, 1.0f, 0);
        ok &= AttachVisual(kShadowform);
        if (stage == 5)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\Shadow_Form_Precast.m2", kAttachBase, 1.0f});
            return ok;
        }

        ok &= AttachVisual(kPsyfiend);
        if (stage == 6)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\Priest_Psyfiend_Impact.m2", kAttachBase, 0.90f,
                0.0f, 0.0f, 0, 1.45f, 0.15f, 0.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Priest_Psyfiend_State_Base.m2", kAttachBase, 0.90f,
                0.0f, 0.0f, 0, 1.45f, 0.15f, 0.0f});
            return ok;
        }

        ok &= AttachVisual(VisualEffect{
            "Spells\\CFX_Priest_MindFlay_ChannelHand.mdx",
            kAttachLeftHand, 1.0f});
        ok &= AttachVisual(VisualEffect{
            "Spells\\CFX_Priest_MindFlay_ChannelHand.mdx",
            kAttachRightHand, 1.0f});
        ok &= AttachVisual(VisualEffect{
            "Spells\\CFX_Priest_MindFlay_ChannelHead.mdx",
            kAttachHead, 1.0f});
        // Spell 15407's WotLK target state is ManaFunnel_Impact_Chest. Keep it at the
        // same accepted top-left target as the other class missiles. The actual beam is
        // an engine ribbon between two units; do not substitute unrelated missile geometry.
        ok &= AttachVisual(VisualEffect{
            "Spells\\ManaFunnel_Impact_Chest.mdx", kAttachBase, 1.10f,
            0.0f, 0.0f, 0, -6.85f, 0.0f, 5.45f});
        StartWotlkPriestChannelEmitter();
        return ok;
    }

    bool PlayWotlkDeathKnightVisuals(uint32_t stage) noexcept
    {
        if (stage < 1 || stage > 6)
            return false;

        // The native WotLK Northrend ghoul uses CreatureDisplayInfo's monster-skin slot.
        // Keep it alive after Raise Dead so the companion does not blink between attacks.
        static constexpr VisualEffect kGhoul = {
            "Creature\\NorthrendGhoul\\NorthrendGhoul.m2", kAttachBase, 0.66f,
            0.0f, 0.0f, 0, 1.45f, 0.12f, 0.0f,
            "Creature\\NorthrendGhoul\\NorthrendGhoul01.blp", 11,
            nullptr, 0, 0, 1.0f, 1, false, true,
        };
        static constexpr VisualEffect kDeathAndDecayBase = {
            "Spells\\DeathAndDecay_Area_Base.m2", kAttachBase, 1.08f,
            0.0f, 0.0f, 0, -0.15f, 0.45f, 0.16f,
            nullptr, 2, nullptr, 0, UINT32_MAX, 1.0f, 1, false, true,
        };
        static constexpr VisualEffect kDeathAndDecayRunes = {
            "Spells\\DeathAndDecay_Area_Runes.m2", kAttachBase, 1.08f,
            0.0f, 0.0f, 0, -0.15f, 0.45f, 0.16f,
            nullptr, 2, nullptr, 0, UINT32_MAX, 1.0f, 1, false, true,
        };

        bool ok = HasActiveVisual(kGhoul.path) ? true : AttachVisual(kGhoul);
        if (stage == 1)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\SummonGhouls.m2", kAttachBase, 0.92f,
                0.0f, 0.0f, 0, 1.45f, 0.12f, 0.0f});
            return ok;
        }

        ok &= HasActiveVisual(kDeathAndDecayBase.path)
            ? true : AttachVisual(kDeathAndDecayBase);
        ok &= HasActiveVisual(kDeathAndDecayRunes.path)
            ? true : AttachVisual(kDeathAndDecayRunes);

        if (stage == 2)
            return ok;
        if (stage == 3)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\DeathKnight_FrostStrike.m2",
                kAttachRightWeapon, 1.08f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\DeathKnight_FrostStrike_Impact.m2", kAttachBase, 1.0f,
                0.0f, 0.0f, 0, -0.50f, 0.75f, 1.00f});
            return ok;
        }
        if (stage == 4)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\DeathKnight_BloodStrike.m2",
                kAttachRightWeapon, 1.08f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\DeathKnight_BloodStrike_Impact.m2", kAttachBase, 1.0f,
                0.0f, 0.0f, 0, -0.50f, 0.75f, 1.00f});
            return ok;
        }
        if (stage == 5)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\DeathKnight_DeathCoil_Missile.m2",
                kAttachBase, 1.05f,
                -7.2f, 4.3f, 950, 0.35f, 0.0f, 1.15f});
            return ok;
        }

        ok &= AttachVisual(VisualEffect{
            "Spells\\DeathKnight_BloodBoil.m2", kAttachChest, 1.08f});
        ok &= AttachVisual(VisualEffect{
            "Spells\\BloodLust_State_Hand.mdx", kAttachLeftHand, 0.92f});
        ok &= AttachVisual(VisualEffect{
            "Spells\\BloodLust_State_Hand.mdx", kAttachRightHand, 0.92f});
        return ok;
    }

    bool PlayWotlkShamanVisuals(uint32_t stage) noexcept
    {
        if (stage < 1 || stage > 9)
            return false;

        static constexpr VisualEffect kGhostWolf = {
            "Creature\\Wolf\\Wolf_ghost.m2", kAttachBase, 0.82f,
            0.0f, 0.0f, 0, 0.0f, 0.05f, 0.0f,
            "Creature\\Wolf\\WolfSkin_Ghost.blp", 11,
            nullptr, 0, 0, 1.0f, 1,
        };
        static constexpr VisualEffect kTotems[] = {
            {"Creature\\Spells\\EarthElementalTotem.m2", kAttachBase, 0.58f,
             0.0f, 0.0f, 0, -1.18f, -0.55f, 0.02f,
             nullptr, 2, nullptr, 0, UINT32_MAX, 1.0f, 1, false, true},
            {"Creature\\Spells\\FireElementalTotem.m2", kAttachBase, 0.58f,
             0.0f, 0.0f, 0, 1.18f, -0.55f, 0.02f,
             nullptr, 2, nullptr, 0, UINT32_MAX, 1.0f, 1, false, true},
            {"Creature\\Spells\\WaterElementalTotem.m2", kAttachBase, 0.58f,
             0.0f, 0.0f, 0, -1.18f, 0.78f, 0.02f,
             nullptr, 2, nullptr, 0, UINT32_MAX, 1.0f, 1, false, true},
            {"Creature\\Spells\\AirElementalTotem.m2", kAttachBase, 0.58f,
             0.0f, 0.0f, 0, 1.18f, 0.78f, 0.02f,
             nullptr, 2, nullptr, 0, UINT32_MAX, 1.0f, 1, false, true},
        };

        if (stage == 1)
        {
            bool ok = AttachVisual(VisualEffect{
                "Spells\\Shaman_AstralShift.m2", kAttachBase, 1.0f});
            VisualEffect ghostWolf = kGhostWolf;
            ghostWolf.forceVisible = true;
            ok &= AttachVisual(ghostWolf);
            return ok;
        }
        if (stage == 2)
            return AttachVisual(VisualEffect{
                "Spells\\Shaman_AstralShift.m2", kAttachBase, 1.0f});

        bool ok = true;
        for (const VisualEffect& totem : kTotems)
            ok &= HasActiveVisual(totem.path) ? true : AttachVisual(totem);
        if (stage == 3)
            return ok;
        if (stage == 4)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\New_Windfury.m2", kAttachBase, 1.05f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Enchantments\\Shaman_Wind.m2",
                kAttachRightWeapon, 1.0f});
            return ok;
        }
        if (stage == 5)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\Enchantments\\Shaman_Fire.m2",
                kAttachRightWeapon, 1.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\FlameShock_Impact_Chest.m2", kAttachBase, 0.95f,
                0.0f, 0.0f, 0, -0.48f, 0.72f, 1.0f});
            return ok;
        }
        if (stage == 6)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\Lightning_PreCast_Low_Hand.mdx", kAttachLeftHand, 1.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Lightning_PreCast_Low_Hand.mdx", kAttachRightHand, 1.0f});
            return ok;
        }
        if (stage == 7)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\LightningBolt_Missile.m2", kAttachBase, 1.15f,
                -7.2f, 4.3f, 900, 0.35f, 0.0f, 1.15f});
            return ok;
        }
        if (stage == 8)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\Fire_Precast_Hand.mdx", kAttachLeftHand, 1.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Fire_Precast_Hand.mdx", kAttachRightHand, 1.0f});
            return ok;
        }

        ok &= AttachVisual(VisualEffect{
            "Spells\\Fire_Cast_Hand.mdx", kAttachLeftHand, 1.0f});
        ok &= AttachVisual(VisualEffect{
            "Spells\\Fire_Cast_Hand.mdx", kAttachRightHand, 1.0f});
        ok &= AttachVisual(VisualEffect{
            "Spells\\Shaman_LavaBurst_Missile.m2", kAttachBase, 1.05f,
            -7.8f, 4.5f, 950, 0.35f, 0.0f, 1.15f});
        return ok;
    }

    bool PlayWotlkWarlockVisuals(uint32_t stage) noexcept
    {
        if (stage < 1 || stage > 6)
            return false;

        // The Imp is a stock 3.3.5 creature. Keep its monster-skin binding and placement stable
        // while its own cast animation and Firebolt stream run independently of the player.
        static constexpr VisualEffect kImp = {
            "Creature\\Imp\\Imp.m2", kAttachBase, 0.68f,
            0.0f, 0.0f, 0, 1.45f, 0.15f, 0.0f,
            "Creature\\Imp\\ImpSkinRed.blp", 11,
            nullptr, 0, 53, 0.18f, 1, false, true,
        };

        bool ok = HasActiveVisual(kImp.path) ? true : AttachVisual(kImp);
        if (stage == 1)
        {
            PreloadVisual("Spells\\Fel_Fire_Precast_Hand.m2");
            PreloadVisual("Spells\\ShadowBolt_Missile.m2");
            PreloadVisual("Spells\\cfx_warlock_chaosbolt_missile.m2");
            ok &= AttachVisual(VisualEffect{
                "Spells\\Shadow_Form_Precast.m2", kAttachBase, 0.88f,
                0.0f, 0.0f, 0, 1.45f, 0.15f, 0.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Shadow_Precast_Low_Hand.mdx", kAttachLeftHand, 1.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Shadow_Precast_Low_Hand.mdx", kAttachRightHand, 1.0f});
            StartWotlkWarlockImpEmitter();
            return ok;
        }
        if (stage == 2)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\Shadow_Precast_Low_Hand.mdx", kAttachLeftHand, 0.95f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Shadow_Precast_Low_Hand.mdx", kAttachRightHand, 0.95f});
            return ok;
        }
        if (stage == 3)
        {
            // Same proven stock missile/Stand loop as Priest, with a slower flight.
            ok &= AttachVisual(VisualEffect{
                "Spells\\ShadowBolt_Missile.m2", kAttachBase, 1.0f,
                -7.2f, 4.3f, 1400, 0.35f, 0.0f, 1.15f,
                nullptr, 2, nullptr, 0, 0, 1.0f, 1});
            return ok;
        }
        if (stage == 4)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\Fel_Fire_Precast_Hand.m2",
                kAttachLeftHand, 1.0f});
            ok &= AttachVisual(VisualEffect{
                "Spells\\Fel_Fire_Precast_Hand.m2",
                kAttachRightHand, 1.0f});
            return ok;
        }
        if (stage == 5)
        {
            ok &= AttachVisual(VisualEffect{
                "Spells\\cfx_warlock_chaosbolt_missile.m2", kAttachBase, 1.0f,
                -7.2f, 4.3f, 1400, 0.35f, 0.0f, 1.15f,
                nullptr, 2, nullptr, 0, 144, 1.0f, 1}); // Authored InFlight, not Stand/Impact.
            return ok;
        }

        // Rain of Fire ends the Imp volley, then drops the real WotLK missiles around the actor.
        g_warlockImpEmitter.active = false;
        RemoveActiveVisualsByPath("Spells\\Fire_Precast_Hand.mdx");
        ok &= AttachVisual(VisualEffect{
            "Spells\\RainOfFire_Impact_Base.m2", kAttachBase, 1.12f,
            0.0f, 0.0f, 0, 0.0f, 0.30f, 0.08f});
        ok &= AttachVisual(VisualEffect{
            "Spells\\Fire_Precast_Hand.mdx", kAttachLeftHand, 1.0f});
        ok &= AttachVisual(VisualEffect{
            "Spells\\Fire_Precast_Hand.mdx", kAttachRightHand, 1.0f});
        ok &= AttachVisual(VisualEffect{
            "Spells\\Fire_Cast_Hand.mdx", kAttachLeftHand, 0.92f});
        ok &= AttachVisual(VisualEffect{
            "Spells\\Fire_Cast_Hand.mdx", kAttachRightHand, 0.92f});
        StartWotlkWarlockRainEmitter();
        return ok;
    }

    void UpdateDruidEffectLifecycle() noexcept
    {
        const uint32_t now = GetTickCount();
        if (g_druidMorphStart && now - g_druidMorphStart >= 2800)
        {
            RemoveActiveVisualsByPath("Spells\\DruidMorph_Impact_Base.m2");
            g_druidMorphStart = 0;
        }
        if (g_druidTranquilityStart && now - g_druidTranquilityStart >= 1867)
        {
            RestartActiveVisualAnimation("Spells\\Tranquility_Area.m2", 158, 1.0f, 1);
            RestartActiveVisualAnimation("Spells\\Tranquility_Impact_Base.m2", 158, 1.0f, 1);
            g_druidTranquilityStart = 0;
        }
    }

    bool PlayWotlkDruidVisuals(uint32_t stage) noexcept
    {
        if (stage < 1 || stage > kMaxDruidStage) return false;
        if (stage == 16)
        {
            // Begin the authored cloud before swapping the form 450 ms later.
            RemoveActiveVisualsByPath("Spells\\DruidMorph_Impact_Base.m2");
            VisualEffect morph{"Spells\\DruidMorph_Impact_Base.m2", kAttachBase, 1.1f,
                0.0f, 0.0f, 0, 0.0f, 0.05f, 0.05f};
            morph.forceVisible = true;
            morph.persistAcrossPhases = true;
            morph.modelAnimation = 0;
            morph.modelAnimationLoop = 0;
            g_druidMorphStart = GetTickCount();
            if (g_druidFormPath)
            {
                g_druidFormAnimation = 0;
                RestartActiveVisualAnimation(g_druidFormPath, 0, 1.0f, 1);
            }
            return AttachVisual(morph);
        }
        if (stage == 17)
        {
            // End the eight-second channel using the authored Decay sequences.
            g_druidTranquilityStart = 0;
            RestartActiveVisualAnimation("Spells\\Tranquility_Area.m2", 159, 1.0f, 0);
            RestartActiveVisualAnimation("Spells\\Tranquility_Impact_Base.m2", 159, 1.0f, 0);
            g_druidFormAnimation = 0;
            return RestartActiveVisualAnimation(g_druidFormPath, 0, 1.0f, 1);
        }
        static const char* const forms[] = {
            "Creature\\DruidBear\\DruidBear.m2", "Creature\\DruidCat\\DruidCat.m2",
            "Creature\\DruidOwlbear\\DruidOwlBear.m2", "Creature\\Ent\\Ent.m2"
        };
        static const char* const skins[] = {
            "Creature\\DruidBear\\DruidBearSkin.blp", "Creature\\DruidCat\\DruidCatSkinBlack.blp",
            "Creature\\DruidOwlbear\\DruidOwlBearNESkin1.blp", "Creature\\Ent\\EntSkinBrown.blp"
        };
        static constexpr float scales[] = {0.85f, 0.85f, 0.75f, 0.70f};
        const uint32_t form = stage <= 3 ? 0 : stage <= 6 ? 1 : stage <= 10 ? 2 : 3;
        for (uint32_t i = 0; i < 4; ++i)
            if (i != form) RemoveActiveVisualsByPath(forms[i]);
        if (!HasActiveVisual(forms[form]))
        {
            VisualEffect body{forms[form], kAttachBase, scales[form],
                0.0f, 0.0f, 0, 0.0f, 0.05f, 0.0f, skins[form], 11};
            if (form == 2)
            {
                body.secondaryTexturePath = "Creature\\DruidOwlbear\\DruidOwlBearNESkin2.blp";
                body.secondaryTextureType = 12;
            }
            body.modelAnimation = 0;
            body.persistAcrossPhases = true;
            body.forceVisible = true;
            if (!AttachVisual(body)) return false;
        }
        // Animate the visible form itself, never just the hidden player actor.
        uint32_t animation = 0;
        if (stage == 2) animation = 176;       // DruidBearSwipe
        else if (stage == 3) animation = 55;   // BattleRoar
        else if (stage == 5) animation = 16;   // Cat swipe
        else if (stage == 6) animation = 174;  // DruidCatClaw / Mangle
        else if (stage == 8 || stage == 12 || stage == 14) animation = 52;
        else if (stage == 9) animation = 53;
        else if (stage == 10) animation = 54;
        else if (stage == 13) animation = 54;
        else if (stage == 15) animation = 124;
        g_druidFormPath = forms[form];
        g_druidFormAnimation = animation;
        RestartActiveVisualAnimation(forms[form], animation, 1.0f,
            animation == 0 || animation == 52 || animation == 124 ? 1 : 0);
        const auto effect = [](const char* path, float scale, float z) noexcept {
            VisualEffect visual{path, kAttachBase, scale,
                0.0f, 0.0f, 0, 0.0f, 0.05f, z};
            visual.forceVisible = true;
            return AttachVisual(visual);
        };
        if (stage == 1 || stage == 4 || stage == 7 || stage == 11)
            return true; // The persistent morph cloud already covers the swap.
        if (stage == 2 || stage == 5)
            return effect("Spells\\SwipeCaster.m2", 1.0f, 0.60f);
        if (stage == 3)
            return effect("Spells\\DemoralizingShout_Cast_Base.m2", 1.0f, 0.10f);
        if (stage == 6)
            return effect("Spells\\Mangle_Impact.m2", 1.0f, 0.65f);
        if (stage == 8)
            return effect("Spells\\Wrath_PreCast_Hand.m2", 1.0f, 1.20f);
        if (stage == 9)
        {
            VisualEffect missile{"Spells\\Wrath_Missile.m2", kAttachBase, 1.0f,
                -7.2f, 4.3f, 1400, 0.35f, 0.0f, 1.15f};
            missile.forceVisible = true;
            return AttachVisual(missile);
        }
        if (stage == 10)
        {
            VisualEffect moonfire{"Spells\\Moonfire_Impact_Base.m2", kAttachBase, 1.0f,
                0.0f, 0.0f, 0, -2.4f, 0.15f, 0.05f};
            moonfire.forceVisible = true;
            return AttachVisual(moonfire);
        }
        if (stage == 12 || stage == 14)
            return effect("Spells\\Nature_PreCast_Low_Hand.m2", 1.0f, 1.0f);
        if (stage == 13)
            return effect("Spells\\Rejuvenation_Impact_Base.m2", 1.15f, 0.10f);
        bool ok = true;
        for (const char* path : {"Spells\\Tranquility_Area.m2", "Spells\\Tranquility_Impact_Base.m2"})
        {
            VisualEffect tranquility{path, kAttachBase, 1.1f,
                0.0f, 0.0f, 0, 0.0f, 0.05f, 0.10f};
            tranquility.forceVisible = true;
            tranquility.modelAnimation = 0;
            tranquility.modelAnimationLoop = 0;
            ok &= AttachVisual(tranquility);
        }
        g_druidTranquilityStart = GetTickCount();
        return ok;
    }

    bool PlayVisuals(uint32_t classId, uint32_t phase) noexcept
    {
        // State visuals such as Power Word: Shield can explicitly survive phase changes.
        // Full cleanup still occurs through LuaClearVisuals when the showcase ends or resets.
        // Decay must retain the active channel visuals until their exit animation ends.
        if (classId == 11 && phase == 17) return PlayWotlkDruidVisuals(phase);
        ClearVisuals(false);
        if (classId == 1)
            return PlayWotlkWarriorVisuals(phase);
        if (classId == 2)
            return PlayWotlkPaladinVisuals(phase);
        if (classId == 3)
            return PlayWotlkHunterVisuals(phase);
        if (classId == 4)
            return PlayWotlkRogueVisuals(phase);
        if (classId == 5)
            return PlayWotlkPriestVisuals(phase);
        if (classId == 6)
            return PlayWotlkDeathKnightVisuals(phase);
        if (classId == 7)
            return PlayWotlkShamanVisuals(phase);
        if (classId == 8)
            return PlayWotlkMageVisuals(phase);
        if (classId == 9)
            return PlayWotlkWarlockVisuals(phase);
        if (classId == 11)
            return PlayWotlkDruidVisuals(phase);
        const VisualEffect* effects = nullptr;
        size_t count = 0;

        static constexpr VisualEffect kPaladinSpell1[] = {
            {"Spells\\Holy_Precast_Med_Hand.mdx", kAttachLeftHand, 1.0f},
            {"Spells\\Holy_Precast_Med_Hand.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kPaladinSpell2[] = {
            {"Spells\\Holy_Precast_Low_Hand.mdx", kAttachLeftHand, 1.0f},
            {"Spells\\Holy_Precast_Low_Hand.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kRogueSpell1[] = {
            {"Spells\\BackStab_Cast_Base.mdx", kAttachBase, 1.0f},
        };
        static constexpr VisualEffect kRogueSpell2[] = {
            {"Spells\\Eviscerate_Cast_Hands.mdx", kAttachLeftHand, 1.0f},
            {"Spells\\Eviscerate_Cast_Hands.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kPriestSpell1[] = {
            {"Spells\\Holy_Precast_Low_Hand.mdx", kAttachLeftHand, 1.0f},
            {"Spells\\Holy_Precast_Low_Hand.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kPriestSpell2[] = {
            {"Spells\\Shadow_Precast_Low_Hand.mdx", kAttachLeftHand, 1.0f},
            {"Spells\\Shadow_Precast_Low_Hand.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kDeathKnightSpell1[] = {
            {"Spells\\Shadow_Precast_Low_Hand.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kDeathKnightSpell2[] = {
            {"Spells\\BloodLust_State_Hand.mdx", kAttachLeftHand, 1.0f},
            {"Spells\\BloodLust_State_Hand.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kShamanSpell1[] = {
            {"Spells\\Lightning_PreCast_Low_Hand.mdx", kAttachLeftHand, 1.0f},
            {"Spells\\Lightning_PreCast_Low_Hand.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kShamanSpell2[] = {
            {"Spells\\Nature_Cast_Hand.mdx", kAttachLeftHand, 1.0f},
            {"Spells\\Nature_Cast_Hand.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kWarlockSpell1[] = {
            {"Spells\\Shadow_Precast_Low_Hand.mdx", kAttachLeftHand, 1.0f},
            {"Spells\\Shadow_Precast_Low_Hand.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kWarlockSpell2[] = {
            {"Spells\\Fire_Cast_Hand.mdx", kAttachLeftHand, 1.0f},
            {"Spells\\Fire_Cast_Hand.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kDruidSpell1[] = {
            {"spells\\wrath_precast_hand.mdx", kAttachLeftHand, 1.0f},
            {"spells\\wrath_precast_hand.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kDruidSpell2[] = {
            {"Spells\\Nature_Cast_Hand.mdx", kAttachLeftHand, 1.0f},
            {"Spells\\Nature_Cast_Hand.mdx", kAttachRightHand, 1.0f},
        };
        static constexpr VisualEffect kPaladinUltimate[] = {
            {"SPELLS\\AvengingWrath_Impact_Base.m2", kAttachBase, 1.0f},
            {"SPELLS\\AvengingWrath_State_Chest.m2", kAttachChest, 1.0f},
        };
        static constexpr VisualEffect kRogueUltimate[] = {
            {"SPELLS\\ShadowDance_State.M2", kAttachBase, 1.0f},
        };
        static constexpr VisualEffect kPriestUltimate[] = {
            {"SPELLS\\Shadow_Form_Precast.M2", kAttachBase, 1.0f},
        };
        static constexpr VisualEffect kDeathKnightUltimate[] = {
            {"SPELLS\\DeathKnight_Lichborne_State.M2", kAttachChest, 1.0f},
        };
        static constexpr VisualEffect kShamanUltimate[] = {
            {"SPELLS\\LightningShield_State_Base.m2", kAttachBase, 1.0f},
        };
        static constexpr VisualEffect kWarlockUltimate[] = {
            {"SPELLS\\Metamorphosis.M2", kAttachBase, 1.0f},
        };
        static constexpr VisualEffect kDruidUltimate[] = {
            {"SPELLS\\Druid_StarfallState.M2", kAttachBase, 1.0f},
        };

#define SELECT_VISUAL(arrayName) do { effects = arrayName; count = _countof(arrayName); } while (0)
        switch (classId)
        {
            case 2: if (phase == 1) SELECT_VISUAL(kPaladinSpell1);
                    else if (phase == 2) SELECT_VISUAL(kPaladinSpell2);
                    else if (phase == 3) SELECT_VISUAL(kPaladinUltimate); break;
            case 4: if (phase == 1) SELECT_VISUAL(kRogueSpell1);
                    else if (phase == 2) SELECT_VISUAL(kRogueSpell2);
                    else if (phase == 3) SELECT_VISUAL(kRogueUltimate); break;
            case 5: if (phase == 1) SELECT_VISUAL(kPriestSpell1);
                    else if (phase == 2) SELECT_VISUAL(kPriestSpell2);
                    else if (phase == 3) SELECT_VISUAL(kPriestUltimate); break;
            case 6: if (phase == 1) SELECT_VISUAL(kDeathKnightSpell1);
                    else if (phase == 2) SELECT_VISUAL(kDeathKnightSpell2);
                    else if (phase == 3) SELECT_VISUAL(kDeathKnightUltimate); break;
            case 7: if (phase == 1) SELECT_VISUAL(kShamanSpell1);
                    else if (phase == 2) SELECT_VISUAL(kShamanSpell2);
                    else if (phase == 3) SELECT_VISUAL(kShamanUltimate); break;
            case 9: if (phase == 1) SELECT_VISUAL(kWarlockSpell1);
                    else if (phase == 2) SELECT_VISUAL(kWarlockSpell2);
                    else if (phase == 3) SELECT_VISUAL(kWarlockUltimate); break;
            case 11: if (phase == 1) SELECT_VISUAL(kDruidSpell1);
                     else if (phase == 2) SELECT_VISUAL(kDruidSpell2);
                     else if (phase == 3) SELECT_VISUAL(kDruidUltimate); break;
            default: break;
        }
#undef SELECT_VISUAL

        bool ok = true;
        for (size_t i = 0; i < count; ++i) ok &= AttachVisual(effects[i]);
        return ok;
    }

    uint32_t ResolveAnimation(void* model, uint32_t requested) noexcept
    {
        // A profile may name an animation a particular race model does not carry. Follow the
        // stock AnimationData fallback chain, but bound it so malformed/custom DBC rows cannot
        // loop forever.
        uint32_t candidate = requested;
        for (unsigned depth = 0; depth != 16; ++depth)
        {
            if (m2animation::ModelHasSequence(model, candidate)) return candidate;
            if (candidate > m2animation::kLastStockId) break;
            const m2animation::AnimationRow* row = m2animation::Lookup(candidate);
            if (!row || !row->fallback || row->fallback == candidate) break;
            candidate = row->fallback;
        }

        // The playable Tuskarr models contain their own combat-ready/cast sequences but omit many
        // of the stock player showcase IDs used by the class profiles. AnimationData eventually
        // falls all the way back to Stand for those requests, which makes the preview look frozen.
        // Select the closest sequence that both Tuskarr HD sexes actually carry.
        const char* const stem = gm2::M2Model(model).GetPathStem();
        const auto pathContains = [](const char* path, const char* token) noexcept {
            if (!path || !token || !*token) return false;
            const size_t tokenLength = std::strlen(token);
            for (; *path; ++path)
                if (_strnicmp(path, token, tokenLength) == 0) return true;
            return false;
        };
        if (pathContains(stem, "tuskarr"))
        {
            uint32_t tuskarr = UINT32_MAX;
            switch (requested)
            {
                case 18: case 58:
                    tuskarr = 19; break; // Attack2HL
                case 46: case 47: case 57: case 85:
                    tuskarr = 17; break; // Attack1H
                case 29:
                    tuskarr = 26; break; // Ready1H
                case 32: case 33:
                    tuskarr = 54; break; // SpellCastOmni
                default:
                    break;
            }
            if (tuskarr != UINT32_MAX &&
                m2animation::ModelHasSequence(model, tuskarr))
                return tuskarr;
        }
        return m2animation::ModelHasSequence(model, 0) ? 0u : UINT32_MAX;
    }

    bool Play(uint32_t requested, uint32_t& resolved) noexcept
    {
        resolved = UINT32_MAX;
        void* model = CharacterCreationModel();
        if (!model) return false;

        __try
        {
            resolved = ResolveAnimation(model, requested);
            if (resolved == UINT32_MAX) return false;
            wxl::game::Native<m2off::M2_SetBoneSequenceFn>(m2off::kSetBoneSequence)(
                model, nullptr, kAllBoneSlots, resolved, kNoPreviousSequence, 0,
                1.0f, 1, 1);
            WLOG_INFO("play requested=%u resolved=%u model=%p", requested, resolved, model);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            resolved = UINT32_MAX;
            return false;
        }
    }

    int __cdecl LuaPlayAnimation(void* state)
    {
        if (!state || script::ArgCount(state) < 1 || !script::IsNumber(state, 1))
        {
            script::PushBoolean(state, false);
            return 1;
        }

        const double value = script::ToNumber(state, 1);
        if (!std::isfinite(value) || value < 0.0 ||
            value > static_cast<double>((std::numeric_limits<uint32_t>::max)()))
        {
            script::PushBoolean(state, false);
            return 1;
        }

        uint32_t resolved = UINT32_MAX;
        const bool played = Play(static_cast<uint32_t>(value), resolved);
        script::PushBoolean(state, played);
        if (played)
        {
            script::PushNumber(state, static_cast<double>(resolved));
            return 2;
        }
        return 1;
    }

    int __cdecl LuaTrace(void* state)
    {
        const char* message = nullptr;
        if (state && script::ArgCount(state) >= 1 && script::IsString(state, 1))
            message = script::ToString(state, 1);
        if (message && *message) WLOG_INFO("glue: %s", message);
        return 0;
    }

    int __cdecl LuaPlayVisuals(void* state)
    {
        if (!state || script::ArgCount(state) < 2 ||
            !script::IsNumber(state, 1) || !script::IsNumber(state, 2))
        {
            script::PushBoolean(state, false);
            return 1;
        }
        const double classValue = script::ToNumber(state, 1);
        const double phaseValue = script::ToNumber(state, 2);
        if (!std::isfinite(classValue) || !std::isfinite(phaseValue) ||
            classValue < 0.0 || classValue > 255.0 ||
            phaseValue < 0.0 || phaseValue > (classValue == 11.0 ? kMaxDruidStage : 9.0))
        {
            script::PushBoolean(state, false);
            return 1;
        }
        script::PushBoolean(
            state, PlayVisuals(static_cast<uint32_t>(classValue),
                               static_cast<uint32_t>(phaseValue)));
        return 1;
    }

    int __cdecl LuaClearVisuals(void*)
    {
        ClearVisuals();
        return 0;
    }

    int __cdecl LuaRestoreEquipment(void* state)
    {
        script::PushBoolean(state, RestoreEquipment());
        return 1;
    }

    int __cdecl LuaSetWeaponsSheathed(void* state)
    {
        const bool sheathed = state && script::ArgCount(state) >= 1 &&
            script::IsNumber(state, 1) && script::ToNumber(state, 1) != 0.0;
        script::PushBoolean(state, SetWeaponsSheathed(sheathed));
        return 1;
    }

    int __cdecl LuaSetCharacterVisible(void* state)
    {
        const bool visible = state && script::ArgCount(state) >= 1 &&
            script::IsNumber(state, 1) && script::ToNumber(state, 1) != 0.0;
        script::PushBoolean(state, SetCharacterVisible(visible));
        return 1;
    }

    int __cdecl LuaInspect(void*)
    {
        void* component = nullptr;
        __try { component = *reinterpret_cast<void**>(kCharacterCreationComponent); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        void* field20 = SafePointer(component, 0x20);
        void* field38 = SafePointer(component, 0x38);
        WLOG_INFO(
            "inspect component=%p f20=%p data20=%p f38=%p data38=%p f38parent=%p",
            component, field20, SafeModelData(field20), field38, SafeModelData(field38),
            SafePointer(field38, 0x48));
        return 0;
    }

    void __cdecl OnM2PerFrame(void*, const void* raw)
    {
        const auto& args = *static_cast<const ev::M2PerFrameUpdateArgs*>(raw);
        // The Modern M2 detour emits after the native per-instance update. Match the exact child
        // context and rebuild an absolute root-relative position so repeated callbacks cannot
        // accumulate translation before the outer palette/draw pass.
        for (ActiveVisual& visual : g_activeVisuals)
            if (visual.renderContext == args.renderCtx)
            {
                // Flight animation selection may precede asynchronous model initialization.
                // Keep this choreography choice separate from general renderer compatibility.
                if (!visual.animationConfigured &&
                    _stricmp(visual.path, "Spells\\cfx_warlock_chaosbolt_missile.m2") == 0)
                {
                    visual.animationConfigured = TryConfigureVisualAnimation(
                        visual.renderContext, 144, 1.0f, 1);
                    visual.launchMs = GetTickCount();
                    if (visual.animationConfigured)
                        WLOG_INFO("Chaos Bolt InFlight ready; starting 1400 ms top-left flight");
                }
                const uint32_t age = GetTickCount() - visual.launchMs;
                HoldVisualAnimation(visual, age);
                ApplyVisualPlacement(visual);
            }

        // Native updates recurse through children. At the root's completion, detect
        // any later attachment write that replaced our spatial placement. Reapply only
        // owned base effects; this is absolute/idempotent and does not move hand effects.
        for (ActiveVisual& visual : g_activeVisuals)
            if (visual.root == args.renderCtx && visual.attachment == kAttachBase &&
                (visual.offsetX != 0.0f || visual.offsetY != 0.0f ||
                 visual.offsetZ != 0.0f || visual.travel != 0.0f || visual.travelZ != 0.0f))
            {
                __try
                {
                    const auto* before = reinterpret_cast<const float*>(
                        static_cast<const uint8_t*>(visual.renderContext) +
                        m2off::kOffInstPlacement);
                    const float oldX = before[12], oldY = before[13], oldZ = before[14];
                    ApplyVisualPlacement(visual);
                    static uint32_t placementReports = 0;
                    if (placementReports < 12 &&
                        _stricmp(visual.path, "Creature\\Imp\\Imp.m2") == 0 &&
                        (!visual.placementReported ||
                         std::fabs(oldX - before[12]) + std::fabs(oldY - before[13]) +
                         std::fabs(oldZ - before[14]) > 0.01f))
                    {
                        const auto* rootPlacement = reinterpret_cast<const float*>(
                            static_cast<const uint8_t*>(visual.root) + m2off::kOffInstPlacement);
                        WLOG_INFO("Imp placement root=%p before=(%.3f,%.3f,%.3f) "
                                  "final=(%.3f,%.3f,%.3f) rootY=(%.3f,%.3f,%.3f)",
                                  visual.root, oldX, oldY, oldZ, before[12], before[13],
                                  before[14], rootPlacement[4], rootPlacement[5], rootPlacement[6]);
                        visual.placementReported = true;
                        ++placementReports;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }

        // The Glue Model frame's alpha/scale do not control its embedded race actor.
        // Reassert native instance alpha after the engine's own per-frame staging so
        // Vanish and the model-loading curtain suppress the actual rendered model.
        if (g_characterHidden && args.renderCtx == CharacterCreationModel())
        {
            __try
            {
                auto* instance = static_cast<m2off::M2Instance*>(args.renderCtx);
                instance->alphaBase = 0.0f;
                instance->alphaStage = 0.0f;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    void __cdecl OnUpdate(void*, const void*)
    {
        UpdateWotlkBlizzardEmitter();
        UpdateWotlkPriestChannelEmitter();
        UpdateWotlkWarlockImpEmitter();
        UpdateWotlkWarlockRainEmitter();
        UpdateDruidEffectLifecycle();
        if (g_druidFormPath)
            EnsureActiveVisualAnimation(g_druidFormPath, g_druidFormAnimation, 1.0f,
                g_druidFormAnimation == 0 || g_druidFormAnimation == 52 ||
                g_druidFormAnimation == 124 ? 1 : 0);
    }

    // AnimationData IDs are stock 3.3.5 IDs. Each profile is:
    //   basic one, basic two, ultimate, ready stance, delay, basic-two start,
    //   ultimate start, ready-stance start.
    // The native side resolves missing sequences through AnimationData fallbacks per race model.
    constexpr char kBootstrap[] = R"lua(
do
    local owner = _G.WXLCharacterCreationPreview or {}
    _G.WXLCharacterCreationPreview = owner

    owner.nativePlay = owner.nativePlay or _WXL_CC_PREVIEW_PLAY_ANIMATION
    owner.nativeTrace = owner.nativeTrace or _WXL_CC_PREVIEW_TRACE
    owner.nativeInspect = owner.nativeInspect or _WXL_CC_PREVIEW_INSPECT
    owner.nativePlayVisuals = owner.nativePlayVisuals or _WXL_CC_PREVIEW_PLAY_VISUALS
    owner.nativeClearVisuals = owner.nativeClearVisuals or _WXL_CC_PREVIEW_CLEAR_VISUALS
    owner.nativeRestoreEquipment = owner.nativeRestoreEquipment or _WXL_CC_PREVIEW_RESTORE_EQUIPMENT
    owner.nativeSetWeaponsSheathed = owner.nativeSetWeaponsSheathed or _WXL_CC_PREVIEW_SET_WEAPONS_SHEATHED
    owner.nativeSetModelVisible = owner.nativeSetModelVisible or _WXL_CC_PREVIEW_SET_MODEL_VISIBLE
    _WXL_CC_PREVIEW_PLAY_ANIMATION = nil
    _WXL_CC_PREVIEW_TRACE = nil
    _WXL_CC_PREVIEW_INSPECT = nil
    _WXL_CC_PREVIEW_PLAY_VISUALS = nil
    _WXL_CC_PREVIEW_CLEAR_VISUALS = nil
    _WXL_CC_PREVIEW_RESTORE_EQUIPMENT = nil
    _WXL_CC_PREVIEW_SET_WEAPONS_SHEATHED = nil
    _WXL_CC_PREVIEW_SET_MODEL_VISIBLE = nil

    owner.profiles = owner.profiles or {
        [1]  = { 55, 58, 18, 27, 3.00, 4.40, 5.80, 8.80 }, -- Warrior uses the dedicated four-stage WotLK timeline below.
        [2]  = { 33, 54, 18, 27, 3.00, 4.40, 5.80, 8.20 }, -- Paladin uses the dedicated six-stage timeline below.
        [3]  = { 114, 109, 47, 29, 3.00, 3.80, 6.00, 9.10 }, -- Hunter uses KneelStart and the dedicated pet/trap/two-shot timeline below.
        [4]  = { 85, 57, 120, 26, 3.00, 4.40, 5.80, 7.50 }, -- Rogue uses the dedicated Vanish/Shadowstep timeline below.
        [5]  = { 124, 54, 124, 28, 3.00, 4.40, 5.80, 9.40 }, -- Priest uses the dedicated Penance/Shadow timeline below.
        [6]  = { 54, 33, 18, 27, 3.00, 4.40, 5.80, 10.70 }, -- Death Knight uses the dedicated six-stage WotLK timeline below.
        [7]  = { 54, 33, 58, 26, 3.00, 4.40, 5.80, 11.20 }, -- Shaman uses the dedicated Ghost Wolf/totem/combat timeline below.
        [8]  = { 51, 53, 54, 26, 3.00, 4.60, 6.20, 15.00 }, -- Mage uses the dedicated nine-stage WotLK timeline below.
        [9]  = { 54, 53, 52, 28, 3.00, 4.40, 5.80, 10.10 }, -- Warlock uses the dedicated Imp/Chaos Bolt/Rain of Fire timeline below.
        [11] = { 53, 54, 33, 28, 3.00, 4.40, 5.80, 7.50 }, -- Druid polearm / Starfall / Ready2HL
    }

    function owner:IsEnabled()
        return not GetCVar or tonumber(GetCVar("wxlCharacterCreatePreview") or "1") ~= 0
    end

    function owner:SetModelVisible(visible)
        if self.nativeSetModelVisible then
            self.nativeSetModelVisible(visible and 1 or 0)
        end
        self.modelHidden = not visible
    end

    function owner:EnsureModelLoading()
        if self.modelLoading then return self.modelLoading end
        local model = _G.CharacterCreateModel
        if not model or type(CreateFrame) ~= "function" then return nil end
        local parent = type(model.GetParent) == "function" and model:GetParent() or model
        local ok, overlay = pcall(CreateFrame, "Frame", "WXLCharacterCreateModelLoading",
                                  parent)
        if not ok or not overlay then return nil end
        overlay:SetAllPoints(model)
        if type(overlay.SetFrameLevel) == "function" and
           type(model.GetFrameLevel) == "function" then
            overlay:SetFrameLevel(model:GetFrameLevel() + 20)
        end
        local curtain = overlay:CreateTexture(nil, "BACKGROUND")
        curtain:SetAllPoints()
        curtain:SetTexture(0.012, 0.010, 0.008, 0.45)

        local spinnerOK, spinner = pcall(CreateFrame, "Frame", nil, overlay,
                                         "LoadingSpinnerTemplate")
        if spinnerOK and spinner then
            spinner:SetSize(48, 48)
            spinner:SetPoint("CENTER", overlay, "CENTER", 0, 0)
            overlay.spinner = spinner
        end
        local text = overlay:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
        text:SetPoint("TOP", overlay.spinner or overlay, "BOTTOM", 0, -8)
        text:SetText("Loading model")
        overlay.text = text
        overlay:Hide()
        self.modelLoading = overlay
        return overlay
    end

    function owner:SetModelLoading(loading)
        self:SetModelVisible(not loading)
        local overlay = self:EnsureModelLoading()
        if not overlay then return end
        local anim = overlay.spinner and overlay.spinner.AnimFrame and
                     overlay.spinner.AnimFrame.Anim
        if loading then
            overlay:Show()
            if anim and type(anim.Play) == "function" then anim:Play() end
        else
            if anim and type(anim.Stop) == "function" then anim:Stop() end
            overlay:Hide()
        end
    end

    function owner:IsEquipmentReady()
        local ready = _G._WXL_CC_EQUIPMENT_READY
        if type(ready) ~= "function" then return false end
        local ok, value = pcall(ready)
        return ok and value and true or false
    end

    function owner:Begin(classID, modelRebuild)
        self.rebuildDelay = nil
        self.rebuildClassID = nil
        classID = tonumber(classID)
        local profile = classID and self.profiles[classID]
        if not profile or not self:IsEnabled() then
            self.pending = nil
            if self.nativeClearVisuals then self.nativeClearVisuals() end
            self:SetModelLoading(false)
            return
        end
        self.classID = classID
        -- A race/sex rebuild already refreshes its equipment. Defer our extra refresh to the
        -- readiness phase instead of doing a second expensive synchronous rebuild in the click.
        if self.nativeSetWeaponsSheathed and not modelRebuild then
            self.nativeSetWeaponsSheathed(0)
        end
        local firstShowcase = not self.hasBegun
        self.hasBegun = true
        self.pending = {
            profile = profile, elapsed = 0, phase = 1, retry = 0,
            refreshCount = 0, nextRefresh = (modelRebuild or firstShowcase) and 0.75 or 0.25,
            minimumReveal = (modelRebuild or firstShowcase) and 1.25 or 0.75,
            revealedAt = nil, animationBase = nil,
        }
        self:SetModelLoading(true)
        if self.nativeTrace then self.nativeTrace("begin class=" .. classID) end
        if self.nativeInspect then self.nativeInspect() end
    end

    function owner:EnterCustomization()
        self.pending = nil
        self.rebuildDelay = nil
        self.rebuildClassID = nil
        self.customizationStandDelay = 0.10
        if self.nativeClearVisuals then self.nativeClearVisuals() end
        if self.nativePlay then self.nativePlay(0) end -- Stand for appearance edits.
        if self.nativeSetWeaponsSheathed then self.nativeSetWeaponsSheathed(1) end
        self:SetModelLoading(false)
        if self.nativeTrace then self.nativeTrace("customization stand") end
    end

    function owner:QueueRebuildPreview(reason)
        -- SetSelectedSex returns before the remainder of the gender click handler has rebuilt its
        -- race/class controls and reset the camera. Starting synchronously lets that same-frame
        -- Glue work win over the preview on the replacement actor. Defer by one update so Begin
        -- targets the settled model while retaining its normal equipment-readiness wait.
        self.pending = nil
        self.rebuildDelay = 0.05
        self.rebuildClassID = self.classID or _G.SELECTED_CLASS
        if self.nativeClearVisuals then self.nativeClearVisuals() end
        self:SetModelLoading(true)
        if self.nativeTrace then
            self.nativeTrace("queued " .. (reason or "model rebuild") .. " preview")
        end
    end

    function owner:WrapCustomization()
        -- SetCharCustomizeFrame is only the Glue model-registration API and is normally
        -- called before this bootstrap is installed.  The visible button calls the live
        -- customization frame's PlayToggleAnim method instead, so hook that exact path.
        local characterCreate = _G.CharacterCreate
        local frame = characterCreate and characterCreate.CustomizationFrame
        if not frame or type(frame.PlayToggleAnim) ~= "function" then return end
        if self.wrappedCustomizeFrame == frame and
           self.wrappedCustomize == frame.PlayToggleAnim then return end

        local original = frame.PlayToggleAnim
        local wrapped = function(customizationFrame, ...)
            local result = original(customizationFrame, ...)
            if customizationFrame.show then
                owner:EnterCustomization()
            else
                owner.customizationStandDelay = nil
                if owner.nativeSetWeaponsSheathed then
                    owner.nativeSetWeaponsSheathed(0)
                end
            end
            return result
        end
        frame.PlayToggleAnim = wrapped
        self.wrappedCustomizeFrame = frame
        self.wrappedCustomize = wrapped
        if self.nativeTrace then self.nativeTrace("wrapped customization toggle") end
    end

    function owner:WrapCharacterCreation()
        if not C_CharacterCreation or
           type(C_CharacterCreation.SetSelectedClass) ~= "function" then return end

        -- Glue starts with the engine table and Utils/C_CharacterCreation.lua later replaces it
        -- wholesale. Track the exact table and function, not a one-way boolean, so the live wrapper
        -- is reinstalled after that replacement (and after any later compatibility rebuild).
        if self.wrappedTable == C_CharacterCreation and
           self.wrappedClass == C_CharacterCreation.SetSelectedClass and
           self.wrappedRace == C_CharacterCreation.SetSelectedRace and
           self.wrappedSex == C_CharacterCreation.SetSelectedSex then return end

        local tableChanged = self.wrappedTable ~= C_CharacterCreation
        local installed = false

        if tableChanged or self.wrappedClass ~= C_CharacterCreation.SetSelectedClass then
            local originalClass = C_CharacterCreation.SetSelectedClass
            local wrappedClass = function(classID)
                if owner.nativeClearVisuals then owner.nativeClearVisuals() end
                local result = originalClass(classID)
                owner:Begin(classID, false)
                return result
            end
            C_CharacterCreation.SetSelectedClass = wrappedClass
            self.wrappedClass = wrappedClass
            installed = true
        end

        local function wrapRebuild(name, field, deferReplay)
            if not tableChanged and owner[field] == C_CharacterCreation[name] then return end
            local original = C_CharacterCreation[name]
            if type(original) ~= "function" then return end
            local wrapped = function(value)
                if owner.nativeClearVisuals then owner.nativeClearVisuals() end
                owner:SetModelLoading(true)
                local result = original(value)
                if result then
                    if deferReplay then
                        owner:QueueRebuildPreview(name)
                    else
                        owner:Begin(owner.classID or _G.SELECTED_CLASS, true)
                    end
                else owner:SetModelLoading(false) end
                return result
            end
            C_CharacterCreation[name] = wrapped
            owner[field] = wrapped
            installed = true
        end
        wrapRebuild("SetSelectedRace", "wrappedRace", true)
        wrapRebuild("SetSelectedSex", "wrappedSex", true)
        self.wrappedTable = C_CharacterCreation
        if installed and self.nativeTrace then
            self.nativeTrace("wrapped live C_CharacterCreation functions")
        end
    end

    function owner:EnsureRotationInput()
        local model = _G.CharacterCreateModel
        if not model or type(model.EnableMouse) ~= "function" or
           type(model.SetScript) ~= "function" then return end
        if self.rotationInputModel == model then return end

        model:EnableMouse(true)
        local oldDown = type(model.GetScript) == "function" and
                        model:GetScript("OnMouseDown") or nil
        local oldUp = type(model.GetScript) == "function" and
                      model:GetScript("OnMouseUp") or nil
        model:SetScript("OnMouseDown", function(frame, button)
            if oldDown then oldDown(frame, button) end
            if button == "LeftButton" and owner.pending and
               type(GetCursorPosition) == "function" then
                owner.previewRotationStartX = GetCursorPosition()
            end
        end)
        model:SetScript("OnMouseUp", function(frame, button)
            if oldUp then oldUp(frame, button) end
            if button == "LeftButton" then owner.previewRotationStartX = nil end
        end)
        self.rotationInputModel = model
        if self.nativeTrace then self.nativeTrace("enabled preview drag rotation") end
    end

    function owner:InstallClassAvailabilityFilter()
        if self.classAvailabilityFiltered or not C_CharacterCreation or
           type(C_CharacterCreation.GetAvailableClasses) ~= "function" then return end
        local original = C_CharacterCreation.GetAvailableClasses
        C_CharacterCreation.GetAvailableClasses = function(...)
            local classes = original(...)
            for _, data in ipairs(classes or {}) do
                -- Private slot 12 is NAKED. Use the stable class token, never Retail's ID.
                local token = string.upper(data.clientFileString or ""):gsub("[_%s]", "")
                if token == "DEMONHUNTER" then data.hidden = true end
            end
            return classes
        end
        self.classAvailabilityFiltered = true
        -- Install only. Glue creates the frame/pool on the login screen before the
        -- native character-creation component exists. Its normal creation lifecycle
        -- will query this wrapper and build the buttons when that component is ready.
        -- Never call BuildClassData/GetAvailableClasses from the global OnUpdate.
    end

    function owner:OnUpdate(elapsed)
        self:InstallClassAvailabilityFilter()
        self:WrapCharacterCreation()
        self:WrapCustomization()
        self:EnsureRotationInput()

        if self.previewRotationStartX and self.pending and C_CharacterCreation and
           type(C_CharacterCreation.GetCharacterCreateFacing) == "function" and
           type(C_CharacterCreation.SetCharacterCreateFacing) == "function" then
            local x = GetCursorPosition()
            local diff = (x - self.previewRotationStartX) * 0.6
            self.previewRotationStartX = x
            if diff ~= 0 then
                C_CharacterCreation.SetCharacterCreateFacing(
                    C_CharacterCreation.GetCharacterCreateFacing() + diff)
            end
        elseif not self.pending then
            self.previewRotationStartX = nil
        end

        -- Reassert Stand once after the toggle has settled.  This wins any deferred Glue
        -- transition without continuously restarting animation 0 while the user edits.
        if self.customizationStandDelay then
            self.customizationStandDelay = self.customizationStandDelay - (elapsed or 0)
            if self.customizationStandDelay <= 0 then
                self.customizationStandDelay = nil
                local characterCreate = _G.CharacterCreate
                local frame = characterCreate and characterCreate.CustomizationFrame
                if frame and frame.show and frame:IsShown() then
                    if self.nativePlay then self.nativePlay(0) end
                    if self.nativeTrace then self.nativeTrace("customization stand settled") end
                end
            end
        end

        if self.rebuildDelay then
            self.rebuildDelay = self.rebuildDelay - (elapsed or 0)
            if self.rebuildDelay <= 0 then
                local classID = self.rebuildClassID or self.classID or _G.SELECTED_CLASS
                self.rebuildDelay = nil
                self.rebuildClassID = nil
                self:Begin(classID, true)
            end
        end

        local pending = self.pending
        if not pending then return end

        pending.elapsed = pending.elapsed + (elapsed or 0)
        local profile = pending.profile

        if pending.phase == 1 then
            if not pending.revealedAt then
                -- Keep the incomplete model out of view while the native
                -- compositor creates body layers and Modern M2 attaches and
                -- bone-retargets every collection piece.
                self:SetModelLoading(true)
            end
            if not pending.revealedAt and pending.elapsed >= pending.nextRefresh then
                pending.refreshCount = pending.refreshCount + 1
                pending.nextRefresh = pending.elapsed + 2.50
                if self.nativeRestoreEquipment then self.nativeRestoreEquipment() end
            end

            if not pending.revealedAt then
                local ready = pending.elapsed >= pending.minimumReveal and
                              self:IsEquipmentReady()
                local timedOut = pending.elapsed >= 8.0
                if not ready and not timedOut then return end
                pending.revealedAt = pending.elapsed
                pending.animationBase = pending.elapsed + 2.0
                self:SetModelLoading(false)
                if self.nativePlay then self.nativePlay(0) end
                if self.nativeTrace then
                    self.nativeTrace((ready and "equipment ready" or "equipment readiness timeout") ..
                                     " refreshes=" .. pending.refreshCount)
                end
                return
            end

            self:SetModelLoading(false)
            if pending.elapsed < pending.animationBase then return end
            pending.retry = pending.retry - (elapsed or 0)
            if pending.retry <= 0 then
                pending.retry = 0.05
                if self.nativePlay(profile[1]) then
                    local visualReady = true
                    if self.nativePlayVisuals then visualReady = self.nativePlayVisuals(self.classID, self.classID == 11 and 16 or 1) end
                    if self.classID == 7 then
                        self:SetModelVisible(false)
                    end
                    pending.phase = 2
                end
            end
            if pending.elapsed > pending.animationBase + 1.0 and pending.phase == 1 then
                self:SetModelLoading(false)
                self.pending = nil
            end
            return
        end

        -- Druid forms own their animations; the hidden player's skeleton is not the caster.
        if self.classID == 11 then
            local stages = {
                {0.45, 1}, {1.60, 2}, {2.90, 3}, {4.30, 16}, {4.75, 4},
                {5.80, 5}, {7.00, 6}, {8.40, 16}, {8.85, 7},
                {9.90, 8}, {11.00, 9}, {12.60, 10},
                {14.80, 16}, {15.25, 11}, {16.30, 12}, {17.30, 13},
                {19.00, 14}, {20.00, 15}, {28.00, 17},
            }
            local step = stages[pending.phase - 1]
            if step and pending.elapsed >= pending.animationBase + step[1] then
                if self.nativePlayVisuals and self.nativePlayVisuals(11, step[2]) then
                    self:SetModelVisible(false)
                end
                pending.phase = pending.phase + 1
            end
            if pending.phase == 21 and pending.elapsed >= pending.animationBase + 29.90 then
                if self.nativeClearVisuals then self.nativeClearVisuals() end
                self:SetModelVisible(true)
                self.nativePlay(0)
                self.pending = nil
            end
            return
        end

        -- Mage uses the spell models and AnimationData IDs from the 3.3.5a DBCs. Blizzard keeps
        -- its spell models alive for a full channel window while procedure 9 emits impacts.
        if self.classID == 8 then
            local base = pending.animationBase
            if pending.phase == 2 and pending.elapsed >= base + 1.45 then
                self.nativePlay(53)
                if self.nativePlayVisuals then self.nativePlayVisuals(8, 2) end -- Pyroblast release
                pending.phase = 3
            end
            if pending.phase == 3 and pending.elapsed >= base + 2.45 then
                self.nativePlay(51)
                if self.nativePlayVisuals then self.nativePlayVisuals(8, 3) end -- Frostbolt precast
                pending.phase = 4
            end
            if pending.phase == 4 and pending.elapsed >= base + 3.75 then
                self.nativePlay(53)
                if self.nativePlayVisuals then self.nativePlayVisuals(8, 4) end -- Frostbolt release
                pending.phase = 5
            end
            if pending.phase == 5 and pending.elapsed >= base + 4.75 then
                self.nativePlay(52)
                if self.nativePlayVisuals then self.nativePlayVisuals(8, 5) end -- Water Elemental summon precast
                pending.phase = 6
            end
            if pending.phase == 6 and pending.elapsed >= base + 6.20 then
                self.nativePlay(54)
                if self.nativePlayVisuals then self.nativePlayVisuals(8, 6) end -- Water Elemental appears
                pending.phase = 7
            end
            if pending.phase == 7 and pending.elapsed >= base + 8.00 then
                self.nativePlay(52)
                if self.nativePlayVisuals then self.nativePlayVisuals(8, 7) end -- Blizzard precast
                pending.phase = 8
            end
            if pending.phase == 8 and pending.elapsed >= base + 9.30 then
                self.nativePlay(125)
                if self.nativePlayVisuals then self.nativePlayVisuals(8, 8) end -- WotLK Blizzard channel
                pending.phase = 9
            end
            if pending.phase == 9 and pending.elapsed >= base + 12.50 then
                self.nativePlay(54)
                if self.nativePlayVisuals then self.nativePlayVisuals(8, 9) end -- Arcane Explosion
                pending.phase = 10
            end
            if pending.phase == 10 and pending.elapsed >= base + 13.65 then
                if self.nativeClearVisuals then self.nativeClearVisuals() end
                self.nativePlay(profile[4])
                self.pending = nil
            end
            return
        end

        -- Warrior: Battle Shout, Thunder Clap, Slam, then a sustained Bladestorm. Animation 126
        -- is the stock Whirlwind sequence and keeps the final visual synchronized with the model.
        if self.classID == 1 then
            local base = pending.animationBase
            if pending.phase == 2 and pending.elapsed >= base + 1.25 then
                self.nativePlay(58)
                if self.nativePlayVisuals then self.nativePlayVisuals(1, 2) end
                pending.phase = 3
            end
            if pending.phase == 3 and pending.elapsed >= base + 2.45 then
                self.nativePlay(18)
                if self.nativePlayVisuals then self.nativePlayVisuals(1, 3) end
                pending.phase = 4
            end
            if pending.phase == 4 and pending.elapsed >= base + 3.70 then
                self.nativePlay(126)
                if self.nativePlayVisuals then self.nativePlayVisuals(1, 4) end
                pending.phase = 5
            end
            if pending.phase == 5 and pending.elapsed >= base + 5.90 then
                if self.nativeClearVisuals then self.nativeClearVisuals() end
                self.nativePlay(profile[4])
                self.pending = nil
            end
            return
        end

        -- Hunter: use the real KneelStart/Loop/End chain while placing the classic WotLK trap,
        -- then use LoadBow/HoldBow/FireBow so Aimed Shot visibly aims without an early attack.
        -- Every native visual stage recreates the wolf and trap so both remain fixed in place.
        if self.classID == 3 then
            local base = pending.animationBase
            if pending.phase == 2 and pending.elapsed >= base + 0.05 then
                self.nativePlay(114) -- KneelStart.
                if self.nativePlayVisuals then self.nativePlayVisuals(3, 1) end
                pending.phase = 3
            end
            if pending.phase == 3 and pending.elapsed >= base + 1.00 then
                self.nativePlay(115) -- KneelLoop while the closed trap appears.
                if self.nativePlayVisuals then self.nativePlayVisuals(3, 2) end
                pending.phase = 4
            end
            if pending.phase == 4 and pending.elapsed >= base + 1.50 then
                if self.nativePlayVisuals then self.nativePlayVisuals(3, 3) end -- Trap opens.
                pending.phase = 5
            end
            if pending.phase == 5 and pending.elapsed >= base + 2.00 then
                self.nativePlay(116) -- KneelEnd: stand after the trap opens.
                if self.nativePlayVisuals then self.nativePlayVisuals(3, 4) end
                pending.phase = 6
            end
            if pending.phase == 6 and pending.elapsed >= base + 2.55 then
                self.nativePlay(105) -- LoadBow begins Aimed Shot without firing early.
                pending.phase = 7
            end
            if pending.phase == 7 and pending.elapsed >= base + 3.05 then
                self.nativePlay(109) -- HoldBow makes the aimed cast clearly readable.
                pending.phase = 8
            end
            if pending.phase == 8 and pending.elapsed >= base + 3.85 then
                self.nativePlay(47) -- FireBow releases Aimed Shot.
                if self.nativePlayVisuals then self.nativePlayVisuals(3, 5) end
                pending.phase = 9
            end
            if pending.phase == 9 and pending.elapsed >= base + 4.45 then
                self.nativePlay(105) -- Load the Explosive Shot without returning to idle.
                pending.phase = 10
            end
            if pending.phase == 10 and pending.elapsed >= base + 4.95 then
                self.nativePlay(47) -- FireBow releases Explosive Shot cleanly.
                if self.nativePlayVisuals then self.nativePlayVisuals(3, 6) end
                pending.phase = 11
            end
            if pending.phase == 11 and pending.elapsed >= base + 5.75 then
                self.nativePlay(107) -- AttackThrown launches the finishing Flare.
                if self.nativePlayVisuals then self.nativePlayVisuals(3, 7) end
                pending.phase = 12
            end
            if pending.phase == 12 and pending.elapsed >= base + 7.00 then
                if self.nativeClearVisuals then self.nativeClearVisuals() end
                self.nativePlay(profile[4])
                self.pending = nil
            end
            return
        end

        -- Paladin: Consecration, Avenging Wrath, Crusader strike, Holy Light, then Kings/bubble.
        if self.classID == 2 then
            local base = pending.animationBase
            if pending.phase == 2 and pending.elapsed >= base + 1.35 then
                self.nativePlay(54) -- SpellCastOmni: Avenging Wrath.
                if self.nativePlayVisuals then self.nativePlayVisuals(2, 2) end
                pending.phase = 3
            end
            if pending.phase == 3 and pending.elapsed >= base + 2.75 then
                self.nativePlay(18) -- Attack2H: Crusader Strike.
                if self.nativePlayVisuals then self.nativePlayVisuals(2, 3) end
                pending.phase = 4
            end
            if pending.phase == 4 and pending.elapsed >= base + 3.90 then
                self.nativePlay(51) -- ReadySpellDirected: Holy Light precast.
                if self.nativePlayVisuals then self.nativePlayVisuals(2, 4) end
                pending.phase = 5
            end
            if pending.phase == 5 and pending.elapsed >= base + 5.10 then
                self.nativePlay(53) -- SpellCastDirected: Holy Light lands.
                if self.nativePlayVisuals then self.nativePlayVisuals(2, 5) end
                pending.phase = 6
            end
            if pending.phase == 6 and pending.elapsed >= base + 6.20 then
                self.nativePlay(54) -- Kings and Divine Shield finish.
                if self.nativePlayVisuals then self.nativePlayVisuals(2, 6) end
                pending.phase = 7
            end
            if pending.phase == 7 and pending.elapsed >= base + 8.20 then
                if self.nativeClearVisuals then self.nativeClearVisuals() end
                self.nativePlay(profile[4])
                self.pending = nil
            end
            return
        end

        -- Rogue: keep the opening attacks, then Vanish, hold StealthStand, and emerge through
        -- Shadowstep into a Backstab against an implied target directly in front of the actor.
        if self.classID == 4 then
            local base = pending.animationBase
            if pending.phase == 2 and pending.elapsed >= base + 1.30 then
                self.nativePlay(57) -- Special1H with Eviscerate.
                if self.nativePlayVisuals then self.nativePlayVisuals(4, 2) end
                pending.phase = 3
            end
            if pending.phase == 3 and pending.elapsed >= base + 2.60 then
                self.nativePlay(120) -- StealthStand as Vanish fires.
                if self.nativePlayVisuals then self.nativePlayVisuals(4, 3) end
                pending.phase = 4
            end
            if pending.phase == 4 and pending.elapsed >= base + 3.00 then
                self.nativePlay(120) -- Hold stealth after the Vanish cloud clears.
                if self.nativePlayVisuals then self.nativePlayVisuals(4, 4) end
                self:SetModelVisible(false)
                pending.phase = 5
            end
            if pending.phase == 5 and pending.elapsed >= base + 4.10 then
                self:SetModelVisible(true)
                self.nativePlay(85) -- Attack1HPierce: emerge into Backstab.
                if self.nativePlayVisuals then self.nativePlayVisuals(4, 5) end
                pending.phase = 6
            end
            if pending.phase == 6 and pending.elapsed >= base + 5.30 then
                self.nativePlay(33) -- SpellCastArea: Fan of Knives finisher.
                if self.nativePlayVisuals then self.nativePlayVisuals(4, 6) end
                pending.phase = 7
            end
            if pending.phase == 7 and pending.elapsed >= base + 6.80 then
                self:SetModelVisible(true)
                if self.nativeClearVisuals then self.nativeClearVisuals() end
                self.nativePlay(profile[4])
                self.pending = nil
            end
            return
        end

        -- Priest: a full three-bolt Penance channel, self Shield, Shadowform, Psyfiend summon,
        -- then a sustained Mind Flay while Shadowform and Psyfiend remain visible.
        if self.classID == 5 then
            local base = pending.animationBase
            if pending.phase == 2 and pending.elapsed >= base + 0.85 then
                if self.nativePlayVisuals then self.nativePlayVisuals(5, 2) end
                pending.phase = 3
            end
            if pending.phase == 3 and pending.elapsed >= base + 1.70 then
                if self.nativePlayVisuals then self.nativePlayVisuals(5, 3) end
                pending.phase = 4
            end
            if pending.phase == 4 and pending.elapsed >= base + 2.65 then
                self.nativePlay(54) -- Power Word: Shield.
                if self.nativePlayVisuals then self.nativePlayVisuals(5, 4) end
                pending.phase = 5
            end
            if pending.phase == 5 and pending.elapsed >= base + 3.85 then
                self.nativePlay(54) -- Enter Shadowform.
                if self.nativePlayVisuals then self.nativePlayVisuals(5, 5) end
                pending.phase = 6
            end
            if pending.phase == 6 and pending.elapsed >= base + 5.00 then
                self.nativePlay(54) -- Summon Psyfiend beside the actor.
                if self.nativePlayVisuals then self.nativePlayVisuals(5, 6) end
                pending.phase = 7
            end
            if pending.phase == 7 and pending.elapsed >= base + 6.20 then
                self.nativePlay(124) -- Full Mind Flay channel.
                if self.nativePlayVisuals then self.nativePlayVisuals(5, 7) end
                pending.phase = 8
            end
            if pending.phase == 8 and pending.elapsed >= base + 9.40 then
                if self.nativeClearVisuals then self.nativeClearVisuals() end
                self.nativePlay(profile[4])
                self.pending = nil
            end
            return
        end

        -- Death Knight: Raise Dead, persistent Death and Decay, Frost Strike, Blood Strike,
        -- a Death Coil missile to the top-left target, then Blood Boil.
        if self.classID == 6 then
            local base = pending.animationBase
            if pending.phase == 2 and pending.elapsed >= base + 1.35 then
                self.nativePlay(33) -- SpellCastArea: Death and Decay.
                if self.nativePlayVisuals then self.nativePlayVisuals(6, 2) end
                pending.phase = 3
            end
            if pending.phase == 3 and pending.elapsed >= base + 2.75 then
                self.nativePlay(18) -- Attack2H: Frost Strike.
                if self.nativePlayVisuals then self.nativePlayVisuals(6, 3) end
                pending.phase = 4
            end
            if pending.phase == 4 and pending.elapsed >= base + 3.90 then
                self.nativePlay(58) -- Special2H: Blood Strike.
                if self.nativePlayVisuals then self.nativePlayVisuals(6, 4) end
                pending.phase = 5
            end
            if pending.phase == 5 and pending.elapsed >= base + 5.05 then
                self.nativePlay(53) -- SpellCastDirected: Death Coil.
                if self.nativePlayVisuals then self.nativePlayVisuals(6, 5) end
                pending.phase = 6
            end
            if pending.phase == 6 and pending.elapsed >= base + 6.35 then
                self.nativePlay(54) -- SpellCastOmni: clearly read the Blood Boil finisher.
                if self.nativePlayVisuals then self.nativePlayVisuals(6, 6) end
                pending.phase = 7
            end
            if pending.phase == 7 and pending.elapsed >= base + 8.05 then
                if self.nativeClearVisuals then self.nativeClearVisuals() end
                self.nativePlay(profile[4])
                self.pending = nil
            end
            return
        end

        -- Shaman: Ghost Wolf for two seconds, return to the player, drop all four elemental
        -- totems, use Windfury and a fire-imbued melee strike, then visibly precast and release
        -- Lightning Bolt and Lava Burst toward the established top-left target.
        if self.classID == 7 then
            local base = pending.animationBase
            if pending.phase == 2 and pending.elapsed >= base + 2.00 then
                self:SetModelVisible(true)
                self.nativePlay(54) -- Return from Ghost Wolf.
                if self.nativePlayVisuals then self.nativePlayVisuals(7, 2) end
                pending.phase = 3
            end
            if pending.phase == 3 and pending.elapsed >= base + 2.50 then
                self.nativePlay(33) -- Call of the Elements: four totems.
                if self.nativePlayVisuals then self.nativePlayVisuals(7, 3) end
                pending.phase = 4
            end
            if pending.phase == 4 and pending.elapsed >= base + 4.05 then
                self.nativePlay(58) -- Windfury melee beat.
                if self.nativePlayVisuals then self.nativePlayVisuals(7, 4) end
                pending.phase = 5
            end
            if pending.phase == 5 and pending.elapsed >= base + 5.45 then
                self.nativePlay(18) -- Fire-imbued melee beat.
                if self.nativePlayVisuals then self.nativePlayVisuals(7, 5) end
                pending.phase = 6
            end
            if pending.phase == 6 and pending.elapsed >= base + 6.90 then
                self.nativePlay(52) -- SpellPrecastDirected: prepare Lightning Bolt.
                if self.nativePlayVisuals then self.nativePlayVisuals(7, 6) end
                pending.phase = 7
            end
            if pending.phase == 7 and pending.elapsed >= base + 7.75 then
                self.nativePlay(53) -- Release Lightning Bolt.
                if self.nativePlayVisuals then self.nativePlayVisuals(7, 7) end
                pending.phase = 8
            end
            if pending.phase == 8 and pending.elapsed >= base + 8.95 then
                self.nativePlay(52) -- SpellPrecastDirected: prepare Lava Burst.
                if self.nativePlayVisuals then self.nativePlayVisuals(7, 8) end
                pending.phase = 9
            end
            if pending.phase == 9 and pending.elapsed >= base + 9.80 then
                self.nativePlay(53) -- Release Lava Burst.
                if self.nativePlayVisuals then self.nativePlayVisuals(7, 9) end
                pending.phase = 10
            end
            if pending.phase == 10 and pending.elapsed >= base + 11.50 then
                self:SetModelVisible(true)
                if self.nativeClearVisuals then self.nativeClearVisuals() end
                self.nativePlay(profile[4])
                self.pending = nil
            end
            return
        end

        -- Warlock: summon, prepare/release Shadow Bolt, charge/release Chaos Bolt,
        -- then recover into preparation for Rain of Fire while Chaos remains in flight.
        if self.classID == 9 then
            local base = pending.animationBase
            if pending.phase == 2 and pending.elapsed >= base + 1.25 then
                self.nativePlay(52) -- Prepare Shadow Bolt for 1.10 seconds.
                if self.nativePlayVisuals then self.nativePlayVisuals(9, 2) end
                pending.phase = 3
            end
            if pending.phase == 3 and pending.elapsed >= base + 2.35 then
                self.nativePlay(53)
                if self.nativePlayVisuals then self.nativePlayVisuals(9, 3) end
                pending.phase = 4
            end
            if pending.phase == 4 and pending.elapsed >= base + 2.80 then
                self.nativePlay(28) -- Ready stance; keep the travelling missile alive.
                pending.phase = 5
            end
            if pending.phase == 5 and pending.elapsed >= base + 3.85 then
                self.nativePlay(52) -- Charge Chaos Bolt with legacy fel hands.
                if self.nativePlayVisuals then self.nativePlayVisuals(9, 4) end
                pending.phase = 6
            end
            if pending.phase == 6 and pending.elapsed >= base + 5.35 then
                self.nativePlay(53)
                if self.nativePlayVisuals then self.nativePlayVisuals(9, 5) end
                pending.phase = 7
            end
            if pending.phase == 7 and pending.elapsed >= base + 5.80 then
                self.nativePlay(52) -- Prepare Rain; do not hold the cast's ending pose.
                pending.phase = 8
            end
            if pending.phase == 8 and pending.elapsed >= base + 6.85 then
                self.nativePlay(124)
                if self.nativePlayVisuals then self.nativePlayVisuals(9, 6) end
                pending.phase = 9
            end
            if pending.phase == 9 and pending.elapsed >= base + 10.40 then
                if self.nativeClearVisuals then self.nativeClearVisuals() end
                self.nativePlay(profile[4])
                self.pending = nil
            end
            return
        end

        local secondAt = pending.animationBase + (profile[6] - profile[5])
        local ultimateAt = pending.animationBase + (profile[7] - profile[5])
        local readyAt = pending.animationBase + (profile[8] - profile[5])
        if pending.phase == 2 and pending.elapsed >= secondAt then
            self.nativePlay(profile[2])
            if self.nativePlayVisuals then self.nativePlayVisuals(self.classID, 2) end
            pending.phase = 3
        end
        if pending.phase == 3 and pending.elapsed >= ultimateAt then
            self.nativePlay(profile[3])
            if self.nativePlayVisuals then self.nativePlayVisuals(self.classID, 3) end
            pending.phase = 4
        end
        if pending.phase == 4 and pending.elapsed >= readyAt then
            if self.nativeClearVisuals then self.nativeClearVisuals() end
            self.nativePlay(profile[4]) -- Persistent combat-ready idle.
            self.pending = nil
        end
    end

    if not owner.frame then
        owner.frame = CreateFrame("Frame")
        owner.frame:SetScript("OnUpdate", function(_, elapsed) owner:OnUpdate(elapsed) end)
    end
end
)lua";
}

bool wxl_character_creation_preview::InstallCharacterCreationPreview()
{
    const WXL_FrameScriptApi* framescript = FrameScript();
    if (!framescript) return false;

    bool ok = true;
    ok &= g_api && g_api->HookAttachByName &&
        g_api->HookAttachByName(
            "M2.CharAddHandItem", reinterpret_cast<void*>(&hkCharAddHandItem),
            reinterpret_cast<void**>(&g_originalCharAddHandItem),
            WXL_HOOK_DEFAULT_PRIORITY) != 0;
    ok &= framescript->RegisterCVar("wxlCharacterCreatePreview", "1") != 0;
    ok &= framescript->RegisterFunction(
        "_WXL_CC_PREVIEW_PLAY_ANIMATION", &LuaPlayAnimation) != 0;
    ok &= framescript->RegisterFunction(
        "_WXL_CC_PREVIEW_TRACE", &LuaTrace) != 0;
    ok &= framescript->RegisterFunction(
        "_WXL_CC_PREVIEW_INSPECT", &LuaInspect) != 0;
    ok &= framescript->RegisterFunction(
        "_WXL_CC_PREVIEW_PLAY_VISUALS", &LuaPlayVisuals) != 0;
    ok &= framescript->RegisterFunction(
        "_WXL_CC_PREVIEW_CLEAR_VISUALS", &LuaClearVisuals) != 0;
    ok &= framescript->RegisterFunction(
        "_WXL_CC_PREVIEW_RESTORE_EQUIPMENT", &LuaRestoreEquipment) != 0;
    ok &= framescript->RegisterFunction(
        "_WXL_CC_PREVIEW_SET_WEAPONS_SHEATHED", &LuaSetWeaponsSheathed) != 0;
    ok &= framescript->RegisterFunction(
        "_WXL_CC_PREVIEW_SET_MODEL_VISIBLE", &LuaSetCharacterVisible) != 0;
    ok &= framescript->RegisterScript(
        "wxl-character-creation-preview", kBootstrap) != 0;

    if (g_api && g_api->Subscribe)
    {
        g_api->Subscribe(uint32_t(ev::Event::OnM2PerFrameUpdate), &OnM2PerFrame, nullptr);
        g_api->Subscribe(uint32_t(ev::Event::OnUpdate), &OnUpdate, nullptr);
    }

    if (ok)
        WLOG_INFO("registered Glue class-preview choreography and visuals (10 stock classes)");
    else
        WLOG_ERROR("failed to register one or more FrameScript surfaces");
    return ok;
}
