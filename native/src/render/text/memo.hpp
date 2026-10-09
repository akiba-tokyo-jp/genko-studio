#pragma once

#include <QByteArrayView>
#include <QCryptographicHash>

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "core/json.hpp"

// Internal to render/text: pictures remembered by what made them (a line's letters, a balloon's masks), so a page
// drawn again or in parts (the canvas's tiles, a render region) works each out once. A value is only ever what the
// same computation gives again: forgetting it changes no pixel. Safe for several threads.

namespace genko::render::text::detail {

// The key of a computation: its inputs as Python's json.dumps writes them (1 and 1.0 apart). Nothing when they cannot
// be written so (NaN, a text that is not UTF-8): such a computation is not remembered.
inline std::optional<std::string> memo_key(const core::Json& inputs) {
    try {
        return core::dump(inputs, core::DumpOptions{});
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// A text as a key: its SHA-256 and its length (a picture's base64 can be megabytes; its key need not be).
inline std::string digest_of(const std::string& text) {
    const QByteArray hash = QCryptographicHash::hash(QByteArrayView(text.data(), static_cast<qsizetype>(text.size())), QCryptographicHash::Sha256);
    return hash.toHex().toStdString() + ":" + std::to_string(text.size());
}

// A line's style as a key: [the style without its long texts, [[key, digest_of(text)], …]] — a picture balloon's
// picture, picture letters' fill — so the key stays small and quick to compare. (Any other style: [style].)
inline core::Json keyed_style(const core::Json& style) {
    constexpr std::size_t kLong = 1024;
    if (!style.is_object()) return core::Json::array({style});
    core::Json kept = core::Json::object();
    core::Json long_texts = core::Json::array();
    for (const auto& [key, value] : style.items()) {
        if (value.is_string() && value.get_ref<const std::string&>().size() > kLong) {
            long_texts.push_back(core::Json::array({key, digest_of(value.get_ref<const std::string&>())}));
        } else {
            kept[key] = value;
        }
    }
    return core::Json::array({kept, long_texts});
}

template <class T>
class Memo {
public:
    Memo(std::size_t count, std::int64_t bytes) : count_(count), budget_(bytes) {}

    std::shared_ptr<const T> get(const std::string& key) {
        const std::lock_guard lock(mutex_);
        const auto it = index_.find(key);
        if (it == index_.end()) return nullptr;
        items_.splice(items_.begin(), items_, it->second);  // (the most recently used first)
        return it->second->value;
    }

    // (bytes: what the value takes; its key is kept too, and counts)
    void put(const std::string& key, std::shared_ptr<const T> value, std::int64_t bytes) {
        bytes += static_cast<std::int64_t>(key.size());
        const std::lock_guard lock(mutex_);
        if (bytes > budget_ / 4) return;  // (a very large picture is not kept)
        if (const auto it = index_.find(key); it != index_.end()) {
            used_ -= it->second->bytes;
            items_.erase(it->second);
            index_.erase(it);
        }
        items_.push_front(Item{key, std::move(value), bytes});
        index_[key] = items_.begin();
        used_ += bytes;
        while (!items_.empty() && (items_.size() > count_ || used_ > budget_)) {
            used_ -= items_.back().bytes;
            index_.erase(items_.back().key);
            items_.pop_back();
        }
    }

    void clear() {
        const std::lock_guard lock(mutex_);
        items_.clear();
        index_.clear();
        used_ = 0;
    }

private:
    struct Item {
        std::string key;
        std::shared_ptr<const T> value;
        std::int64_t bytes = 0;
    };
    std::mutex mutex_;
    std::list<Item> items_;
    std::map<std::string, typename std::list<Item>::iterator> index_;
    std::size_t count_;
    std::int64_t budget_;
    std::int64_t used_ = 0;
};

}  // namespace genko::render::text::detail
