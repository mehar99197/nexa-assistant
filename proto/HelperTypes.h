#pragma once

#include <QLatin1StringView>

namespace nexa {

// Names shared by Nexa and nexa-helper. They must match packaging/org.nexa.Helper.{conf,service},
// packaging/org.nexa.helper.policy and proto/org.nexa.Helper.xml.
namespace helper {

inline constexpr QLatin1StringView kService{"org.nexa.Helper"};
inline constexpr QLatin1StringView kPath{"/org/nexa/Helper"};
inline constexpr QLatin1StringView kInterface{"org.nexa.Helper"};

// The one polkit action, auth_admin_keep: the user is asked for a password once per login.
//
// One action and not two (running commands as root, typing and clicking) because the two cannot
// meaningfully be told apart: anything that can type into any window can open a terminal and run
// whatever it likes. Two actions only meant two password prompts for the same power.
inline constexpr QLatin1StringView kActionControl{"org.nexa.helper.control"};

// D-Bus error names the helper returns, so the client can tell cases apart without matching text.
inline constexpr QLatin1StringView kErrorDenied{"org.nexa.Helper.Error.Denied"};
inline constexpr QLatin1StringView kErrorInvalidArgument{"org.nexa.Helper.Error.InvalidArgument"};
inline constexpr QLatin1StringView kErrorNoInputDevice{"org.nexa.Helper.Error.NoInputDevice"};
inline constexpr QLatin1StringView kErrorNoSuchJob{"org.nexa.Helper.Error.NoSuchJob"};
inline constexpr QLatin1StringView kErrorFailed{"org.nexa.Helper.Error.Failed"};

// OutputChunk streams.
inline constexpr quint8 kStdout = 1;
inline constexpr quint8 kStderr = 2;

// MouseButton arguments.
enum MouseButton : quint32 { ButtonLeft = 1, ButtonRight = 2, ButtonMiddle = 3 };

// Finished statuses.
inline constexpr QLatin1StringView kStatusExited{"exited"};
inline constexpr QLatin1StringView kStatusCrashed{"crashed"};
inline constexpr QLatin1StringView kStatusTimeout{"timeout"};
inline constexpr QLatin1StringView kStatusCancelled{"cancelled"};

} // namespace helper

} // namespace nexa
