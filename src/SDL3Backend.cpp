#include <vector>
#include <SDL3/SDL.h>
#include <Windows.h>
#include <hidsdi.h>
#include <cstring>

#include "SDL3Backend.h"

namespace XidiSDL3Plugin
{
    using namespace Xidi;

    static constexpr int kMaxPhysicalControllers = 4;

    static std::vector<SDL_Gamepad*> gamepads(kMaxPhysicalControllers, nullptr);
    static int gamepadCount = kMaxPhysicalControllers;

    /// Finds the first physical controller slot that does not currently have a gamepad
    /// assigned to it. Returns -1 if every slot is occupied.
    static int FindFreeGamepadSlot()
    {
        for (int i = 0; i < gamepadCount; i++)
        {
            if (gamepads[i] == nullptr)
                return i;
        }

        return -1;
    }

    /// Finds the physical controller slot currently holding the gamepad with the given SDL
    /// joystick instance ID. Returns -1 if no slot holds that instance.
    static int FindGamepadSlotByInstanceId(SDL_JoystickID instanceId)
    {
        for (int i = 0; i < gamepadCount; i++)
        {
            if ((gamepads[i] != nullptr) && (SDL_GetGamepadID(gamepads[i]) == instanceId))
                return i;
        }

        return -1;
    }

    /// Opens a gamepad (identified by SDL joystick instance ID) and places it into a free
    /// physical controller slot, if one is available. Used both for the initial scan at
    /// startup and for SDL_EVENT_GAMEPAD_ADDED hotplug events.
    static void OpenGamepadForHotplug(SDL_JoystickID instanceId)
    {
        // Guard against duplicate/late add events for a device we already have open.
        if (FindGamepadSlotByInstanceId(instanceId) != -1)
            return;

        const int slot = FindFreeGamepadSlot();
        if (slot == -1)
            return; // all physical controller slots are in use; ignore this device (log when ability added)

        SDL_Gamepad* gp = SDL_OpenGamepad(instanceId);
        if (gp == nullptr)
            return; // failed to open the device (log when ability added)

        gamepads[slot] = gp;
    }

    /// Closes and clears out the physical controller slot holding the gamepad with the given
    /// SDL joystick instance ID, if any. Used for SDL_EVENT_GAMEPAD_REMOVED hotplug events.
    static void CloseGamepadForHotplug(SDL_JoystickID instanceId)
    {
        const int slot = FindGamepadSlotByInstanceId(instanceId);
        if (slot == -1)
            return;

        SDL_CloseGamepad(gamepads[slot]);
        gamepads[slot] = nullptr;
    }

    /// Drains pending SDL gamepad hotplug events (connect/disconnect) and updates the
    /// physical controller slots accordingly. SDL reports hotplug purely as events on its
    /// event queue, so this has to pump that queue; it is cheap and safe to call often, and
    /// returns immediately once there is nothing left pending.
    static void ProcessHotplugEvents()
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_EVENT_GAMEPAD_ADDED:
                OpenGamepadForHotplug(event.gdevice.which);
                break;

            case SDL_EVENT_GAMEPAD_REMOVED:
                CloseGamepadForHotplug(event.gdevice.which);
                break;

            default:
                break;
            }
        }
    }

    /// Populates as many physical controller slots as possible with gamepads that are
    /// already connected at startup, before any hotplug events have had a chance to fire.
    static void ScanForGamepads()
    {
        int connectedCount = 0;
        SDL_JoystickID* connectedIDs = SDL_GetGamepads(&connectedCount);
        if (connectedIDs == nullptr)
            return;

        for (int i = 0; i < connectedCount; i++)
            OpenGamepadForHotplug(connectedIDs[i]);

        SDL_free(connectedIDs);
    }

    std::wstring_view SDL3Backend::PluginName()
    {
        return L"SDL3";
    }

    bool SDL3Backend::Initialize()
    {
        if (!SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"))
            return false;
        if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC))
            return false;

        // Fill in whatever physical controller slots already have a gamepad connected.
        // Anything that connects or disconnects afterward is picked up as a hotplug event
        // the next time ReadInputState() runs.
        ScanForGamepads();

        return true;
    }

    TPhysicalControllerIndex SDL3Backend::MaxPhysicalControllerCount()
    {
        return gamepadCount;
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

        for (int i = 0; i < gamepadCount; i++)
        {
            if (gamepads[i] == nullptr)
                continue;

            if (attributes.VendorID == SDL_GetGamepadVendor(gamepads[i]) &&
                attributes.ProductID == SDL_GetGamepadProduct(gamepads[i]))
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
        // Pump SDL's event queue for gamepad hotplug (connect/disconnect) notifications and
        // update our physical controller slots accordingly. Safe to call on every poll of
        // every physical controller index; if nothing is pending it returns immediately.
        ProcessHotplugEvents();

        SDL_UpdateGamepads();
        SDL_Gamepad* gp = gamepads[physicalControllerIndex];

        if (gp == nullptr)
            return {.deviceStatus = Controller::EPhysicalDeviceStatus::NotConnected};

        if (!SDL_GamepadConnected(gp))
        {
            SDL_CloseGamepad(gp);
            gamepads[physicalControllerIndex] = nullptr;

            return {
                .deviceStatus =
                    Controller::EPhysicalDeviceStatus::NotConnected
            };
        }

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
        SDL_Gamepad* gp = gamepads[physicalControllerIndex];
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
