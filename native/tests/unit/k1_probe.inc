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
    for (const auto& page : project["pages"]) hashes += page["plan"]["input_hash"].get<std::string>() + " ";
    const std::string text = project.dump();
    const std::string sha = QCryptographicHash::hash(QByteArray::fromStdString(text), QCryptographicHash::Sha256).toHex().toStdString();
    return {sha, hashes};
}
