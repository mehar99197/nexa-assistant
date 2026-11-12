#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace nexa {

class AdbDiscovery;
class AndroidDevice;
class AtspiBus;
class BrowserService;
class GlobalShortcuts;
class HelperClient;
class McpConnectors;
class MemoryStore;
class PairingManager;
class PhoneLink;
class RemoteAccess;
class RemoteServer;
class SherpaRuntime;
class SpeakerGate;
class VoicePipeline;
class SkillCatalog;

// What the GUI needs to show about the parts of Nexa that live outside it: the root helper,
// whichever backend can type and click, and the phone. All of them can change while Nexa runs —
// installing the helper switches the input backend, a phone comes and goes — so these are looked
// up when shown, not copied.
struct SystemStatus {
    HelperClient *helper{nullptr};
    std::function<QString()> inputSummary;      // one line for the status bar
    std::function<QStringList()> inputNotes;    // every candidate and why it was or was not used
    AndroidDevice *phone{nullptr};
    AdbDiscovery *discovery{nullptr};
    RemoteServer *remote{nullptr};
    RemoteAccess *remoteAccess{nullptr};    // away from home: Tailscale, Cloudflare, ngrok
    PairingManager *pairing{nullptr};
    PhoneLink *phoneLink{nullptr};          // pairs Nexa Phone over adb (Settings → Android)
    McpConnectors *mcp{nullptr};            // the MCP servers' state (Settings → Connectors)
    GlobalShortcuts *shortcuts{nullptr};
    BrowserService *browser{nullptr};       // Playwright: installed? which browser? (status bar, Settings → Browser)
    MemoryStore *memory{nullptr};           // Settings → Memory
    SkillCatalog *skills{nullptr};          // Settings → Skills
    AtspiBus *a11y{nullptr};                // the accessibility bus: the screen as text (Settings → General)
    SherpaRuntime *sherpa{nullptr};         // instant spotting installed? (Settings → Voice)
    VoicePipeline *voice{nullptr};          // records the owner's voice for enrolment
    SpeakerGate *speakerGate{nullptr};      // and turns it into their profile
    std::function<QString()> microphoneSummary;     // "fine", or what is wrong with its sound
    std::function<void()> lowerMicrophone;          // Settings → Voice → Lower it
    // Settings → Providers → sign-in: a chat turn is in flight (opencode's event stream and agy's app-data folder
    // must not be disturbed), and a sign-in or sign-out succeeded (the providers are built again).
    std::function<bool()> replyRunning;
    std::function<void()> providersChanged;
};

} // namespace nexa
