#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace nexa {

class AdbDiscovery;
class AndroidDevice;
class BrowserService;
class GlobalShortcuts;
class HelperClient;
class McpConnectors;
class MemoryStore;
class PairingManager;
class PhoneLink;
class RemoteServer;
class SherpaRuntime;
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
    PairingManager *pairing{nullptr};
    PhoneLink *phoneLink{nullptr};          // pairs Nexa Phone over adb (Settings → Android)
    McpConnectors *mcp{nullptr};            // the MCP servers' state (Settings → Connectors)
    GlobalShortcuts *shortcuts{nullptr};
    BrowserService *browser{nullptr};       // Playwright: installed? which browser? (status bar, Settings → Browser)
    MemoryStore *memory{nullptr};           // Settings → Memory
    SkillCatalog *skills{nullptr};          // Settings → Skills
    SherpaRuntime *sherpa{nullptr};         // instant spotting installed? (Settings → Voice)
    std::function<QString()> microphoneSummary;     // "fine", or what is wrong with its sound
    std::function<void()> lowerMicrophone;          // Settings → Voice → Lower it
};

} // namespace nexa
