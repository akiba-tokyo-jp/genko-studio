// 入稿前の点検 (formats/checks: Python's genko/checks.py, with the studio preflight it folds in) against Python's
// checks.book on the same books (render_harness.py check-cases): chosen pages that reach every check — lines not
// placed, beyond the trim or the basic frame, lettered too small, with buried tails, overlapping (and grouped); faces and
// people cut by panels; pictures pasted at low resolution; paint layers; art beyond the bleed; faces reported on art
// since replaced; tones beating into a moiré (tone layers, a screened layer, a region, patches, scrape lines); spreads
// that do not face; art only on layers that do not print; empty pages; books made with agents (approvals, stages, panels
// without art, character sheets, placed pictures missing, at low resolution, enlarged, test images, without provenance;
// with and without the book's folder; one whose characters the preflight cannot read); a reported face whose place is
// not numbers (Python's ValueError: this build throws too) — and random drawing books with
// balloons and covers. Each report is the same text as Python's json.dumps writes it, byte for byte. Skipped without the
// Python reference.

#include <QtTest>

#include <QTemporaryDir>

#include <set>
#include <string>

#include "core/json.hpp"
#include "core/paths.hpp"
#include "formats/checks.hpp"
#include "render/brushes.hpp"
#include "render/page.hpp"
#include "render/text/lettering.hpp"
#include "storage/reader.hpp"
#include "rendertest.hpp"

using genko::core::Json;
namespace fs = std::filesystem;

class TestContractChecks : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    Json results_;

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        const auto made = genko::test::render_harness({"check-cases", scratch_.path() + "/run", "--seed", "7", "--count", "40"}, scratch_.path());
        QVERIFY2(made.finished && made.exit_code == 0, made.err.constData());
        results_ = genko::test::read_json(scratch_.path() + "/run/checks.json");
    }

    // Every book's report the same as Python's, byte for byte; every check of checks.py and the preflight met.
    void reportsAsPythons() {
        std::set<std::string> codes;
        int compared = 0;
        for (const auto& [name, want] : results_.items()) {
            const fs::path book = genko::core::path_from_utf8(scratch_.path().toStdString() + "/run/books/" + name);
            genko::render::clear_render_caches();
            genko::render::text::clear_layout_cache();
            const auto loaded = genko::storage::load_document(book);
            genko::render::brushes::clear_custom();
            genko::render::brushes::register_book(loaded.document.brush_custom);
            const std::optional<fs::path> project = want["project"].get<bool>() ? std::optional<fs::path>(book) : std::nullopt;
            if (want.contains("error")) {
                bool threw = false;
                try {
                    (void)genko::formats::checks::book(loaded.document, project);
                } catch (const std::exception&) {
                    threw = true;
                }
                QVERIFY2(threw, (name + ": Python raised " + want["error"].dump()).c_str());
                continue;
            }
            const Json report = genko::formats::checks::book(loaded.document, project);
            const std::string got = genko::core::dump_python(report);
            const std::string& expected = want["report"].get_ref<const std::string&>();
            if (got != expected) {
                std::size_t at = 0;
                while (at < got.size() && at < expected.size() && got[at] == expected[at]) ++at;
                const std::size_t from = at > 200 ? at - 200 : 0;
                QFAIL(qPrintable(QString::fromStdString(name + " differs at " + std::to_string(at) + "\n  C++:    " + got.substr(from, 500) +
                                                        "\n  Python: " + expected.substr(from, 500))));
            }
            for (const Json& issue : report["issues"]) codes.insert(issue["code"].get<std::string>());
            ++compared;
        }
        QVERIFY(compared >= 46);
        QVERIFY(results_["regions-broken.genko"].contains("error"));  // (Python stops there: so does this build)
        for (const char* code : {"line_unplaced", "text_outside_trim", "text_outside_frame", "text_too_small", "tail_hidden", "text_overlap",
                                 "cut_by_panel", "low_dpi", "paint_dpi", "art_outside_page", "regions_stale", "tone_moire", "spread_not_facing",
                                 "art_not_printed", "empty_page", "name_not_approved", "art_not_approved", "page_not_finished",
                                 "panel_without_art", "asset_missing", "fixture_image", "upscaled", "provenance_missing", "sheet_not_approved"}) {
            QVERIFY2(codes.count(code) == 1, code);
        }
        qInfo("checks: %d books, the same reports as Python's (%zu kinds of issue met)", compared, codes.size());
    }
};

QTEST_GUILESS_MAIN(TestContractChecks)
#include "test_contract_checks.moc"
