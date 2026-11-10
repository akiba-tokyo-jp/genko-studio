#include "service/result.hpp"

#include <QRegularExpression>
#include <QString>

#include <algorithm>

#include "service/wording.hpp"

namespace genko::service {

Issue error(std::string code, std::string path, std::string message, std::string hint) {
    return Issue{std::move(code), "error", std::move(path), std::move(message), std::move(hint)};
}

Issue warning(std::string code, std::string path, std::string message, std::string hint) {
    return Issue{std::move(code), "warning", std::move(path), std::move(message), std::move(hint)};
}

bool has_errors(const std::vector<Issue>& issues) { return studio::has_errors(issues); }

ToolResult::ToolResult(bool is_ok, Json with, std::vector<Issue> found, std::vector<std::string> pngs, std::vector<std::string> paths)
    : ok(is_ok), data(std::move(with)), issues(std::move(found)), images(std::move(pngs)), files(std::move(paths)) {}

Json ToolResult::to_dict() const {
    Json out = Json::object();
    out["ok"] = ok;
    for (const auto& [key, value] : data.items()) out[key] = value;
    std::vector<const Issue*> sorted;
    for (const Issue& issue : issues) sorted.push_back(&issue);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Issue* a, const Issue* b) {
        return (a->severity == "error") > (b->severity == "error");  // (sorted(key=severity != "error"): errors first)
    });
    Json listed = Json::array();
    for (const Issue* issue : sorted) listed.push_back(issue->to_json());
    out["issues"] = std::move(listed);
    Json paths = Json::array();
    for (const std::string& file : files) paths.push_back(file);
    out["files"] = std::move(paths);
    return out;
}

std::string wording_error(const std::string& message) {
    static const QRegularExpression op(QStringLiteral(R"(^(ops\[\d+\] [a-z_]+: )(.*))"), QRegularExpression::DotMatchesEverythingOption);
    const QString text = QString::fromStdString(message);
    const auto m = op.match(text);
    const QString prefix = m.hasMatch() ? m.captured(1) : QString();
    const QString core = m.hasMatch() ? m.captured(2) : text;
    return (prefix + wording::reason(core)).toStdString();
}

ToolResult fail(const std::string& message, const std::string& code, const std::string& path) {
    const std::string shown = wording_error(message);
    const std::string head = message.substr(0, message.find(" ‖ "));
    const std::string short_words = head != message ? wording_error(head) : shown;
    Json data = Json::object();
    data["error"] = shown;
    if (short_words != head) data["detail"] = head;
    return ToolResult(false, std::move(data), {error(code, path, short_words)});
}

}  // namespace genko::service
