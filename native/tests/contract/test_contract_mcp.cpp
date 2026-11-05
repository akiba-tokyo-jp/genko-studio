// `genko mcp` (stdio and Streamable HTTP) and `genko serve` against Python's (src/genko/mcp/server.py on the MCP SDK
// that uv.lock pins, and src/genko/server.py): the jobs of contract/mcp_cases.json (tools/migration/make_mcp_cases.py),
// each run on this build and on Python by tools/migration/mcp_harness.py, which drives both the same way — the raw
// JSON-RPC of both protocol eras over stdio (the handshake and its versions, the params the SDK refuses, the 2026 era's
// envelope, methods not served, lines that are not messages, invalid UTF-8, cancellation, the end of input with a tool
// still running), the raw Streamable HTTP (sessions, SSE answers, 202, the GET stream, DELETE, the Accept, Content-Type,
// session, Host, Origin, size and method refusals; the 2026 era's single POSTs and routing headers), the MCP SDK's own
// Client over stdio and HTTP in both eras (the 41 tools, the 4 resources read, tools that work, fail, are cancelled,
// send pictures and run background jobs, an agent's refusals: gates, page locks, the person's switch for chat
// approvals, another conversation's or a person's change, the book held back once for another conversation; the tool
// log and presence it leaves in the book) and the HTTP API (the guard: Host, Origin, Content-Type, tokens; paths
// outside the root and links out of it; a book made, changed as an agent and as a person, drawn and written out;
// uploads). Both records must be the same once normalised (paths, random ids, times; the revision, which this build
// counts on through an Undo, and the replies after an Undo went through, compared with their objects' keys sorted: a
// state this build restores is canonical JSON, schema-v4.md; the op catalogue's words the user changed on 2026-10-28
// put into Python's: testsupport's with_words_of_2026_10_28), the pictures within the drawing tolerance of
// ARCHITECTURE.md §9 (mean absolute difference ≤ 2/255, 99% of pixels within 32/255), Python's text laid out with
// Pillow's BASIC layout as render_harness holds the reference. Skipped without a Python that has the mcp extra
// ($GENKO_MCP_PYTHON), as the other contract tests are without the reference Python.

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <cmath>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

Json cases() { return genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/mcp_cases.json"); }

// The pictures in a record ({"png": {"mode", "size", "file"}}), in order.
void pictures(Json& value, std::vector<Json*>& out) {
    if (value.is_object()) {
        if (value.contains("png") && value["png"].is_object() && value["png"].contains("file")) out.push_back(&value["png"]);
        for (auto& [key, item] : value.items()) {
            (void)key;
            pictures(item, out);
        }
    } else if (value.is_array()) {
        for (Json& item : value) pictures(item, out);
    }
}

// Whether two pictures are the same drawing (ARCHITECTURE.md §9), and how far apart they are.
bool close(const QString& a_file, const QString& b_file, std::string& why) {
    const auto a = genko::render::read_png(genko::test::read_bytes(a_file)).convert("RGBA");
    const auto b = genko::render::read_png(genko::test::read_bytes(b_file)).convert("RGBA");
    if (a.width() != b.width() || a.height() != b.height()) {
        why = "sizes differ";
        return false;
    }
    const std::string pa = a.tobytes();
    const std::string pb = b.tobytes();
    double total = 0;
    std::size_t far = 0;
    for (std::size_t i = 0; i < pa.size(); i += 4) {
        int worst = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            const int d = std::abs(static_cast<int>(static_cast<unsigned char>(pa[i + c])) - static_cast<int>(static_cast<unsigned char>(pb[i + c])));
            total += d;
            worst = std::max(worst, d);
        }
        if (worst > 32) ++far;
    }
    const double mean = pa.empty() ? 0 : total / static_cast<double>(pa.size());
    const double far_share = pa.empty() ? 0 : static_cast<double>(far) / static_cast<double>(pa.size() / 4);
    why = "mean " + std::to_string(mean) + ", pixels past 32: " + std::to_string(far_share * 100) + "%";
    return mean <= 2.0 && far_share <= 0.01;
}

}  // namespace

class ContractMcp : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        if (genko::test::mcp_python().isEmpty()) {
            QSKIP("no Python with the mcp extra: set GENKO_MCP_PYTHON (a venv with uv.lock's mcp, pydantic, Pillow and numpy)");
        }
        QVERIFY(scratch_.isValid());
    }

    void jobs_data() {
        QTest::addColumn<int>("index");
        const Json all = cases();
        for (std::size_t i = 0; i < all.size(); ++i) QTest::newRow(all[i]["name"].get<std::string>().c_str()) << static_cast<int>(i);
    }

    void jobs() {
        QFETCH(int, index);
        const Json job = cases()[static_cast<std::size_t>(index)];
        const QString name = QString::fromStdString(job["name"].get<std::string>());
        Json records[2];
        QString folders[2];
        for (int side = 0; side < 2; ++side) {
            const QString dir = scratch_.path() + "/" + name + (side == 0 ? "/cpp" : "/py");
            QVERIFY(QDir().mkpath(dir));
            Json full = Json::object();
            full["server"] = side == 0 ? Json::object({{"kind", "cpp"}, {"genko", genko::test::genko_cli().toStdString()}})
                                       : Json::object({{"kind", "py"},
                                                       {"python", genko::test::mcp_python().toStdString()},
                                                       {"repo", genko::test::repo_root().toStdString()}});
            full["root"] = (dir + "/root").toStdString();
            full["config"] = (dir + "/config").toStdString();
            full["agent"] = "ai:harness";
            full["steps"] = job["steps"];
            if (job.contains("allow_origins")) full["allow_origins"] = job["allow_origins"];
            genko::test::write_bytes(dir + "/job.json", genko::core::dump_python(full));
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.remove(QStringLiteral("PYTHONPATH"));
            env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
            env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
            const genko::test::Run run = genko::test::run(
                genko::test::mcp_python(),
                {genko::test::repo_root() + "/tools/migration/mcp_harness.py", QStringLiteral("run"), dir + "/job.json", dir + "/out.json"}, env,
                1800000);
            QVERIFY2(run.finished && run.exit_code == 0, (QString(side == 0 ? "this build: " : "Python: ") + QString::fromUtf8(run.err.right(4000))).toUtf8());
            records[side] = genko::test::read_json(dir + "/out.json");
            folders[side] = dir + "/out.json.images";
        }
        // the pictures: compared by their pixels, then left out of the records
        std::vector<Json*> ours;
        std::vector<Json*> theirs;
        pictures(records[0], ours);
        pictures(records[1], theirs);
        QCOMPARE(ours.size(), theirs.size());
        for (std::size_t i = 0; i < ours.size(); ++i) {
            std::string why;
            const QString a = folders[0] + "/" + QString::fromStdString((*ours[i])["file"].get<std::string>());
            const QString b = folders[1] + "/" + QString::fromStdString((*theirs[i])["file"].get<std::string>());
            QVERIFY2(close(a, b, why), (a + " vs " + b + ": " + QString::fromStdString(why)).toUtf8());
            (*ours[i])["file"] = "<compared>";
            (*theirs[i])["file"] = "<compared>";
        }
        // (the user's decisions of 2026-10-28: the catalogue's words this build changed, in Python's replies)
        genko::test::with_words_of_2026_10_28(records[1]);
        std::string where;
        QVERIFY2(genko::test::strict_equal(records[0], records[1], &where), where.c_str());
    }

private:
    QTemporaryDir scratch_;
};

QTEST_GUILESS_MAIN(ContractMcp)
#include "test_contract_mcp.moc"
