#include "LaunchIntent.h"

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#endif

namespace nexa {

#ifdef Q_OS_ANDROID

LaunchIntent LaunchIntent::fromActivity() {
    LaunchIntent intent;
    const QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (!activity.isValid()) return intent;
    const QJniObject launch = activity.callObjectMethod("getIntent", "()Landroid/content/Intent;");
    if (!launch.isValid()) return intent;
    const auto extra = [&launch](const char *key) {
        const QJniObject value = launch.callObjectMethod("getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;",
                                                         QJniObject::fromString(QString::fromLatin1(key)).object<jstring>());
        return value.isValid() ? value.toString().trimmed() : QString();
    };
    intent.address = extra("nexa.address");
    intent.code = extra("nexa.code");
    intent.name = extra("nexa.name");
    return intent;
}

#else

LaunchIntent LaunchIntent::fromActivity() {
    return {};
}

#endif

} // namespace nexa
