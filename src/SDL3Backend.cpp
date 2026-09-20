#include <vector>
#include <SDL3/SDL.h>
#include <Windows.h>
#include <hidsdi.h>

#include "SDL3Backend.h"

namespace XidiSDL3Plugin
{
    using namespace Xidi;

    std::wstring_view SDL3Backend::PluginName()
    {
        return L"SDL3";
    }

    // Each slot corresponds to one physical controller index that has ever been handed out.
    // Slots are never removed or reordered once created, so a physical controller index, once
    // seen by the rest of Xidi, always continues to refer to the same slot -- it just reports
    // NotConnected if the underlying device is unplugged. This mirrors how ReadInputState()
    // already distinguishes "never going to work" (Error) from "not currently connected"
    // (NotConnected).
    struct SGamepadSlot
    {
        SDL_JoystickID instanceID = 0;
        SDL_Gamepad* gamepad = nullptr;
    };

    // Xidi reads MaxPhysicalControllerCount() once, at startup, to decide how many slots exist,
    // and only ever calls ReadInputState() for indices in that range afterward. So the slot pool
    // must be pre-allocated to this fixed size up front -- NOT sized to however many controllers
    // happen to be plugged in when Initialize() runs -- or a controller plugged in later will
    // land in a slot Xidi never asks about. TODO: replace with whatever constant Xidi actually
    // uses for its controller limit (e.g. something alongside Controller::kPhysicalCapabilities...)
    // if one exists, instead of this hardcoded value.
    static constexpr size_t kMaxGamepadCount = 16;

    static std::vector<SGamepadSlot> gamepadSlots = {};

    // Finds the slot index currently holding the given SDL joystick instance ID, or -1 if none.
    static int FindSlotByInstanceID(SDL_JoystickID instanceID)
    {
        for (size_t i = 0; i < gamepadSlots.size(); i++)
        {
            if (gamepadSlots[i].gamepad != nullptr && gamepadSlots[i].instanceID == instanceID)
                return static_cast<int>(i);
        }

        return -1;
    }

    // Opens a newly-connected gamepad and places it into the first free pre-allocated slot. If
    // every slot is already occupied (kMaxGamepadCount controllers already connected), the new
    // controller is left unopened and ignored -- there's no slot for Xidi to read it from anyway.
    static void HandleGamepadAdded(SDL_JoystickID instanceID)
    {
        if (FindSlotByInstanceID(instanceID) >= 0)
            return;

        for (auto& slot : gamepadSlots)
        {
            if (slot.gamepad == nullptr)
            {
                SDL_Gamepad* gp = SDL_OpenGamepad(instanceID);
                if (gp == nullptr)
                    return; // output error when ability added;

                slot.instanceID = instanceID;
                slot.gamepad = gp;
                return;
            }
        }
    }

    // Closes a disconnected gamepad and frees its slot for reuse by a future device, while
    // leaving the slot (and therefore its physical controller index) in place.
    static void HandleGamepadRemoved(SDL_JoystickID instanceID)
    {
        int index = FindSlotByInstanceID(instanceID);
        if (index < 0)
            return;

        SDL_CloseGamepad(gamepadSlots[index].gamepad);
        gamepadSlots[index].gamepad = nullptr;
        gamepadSlots[index].instanceID = 0;
    }

    // Drives SDL's hardware hotplug detection and drains any resulting add/remove events.
    // SDL_UpdateGamepads() is what actually polls the platform for newly connected or
    // disconnected hardware and queues the corresponding events; SDL_PollEvent() then drains
    // them. Cheap to call often -- it's a no-op once the queue is empty.
    //
    // Note: this drains the *entire* SDL event queue, which is fine as long as this plugin owns
    // its own private SDL instance solely for controller input (as Initialize() below suggests).
    // If SDL ever ends up shared with other consumers in the host process, prefer
    // SDL_AddEventWatch() instead, since it observes events without removing them from the queue.
    static void ProcessHotplugEvents()
    {
        SDL_UpdateGamepads();

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_EVENT_GAMEPAD_ADDED:
                HandleGamepadAdded(event.gdevice.which);
                break;

            case SDL_EVENT_GAMEPAD_REMOVED:
                HandleGamepadRemoved(event.gdevice.which);
                break;

            default:
                break;
            }
        }
    }

    bool SDL3Backend::Initialize()
    {
        if (!SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"))
            return false;
        if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC))
            return false;

        // Make sure add/remove events are actually generated so ProcessHotplugEvents() can see them.
        SDL_SetGamepadEventsEnabled(true);

        // Pre-allocate every slot up front, regardless of how many controllers are actually
        // connected right now -- see the comment on kMaxGamepadCount above for why.
        gamepadSlots.resize(kMaxGamepadCount);

        int initialGamepadCount = 0;
        SDL_JoystickID* initialGamepadIDs = SDL_GetGamepads(&initialGamepadCount);
        if (initialGamepadIDs == nullptr)
            return false;

        for (auto i = 0; i < initialGamepadCount && static_cast<size_t>(i) < kMaxGamepadCount; i++)
        {
            SDL_Gamepad* gp = SDL_OpenGamepad(initialGamepadIDs[i]);
            if (gp == nullptr)
                continue; // output error when ability added;

            gamepadSlots[i].instanceID = initialGamepadIDs[i];
            gamepadSlots[i].gamepad = gp;
        }

        SDL_free(initialGamepadIDs);

        return true;
    }

    TPhysicalControllerIndex SDL3Backend::MaxPhysicalControllerCount()
    {
        return static_cast<TPhysicalControllerIndex>(gamepadSlots.size());
    }

    bool SDL3Backend::SupportsControllerByGuidAndPath(const wchar_t* guidAndPath)
    {
        HANDLE hDevice = CreateFileW(guidAndPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (hDevice == INVALID_HANDLE_VALUE)
            return false;

        HIDD_ATTRIBUTES attributes = { .Size = sizeof(HIDD_ATTRIBUTES) };
        HidD_GetAttributes(hDevice, &attributes);
        CloseHandle(hDevice);

        for (const auto& slot : gamepadSlots)
        {
            if (slot.instanceID == 0)
                continue;

            if (attributes.VendorID == SDL_GetGamepadVendorForID(slot.instanceID) &&
                attributes.ProductID == SDL_GetGamepadProductForID(slot.instanceID))
                return true;
        }

        return false;
    }

    SPhysicalControllerCapabilities SDL3Backend::GetCapabilities()
    {
        /* right now Xidi provides no way to alter this per-controller, so we claim to support everything */
        return {
            .stick = Controller::kPhysicalCapabilitiesAllAnalogSticks,
            .trigger = Controller::kPhysicalCapabilitiesAllAnalogTriggers,
            .button = Controller::kPhysicalCapabilitiesAllButtons,
            .forceFeedbackActuator = Controller::kPhysicalCapabilitiesAllForceFeedbackActuators
        };
    }

    SPhysicalControllerState SDL3Backend::ReadInputState(TPhysicalControllerIndex physicalControllerIndex)
    {
        ProcessHotplugEvents();

        if (physicalControllerIndex < 0 ||
            static_cast<size_t>(physicalControllerIndex) >= gamepadSlots.size())
            return {.deviceStatus = Controller::EPhysicalDeviceStatus::Error};

        SDL_Gamepad* gp = gamepadSlots[physicalControllerIndex].gamepad;

        if (gp == nullptr)
            return {.deviceStatus = Controller::EPhysicalDeviceStatus::NotConnected};

        if (!SDL_GamepadConnected(gp))
            return {.deviceStatus = Controller::EPhysicalDeviceStatus::NotConnected};

        SPhysicalControllerState state = {
            .deviceStatus = Controller::EPhysicalDeviceStatus::Ok,
            .stick = {
                /* vertical axes in SDL3 are inverted compared to what Xidi expects,
                 * and all axes are not clamped to -32767) so we need to do it manually here */
                std::max<Sint16>(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX), -32767),
                static_cast<int16_t>(-std::max<Sint16>(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY), -32767)),
                std::max<Sint16>(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX), -32767),
                static_cast<int16_t>(-std::max<Sint16>(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY), -32767))
            },
            .trigger = {
                /* Xidi expects triggers from 0-255, where SDL expects 0-32767, so integer divide by 128 */
                static_cast<uint8_t>(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 128),
                static_cast<uint8_t>(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 128)
            },
            .button = [&]() -> std::bitset<16>
            {
                /* this lambda is annoying but bitset doesn't support brace initialization, don't know of a better
                 * way to do this */
                std::bitset<16> button;

                button[static_cast<uint8_t>(Controller::EPhysicalButton::DpadUp)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::DpadDown)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::DpadLeft)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::DpadRight)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::Start)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::Back)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::LS)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_STICK);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::RS)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::LB)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::RB)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::Guide)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_GUIDE);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::Share)] =
                    /* games usually map the same control to share and touchpad, so use touchpad if available, otherwise
                    * use misc1 (which is share on Xbox controllers) */
                    SDL_GamepadHasButton(gp, SDL_GAMEPAD_BUTTON_TOUCHPAD) ?
                        SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_TOUCHPAD) :
                        SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_MISC1);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::A)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::B)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::X)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST);
                button[static_cast<uint8_t>(Controller::EPhysicalButton::Y)] =
                    SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_NORTH);

                return button;
            }()
        };

        return state;
    }

    bool SDL3Backend::WriteForceFeedbackState(TPhysicalControllerIndex physicalControllerIndex,
        SPhysicalControllerVibration vibrationState)
    {
        if (physicalControllerIndex < 0 ||
            static_cast<size_t>(physicalControllerIndex) >= gamepadSlots.size())
            return false;

        SDL_Gamepad* gp = gamepadSlots[physicalControllerIndex].gamepad;
        if (gp == nullptr)
            return false;

        SDL_PropertiesID properties = SDL_GetGamepadProperties(gp);

        if (SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false))
            SDL_RumbleGamepad(gp, vibrationState.leftMotor, vibrationState.rightMotor, 250);

        if (SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false))
            SDL_RumbleGamepadTriggers(gp, vibrationState.leftImpulseTrigger, vibrationState.rightImpulseTrigger, 250);

        SDL_UpdateGamepads();
        return true;
    }
}
