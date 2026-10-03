#include "core/SceneToml.h"

#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>

#include <sstream>
#include <toml++/toml.hpp>

namespace rostrum::toml_io {

namespace {

std::string s(const QString &v) { return v.toStdString(); }

QString qs(const toml::node_view<const toml::node> &n, const QString &fallback = {})
{
    if (auto v = n.value<std::string>()) {
        return QString::fromStdString(*v);
    }
    return fallback;
}

double num(const toml::node_view<const toml::node> &n, double fallback)
{
    if (auto v = n.value<double>()) {
        return *v;
    }
    return fallback;
}

bool flag(const toml::node_view<const toml::node> &n, bool fallback)
{
    if (auto v = n.value<bool>()) {
        return *v;
    }
    return fallback;
}

toml::date_time toToml(const QDateTime &dt)
{
    const QDateTime u = dt.toUTC();
    const QDate d = u.date();
    const QTime t = u.time();
    return toml::date_time{toml::date{d.year(), unsigned(d.month()), unsigned(d.day())},
                           toml::time{unsigned(t.hour()), unsigned(t.minute()), unsigned(t.second())},
                           toml::time_offset{}};
}

QDateTime fromToml(const toml::node_view<const toml::node> &n)
{
    if (auto v = n.value<toml::date_time>()) {
        const QDate d(int(v->date.year), int(v->date.month), int(v->date.day));
        const QTime t(int(v->time.hour), int(v->time.minute), int(v->time.second));
        const int offsetMin = v->offset ? v->offset->minutes : 0;
        return QDateTime(d, t, QTimeZone::fromSecondsAheadOfUtc(offsetMin * 60)).toUTC();
    }
    if (auto v = n.value<std::string>()) {
        return QDateTime::fromString(QString::fromStdString(*v), Qt::ISODate).toUTC();
    }
    return {};
}

toml::table sceneTable(const Scene &scene)
{
    toml::table t;
    t.insert("name", s(scene.name));
    t.insert("master", toml::table{
                           {"phones", scene.masterPhones},
                           {"phones_muted", scene.masterPhonesMuted},
                           {"stream", scene.masterStream},
                           {"stream_muted", scene.masterStreamMuted},
                       });
    t.insert("mic", toml::table{{"sidetone", scene.sidetoneVolume}});

    toml::array buses;
    for (const auto &b : scene.buses) {
        buses.push_back(toml::table{
            {"id", s(b.id)},
            {"name", s(b.name)},
            {"kind", b.isInput() ? "input" : "playback"},
            {"color", s(b.color)},
            {"volume", b.volume},
            {"muted", b.muted},
            {"destination", s(destinationName(b.destination))},
            {"auto", s(categoryName(b.autoCategory))},
        });
    }
    t.insert("bus", std::move(buses));

    toml::array rules;
    for (const auto &r : scene.rules) {
        toml::table rt{
            {"match", s(r.match)},
            {"key", s(matchKeyName(r.key))},
            {"bus", s(r.busId)},
            {"volume", r.volume},
        };
        if (r.muted) {
            rt.insert("muted", true);
        }
        if (!r.label.isEmpty()) {
            rt.insert("label", s(r.label));
        }
        if (r.lastSeen.isValid()) {
            rt.insert("last_seen", toToml(r.lastSeen));
        }
        rules.push_back(std::move(rt));
    }
    if (!rules.empty()) {
        t.insert("rule", std::move(rules));
    }
    return t;
}

Scene sceneFrom(const toml::table &t)
{
    const toml::node_view<const toml::node> v{t};
    Scene scene;
    scene.buses.clear();
    scene.name = qs(v["name"], QString::fromLatin1(kDefaultSceneName));
    scene.masterPhones = num(v["master"]["phones"], 1.0);
    scene.masterPhonesMuted = flag(v["master"]["phones_muted"], false);
    scene.masterStream = num(v["master"]["stream"], 1.0);
    scene.masterStreamMuted = flag(v["master"]["stream_muted"], false);
    scene.sidetoneVolume = num(v["mic"]["sidetone"], 0.0);

    if (const auto *buses = t["bus"].as_array()) {
        for (const auto &node : *buses) {
            const auto *bt = node.as_table();
            if (!bt) {
                continue;
            }
            const toml::node_view<const toml::node> bv{*bt};
            Bus b;
            b.id = qs(bv["id"]);
            b.name = qs(bv["name"], b.id);
            b.kind = qs(bv["kind"]) == QLatin1String("input") ? BusKind::Input : BusKind::Playback;
            b.color = qs(bv["color"]);
            b.volume = num(bv["volume"], 1.0);
            b.muted = flag(bv["muted"], false);
            b.destination = destinationFromString(qs(bv["destination"])).value_or(defaults::destinationFor(b.id));
            // Scenes saved before automatic assignment existed: the default buses keep their job.
            b.autoCategory = categoryFromString(qs(bv["auto"])).value_or(defaults::autoCategoryFor(b.id));
            scene.buses.append(b);
        }
    }
    if (const auto *rules = t["rule"].as_array()) {
        for (const auto &node : *rules) {
            const auto *rt = node.as_table();
            if (!rt) {
                continue;
            }
            const toml::node_view<const toml::node> rv{*rt};
            AppRule r;
            r.match = qs(rv["match"]);
            r.key = matchKeyFromString(qs(rv["key"])).value_or(MatchKey::Name);
            r.busId = qs(rv["bus"]);
            r.volume = num(rv["volume"], 1.0);
            r.muted = flag(rv["muted"], false);
            r.label = qs(rv["label"]).trimmed().left(64);
            r.lastSeen = fromToml(rv["last_seen"]);
            scene.rules.append(r);
        }
    }
    return sanitize(scene);
}

QString toText(const toml::table &t)
{
    // Fader levels are written as 0.8, not 0.80000000000000004; 15 digits is plenty for a fader.
    std::ostringstream out;
    out << toml::toml_formatter{t, toml::toml_formatter::default_flags | toml::format_flags::relaxed_float_precision}
        << '\n';
    return QString::fromStdString(out.str());
}

} // namespace

Scene sanitize(Scene scene)
{
    static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    static const QRegularExpression idChars(QStringLiteral("^[a-z0-9][a-z0-9-]{0,31}$"));
    static const QSet<QString> reserved = {QStringLiteral("phones"), QStringLiteral("stream"),
                                           QStringLiteral("sidetone")};
    const Scene defaults = defaults::scene();

    scene.name = scene.name.trimmed().isEmpty() ? QString::fromLatin1(kDefaultSceneName) : scene.name.trimmed();
    scene.masterPhones = std::clamp(scene.masterPhones, 0.0, 1.0);
    scene.masterStream = std::clamp(scene.masterStream, 0.0, 1.0);
    scene.sidetoneVolume = std::clamp(scene.sidetoneVolume, 0.0, 1.0);

    QList<Bus> out;
    QSet<QString> seen;
    // The mic bus always exists and always comes first.
    Bus mic = *defaults.micBus();
    for (const auto &b : scene.buses) {
        if (b.id == QLatin1String(kMicBusId)) {
            mic = b;
            mic.kind = BusKind::Input;
            break;
        }
    }
    out.append(mic);
    seen.insert(mic.id);
    for (Bus b : scene.buses) {
        if (b.id == QLatin1String(kMicBusId) || !idChars.match(b.id).hasMatch() || reserved.contains(b.id) ||
            seen.contains(b.id) || out.size() >= kMaxBuses) {
            continue;
        }
        b.kind = BusKind::Playback;
        seen.insert(b.id);
        out.append(b);
    }
    for (int k = 0; k < out.size(); ++k) {
        Bus &b = out[k];
        if (b.name.trimmed().isEmpty()) {
            b.name = b.id;
        }
        if (!hex.match(b.color).hasMatch()) {
            b.color = QString::fromLatin1(defaults::palette().at(k % defaults::palette().size()).hex);
        }
        b.volume = std::clamp(b.volume, 0.0, b.isInput() ? 1.5 : 1.0);
    }
    QSet<int> claimed;
    for (Bus &b : out) {
        if (b.isInput() || claimed.contains(int(b.autoCategory))) {
            b.autoCategory = AppCategory::None;
        } else if (b.autoCategory != AppCategory::None) {
            claimed.insert(int(b.autoCategory));
        }
    }
    scene.buses = out;

    QList<AppRule> rules;
    for (const auto &r : scene.rules) {
        const Bus *b = scene.bus(r.busId);
        if (r.match.trimmed().isEmpty() || !b || b->isInput()) {
            continue;
        }
        const bool dup = std::any_of(rules.cbegin(), rules.cend(), [&](const AppRule &x) {
            return x.key == r.key && x.match.compare(r.match, Qt::CaseInsensitive) == 0;
        });
        if (dup) {
            continue;
        }
        AppRule c = r;
        c.volume = std::clamp(c.volume, 0.0, 1.0);
        rules.append(c);
    }
    scene.rules = rules;
    return scene;
}

QString serializeScene(const Scene &scene)
{
    toml::table t;
    t.insert("format", kFormatVersion);
    for (auto &&[k, v] : sceneTable(scene)) {
        t.insert(k, v);
    }
    return toText(t);
}

std::optional<Scene> parseScene(const QString &text, QString *error)
{
    try {
        const toml::table t = toml::parse(text.toStdString());
        return sceneFrom(t);
    } catch (const toml::parse_error &e) {
        if (error) {
            *error = QStringLiteral("Line %1: %2")
                         .arg(e.source().begin.line)
                         .arg(QString::fromStdString(std::string(e.description())));
        }
        return std::nullopt;
    }
}

QString serializeBundle(const QList<Scene> &scenes)
{
    toml::table root;
    root.insert("format", kFormatVersion);
    root.insert("exported", toToml(QDateTime::currentDateTimeUtc()));
    toml::array arr;
    for (const auto &sc : scenes) {
        arr.push_back(sceneTable(sc));
    }
    root.insert("scene", std::move(arr));
    return toText(root);
}

QList<Scene> parseBundle(const QString &text, QString *error)
{
    QList<Scene> out;
    try {
        const toml::table t = toml::parse(text.toStdString());
        if (const auto *arr = t["scene"].as_array()) {
            for (const auto &node : *arr) {
                if (const auto *st = node.as_table()) {
                    out.append(sceneFrom(*st));
                }
            }
        } else if (t.contains("bus")) {
            // A single scene file is accepted too.
            out.append(sceneFrom(t));
        }
        if (out.isEmpty() && error) {
            *error = QStringLiteral("The file has no scenes in it.");
        }
    } catch (const toml::parse_error &e) {
        if (error) {
            *error = QStringLiteral("Line %1: %2")
                         .arg(e.source().begin.line)
                         .arg(QString::fromStdString(std::string(e.description())));
        }
    }
    return out;
}

} // namespace rostrum::toml_io
