#include "app/client_config.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <set>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"

namespace genko::app::client_config {

using core::Json;

namespace {

constexpr qint64 kLargest = 16 * 1024 * 1024;  // (a settings file larger than this is not one Genko changes)

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }
fs::path fpath(const QString& text) { return core::path_from_utf8(text.toStdString()); }

QString env(const char* name) { return qEnvironmentVariable(name); }

std::optional<QByteArray> read_all(const fs::path& path, QString* why = nullptr) {
    QFile file(qpath(path));
    if (!file.exists()) return std::nullopt;
    if (!file.open(QIODevice::ReadOnly)) {
        if (why != nullptr) *why = QStringLiteral("設定ファイルを読めません（%1）").arg(file.errorString());
        return std::nullopt;
    }
    if (file.size() > kLargest) {
        if (why != nullptr) *why = QStringLiteral("設定ファイルが大きすぎます（16 MB まで）");
        return std::nullopt;
    }
    return file.readAll();
}

Json strings_json(const QStringList& parts) {
    Json out = Json::array();
    for (const QString& part : parts) out.push_back(part.toStdString());
    return out;
}

QString dumped(const Json& value) { return QString::fromStdString(core::dump_python(value)); }

// What is shown of Genko's entry: its command and its arguments (never the other keys' values: env can hold tokens).
QString shown_entry(const QString& command, const QString& args) {
    QStringList rows;
    if (!command.isEmpty()) rows << QStringLiteral("command: %1").arg(command);
    if (!args.isEmpty()) rows << QStringLiteral("args: %1").arg(args);
    return rows.join(QLatin1Char('\n'));
}

fs::path backup_path(const fs::path& file) {
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString base = qpath(file) + QStringLiteral(".genko-backup-") + stamp;
    QString candidate = base;
    for (int n = 2; QFileInfo::exists(candidate); ++n) candidate = QStringLiteral("%1-%2").arg(base).arg(n);
    return fpath(candidate);
}

// What undo() keeps of a file it replaces: named apart from the copies apply() makes (backups() lists only those), so
// another undo never puts back what was undone.
fs::path aside_path(const fs::path& file) {
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString base = qpath(file) + QStringLiteral(".genko-before-undo-") + stamp;
    QString candidate = base;
    for (int n = 2; QFileInfo::exists(candidate); ++n) candidate = QStringLiteral("%1-%2").arg(base).arg(n);
    return fpath(candidate);
}

const QByteArray kUtf8Bom("\xEF\xBB\xBF");

// Why bytes are not UTF-8 text Genko can change (a UTF-16 or UTF-32 file says so by its BOM); empty when they are.
QString not_utf8(const QByteArray& bytes) {
    if (bytes.startsWith(QByteArray("\xFF\xFE", 2)) || bytes.startsWith(QByteArray("\xFE\xFF", 2)) ||
        bytes.startsWith(QByteArray("\x00\x00\xFE\xFF", 4))) {  // (UTF-16 LE/BE, UTF-32 LE — FF FE 00 00 — and BE)
        return QStringLiteral("設定ファイルが UTF-8 ではありません（先頭の BOM から、UTF-16 か UTF-32 で書かれています）。Genko からは書き換えません");
    }
    const QByteArray text = bytes.startsWith(kUtf8Bom) ? bytes.mid(3) : bytes;
    if (QString::fromUtf8(text).toUtf8() != text) return QStringLiteral("設定ファイルが UTF-8 ではありません。Genko からは書き換えません");
    return {};
}

// The first key that an object of the JSON text has twice (its JSON pointer), if any.
struct DuplicateKeys : nlohmann::json_sax<Json> {
    std::vector<std::set<std::string>> seen;
    std::vector<std::string> where;
    std::optional<std::string> found;
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t) override { return true; }
    bool number_unsigned(number_unsigned_t) override { return true; }
    bool number_float(number_float_t, const string_t&) override { return true; }
    bool string(string_t&) override { return true; }
    bool binary(binary_t&) override { return true; }
    bool start_object(std::size_t) override {
        seen.emplace_back();
        return true;
    }
    bool key(string_t& name) override {
        if (!seen.back().insert(name).second && !found) found = name;
        return !found;
    }
    bool end_object() override {
        seen.pop_back();
        return true;
    }
    bool start_array(std::size_t) override { return true; }
    bool end_array() override { return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
};

std::optional<std::string> duplicate_key(const QByteArray& text) {
    DuplicateKeys sax;
    const std::string body = text.startsWith(kUtf8Bom) ? text.mid(3).toStdString() : text.toStdString();
    (void)Json::sax_parse(body, &sax, nlohmann::detail::input_format_t::json, false);
    return sax.found;
}

// --- claude_desktop_config.json ---------------------------------------------------------------------------------------

void propose_json(Proposal& out, const QStringList& server) {
    Json root = Json::object();
    const bool bom = out.before.startsWith(kUtf8Bom);
    if (out.exists && !out.before.trimmed().isEmpty()) {
        if (const QString why = not_utf8(out.before); !why.isEmpty()) {
            out.refusal = why;
            return;
        }
        core::ParseRepairs repairs;
        try {
            core::ParseOptions options;
            options.bytes = true;  // (a UTF-8 BOM at the start is read past, and written back)
            root = core::parse_python_json(out.before.toStdString(), &repairs, options);
        } catch (const core::Error& error) {
            out.refusal = QStringLiteral("設定ファイルを JSON として読めません（%1）。Genko からは書き換えません").arg(QString::fromUtf8(error.what()));
            return;
        }
        // (values that would not be written back as they are: Genko never changes such a file)
        if (!repairs.nonfinite.empty()) {
            out.refusal = QStringLiteral("設定ファイルに、NaN・Infinity や 1e400 のような、ふつうの数で表せない数があります（%1）。Genko からは書き換えません")
                              .arg(QString::fromStdString(repairs.nonfinite.front()));
            return;
        }
        if (!repairs.inexact.empty()) {
            out.refusal = QStringLiteral("設定ファイルに、64 ビットに収まらない大きな整数があります（%1）。Genko からは書き換えません")
                              .arg(QString::fromStdString(repairs.inexact.front()));
            return;
        }
        if (const auto twice = duplicate_key(out.before)) {
            out.refusal = QStringLiteral("設定ファイルの同じオブジェクトに、同じ鍵（%1）が二つあります。Genko からは書き換えません")
                              .arg(QString::fromStdString(*twice));
            return;
        }
        if (!root.is_object()) {
            out.refusal = QStringLiteral("設定ファイルの中身が JSON のオブジェクト（{ … }）ではありません。Genko からは書き換えません");
            return;
        }
    }
    if (root.contains("mcpServers") && !root["mcpServers"].is_object()) {
        out.refusal = QStringLiteral("設定ファイルの mcpServers がオブジェクトではありません。Genko からは書き換えません");
        return;
    }
    Json servers = root.contains("mcpServers") ? root["mcpServers"] : Json::object();
    if (servers.contains("genko") && !servers["genko"].is_object()) {
        out.refusal = QStringLiteral("設定ファイルの mcpServers.genko がオブジェクトではありません。Genko からは書き換えません");
        return;
    }
    const Json command = server.value(0).toStdString();
    const Json args = strings_json(server.mid(1));
    Json entry = servers.contains("genko") ? servers["genko"] : Json::object();
    if (servers.contains("genko")) {
        out.entry_before = shown_entry(entry.contains("command") ? dumped(entry["command"]) : QString(),
                                       entry.contains("args") ? dumped(entry["args"]) : QString());
        out.same = entry.contains("command") && entry["command"] == command && entry.contains("args") && entry["args"] == args;
        QStringList others;
        for (const auto& [key, value] : entry.items()) {
            (void)value;
            if (key != "command" && key != "args") others << QString::fromStdString(key);
        }
        if (!others.isEmpty()) out.kept << QStringLiteral("genko のほかの項目: %1").arg(others.join(QStringLiteral(", ")));
    }
    entry["command"] = command;
    entry["args"] = args;
    out.entry_after = shown_entry(dumped(command), dumped(args));
    QStringList names;
    for (const auto& [key, value] : servers.items()) {
        (void)value;
        if (key != "genko") names << QString::fromStdString(key);
    }
    if (!names.isEmpty()) out.kept << QStringLiteral("ほかのサーバー: %1").arg(names.join(QStringLiteral(", ")));
    QStringList settings;
    for (const auto& [key, value] : root.items()) {
        (void)value;
        if (key != "mcpServers") settings << QString::fromStdString(key);
    }
    if (!settings.isEmpty()) out.kept << QStringLiteral("ほかの設定: %1").arg(settings.join(QStringLiteral(", ")));
    servers["genko"] = entry;
    root["mcpServers"] = servers;
    if (bom) out.kept << QStringLiteral("先頭の BOM（UTF-8 の印）");
    out.after = (bom ? kUtf8Bom : QByteArray()) + QByteArray::fromStdString(core::dump_python_indent2(root) + "\n");
}

// --- ~/.hermes/config.yaml (only the block style Genko's own text uses) ---------------------------------------------------

struct Line {
    QString text;  // without its line end
    int indent() const {
        int n = 0;
        while (n < text.size() && text[n] == QLatin1Char(' ')) ++n;
        return n;
    }
    bool neutral() const {  // blank or a comment: it neither starts nor ends a block
        const QString t = text.trimmed();
        return t.isEmpty() || t.startsWith(QLatin1Char('#'));
    }
};

// "key: rest" at this indent: the key (quotes taken off) and the rest; nothing for another kind of line.
std::optional<std::pair<QString, QString>> key_line(const Line& line, int indent) {
    if (line.indent() != indent) return std::nullopt;
    static const QRegularExpression kKey(QStringLiteral("^(\"[^\"]*\"|'[^']*'|[^\\s\"'#:\\-][^:#]*?|-[^\\s:#][^:#]*?)\\s*:(\\s.*|)$"));
    const auto match = kKey.match(line.text.mid(indent));
    if (!match.hasMatch()) return std::nullopt;
    QString key = match.captured(1).trimmed();
    if (key.size() >= 2 && (key.startsWith(QLatin1Char('"')) || key.startsWith(QLatin1Char('\'')))) key = key.mid(1, key.size() - 2);
    return std::pair{key, match.captured(2)};
}

// Why a key cannot be read as a plain key Genko may write next to: an explicit key ("? …"), a flow collection, an
// anchor, an alias, a tag, a block scalar, a directive, a merge key ("<<"). Empty when it can.
QString odd_key(const QString& key) {
    static const QString kOdd = QStringLiteral("?{[&*!|>%@`");
    if (key == QLatin1String("<<")) return QStringLiteral("<< で別の鍵を取り込んでいる");
    if (!key.isEmpty() && kOdd.contains(key.front())) return QStringLiteral("「%1」で始まる鍵や値（YAML の別の書き方）").arg(key.front());
    return {};
}

// Whether YAML text has one of `marks` (& an anchor, * an alias, ! a tag) where a node starts: at the start, or after a
// space, "[", "{" or ","; quoted text is read past, and a comment ends it. (Text in a plain scalar after a space is
// taken for one too: nothing is written then rather than something that might be wrong.)
bool has_node_mark(const QString& text, const QString& marks) {
    QChar quote;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text[i];
        if (!quote.isNull()) {
            if (quote == QLatin1Char('"') && ch == QLatin1Char('\\')) {
                ++i;
            } else if (ch == quote) {
                if (quote == QLatin1Char('\'') && i + 1 < text.size() && text[i + 1] == QLatin1Char('\'')) {
                    ++i;  // ('' inside '…')
                } else {
                    quote = QChar();
                }
            }
            continue;
        }
        const bool starts = i == 0 || text[i - 1].isSpace() || text[i - 1] == QLatin1Char('[') || text[i - 1] == QLatin1Char('{') ||
                            text[i - 1] == QLatin1Char(',');
        if (!starts) continue;
        if (ch == QLatin1Char('#')) return false;
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            quote = ch;
        } else if (marks.contains(ch)) {
            return true;
        }
    }
    return false;
}

// The rest of a key line holds nothing but perhaps a comment: a block follows.
bool opens_block(const QString& rest) {
    const QString t = rest.trimmed();
    return t.isEmpty() || t.startsWith(QLatin1Char('#'));
}

void propose_yaml(Proposal& out, const QStringList& server) {
    const QString command = dumped(server.value(0).toStdString());
    const QString args = dumped(strings_json(server.mid(1)));
    out.entry_after = shown_entry(command, args);
    if (out.exists) {
        if (const QString why = not_utf8(out.before); !why.isEmpty()) {
            out.refusal = why;
            return;
        }
    }
    const bool bom = out.before.startsWith(kUtf8Bom);  // (read past, and written back)
    const QString text = QString::fromUtf8(bom ? out.before.mid(3) : out.before);
    if (bom) out.kept << QStringLiteral("先頭の BOM（UTF-8 の印）");
    const QByteArray lead = bom ? kUtf8Bom : QByteArray();
    const bool crlf = text.contains(QStringLiteral("\r\n"));
    const QString eol = crlf ? QStringLiteral("\r\n") : QStringLiteral("\n");
    // Python's text (ai_link.snippet), for a file that has no mcp_servers yet
    const QString fresh = QStringLiteral("mcp_servers:\n  genko:\n    command: %1\n    args: %2\n    timeout: 60\n").arg(command, args);
    if (!out.exists || text.trimmed().isEmpty()) {
        out.after = lead + QString(fresh).replace(QStringLiteral("\n"), eol).toUtf8();
        return;
    }
    QStringList raw = text.split(QLatin1Char('\n'));
    const bool ends_with_eol = text.endsWith(QLatin1Char('\n'));
    if (ends_with_eol) raw.removeLast();
    std::vector<Line> lines;
    for (QString row : raw) {
        if (row.endsWith(QLatin1Char('\r'))) row.chop(1);
        lines.push_back(Line{row});
    }
    const auto refuse = [&](const QString& why) {
        out.refusal = QStringLiteral("設定ファイルの形（%1）は Genko からは安全に書き換えられません。下の文を手で貼ってください").arg(why);
    };
    // (one "---" before everything else starts the one document: kept, and read past)
    std::optional<std::size_t> start;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].neutral()) continue;
        if (lines[i].text == QLatin1String("---") || lines[i].text.startsWith(QLatin1String("--- #"))) start = i;
        break;
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Line& line = lines[i];
        int n = 0;
        while (n < line.text.size() && (line.text[n] == QLatin1Char(' ') || line.text[n] == QLatin1Char('\t'))) ++n;
        if (line.text.left(n).contains(QLatin1Char('\t'))) return refuse(QStringLiteral("タブの字下げ"));
        const QString t = line.text.trimmed();
        if (start && i == *start) continue;
        if (line.indent() == 0 && (t == QLatin1String("---") || t.startsWith(QLatin1String("--- ")) || t == QLatin1String("..."))) {
            return refuse(QStringLiteral("複数の文書・文書の区切り"));
        }
    }
    const QString start_text = start ? lines[*start].text : QString();
    if (start) lines[*start].text = QStringLiteral("# ---");  // (read as a comment below; written back as it was)
    // the top-level keys: every line at the left edge is a plain "key: …" (a flow document, an explicit "? key", an
    // anchor, an alias, a tag, a block scalar or a directive there is not read: nothing is written then)
    std::optional<std::size_t> at;
    QStringList settings;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].neutral() || lines[i].indent() != 0) continue;
        if (lines[i].text.startsWith(QLatin1Char('-'))) return refuse(QStringLiteral("一番上がリスト"));
        if (const QString why = odd_key(lines[i].text); !why.isEmpty()) return refuse(why);
        const auto key = key_line(lines[i], 0);
        if (!key) return refuse(QStringLiteral("一番上に、鍵として読めない行がある: %1").arg(lines[i].text.left(40)));
        if (const QString why = odd_key(key->first); !why.isEmpty()) return refuse(why);
        if (key->first == QLatin1String("mcp_servers")) {
            if (at) return refuse(QStringLiteral("mcp_servers が二つ"));
            at = i;
        } else {
            settings << key->first;
        }
    }
    if (!settings.isEmpty()) out.kept << QStringLiteral("ほかの設定: %1").arg(settings.join(QStringLiteral(", ")));
    std::vector<QString> result;
    for (const Line& line : lines) result.push_back(line.text);
    if (!at) {  // no mcp_servers: Python's text after the rest
        if (start) result[*start] = start_text;
        QString joined;
        for (const QString& row : result) joined += row + eol;
        out.after = lead + (joined + QString(fresh).replace(QStringLiteral("\n"), eol)).toUtf8();
        return;
    }
    const std::size_t i = *at;
    const QString rest = key_line(lines[i], 0)->second.trimmed();
    if (!opens_block(rest)) {
        if (rest == QLatin1String("{}") || rest == QLatin1String("null") || rest == QLatin1String("~")) {
            result[i] = QStringLiteral("mcp_servers:");  // (an empty map written inline: made a block)
        } else {
            return refuse(QStringLiteral("mcp_servers が一行で書かれている"));
        }
    }
    std::size_t j = i + 1;  // the end of mcp_servers' block
    while (j < lines.size() && (lines[j].neutral() || lines[j].indent() > 0)) ++j;
    int c = -1;
    for (std::size_t k = i + 1; k < j; ++k) {
        if (!lines[k].neutral()) c = c < 0 ? lines[k].indent() : std::min(c, lines[k].indent());
    }
    if (c < 0) c = 2;
    const int d = c * 2;
    std::optional<std::size_t> genko;
    QStringList names;
    for (std::size_t k = i + 1; k < j; ++k) {
        if (lines[k].neutral() || lines[k].indent() != c) continue;
        const auto key = key_line(lines[k], c);
        if (!key) return refuse(QStringLiteral("mcp_servers の中がリスト"));
        if (const QString why = odd_key(key->first); !why.isEmpty()) return refuse(why);
        if (key->first == QLatin1String("genko")) {
            if (genko) return refuse(QStringLiteral("genko が二つ"));
            if (has_node_mark(key->second, QStringLiteral("&*!"))) return refuse(QStringLiteral("genko にアンカー・別名・タグ（& * !）が付いている"));
            if (!opens_block(key->second)) return refuse(QStringLiteral("genko が一行で書かれている"));
            genko = k;
        } else {
            names << key->first;
        }
    }
    if (!names.isEmpty()) out.kept.prepend(QStringLiteral("ほかのサーバー: %1").arg(names.join(QStringLiteral(", "))));
    const QString pad_c(c, QLatin1Char(' '));
    const QString pad_d(d, QLatin1Char(' '));
    const QString command_line = pad_d + QStringLiteral("command: ") + command;
    const QString args_line = pad_d + QStringLiteral("args: ") + args;
    if (!genko) {  // a new entry after the last server
        std::size_t after = i;
        for (std::size_t k = i + 1; k < j; ++k) {
            if (!lines[k].neutral()) after = k;
        }
        const std::vector<QString> added = {pad_c + QStringLiteral("genko:"), command_line, args_line, pad_d + QStringLiteral("timeout: 60")};
        result.insert(result.begin() + static_cast<std::ptrdiff_t>(after + 1), added.begin(), added.end());
    } else {
        const std::size_t k0 = *genko;
        std::size_t m = k0 + 1;  // the end of genko's block
        while (m < j && (lines[m].neutral() || lines[m].indent() > c)) ++m;
        int dd = -1;
        for (std::size_t k = k0 + 1; k < m; ++k) {
            if (!lines[k].neutral()) dd = dd < 0 ? lines[k].indent() : std::min(dd, lines[k].indent());
        }
        const int key_indent = dd < 0 ? d : dd;
        const QString pad_k(key_indent, QLatin1Char(' '));
        struct Part {
            QString key;
            std::size_t from, to;
            QString rest;
        };
        std::vector<Part> parts;
        for (std::size_t k = k0 + 1; k < m; ++k) {
            if (lines[k].neutral() || lines[k].indent() != key_indent) continue;
            const auto key = key_line(lines[k], key_indent);
            if (!key) return refuse(QStringLiteral("genko の中がリスト"));
            if (const QString why = odd_key(key->first); !why.isEmpty()) return refuse(why);
            if (!parts.empty()) parts.back().to = k;
            parts.push_back(Part{key->first, k, m, key->second.trimmed()});
        }
        // (a part's lines end with its last line of content: the comments and blank lines after it stay where they are)
        const auto content_end = [&](const Part& part) {
            std::size_t end = part.from + 1;
            for (std::size_t k = part.from + 1; k < part.to; ++k) {
                if (!lines[k].neutral()) end = k + 1;
            }
            return end;
        };
        // (command's and args' values are written over: an anchor, an alias or a tag on them — on the key's line or the
        // lines of its value — is not)
        for (const Part& part : parts) {
            if (part.key != QLatin1String("command") && part.key != QLatin1String("args")) continue;
            bool marked = has_node_mark(part.rest, QStringLiteral("&*!"));
            for (std::size_t k = part.from + 1; k < content_end(part) && !marked; ++k) {
                marked = !lines[k].neutral() && has_node_mark(lines[k].text, QStringLiteral("&*!"));
            }
            if (marked) return refuse(QStringLiteral("genko の %1 の値にアンカー・別名・タグ（& * !）が付いている").arg(part.key));
        }
        // (an anchor anywhere in genko's block: what Genko writes there could cut it off from what refers to it)
        for (std::size_t k = k0 + 1; k < m; ++k) {
            if (!lines[k].neutral() && has_node_mark(lines[k].text, QStringLiteral("&"))) {
                return refuse(QStringLiteral("genko の中にアンカー（&）がある"));
            }
        }
        const auto tail = [&](const Part& part, std::vector<QString>& into) {
            for (std::size_t k = content_end(part); k < part.to; ++k) into.push_back(lines[k].text);
        };
        std::vector<QString> block;
        // (lines of genko's block before its first key: comments, kept)
        for (std::size_t k = k0 + 1; k < (parts.empty() ? m : parts.front().from); ++k) block.push_back(lines[k].text);
        QString old_command, old_args;
        bool has_command = false, has_args = false, has_timeout = false;
        QStringList others;
        for (const Part& part : parts) {
            if (part.key == QLatin1String("command")) {
                old_command = part.rest;
                if (!has_command) block.push_back(pad_k + QStringLiteral("command: ") + command);  // (not when args came first)
                has_command = true;
                if (!has_args && std::none_of(parts.begin(), parts.end(), [](const Part& p) { return p.key == QLatin1String("args"); })) {
                    block.push_back(pad_k + QStringLiteral("args: ") + args);
                    has_args = true;
                }
                tail(part, block);
                continue;
            }
            if (part.key == QLatin1String("args")) {
                if (!has_command) {
                    block.push_back(pad_k + QStringLiteral("command: ") + command);
                    has_command = true;
                }
                has_args = true;
                old_args = part.rest;
                block.push_back(pad_k + QStringLiteral("args: ") + args);
                tail(part, block);
                continue;
            }
            if (part.key == QLatin1String("timeout")) has_timeout = true;
            others << part.key;
            for (std::size_t k = part.from; k < part.to; ++k) block.push_back(lines[k].text);
        }
        if (!has_command) block.insert(block.begin(), {pad_k + QStringLiteral("command: ") + command, pad_k + QStringLiteral("args: ") + args});
        if (!has_timeout) {
            // (after the last line of content: the comments and blank lines that end the block stay at its end)
            std::size_t at_end = block.size();
            while (at_end > 0 && Line{block[at_end - 1]}.neutral()) --at_end;
            block.insert(block.begin() + static_cast<std::ptrdiff_t>(at_end), pad_k + QStringLiteral("timeout: 60"));
        }
        if (!others.isEmpty()) out.kept.prepend(QStringLiteral("genko のほかの項目: %1").arg(others.join(QStringLiteral(", "))));
        out.entry_before = shown_entry(old_command, old_args);
        out.same = old_command == command && old_args == args && has_timeout;
        result.erase(result.begin() + static_cast<std::ptrdiff_t>(k0 + 1), result.begin() + static_cast<std::ptrdiff_t>(m));
        result.insert(result.begin() + static_cast<std::ptrdiff_t>(k0 + 1), block.begin(), block.end());
    }
    if (start) result[*start] = start_text;
    QString joined;
    for (const QString& row : result) joined += row + eol;  // (the file ends with a line end after this)
    out.after = lead + joined.toUtf8();
}

bool write_bytes(const fs::path& file, const QByteArray& bytes, QString* why) {
    QDir().mkpath(qpath(file.parent_path()));
    QSaveFile save(qpath(file));
    if (!save.open(QIODevice::WriteOnly) || save.write(bytes) != bytes.size() || !save.commit()) {
        *why = save.errorString();
        return false;
    }
    return true;
}

}  // namespace

std::optional<fs::path> msix_claude_file(const QString& local_appdata) {
    if (local_appdata.isEmpty()) return std::nullopt;
    const QDir packages(local_appdata + QStringLiteral("/Packages"));
    for (const QString& name : packages.entryList({QStringLiteral("Claude_*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString claude = packages.filePath(name) + QStringLiteral("/LocalCache/Roaming/Claude");
        if (QFileInfo(claude).isDir()) return fpath(claude) / "claude_desktop_config.json";
    }
    return std::nullopt;
}

std::optional<fs::path> default_file(const QString& client) {
    const fs::path home = fpath(QDir::homePath());
    if (client == QLatin1String("claude-desktop")) {
#if defined(_WIN32)
        // (the Microsoft Store / MSIX install keeps its settings in its package's own folder: looked at first; SPEC
        // AI-01, to be seen on a real Windows in M6)
        if (const auto msix = msix_claude_file(env("LOCALAPPDATA"))) return msix;
        const QString appdata = env("APPDATA");
        return (appdata.isEmpty() ? home / "AppData" / "Roaming" : fpath(appdata)) / "Claude" / "claude_desktop_config.json";
#elif defined(__APPLE__)
        return home / "Library" / "Application Support" / "Claude" / "claude_desktop_config.json";
#else
        const QString config = env("XDG_CONFIG_HOME");
        return (config.isEmpty() ? home / ".config" : fpath(config)) / "Claude" / "claude_desktop_config.json";
#endif
    }
    if (client == QLatin1String("hermes")) return home / ".hermes" / "config.yaml";
    return std::nullopt;
}

Proposal propose(const QString& client, const fs::path& file, const QStringList& server) {
    Proposal out;
    out.client = client;
    out.file = file;
    QString why;
    const auto bytes = read_all(file, &why);
    if (!why.isEmpty()) {
        out.exists = true;
        out.refusal = why;
        return out;
    }
    out.exists = bytes.has_value();
    out.before = bytes.value_or(QByteArray());
    if (client == QLatin1String("claude-desktop")) {
        propose_json(out, server);
    } else if (client == QLatin1String("hermes")) {
        propose_yaml(out, server);
    } else {
        out.refusal = QStringLiteral("この AI の設定は Genko からは書き換えません。上の文をコピーして使ってください");
    }
    if (out.refusal.isEmpty() && out.exists && !out.same) out.backup = backup_path(file);
    if (out.same) out.after = out.before;
    return out;
}

Applied apply(const Proposal& proposal) {
    if (!proposal.refusal.isEmpty()) throw core::Error("io", proposal.refusal.toStdString());
    QString why;
    const auto now = read_all(proposal.file, &why);
    if (!why.isEmpty()) throw core::Error("io", why.toStdString());
    if (now.has_value() != proposal.exists || now.value_or(QByteArray()) != proposal.before) {
        throw core::Error("io", "設定ファイルが、変更案を作った後に変わりました。もう一度「設定ファイルに書き込む…」から確かめてください");
    }
    Applied applied;
    applied.client = proposal.client;
    applied.file = proposal.file;
    applied.written = proposal.after;
    if (proposal.same) return applied;
    if (proposal.exists) {
        if (proposal.backup.empty() || QFileInfo::exists(qpath(proposal.backup))) {
            throw core::Error("io", "バックアップの置き場所が使えません。もう一度確かめてください");
        }
        if (!QFile::copy(qpath(proposal.file), qpath(proposal.backup))) {
            throw core::Error("io", "バックアップを作れませんでした（" + qpath(proposal.backup).toStdString() + "）。設定ファイルは変えていません");
        }
        const auto copied = read_all(proposal.backup);
        if (!copied || *copied != proposal.before) {
            QFile::remove(qpath(proposal.backup));
            throw core::Error("io", "バックアップを確かめられませんでした。設定ファイルは変えていません");
        }
        applied.backup = proposal.backup;
    }
    const QFileDevice::Permissions permissions = QFileInfo(qpath(proposal.file)).permissions();
    if (!write_bytes(proposal.file, proposal.after, &why)) {
        throw core::Error("io", "設定ファイルに書き込めませんでした（" + why.toStdString() + "）。元のファイルはそのままです");
    }
    if (proposal.exists) QFile::setPermissions(qpath(proposal.file), permissions);
    return applied;
}

bool untouched(const Applied& applied) {
    const auto now = read_all(applied.file);
    return now && *now == applied.written;
}

fs::path undo(const Applied& applied) {
    fs::path aside;
    const auto now = read_all(applied.file);
    if (now && *now != applied.written) {  // (changed since: kept aside before it is replaced)
        aside = aside_path(applied.file);
        if (!QFile::copy(qpath(applied.file), qpath(aside))) {
            throw core::Error("io", "今の設定ファイルを残せなかったので、元に戻しませんでした");
        }
    }
    if (applied.backup.empty()) {
        if (now && !QFile::remove(qpath(applied.file))) throw core::Error("io", "Genko が作った設定ファイルを消せませんでした");
        return aside;
    }
    const auto before = read_all(applied.backup);
    if (!before) throw core::Error("io", "バックアップ（" + qpath(applied.backup).toStdString() + "）を読めません");
    QString why;
    const QFileDevice::Permissions permissions = QFileInfo(qpath(applied.backup)).permissions();
    if (!write_bytes(applied.file, *before, &why)) throw core::Error("io", "設定ファイルを元に戻せませんでした（" + why.toStdString() + "）");
    QFile::setPermissions(qpath(applied.file), permissions);
    return aside;
}

std::vector<fs::path> backups(const fs::path& file) {
    const QFileInfo info(qpath(file));
    const QDir folder = info.dir();
    QStringList names = folder.entryList({info.fileName() + QStringLiteral(".genko-backup-*")}, QDir::Files, QDir::Name);
    std::reverse(names.begin(), names.end());
    std::vector<fs::path> out;
    for (const QString& name : names) out.push_back(fpath(folder.filePath(name)));
    return out;
}

bool configured(const QString& client, const std::optional<fs::path>& file) {
    if (!file) return false;
    const Proposal now = propose(client, *file, {QStringLiteral("genko")});
    return now.refusal.isEmpty() && !now.entry_before.isEmpty();
}

}  // namespace genko::app::client_config
