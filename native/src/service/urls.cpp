#include "service/urls.hpp"

#include <QString>

#include "core/error.hpp"

namespace genko::service::urls {

namespace {

bool ascii_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool ascii_digit(char c) { return c >= '0' && c <= '9'; }
bool scheme_char(char c) { return ascii_alpha(c) || ascii_digit(c) || c == '+' || c == '-' || c == '.'; }

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// urllib.parse._splitnetloc(url, 2)
std::pair<std::string, std::string> split_netloc(const std::string& url) {
    std::size_t delim = url.size();
    for (char c : {'/', '?', '#'}) {
        const std::size_t at = url.find(c, 2);
        if (at != std::string::npos && at < delim) delim = at;
    }
    return {url.substr(2, delim - 2), url.substr(delim)};
}

}  // namespace

Parts urlsplit(std::string_view text) {
    std::string url(text);
    // (the WHATWG C0 controls and space stripped from the front; tab, CR and LF removed everywhere)
    std::size_t lead = 0;
    while (lead < url.size() && static_cast<unsigned char>(url[lead]) <= 0x20) ++lead;
    url.erase(0, lead);
    std::string cleaned;
    for (char c : url) {
        if (c != '\t' && c != '\r' && c != '\n') cleaned += c;
    }
    url = std::move(cleaned);
    Parts out;
    const std::size_t colon = url.find(':');
    if (colon != std::string::npos && colon > 0 && ascii_alpha(url[0])) {
        bool all = true;
        for (std::size_t i = 0; i < colon; ++i) all = all && scheme_char(url[i]);
        if (all) {
            out.scheme = lower(url.substr(0, colon));
            url = url.substr(colon + 1);
        }
    }
    if (url.compare(0, 2, "//") == 0) {
        auto [netloc, rest] = split_netloc(url);
        const bool open = netloc.find('[') != std::string::npos;
        const bool close = netloc.find(']') != std::string::npos;
        if (open != close) throw core::PyValueError("Invalid IPv6 URL");
        out.netloc = std::move(netloc);
        url = std::move(rest);
    }
    if (const std::size_t hash = url.find('#'); hash != std::string::npos) {
        out.fragment = url.substr(hash + 1);
        url = url.substr(0, hash);
    }
    if (const std::size_t q = url.find('?'); q != std::string::npos) {
        out.query = url.substr(q + 1);
        url = url.substr(0, q);
    }
    out.path = std::move(url);
    return out;
}

Parts urlparse(std::string_view text) {
    Parts out = urlsplit(text);
    // uses_params: '', ftp, hdl, prospero, http, imap, https, shttp, rtsp, rtsps, rtspu, sip, sips, mms, sftp, tel
    static const char* const kUsesParams[] = {"",     "ftp",  "hdl",   "prospero", "http",  "imap", "https", "shttp",
                                              "rtsp", "rtsps", "rtspu", "sip",      "sips",  "mms",  "sftp",  "tel"};
    bool uses = false;
    for (const char* s : kUsesParams) uses = uses || out.scheme == s;
    if (uses && out.path.find(';') != std::string::npos) {
        std::size_t i = std::string::npos;
        if (const std::size_t slash = out.path.rfind('/'); slash != std::string::npos) {
            i = out.path.find(';', slash);
        } else {
            i = out.path.find(';');
        }
        if (i != std::string::npos) {
            out.params = out.path.substr(i + 1);
            out.path = out.path.substr(0, i);
        }
    }
    return out;
}

std::string quote(std::string_view text) {
    static const char* const kHex = "0123456789ABCDEF";
    std::string out;
    for (char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (ascii_alpha(ch) || ascii_digit(ch) || ch == '_' || ch == '.' || ch == '-' || ch == '~' || ch == '/') {
            out += ch;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string unquote_to_bytes(std::string_view text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '%') {
            const int hi = i + 1 < text.size() ? hex_value(text[i + 1]) : -1;
            const int lo = i + 2 < text.size() ? hex_value(text[i + 2]) : -1;
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 3;
                continue;
            }
        }
        out += text[i];
        ++i;
    }
    return out;
}

std::string unquote(std::string_view text) {
    if (text.find('%') == std::string_view::npos) return std::string(text);
    // (the ASCII runs are unquoted to bytes and decoded as UTF-8 with replacement; the rest is kept as it is)
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = i;
        const bool ascii = static_cast<unsigned char>(text[i]) < 0x80;
        while (j < text.size() && (static_cast<unsigned char>(text[j]) < 0x80) == ascii) ++j;
        const std::string_view run = text.substr(i, j - i);
        if (ascii) {
            const std::string bytes = unquote_to_bytes(run);
            out += QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size())).toStdString();
        } else {
            out += run;
        }
        i = j;
    }
    return out;
}

std::optional<std::string> parse_qs_first(std::string_view query, std::string_view name) {
    std::size_t start = 0;
    for (;;) {
        const std::size_t amp = query.find('&', start);
        const std::string_view pair = query.substr(start, amp == std::string_view::npos ? std::string_view::npos : amp - start);
        if (!pair.empty()) {
            const std::size_t eq = pair.find('=');
            if (eq != std::string_view::npos && eq + 1 < pair.size()) {
                std::string key(pair.substr(0, eq));
                std::string value(pair.substr(eq + 1));
                for (char& c : key) c = c == '+' ? ' ' : c;
                for (char& c : value) c = c == '+' ? ' ' : c;
                if (unquote(key) == name) return unquote(value);
            }
        }
        if (amp == std::string_view::npos) break;
        start = amp + 1;
    }
    return std::nullopt;
}

std::string lower(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())).toLower().toStdString();
}

}  // namespace genko::service::urls
