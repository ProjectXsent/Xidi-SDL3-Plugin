#include <mutex>
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

    // Maximum number of physical controllers this backend will track at once. SDL3
    // gamepads can be hotplugged at any time, so rather than growing/shrinking a
    // dynamically-sized array (which would shift indices out from under callers who
    // cache a TPhysicalControllerIndex), a fixed number of slots is reserved up front
    // and handed out/reclaimed as controllers connect and disconnect. Adjust as needed.
    static constexpr int kMaxPhysicalControllerCount = 16;

    // Guards all access to the gamepad slot table below. The SDL hotplug event watcher
    // can be invoked synchronously from whatever thread happens to be pumping SDL
    // events -- in this backend, that is any thread calling SDL_UpdateGamepads() (e.g.
    // from ReadInputState or WriteForceFeedbackState) -- so the table needs protecting
    // against concurrent access from another thread doing the same thing.
    static std::mutex gamepadsMutex;

    // Fixed-size table of gamepad slots, sized kMaxPhysicalControllerCount. A nullptr
    // entry means that slot is currently unoccupied. Once a controller occupies a slot,
    // its physical controller index does not change for as long as it stays connected,
    // and even across a disconnect/reconnect provided the slot has not since been
    // claimed by a different controller.
    static std::vector<SDL_Gamepad*> gamepads(kMaxPhysicalControllerCount, nullptr);

    // Opens the given joystick instance as a gamepad and stores it in the first free
    // slot, if one is available. If every slot is occupied the controller is simply not
    // made visible to Xidi until another controller disconnects and frees one up.
    // Caller must hold gamepadsMutex.
    static void AddGamepadForJoystickID(SDL_JoystickID joystickID)
    {
        for (int i = 0; i < kMaxPhysicalControllerCount; i++)
        {
            if (gamepads[i] == nullptr)
            {
                SDL_Gamepad* gp = SDL_OpenGamepad(joystickID);
                if (gp == nullptr)
                    ; // output error when ability added;

                gamepads[i] = gp;
                return;
            }
        }
    }

    // Closes and clears whichever slot currently holds the given joystick instance, if
    // any. Caller must hold gamepadsMutex.
    static void RemoveGamepadForJoystickID(SDL_JoystickID joystickID)
    {
        for (int i = 0; i < kMaxPhysicalControllerCount; i++)
        {
            SDL_Gamepad* gp = gamepads[i];
            if ((gp != nullptr) && (SDL_GetGamepadID(gp) == joystickID))
            {
                SDL_CloseGamepad(gp);
                gamepads[i] = nullptr;
                return;
            }
        }
    }

    // SDL event watch callback used to detect gamepad hotplug events, registered with
    // SDL_AddEventWatch during Initialize(). SDL invokes this synchronously on whatever
    // thread pumps the responsible event -- in this backend that means any thread that
    // calls SDL_UpdateGamepads(), since that implicitly processes device add/remove
    // notifications even without a full SDL_PollEvent loop.
    static bool SDLCALL HandleSDLGamepadEvent(void* userdata, SDL_Event* event)
    {
        switch (event->type)
        {
        case SDL_EVENT_GAMEPAD_ADDED:
        {
            std::lock_guard lock(gamepadsMutex);
            AddGamepadForJoystickID(event->gdevice.which);
            break;
        }

        case SDL_EVENT_GAMEPAD_REMOVED:
        {
            std::lock_guard lock(gamepadsMutex);
            RemoveGamepadForJoystickID(event->gdevice.which);
            break;
        }

        default:
            break;
        }

        return true;
    }

    bool SDL3Backend::Initialize()
    {
        if (!SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"))
            return false;
        if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC))
            return false;

        {
            std::lock_guard lock(gamepadsMutex);

            int connectedCount = 0;
            SDL_JoystickID* joystickIDs = SDL_GetGamepads(&connectedCount);
            if (joystickIDs == nullptr)
                return false;

            for (int i = 0; (i < connectedCount) && (i < kMaxPhysicalControllerCount); i++)
                AddGamepadForJoystickID(joystickIDs[i]);

            SDL_free(joystickIDs);
        }

        // From this point on, controllers connecting or disconnecting are picked up by
        // HandleSDLGamepadEvent, which keeps the gamepads table current.
        if (!SDL_AddEventWatch(&HandleSDLGamepadEvent, nullptr))
            return false;

        return true;
    }

    TPhysicalControllerIndex SDL3Backend::MaxPhysicalControllerCount()
    {
        // This is the fixed size of the slot table, not the number of controllers
        // currently connected -- a slot for a disconnected controller still counts
        // towards it until a different controller claims that slot.
        return kMaxPhysicalControllerCount;
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

        std::lock_guard lock(gamepadsMutex);

        for (int i = 0; i < kMaxPhysicalControllerCount; i++)
        {
            SDL_Gamepad* gp = gamepads[i];
            if (gp == nullptr)
                continue;

            if (attributes.VendorID == SDL_GetGamepadVendor(gp) &&
                attributes.ProductID == SDL_GetGamepadProduct(gp))
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
        SDL_UpdateGamepads();

        std::lock_guard lock(gamepadsMutex);

        if ((physicalControllerIndex < 0) || (physicalControllerIndex >= kMaxPhysicalControllerCount))
             return {.deviceStatus = Controller::EPhysicalDeviceStatus::NotConnected};

        SDL_Gamepad* gp = gamepads[physicalControllerIndex];

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
        {
            std::lock_guard lock(gamepadsMutex);

            if ((physicalControllerIndex < 0) || (physicalControllerIndex >= kMaxPhysicalControllerCount))
                return false;

            SDL_Gamepad* gp = gamepads[physicalControllerIndex];
            if (gp == nullptr)
                return false;

            SDL_PropertiesID properties = SDL_GetGamepadProperties(gp);

            if (SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false))
                SDL_RumbleGamepad(gp, vibrationState.leftMotor, vibrationState.rightMotor, 250);

            if (SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false))
                SDL_RumbleGamepadTriggers(gp, vibrationState.leftImpulseTrigger, vibrationState.rightImpulseTrigger, 250);
        }

        SDL_UpdateGamepads();
        return true;
    }
}
