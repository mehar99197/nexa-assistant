#pragma once

namespace nexa {

class AndroidDevice;
class FileAccess;
class ToolRegistry;

// Registers the phone tools — android_status, android_open_app, android_list_apps, android_tap,
// android_swipe, android_type, android_key, android_screenshot, android_notifications,
// android_sms_read, android_sms_send, android_call, android_media, android_volume,
// android_push_file, android_pull_file, android_install_apk, android_shell — acting through
// `device`. `files` confines the PC side of file transfers to the same folders as read_file.
void registerAndroidTools(ToolRegistry &registry, AndroidDevice *device, FileAccess *files);

} // namespace nexa
