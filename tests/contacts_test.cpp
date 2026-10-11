#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "mind/graph/RelationshipGraph.h"
#include "providers/llm/tool/ToolRegistry.h"
#include "runtime/ContactTools.h"

int main() {
    const auto dir = std::filesystem::temp_directory_path() /
        ("mio-contacts-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto file = dir / "relationships.json";
    std::string aliceId;
    {
        mio::RelationshipGraph graph(file);
        mio::ToolRegistry tools;
        mio::registerContactTools(tools, graph);
        assert(tools.find("upsert_contact"));
        assert(tools.find("list_contacts"));
        assert(tools.find("set_nickname"));
        assert(tools.find("set_notes"));

        // People learned about in a conversation can be remembered before they speak.
        auto added = nlohmann::json::parse(tools.invoke("upsert_contact",
            {{"name", "Alice"}, {"qq", "123456"}, {"notes", "likes tea"}}));
        aliceId = added.at("internal_id");
        assert(added.at("qq") == "123456");
        assert(graph.contacts().size() == 1);

        // A platform event for that contact must reuse its identity and chosen name.
        auto seen = graph.onSeen("napcat", "123456", "123456(platform nickname)", 100);
        assert(seen.internalId == aliceId);
        assert(graph.findByPlatform("onebot", "123456")->internalId == aliceId);
        assert(graph.resolveContact("123456").name == "Alice");
        assert(graph.resolveContact("[onebot][123456]").internalId == aliceId);

        tools.invoke("set_nickname", {{"current", "123456"}, {"nickname", "Ally"}});
        tools.invoke("set_notes", {{"person", aliceId}, {"notes", "likes coffee"}});
        assert(graph.resolveContact(aliceId).name == "Ally");
        assert(graph.resolveContact(aliceId).personal == "likes coffee");

        // Omitted fields stay intact on an upsert; IDs distinguish duplicate names.
        tools.invoke("upsert_contact", {{"name", "Ally"}, {"qq", "123456"}});
        assert(graph.resolveContact(aliceId).personal == "likes coffee");
        tools.invoke("upsert_contact", {{"name", "Ally"}, {"qq", "654321"}});
        bool ambiguous = false;
        try { tools.invoke("set_notes", {{"person", "Ally"}, {"notes", "wrong"}}); }
        catch (const std::invalid_argument&) { ambiguous = true; }
        assert(ambiguous);
        assert(graph.resolveContact("123456").personal == "likes coffee");
        tools.invoke("upsert_contact", {{"name", "123456"}, {"qq", "888888"}});
        tools.invoke("upsert_contact", {{"name", "[qq][123456]"}, {"qq", "999999"}});
        assert(graph.resolveContact("123456").internalId == aliceId);
        assert(graph.resolveContact("[qq][123456]").internalId == aliceId);

        // Name-only acquaintances may receive their QQ identity later.
        auto nameOnly = nlohmann::json::parse(tools.invoke("upsert_contact", {{"name", "Bob"}}));
        auto bound = nlohmann::json::parse(tools.invoke("upsert_contact",
            {{"name", "Bob"}, {"qq", "777777"}}));
        assert(nameOnly.at("internal_id") == bound.at("internal_id"));
        auto matches = nlohmann::json::parse(tools.invoke("list_contacts", {{"query", "777777"}}));
        assert(matches.size() == 1 && matches[0].at("name") == "Bob");

        // Refuse incomplete or contradictory identities instead of polluting the graph.
        const auto before = graph.contacts().size();
        for (const auto& args : {nlohmann::json{{"name", "Bad"}, {"platform", "qq"}},
                                 nlohmann::json{{"name", "Bad"}, {"qq", "not-a-number"}},
                                 nlohmann::json{{"name", "Bad"}, {"qq", "123456"}, {"user_id", "999999"}}}) {
            bool rejected = false;
            try { tools.invoke("upsert_contact", args); }
            catch (const std::invalid_argument&) { rejected = true; }
            assert(rejected);
        }
        assert(graph.contacts().size() == before);
    }
    {
        mio::RelationshipGraph reloaded(file);
        assert(reloaded.resolveContact(aliceId).name == "Ally");
        assert(reloaded.resolveContact(aliceId).personal == "likes coffee");
        assert(reloaded.resolveContact("777777").name == "Bob");
    }
    std::filesystem::remove_all(dir);
    std::cout << "contacts_test passed\n";
}
