#include <catch2/catch_all.hpp>
#include "slang/text/SourceManager.h"
#include <fstream>

using namespace slang;
namespace fs = std::filesystem;

TEST_CASE("Update Source Tests") {
    SourceManager sm;
    auto path = fs::temp_directory_path() / "test_file.sv";

    // Write initial content
    {
        std::ofstream file(path);
        file << "module m; endmodule";
    }

    auto getText = [&](BufferID id) -> std::string_view {
        auto txt = sm.getSourceText(id);
        if (!txt.empty() && txt.back() == '\0')
            txt.remove_suffix(1);
        return txt;
    };

    // Load first version
    SourceManager::BufferOrError buffer1 = sm.readSource(path, nullptr);
    REQUIRE(buffer1);
    CHECK(getText(buffer1->id) == "module m; endmodule");

    // Update source in memory
    std::string_view newText = "module n; endmodule";
    SourceBuffer buffer2 = sm.updateSource(path, newText);

    CHECK(buffer2.id != buffer1->id);
    CHECK(getText(buffer2.id) == "module n; endmodule");

    // Check old buffer is still valid
    CHECK(getText(buffer1->id) == "module m; endmodule");

    // Check subsequent reads see the updated version
    auto buffer3 = sm.readSource(path, nullptr);
    REQUIRE(buffer3);
    CHECK(buffer3->id != buffer1->id);
    CHECK(buffer3->id != buffer2.id); // It creates a new buffer entry
    CHECK(getText(buffer3->id) == "module n; endmodule");

    // Verify that the old file data is still accessible via BufferID 1
    // even though the cache has been updated.
    // This confirms shared_ptr kept the old revisions alive.
    CHECK(getText(buffer1->id) == "module m; endmodule");

    // Cleanup
    fs::remove(path);
}

TEST_CASE("Retire Buffer Tests") {
    SourceManager sm;
    auto path = fs::temp_directory_path() / "retire_test_file.sv";

    auto getText = [&](BufferID id) -> std::string_view {
        auto txt = sm.getSourceText(id);
        if (!txt.empty() && txt.back() == '\0')
            txt.remove_suffix(1);
        return txt;
    };

    SourceBuffer buffer1 = sm.assignText(path.string(), "module m; endmodule");
    SourceBuffer buffer2 = sm.updateSource(path, "module n; endmodule");
    CHECK(getText(buffer1.id) == "module m; endmodule");
    CHECK(getText(buffer2.id) == "module n; endmodule");

    // The superseded revision releases its text; the current one is untouched.
    CHECK(sm.retireBuffer(buffer1.id));
    CHECK(getText(buffer1.id).empty());
    CHECK(getText(buffer2.id) == "module n; endmodule");

    // Retiring is idempotent, and reports that there was nothing left to do.
    CHECK(!sm.retireBuffer(buffer1.id));

    // The entry survives as a tombstone, so later buffers keep their IDs and
    // every location already handed out still refers to the same buffer.
    SourceBuffer buffer3 = sm.updateSource(path, "module o; endmodule");
    CHECK(buffer3.id != buffer1.id);
    CHECK(buffer3.id != buffer2.id);
    CHECK(getText(buffer2.id) == "module n; endmodule");
    CHECK(getText(buffer3.id) == "module o; endmodule");

    // Queries against a retired buffer degrade rather than fail.
    CHECK(sm.getRawFileName(buffer1.id).empty());
    CHECK(sm.getLineNumber(SourceLocation(buffer1.id, 0)) == 0);

    // The lookup cache holds the newest revision of a path, so retiring that
    // entry does not free the text and a re-read still finds it.
    CHECK(sm.retireBuffer(buffer3.id));
    auto buffer4 = sm.readSource(path, nullptr);
    REQUIRE(buffer4);
    CHECK(getText(buffer4->id) == "module o; endmodule");

    // Expansion buffers are not file buffers and have nothing to retire.
    CHECK(!sm.retireBuffer(BufferID()));
}
