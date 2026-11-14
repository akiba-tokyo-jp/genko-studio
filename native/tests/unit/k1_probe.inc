// The probe of K1's compatibility check: the demo4 names submitted (committed) through the agent's tool into a new book,
// with counted ids; project.json with its book id (always random) zeroed, and each page's plan input_hash.
// (The same code runs in 8362d5fc's build — where its numbers were taken — and in test_layout_ideas.)
inline std::pair<std::string, std::string> k1_probe(const std::string& root) {
    genko::core::ScopedIdSource ids(genko::core::counting_ids());
    const auto fixture = [](const char* name) {
        QFile file(QStringLiteral(GENKO_REPO_ROOT "/native/tests/fixtures/studio/demo4/") + QString::fromLatin1(name));
        if (!file.open(QIODevice::ReadOnly)) return genko::core::Json();
        return genko::core::Json::parse(file.readAll().toStdString());
    };
    genko::service::StudioService agent(root, "ai:test");
    agent.call("create_project", genko::core::Json::array({"demo.genko", "demo", 4}), genko::core::Json::object());
    agent.call("set_bible", genko::core::Json::array({"demo.genko", fixture("bible.json")}), genko::core::Json::object({{"commit", true}}));
    agent.call("set_script", genko::core::Json::array({"demo.genko", fixture("script.json")}), genko::core::Json::object({{"commit", true}}));
    std::string hashes;
    for (const char* name : {"p001.json", "p002.json", "p003.json", "p004.json"}) {
        const auto result = agent.call("submit_name", genko::core::Json::array({"demo.genko", fixture(name)}), genko::core::Json::object({{"commit", true}}));
        hashes += result.ok ? "ok " : "NG ";
    }
    QFile file(QString::fromStdString(root + "/demo.genko/project.json"));
    if (!file.open(QIODevice::ReadOnly)) return {"no project.json", hashes};
    genko::core::Json project = genko::core::Json::parse(file.readAll().toStdString());
    project["book_id"] = std::string(32, '0');
    // (a book made new names 源暎アンチック as its usual face since the user's decision of 2026-11-14 — font_path and its
    // fonts@1 —, which 8362d5fc's did not: the two keys put back as that build wrote them, the rest compared as it was.
    // A no-op in that build. test_layout_ideas checks the book did name the face.)
    if (project.value("font_path", std::string()) == "antique_genei") project["font_path"] = "";
    if (project.contains("features") && project["features"].is_array()) {
        genko::core::Json kept = genko::core::Json::array();
        for (const auto& f : project["features"])
            if (f != genko::core::Json("fonts@1")) kept.push_back(f);
        project["features"] = kept;
    }
    for (const auto& page : project["pages"]) hashes += page["plan"]["input_hash"].get<std::string>() + " ";
    const std::string text = project.dump();
    const std::string sha = QCryptographicHash::hash(QByteArray::fromStdString(text), QCryptographicHash::Sha256).toHex().toStdString();
    return {sha, hashes};
}
