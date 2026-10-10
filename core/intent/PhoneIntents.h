#pragma once

#include "core/intent/IIntentHandler.h"

namespace nexa {

// Fast path for the phone: one link of the IntentRouter chain, tried before the PC handlers so
// "open youtube on my phone" is not read as a PC app called "youtube on my phone".
//
// Anything that could be ordinary chat needs the phone named: "call mom" is left to the model,
// "call mom on my phone" and "call 0300 1234567" are not. A wrong guess here dials someone.
//
//   on my phone open youtube · open whatsapp on my phone          → android_open_app
//   see my phone · what's on my phone · phone screenshot          → android_screenshot, then the model
//   phone volume 30 · phone volume up                             → android_volume
//   pause my phone · next song on my phone · phone play           → android_media
//   call 0300 1234567 · call mom on my phone                      → android_call
//   text mom on my phone saying I'm late                          → android_sms_send (composes, not sends)
//   read my notifications · phone notifications                   → android_notifications, then the model
//   read my texts · check my sms                                  → android_sms_read, then the model
//   lock my phone · go home on my phone · phone back              → android_key
class PhoneIntent : public IIntentHandler {
public:
    std::optional<IntentMatch> match(const IntentInput &input) const override;
};

} // namespace nexa
