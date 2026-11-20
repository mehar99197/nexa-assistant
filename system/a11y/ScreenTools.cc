#include "system/a11y/ScreenTools.h"

#include "core/tools/FunctionTool.h"
#include "core/tools/ToolArguments.h"
#include "core/tools/ToolRegistry.h"
#include "proto/KeyMap.h"
#include "system/a11y/IScreenReader.h"
#include "system/a11y/ScreenOutline.h"
#include "system/input/IInputBackend.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include <memory>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

using namespace toolargs;

constexpr int kMaxReadChars = 20'000;
constexpr int kDefaultReadChars = 4'000;
constexpr int kPollMs = 500;
constexpr int kDefaultWaitMs = 10'000;
constexpr int kMaxWaitMs = 30'000;
constexpr qsizetype kMaxTypedCharacters = 2'000;

// What every tool here shares: the reader, the last outline (its refs are what the model names), and an anchor
// object whose lifetime ends with the tools, so a poll still pending then is dropped.
struct Shared {
    IScreenReader *reader{nullptr};
    std::function<IInputBackend *()> input;
    std::function<bool()> allowed;
    ScreenOutline current;
    quint64 generation{0};          // a wait cancelled or restarted leaves the old poll behind
    QObject anchor;
};

Result<atspi::NodeRef> refArgument(const Shared &shared, const QJsonObject &arguments) {
    const Result<QString> ref = requiredString(arguments, u"ref"_s);
    if (!ref) return ref.error();
    const QString id = ref.value().trimmed();
    const auto found = shared.current.refs.constFind(id);
    if (found == shared.current.refs.cend()) {
        return Error{ErrorCode::InvalidArgument,
                     u"There is no element %1 in the last screen_snapshot; take one again and use a ref from it."_s.arg(id)};
    }
    return *found;
}

QString shownName(const Shared &shared, const QString &id) {
    const QString name = shared.current.names.value(id);
    return name.isEmpty() ? atspi::roleName(shared.current.roles.value(id)) : u"\"%1\""_s.arg(name);
}

bool unavailable(const Shared &shared, ITool::Done &done) {
    if (shared.reader && shared.reader->isAvailable()) return false;
    done(Error{ErrorCode::NotSupported,
               u"The screen cannot be read as text here: %1"_s.arg(shared.reader ? shared.reader->unavailableReason()
                                                                                 : u"no accessibility bus."_s)});
    return true;
}

// The outline needs the words of the elements it shows, which the cache does not carry: one pass finds which, the
// reader fetches them, a second pass writes them in.
void snapshot(const std::shared_ptr<Shared> &shared, const OutlineOptions &options, std::function<void(Result<ScreenOutline>)> done) {
    shared->reader->apps([shared, options, done](Result<QList<AtspiApp>> apps) {
        if (!apps) {
            done(apps.error());
            return;
        }
        QList<IScreenReader::TextRequest> wanted;
        buildScreenOutline(apps.value(), options, [&wanted](const atspi::NodeRef &ref, int maxChars, bool fromEnd) {
            wanted.append({ref, maxChars, fromEnd});
            return QString();
        });
        shared->reader->texts(wanted, [shared, options, done, apps = apps.value()](QHash<atspi::NodeRef, QString> texts) {
            done(buildScreenOutline(apps, options, [&texts](const atspi::NodeRef &ref, int, bool) { return texts.value(ref); }));
        });
    });
}

OutlineOptions optionsFrom(const QJsonObject &arguments) {
    OutlineOptions options;
    if (const QString scope = arguments.value(u"scope"_s).toString().trimmed(); !scope.isEmpty()) options.scope = scope;
    options.filter = arguments.value(u"filter"_s).toString();
    return options;
}

bool isToggle(uint role) {
    using atspi::Role;
    return role == uint(Role::CheckBox) || role == uint(Role::ToggleButton) || role == uint(Role::RadioButton)
           || role == uint(Role::Switch) || role == uint(Role::CheckMenuItem) || role == uint(Role::RadioMenuItem);
}

void addSnapshot(ToolRegistry &registry, const std::shared_ptr<Shared> &shared) {
    ToolSpec spec{
        u"screen_snapshot"_s,
        u"Read the screen as text: the window in front as an outline in which every element has a ref like s12 — works "
        "whether or not you can see images. Act on an element with screen_click or screen_type, read a long text "
        "(a terminal, a document) with screen_read. scope: \"active\" (default), \"all\", or words of a window's title or "
        "app; filter keeps only the lines containing the given words."_s,
        schema::object({{u"scope"_s, schema::string(u"active | all | words of a window title or app name"_s)},
                        {u"filter"_s, schema::string(u"Keep only the lines containing these words"_s)}}),
    };
    spec.mutating = false;
    registry.add(std::make_unique<FunctionTool>(spec, [shared](const QJsonObject &arguments, ITool::Done done) {
        if (unavailable(*shared, done)) return;
        snapshot(shared, optionsFrom(arguments), [shared, done](Result<ScreenOutline> outline) {
            if (!outline) {
                done(outline.error());
                return;
            }
            shared->current = outline.value();
            done(text(outline.value().text));
        });
    }));
}

void addRead(ToolRegistry &registry, const std::shared_ptr<Shared> &shared) {
    ToolSpec spec{
        u"screen_read"_s,
        u"The whole text of a text element from screen_snapshot (a terminal, a document, an editor): from offset, at "
        "most max_chars characters (default 4000, up to 20000)."_s,
        schema::object({{u"ref"_s, schema::string(u"The element's ref from screen_snapshot"_s)},
                        {u"offset"_s, schema::integer(u"Start at this character (default 0)"_s, 0, 10'000'000)},
                        {u"max_chars"_s, schema::integer(u"How many characters at most"_s, 1, kMaxReadChars)}},
                       {u"ref"_s}),
    };
    spec.mutating = false;
    registry.add(std::make_unique<FunctionTool>(spec, [shared](const QJsonObject &arguments, ITool::Done done) {
        if (unavailable(*shared, done)) return;
        const Result<atspi::NodeRef> ref = refArgument(*shared, arguments);
        if (!ref) {
            done(ref.error());
            return;
        }
        const int offset = std::max(0, intArg(arguments, u"offset"_s).value_or(0));
        const int max = qBound(1, intArg(arguments, u"max_chars"_s).value_or(kDefaultReadChars), kMaxReadChars);
        shared->reader->text(ref.value(), offset, max, [done, offset, max](Result<IScreenReader::TextSlice> slice) {
            if (!slice) {
                done(slice.error());
                return;
            }
            const IScreenReader::TextSlice &value = slice.value();
            QString out = value.text;
            const int end = value.offset + int(value.text.size());
            if (end < value.total) {
                out += u"\n… %1 more characters; ask again with offset=%2."_s.arg(value.total - end).arg(end);
            } else if (offset > 0 || value.text.size() == max) {
                out += u"\n(the end; %1 characters in all)"_s.arg(value.total);
            }
            done(text(out));
        });
    }));
}

void addClick(ToolRegistry &registry, const std::shared_ptr<Shared> &shared) {
    ToolSpec spec{
        u"screen_click"_s,
        u"Click an element from screen_snapshot by its ref: the element's own action (click, press, toggle, jump…), "
        "or — when it has none — the keyboard focus and Enter (Space for a check box). action names one of its actions "
        "when the first is not the one wanted."_s,
        schema::object({{u"ref"_s, schema::string(u"The element's ref from screen_snapshot"_s)},
                        {u"action"_s, schema::string(u"The action's name, when not the first"_s)}},
                       {u"ref"_s}),
    };
    spec.privileged = true;
    spec.risk = u"clicks for you"_s;
    registry.add(std::make_unique<FunctionTool>(spec, [shared](const QJsonObject &arguments, ITool::Done done) {
        if (unavailable(*shared, done)) return;
        const Result<atspi::NodeRef> ref = refArgument(*shared, arguments);
        if (!ref) {
            done(ref.error());
            return;
        }
        const QString id = arguments.value(u"ref"_s).toString().trimmed();
        const QString wanted = arguments.value(u"action"_s).toString().trimmed().toLower();
        const QString name = shownName(*shared, id);
        const atspi::NodeRef node = ref.value();
        shared->reader->actions(node, [shared, node, id, name, wanted, done](Result<QStringList> actions) {
            if (!actions) {
                done(actions.error());
                return;
            }
            const QStringList names = actions.value();
            if (!names.isEmpty()) {
                int index = 0;
                if (!wanted.isEmpty()) {
                    index = -1;
                    for (qsizetype i = 0; i < names.size(); ++i) {
                        if (names.at(i).compare(wanted, Qt::CaseInsensitive) == 0) index = int(i);
                    }
                    if (index < 0) {
                        done(Error{ErrorCode::InvalidArgument, u"%1 (%2) has no action \"%3\"; it has: %4."_s.arg(name, id, wanted, names.join(u", "_s))});
                        return;
                    }
                }
                shared->reader->doAction(node, index, [done, name, id, action = names.at(index)](Result<void> result) {
                    if (!result) {
                        done(result.error());
                        return;
                    }
                    done(text(u"Clicked %1 (%2) — its \"%3\" action."_s.arg(name, id, action)));
                });
                return;
            }
            // Nothing to do on it: give it the keyboard and press the key that activates it.
            IInputBackend *backend = shared->input ? shared->input() : nullptr;
            if (!backend) {
                done(Error{ErrorCode::NotSupported,
                           u"%1 (%2) has no action of its own, and Nexa cannot press keys on this desktop: the root helper "
                           "is not installed (sudo ./packaging/install-helper.sh) and the desktop offers no remote-control portal."_s
                               .arg(name, id)});
                return;
            }
            const bool toggle = isToggle(shared->current.roles.value(id));
            shared->reader->grabFocus(node, [shared, backend, toggle, name, id, done](Result<void> focused) {
                if (!focused) {
                    done(focused.error());
                    return;
                }
                const QString key = toggle ? u"space"_s : u"enter"_s;
                const Result<QList<int>> codes = keymap::parseChord(key);
                if (!codes) {
                    done(codes.error());
                    return;
                }
                backend->pressKeys(codes.value(), [done, name, id, key](Result<QString> pressed) {
                    if (!pressed) {
                        done(pressed.error());
                        return;
                    }
                    done(text(u"Focused %1 (%2) and pressed %3."_s.arg(name, id, key == u"space"_s ? u"Space"_s : u"Enter"_s)));
                });
            });
        });
    }));
}

void addType(ToolRegistry &registry, const std::shared_ptr<Shared> &shared) {
    ToolSpec spec{
        u"screen_type"_s,
        u"Type into an element from screen_snapshot by its ref: an editable field gets the text set, anything else is "
        "given the keyboard focus and typed into. End with \\n for Enter."_s,
        schema::object({{u"ref"_s, schema::string(u"The element's ref from screen_snapshot"_s)},
                        {u"text"_s, schema::string(u"The text"_s)}},
                       {u"ref"_s, u"text"_s}),
    };
    spec.privileged = true;
    spec.risk = u"types for you"_s;
    registry.add(std::make_unique<FunctionTool>(spec, [shared](const QJsonObject &arguments, ITool::Done done) {
        if (unavailable(*shared, done)) return;
        const Result<atspi::NodeRef> ref = refArgument(*shared, arguments);
        if (!ref) {
            done(ref.error());
            return;
        }
        const Result<QString> value = requiredText(arguments, u"text"_s);
        if (!value) {
            done(value.error());
            return;
        }
        if (value.value().size() > kMaxTypedCharacters) {
            done(Error{ErrorCode::InvalidArgument, u"That is %1 characters; type at most %2 at a time."_s.arg(value.value().size()).arg(kMaxTypedCharacters)});
            return;
        }
        const QString id = arguments.value(u"ref"_s).toString().trimmed();
        const QString name = shownName(*shared, id);
        const atspi::NodeRef node = ref.value();
        const QString words = value.value();
        if (shared->current.editable.contains(id) && !words.contains(u'\n')) {
            shared->reader->setText(node, words, [done, name, id, count = words.size()](Result<void> result) {
                if (!result) {
                    done(result.error());
                    return;
                }
                done(text(u"Set the text of %1 (%2): %3 characters."_s.arg(name, id).arg(count)));
            });
            return;
        }
        IInputBackend *backend = shared->input ? shared->input() : nullptr;
        if (!backend) {
            done(Error{ErrorCode::NotSupported,
                       u"Nexa cannot type on this desktop: the root helper is not installed (sudo ./packaging/install-helper.sh) "
                       "and the desktop offers no remote-control portal."_s});
            return;
        }
        shared->reader->grabFocus(node, [backend, words, name, id, done](Result<void> focused) {
            if (!focused) {
                done(focused.error());
                return;
            }
            backend->typeText(words, [done, name, id, count = words.size(), backend](Result<QString> typed) {
                if (!typed) {
                    done(typed.error());
                    return;
                }
                QString summary = u"Focused %1 (%2) and typed %3 characters with the %4."_s.arg(name, id).arg(count).arg(backend->name());
                if (!typed.value().isEmpty()) summary += u" "_s + typed.value();
                done(text(summary));
            });
        });
    }));
}

void poll(const std::shared_ptr<Shared> &shared, const QStringList &words, const QString &phrase, QElapsedTimer started,
          int timeoutMs, quint64 generation, ITool::Done done) {
    if (generation != shared->generation) {
        done(Error{ErrorCode::Cancelled, u"Cancelled."_s});
        return;
    }
    if (shared->allowed && !shared->allowed()) {
        done(Error{ErrorCode::Cancelled, u"The screen is no longer shared."_s});
        return;
    }
    OutlineOptions options;
    snapshot(shared, options, [shared, words, phrase, started, timeoutMs, generation, done](Result<ScreenOutline> outline) {
        if (!outline) {
            done(outline.error());
            return;
        }
        if (generation != shared->generation) {
            done(Error{ErrorCode::Cancelled, u"Cancelled."_s});
            return;
        }
        shared->current = outline.value();
        const QString lower = outline.value().text.toLower();
        const bool found = std::all_of(words.cbegin(), words.cend(), [&lower](const QString &word) { return lower.contains(word); });
        if (found) {
            done(text(u"\"%1\" appeared after %2 s.\n%3"_s.arg(phrase).arg(started.elapsed() / 1000.0, 0, 'f', 1).arg(outline.value().text)));
            return;
        }
        if (started.elapsed() >= timeoutMs) {
            done(Error{ErrorCode::Timeout, u"\"%1\" did not appear within %2 s. The window in front shows:\n%3"_s.arg(phrase)
                                               .arg(timeoutMs / 1000.0, 0, 'f', 1)
                                               .arg(outline.value().text)});
            return;
        }
        QTimer::singleShot(kPollMs, &shared->anchor, [shared, words, phrase, started, timeoutMs, generation, done] {
            poll(shared, words, phrase, started, timeoutMs, generation, done);
        });
    });
}

void addWaitFor(ToolRegistry &registry, const std::shared_ptr<Shared> &shared) {
    ToolSpec spec{
        u"screen_wait_for"_s,
        u"Wait until the given words appear in the window in front (screen_snapshot is taken every half second), at "
        "most timeout_ms (default 10000, up to 30000). Answers with the outline that showed them."_s,
        schema::object({{u"text"_s, schema::string(u"The words to wait for"_s)},
                        {u"timeout_ms"_s, schema::integer(u"How long to wait at most"_s, 100, kMaxWaitMs)}},
                       {u"text"_s}),
    };
    spec.mutating = false;
    registry.add(std::make_unique<FunctionTool>(
        spec,
        [shared](const QJsonObject &arguments, ITool::Done done) {
            if (unavailable(*shared, done)) return;
            const Result<QString> phrase = requiredString(arguments, u"text"_s);
            if (!phrase) {
                done(phrase.error());
                return;
            }
            const QStringList words = phrase.value().simplified().toLower().split(u' ', Qt::SkipEmptyParts);
            if (words.isEmpty()) {
                done(Error{ErrorCode::InvalidArgument, u"Give the words to wait for."_s});
                return;
            }
            const int timeout = qBound(100, intArg(arguments, u"timeout_ms"_s).value_or(kDefaultWaitMs), kMaxWaitMs);
            QElapsedTimer started;
            started.start();
            poll(shared, words, phrase.value().simplified(), started, timeout, ++shared->generation, std::move(done));
        },
        [shared] { ++shared->generation; }));
}

} // namespace

void registerScreenTools(ToolRegistry &registry, const ScreenServices &services) {
    auto shared = std::make_shared<Shared>();
    shared->reader = services.reader;
    shared->input = services.input;
    shared->allowed = services.allowed;
    addSnapshot(registry, shared);
    addRead(registry, shared);
    addClick(registry, shared);
    addType(registry, shared);
    addWaitFor(registry, shared);
}

} // namespace nexa
